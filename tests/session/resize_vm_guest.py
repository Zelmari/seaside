"""Check real Dwindle opening and dragging with late Wayland buffers."""

from contextlib import ExitStack, contextmanager
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time


event = standard["event"]
upload = standard["upload"]
revision = standard["REVISION"]
base = standard["BASE"]
require = helpers["require"]
checked = helpers["checked"]
fixture = helpers["fixture"]
NATIVE_CLIENT = "seaside-part7-native-client"
CONFIG = """[window]
layout = "dwindle"
gaps_outer = 0
gaps_inner = 0
border_width = 0
"""
RED = bytes.fromhex("ab2929")
GREEN = bytes.fromhex("29ab29")
LEFT = "Resize Left"
RIGHT = "Resize Right"


@contextmanager
def paused(*clients):
    suspended = []
    try:
        for child in clients:
            require(child.poll() is None, "A pattern client exited before its pause")
            child.send_signal(signal.SIGSTOP)
            suspended.append(child)
        yield
    finally:
        for child in suspended:
            if child.poll() is None:
                child.send_signal(signal.SIGCONT)


def sample_row(session, origin, width):
    x, y = origin
    data = checked(["grim", "-s", "1", "-t", "ppm", "-g", f"{x},{y} {width}x1", "-"],
                   env=session.env)
    magic, columns, rows, depth, pixels = data.split(maxsplit=4)
    require((magic, columns, rows, depth) == (b"P6", str(width).encode(), b"1", b"255")
            and len(pixels) == width * 3, "grim did not return the requested RGB row")
    return pixels


def divider(pixels):
    width = len(pixels) // 3
    split = next((column for column in range(width)
                  if pixels[column * 3:column * 3 + 3] != RED), width)
    return split if split > 0 and pixels[split * 3:] == GREEN * (width - split) else None


