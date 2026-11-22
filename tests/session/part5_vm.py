#!/usr/bin/env python3
"""Exercise Part 5 on the dedicated Arch guest through the Council VM runner."""

import argparse
from contextlib import contextmanager
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
COUNCIL = ROOT.parent / "Council/seaside-vm/scripts/vm_runner.py"
SCAN = {
    "open": ("1c", "9c"),
    "left": ("e0", "4b", "e0", "cb"),
    "right": ("e0", "4d", "e0", "cd"),
    "workspace-one": ("02", "82"),
    "workspace-two": ("03", "83"),
}
CAPTURES = {"dwindle.png", "scrolling.png"}
BUNDLE_LIMIT = 64 * 1024 * 1024


def mouse(runner, vm, request):
    start = request["start"]
    end = request.get("end", start)
    for point in (start, end):
        if len(point) != 2 or any(type(p) is not int or not 0 <= p < 16384 for p in point):
            raise RuntimeError("Invalid Part 5 pointer coordinates")
    button = request["button"]
    if button not in (1, 2):
        raise RuntimeError("Invalid Part 5 pointer button")
    x, y = (p + 1 for p in start)
    end_x, end_y = (p + 1 for p in end)
    before_x = x + 17 if x < 16367 else x - 17
    machine = "'" + vm.replace("'", "''") + "'"
    actions = (
        "$ErrorActionPreference = 'Stop'",
        "$virtualbox = New-Object -ComObject VirtualBox.VirtualBox",
        "$session = New-Object -ComObject VirtualBox.Session",
        f"$virtualbox.FindMachine({machine}).LockMachine($session, 1)",
        "$pressed = $false",
        "try { $mouse = $session.Console.Mouse",
        "if (-not $mouse.AbsoluteSupported) { throw 'Guest absolute mouse is unavailable' }",
        f"$mouse.PutMouseEventAbsolute({before_x}, {y}, 0, 0, 0)",
        "Start-Sleep -Milliseconds 120",
        f"$mouse.PutMouseEventAbsolute({x}, {y}, 0, 0, 0)",
        "Start-Sleep -Milliseconds 120",
        f"$pressed = $true; $mouse.PutMouseEventAbsolute({x}, {y}, 0, 0, {button})",
        "Start-Sleep -Milliseconds 150",
        f"$mouse.PutMouseEventAbsolute({end_x}, {end_y}, 0, 0, {button})",
        "Start-Sleep -Milliseconds 150",
        "} finally { try { if ($pressed) { "
        f"$mouse.PutMouseEventAbsolute({end_x}, {end_y}, 0, 0, 0)"
        " } } finally { $session.UnlockMachine() } }",
    )
    drag = "end" in request
    try:
        if drag:
            runner.keys(vm, "e0", "5b")
        subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                        "; ".join(actions)], check=True, timeout=15)
    finally:
        if drag:
            runner.keys(vm, "e0", "db")


def route(runner, vm, message):
    if action := message.get("key"):
        runner.keys(vm, "e0", "5b", *SCAN[action], "e0", "db")
    if request := message.get("mouse"):
        mouse(runner, vm, request)
    marker = message.get("marker")
    if marker is None:
        return
    if not re.fullmatch(r"/run/user/\d+/seaside-vm-[\w-]+/[a-z-]+\.txt", marker):
        raise RuntimeError("Invalid Part 5 marker path")
    time.sleep(.4)
    path = shlex.quote(marker)
    runner.type_command(vm, '{ printf \'%s\\n\' "$$"; stty size; '
                        'printf \'%s\\n\' "$WAYLAND_DISPLAY"; } > ' + path + ".tmp"
                        + " && mv -- " + path + ".tmp " + path)


def load_runner():
    spec = spec_from_file_location("seaside_vm_runner", COUNCIL)
    runner = module_from_spec(spec)
    spec.loader.exec_module(runner)
    return runner


def run(args, revision, bundle, *, scenario="part5", captures=None, send_input=route,
        settings="", scenario_source=None, downloads=None):
    runner = load_runner()
    if captures is None:
        captures = CAPTURES | {"dwindle.log", "scrolling.log"}
    runner.ARTIFACTS.update(captures)
    standard_worker = COUNCIL.with_name("guest_trial.py").read_text()
    if scenario_source is None:
        scenario_source = Path(__file__).with_name(f"{scenario}_vm_guest.py").read_text()
    worker = ("standard = {'__name__': 'seaside_vm_guest'}\n"
              f"exec(compile({standard_worker!r}, 'guest_trial.py', 'exec'), standard)\n"
              + settings + scenario_source).encode()
    parent = runner.RunBridge
    files = {"worker": worker, "bundle": bundle, **(downloads or {})}

    class ScenarioBridge(parent):
        def __init__(self, directory, token):
            super().__init__(directory, token)
            queue = self.events

            class Events:
                put = queue.put

                def get(self, timeout):
                    deadline = time.monotonic() + timeout
                    message = queue.get(timeout=timeout)
                    while message["phase"] == f"{scenario}-input":
                        runner.save_event(directory, message)
                        send_input(runner, args.vm, message)
                        message = queue.get(timeout=max(0, deadline - time.monotonic()))
                    return message

            self.events = Events()

        def handler(self):
            handler = super().handler()
            prefix = self.prefix

            class BundleHandler(handler):
                def do_GET(self):
                    name = self.path.removeprefix(prefix)
                    if self.path.startswith(prefix) and name in files:
                        self.send_bytes(files[name])
                    else:
                        super().do_GET()

            return BundleHandler

    runner.RunBridge = ScenarioBridge
    return runner.run(args.vm, "tty" if args.tty_only else "run", revision, args.state)


@contextmanager
def revision_bundle():
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                                       timeout=5).strip()
    if not re.fullmatch("[0-9a-f]{40}", revision):
        raise RuntimeError("Expected an exact Git revision")
    with tempfile.TemporaryDirectory(prefix="seaside-vm-bundle-") as name:
        archive = Path(name) / "revision.bundle"
        subprocess.run(["git", "bundle", "create", str(archive), "HEAD"], cwd=ROOT, check=True,
                       timeout=30)
        if archive.stat().st_size > BUNDLE_LIMIT:
            raise RuntimeError("The revision bundle exceeds the test limit")
        yield revision, archive.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console-ready", required=True, action="store_true",
                        help="confirm active TTY 1 shows an idle guest shell")
    parser.add_argument("--vm", default="Arch Seaside Testing")
    parser.add_argument("--state", type=Path, default=Path.home() / ".local/state/seaside-vm")
    parser.add_argument("--tty-only", action="store_true")
    args = parser.parse_args()
    with revision_bundle() as (revision, bundle):
        return run(args, revision, bundle)


if __name__ == "__main__":
    raise SystemExit(main())
