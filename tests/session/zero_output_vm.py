#!/usr/bin/env python3
"""Verify a cold Seaside launch with no usable output in a disposable Arch VM."""

import argparse
from pathlib import Path
from queue import Empty
import re
import subprocess
import time

from part5_vm import revision_bundle, run
from part10_vm import helper


ARTIFACTS = {"prepare.log", "zero-output-session.log", "zero-output-client.log", "part9-events.log"}


def follow(runner, vm, bridge, directory):
    deadline = time.monotonic() + 640
    while (remaining := deadline - time.monotonic()) > 0:
        try:
            event = bridge.events.get(timeout=min(remaining, 5))
        except Empty:
            continue
        runner.save_event(directory, event)
        if event["phase"] == "done":
            return event.get("ok") is True
    raise RuntimeError("The zero-output trial exceeded its deadline")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm", required=True, help="the disposable Arch test guest")
    parser.add_argument("--disposable", required=True, action="store_true",
                        help="confirm the selected guest is safe to force-disconnect")
    parser.add_argument("--console-ready", required=True, action="store_true",
                        help="confirm the guest shows an idle foreground shell on TTY 1")
    parser.add_argument("--connector", required=True, help="guest DRM connector, e.g. card0-Virtual-1")
    parser.add_argument("--vmwgfx-layout-hotplug", action="store_true",
                        help="use the disposable guest's fixed vmwgfx layout helper on reconnect")
    parser.add_argument("--pkg-config-path", type=Path)
    parser.add_argument("--state", type=Path, default=Path.home() / ".local/state/seaside-vm")
    parser.add_argument("--renderer", choices=("auto", "pixman"), default="pixman")
    args = parser.parse_args()
    if re.fullmatch(r"card[0-9]+-Virtual-[0-9]+", args.connector) is None:
        parser.error("--connector must name a VirtualBox DRM connector")
    if args.vmwgfx_layout_hotplug and args.connector != "card0-Virtual-1":
        parser.error("--vmwgfx-layout-hotplug requires card0-Virtual-1")
    if args.pkg_config_path is not None and not args.pkg_config_path.is_absolute():
        parser.error("the guest pkg-config path must be absolute")
    args.tty_only = True
    try:
        with revision_bundle() as (revision, bundle):
            settings = (
                f"zero_output_connector = {args.connector!r}\n"
                f"zero_output_layout_hotplug = {args.vmwgfx_layout_hotplug!r}\n"
                f"zero_output_artifacts = {tuple(sorted(ARTIFACTS))!r}\n"
                + helper(revision, "part8_vm_guest.py", "helpers",
                         "standard=standard, part8_runtime_prefix='', "
                         f"part8_pkg_config_path={str(args.pkg_config_path or '')!r}, "
                         f"part8_renderer={args.renderer!r}")
                + helper(revision, "part9_vm_guest.py", "recovery", "standard=standard, helpers=helpers")
            )
            return run(args, revision, bundle, scenario="zero_output", captures=ARTIFACTS,
                       settings=settings, follow=follow)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Zero-output VM trial failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