def run_resize(session, state):
    output = session.wait(session.focused_output, "the focused guest output")
    require(output["hardware_cursor"], "The virtual output has no working cursor plane")
    require([row["id"] for row in session.ctl("outputs")["outputs"] if row["enabled"]]
            == [output["id"]], "The resize trial requires exactly one enabled virtual output")
    width, height = output["mode"]["width"], output["mode"]["height"]
    require(640 <= width <= 4096 and 360 <= height <= width,
            "Use a landscape virtual output of at least 640×360 and at most 4096×4096")
    require(output["scale"] == 1.0 and output["rotation"] == 0,
            "The resize trial requires an untransformed, unit-scale output")
    x, y = output["position"]["x"], output["position"]["y"] + height // 2
    state.update(output=output, renderer=session.diagnostics(output)["renderer"])
    left_log = session.results / "resize-left-client.log"
    right_log = session.results / "resize-right-client.log"

    def committed_sizes(log):
        sizes = re.findall(r"^color [0-9a-f]{6} (\d+) (\d+)$", log.read_text(), re.M)
        return [tuple(map(int, size)) for size in sizes]

    def last_width(log):
        sizes = committed_sizes(log)
        return sizes[-1][0] if sizes and sizes[-1][1] == height else None

    def covered(name):
        split = divider(sample_row(session, (x, y), width))
        if split is None:
            session.grab(output, name)
            raise RuntimeError(f"{name}: a pixel between the two tiles showed uncovered wallpaper")
        return split

    def settled(expected):
        def check():
            a, b = last_width(left_log), last_width(right_log)
            return (a is not None and b is not None and a + b == width and
                    abs(a - expected) <= 3 and divider(sample_row(session, (x, y), width)) == a)
        session.wait(check, f"both clients to present their final buffers near column {expected}")
        return last_width(left_log)

    def drag(name, expected, delta):
        initial = expected - delta
        start_x = (initial + width) // 2
        start = [x + start_x, y]
        end = [x + start_x + delta, y]
        event("resize-input", f"drag the divider {name} with both clients paused",
              mouse={"button": 2, "start": start, "end": end})
        observed = []
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            observed.append(covered(f"drag-{name}"))
            if abs(observed[-1] - expected) <= 3:
                session.grab(output, f"drag-{name}")
                return {"sampled_columns": observed, "expected_column": expected}
        session.grab(output, f"drag-{name}")
        raise RuntimeError(f"The {name} drag did not move while the clients were paused: {observed}")

    with ExitStack() as clients:
        left = clients.enter_context(fixture(session, left_log, NATIVE_CLIENT, LEFT, RED.hex(),
                                             "--delay-resize=750", interactive=True))
        session.wait(lambda: last_width(left_log) == width and session.window(LEFT) is not None and
                     divider(sample_row(session, (x, y), width)) == width,
                     "the settled single tile")
        with paused(left):
            right = clients.enter_context(fixture(session, right_log, NATIVE_CLIENT, RIGHT,
                                                  GREEN.hex(), "--delay-resize=750", interactive=True))
            samples = []
            deadline = time.monotonic() + 0.6
            while time.monotonic() < deadline:
                samples.append(covered("opening"))
            require(samples, "The opening trial took no frame samples")
            session.grab(output, "opening")
            state["opening_columns"] = samples

        initial = settled(width // 2)
        require(committed_sizes(right_log)[0] == (width - initial, height),
                "The new tiled client did not receive its layout size on the first configure")
        right_window = session.window(RIGHT)
        require(right_window and session.ctl("focus")["window_id"] == right_window["id"],
                "The newly presented tile did not gain keyboard focus")
        session.grab(output, "open-settled")

        span = width // 7
        with paused(left, right):
            state["drag_right"] = drag("right", initial + span, span)
        expanded = settled(initial + span)
        with paused(left, right):
            state["drag_left"] = drag("left", expanded - span, -span)
        state["final_column"] = settled(expanded - span)
        session.grab(output, "final")


def trial():
    tty, active = os.ttyname(0), standard["active"]()
    existing = subprocess.run(["pgrep", "-x", "-t", "tty1", "seaside"],
                              capture_output=True, timeout=3)
    event("connected", f"stdin={tty} active={active}")
    if tty != "/dev/tty1" or active != "tty1" or os.tcgetpgrp(0) != os.getpgrp() or existing.returncode != 1:
        event("done", "An idle foreground shell on active TTY 1 is required", ok=False)
        return

    results = Path.home() / "Projects/seaside/test-results" / f"vm-{base.rsplit('/', 1)[-1][:12]}"
    results.mkdir(mode=0o700, parents=True)
    state = {"revision": revision, "installed": resize_installed, "guest_evidence": str(results),
             "ok": False}
    try:
        env = helpers["environment"]()
        targets = (NATIVE_CLIENT,)
        if resize_installed:
            for path in ("/usr/bin/seaside", "/usr/bin/seasidectl"):
                require(checked(["pacman", "-Qoq", path], env=env).strip() == b"seaside" and
                        f"(revision {revision})" in checked([path, "--version"], env=env).decode(),
                        f"{path} is not the installed Seaside package at {revision}")
        else:
            targets = ("seaside", "seasidectl", *targets)
        with (tempfile.TemporaryDirectory(prefix="seaside-resize-source-") as source,
              tempfile.TemporaryDirectory(prefix="seaside-resize-", dir=env["XDG_RUNTIME_DIR"]) as root):
            repo = helpers["prepare"](Path(source), results, state, env, targets=targets)
            with helpers["Session"](repo, Path(root), results, env, label="resize",
                                    config=CONFIG, installed=resize_installed) as session:
                run_resize(session, state)
                session.close()
        state["ok"] = True
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError,
            KeyboardInterrupt) as error:
        state["error"] = str(error) or type(error).__name__
        event("diagnostic", state["error"])
    finally:
        for name in ("prepare.log", *resize_artifacts):
            upload(name, results / name)
        standard["send"]("guest.json", json.dumps(state, indent=2).encode() + b"\n")
        event("done", state.get("error", "Window opening and resize VM trial passed"), ok=state["ok"])


if __name__ == "__main__":
    trial()
