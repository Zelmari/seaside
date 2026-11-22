#!/usr/bin/env python3
"""Exercise window opening and interactive resize on the Arch VirtualBox TTY."""

import argparse
from pathlib import Path
from queue import Empty
import subprocess
import time

from part5_vm import mouse, revision_bundle, run
from part9_vm import guest_source
from part10_vm import helper


ARTIFACTS = {
    "resize-session.log", "resize-left-client.log", "resize-right-client.log",
    "resize-opening.png", "resize-open-settled.png", "resize-drag-right.png",
    "resize-drag-left.png", "resize-final.png",
}


def send_input(runner, vm, message):
    mouse(runner, vm, message["mouse"], hold_ms=1000, steps=16)


def follow(runner, vm, bridge, directory):
    deadline = time.monotonic() + 640
    while (remaining := deadline - time.monotonic()) > 0:
        try:
            event = bridge.events.get(timeout=min(remaining, 5))
        except Empty:
            continue
        runner.save_event(directory, event)
        if event["phase"] == "diagnostic":
            runner.screenshot(vm, directory / "console-diagnostic.png")
        if event["phase"] == "done":
            return event.get("ok") is True
    raise RuntimeError("The resize trial exceeded its deadline")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console-ready", required=True, action="store_true",
                        help="confirm active TTY 1 shows an idle guest shell")
    parser.add_argument("--vm", default="Arch Seaside Testing")
    parser.add_argument("--state", type=Path, default=Path.home() / ".local/state/seaside-vm")
    parser.add_argument("--installed", action="store_true",
                        help="test the package-owned /usr/bin/seaside at the current revision")
    parser.add_argument("--renderer", choices=("auto", "pixman"), default="pixman")
    parser.add_argument("--pkg-config-path", type=Path,
                        help="additional pkg-config directory for the guest build")
    args = parser.parse_args()
    if args.pkg_config_path is not None and not args.pkg_config_path.is_absolute():
        parser.error("the guest pkg-config path must be absolute")
    args.tty_only = True
    try:
        with revision_bundle() as (revision, bundle):
            settings = (
                f"resize_artifacts = {tuple(sorted(ARTIFACTS))!r}\n"
                f"resize_installed = {args.installed!r}\n"
                + helper(revision, "part8_vm_guest.py", "helpers",
                         "standard=standard, part8_runtime_prefix='', "
                         f"part8_pkg_config_path={str(args.pkg_config_path or '')!r}, "
                         f"part8_renderer={args.renderer!r}")
            )
            return run(args, revision, bundle, scenario="resize", captures=ARTIFACTS,
                       send_input=send_input, settings=settings, follow=follow,
                       scenario_source=guest_source(revision, "resize_vm_guest.py"))
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Resize VM trial failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
