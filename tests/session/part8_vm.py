#!/usr/bin/env python3
"""Run the Part 8 display and capture checks on the Arch VirtualBox TTY."""

import argparse
from pathlib import Path

from part5_vm import revision_bundle, run


ARTIFACTS = {
    "baseline.json", "part8-session.log", "part8-pattern.log", "part8-portal-monitor.log",
    "part8-portal-window.log", "part8-night-light-5000K.png",
    "part8-preserve.png", "part8-stretch.png", "part8-rotated-90.png",
    "part8-rotated-180.png", "part8-rotated-270.png", "part8-rotated-0.png",
}


def send_input(runner, vm, message):
    key = message.get("key")
    if key == "night-light":
        runner.keys(vm, "e0", "5b", "31", "b1", "e0", "db")
    elif key == "monitor":
        runner.type_command(vm, "Monitor")
    elif key == "window":
        runner.type_command(vm, "Part 8 Fullscreen Pattern")
    else:
        raise RuntimeError(f"Unexpected Part 8 input request {key!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console-ready", required=True, action="store_true",
                        help="confirm active TTY 1 shows an idle guest shell")
    parser.add_argument("--vm", default="Arch Seaside Testing")
    parser.add_argument("--state", type=Path, default=Path.home() / ".local/state/seaside-vm")
    parser.add_argument("--tty-only", action="store_true")
    parser.add_argument("--pkg-config-path", type=Path,
                        help="additional pkg-config directory on the guest")
    parser.add_argument("--runtime-prefix", type=Path,
                        help="guest-local signed packages with bin/ and lib/ directories")
    parser.add_argument("--renderer", choices=("auto", "pixman"), default="auto",
                        help="use automatic rendering or explicitly exercise guest software composition")
    args = parser.parse_args()
    for path in (args.pkg_config_path, args.runtime_prefix):
        if path is not None and not path.is_absolute():
            parser.error("package and runtime paths must be absolute guest paths")
    settings = (f"part8_pkg_config_path = {str(args.pkg_config_path or '')!r}\n"
                f"part8_runtime_prefix = {str(args.runtime_prefix or '')!r}\n"
                f"part8_renderer = {args.renderer!r}\n"
                f"part8_artifacts = {tuple(sorted(ARTIFACTS))!r}\n")
    with revision_bundle() as (revision, bundle):
        return run(args, revision, bundle, scenario="part8", captures=ARTIFACTS,
                   send_input=send_input, settings=settings)


if __name__ == "__main__":
    raise SystemExit(main())
