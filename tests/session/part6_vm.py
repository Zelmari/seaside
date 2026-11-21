#!/usr/bin/env python3
"""Exercise live config and IPC on Arch using the Council VM runner."""

import argparse
from pathlib import Path

from part5_vm import revision_bundle, run


CAPTURES = {"part6-live.log", "part6-restart.log", "part6-appearance.log", "part6-preflight.log", "part6-clean.png", "part6-error.png",
            "part6-recovered.png", "part6-startup-error.png"}
KEYS = {"open": ("1c", "9c"), "remapped": ("3c", "bc")}


def send_input(runner, vm, message):
    action = message.get("key")
    if action not in KEYS:
        raise RuntimeError(f"Unknown Part 6 key request {action!r}")
    runner.keys(vm, "e0", "5b", *KEYS[action], "e0", "db")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console-ready", required=True, action="store_true",
                        help="confirm active TTY 1 shows an idle guest shell")
    parser.add_argument("--vm", default="Arch Seaside Testing")
    parser.add_argument("--state", type=Path, default=Path.home() / ".local/state/seaside-vm")
    parser.add_argument("--tty-only", action="store_true")
    parser.add_argument("--pkg-config-path", type=Path,
                        help="optional additional pkg-config directory on the guest")
    args = parser.parse_args()
    if args.pkg_config_path is not None and not args.pkg_config_path.is_absolute():
        parser.error("--pkg-config-path must be an absolute guest path")
    settings = f"part6_pkg_config_path = {str(args.pkg_config_path or '')!r}\n"
    with revision_bundle() as (revision, bundle):
        return run(args, revision, bundle, scenario="part6", captures=CAPTURES,
                   send_input=send_input, settings=settings)


if __name__ == "__main__":
    raise SystemExit(main())
