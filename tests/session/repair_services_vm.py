#!/usr/bin/env python3
"""Verify Seaside service startup, desktop activation and teardown on the dedicated VM."""

import argparse
from pathlib import Path

from part5_vm import revision_bundle, run
from part9_vm import guest_source
from resize_vm import follow


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console-ready", required=True, action="store_true",
                        help="confirm active TTY 1 shows an idle guest shell")
    parser.add_argument("--vm", default="Arch Seaside Testing")
    parser.add_argument("--state", type=Path, default=Path.home() / ".local/state/seaside-vm")
    parser.add_argument("--tty-only", action="store_true")
    parser.add_argument("--pkg-config-path", type=Path)
    parser.add_argument("--runtime-prefix", type=Path)
    parser.add_argument("--baseline", action="store_true", help="record behavior before repairs")
    parser.add_argument("--sanitize", action="store_true", help="instrument the guest compositor with ASan/UBSan")
    args = parser.parse_args()
    for path in (args.pkg_config_path, args.runtime_prefix):
        if path is not None and not path.is_absolute():
            parser.error("package and runtime paths must be absolute guest paths")
    with revision_bundle() as (revision, bundle):
        shared = guest_source(revision, "part8_vm_guest.py")
        settings = (
            f"repair_baseline = {args.baseline!r}\nrepair_sanitize = {args.sanitize!r}\n"
            "helpers = {'__name__': 'seaside_guest_helpers', 'standard': standard, "
            f"'part8_pkg_config_path': {str(args.pkg_config_path or '')!r}, "
            f"'part8_runtime_prefix': {str(args.runtime_prefix or '')!r}, 'part8_renderer': 'pixman'}}\n"
            f"exec(compile({shared!r}, 'part8_vm_guest.py', 'exec'), helpers)\n"
        )
        captures = {"repair-healthy-session.log", "repair-restart-session.log",
                    "repair-unavailable-session.log", "repair-shutdown-session.log",
                    "repair-foreign-session.log", "repair-bus-session.log", "repair-autostart-session.log", "sanitize-build.log"}
        return run(args, revision, bundle, scenario="repair_services", captures=captures, settings=settings, follow=follow)


if __name__ == "__main__":
    raise SystemExit(main())
