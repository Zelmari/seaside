#!/usr/bin/env python3
"""Run Part 9 on the dedicated Arch guest without changing host focus."""

import argparse
from pathlib import Path

from part5_vm import revision_bundle, run


ARTIFACTS = {
    "baseline.json", "part9-session.log", "part9-panel.log", "part9-pattern.log",
    "part9-portal-monitor.log", "part9-events.log", "part9-private-pipewire.log",
    "part9-trace.jsonl", "part9-panel-visible.png", "part9-panel-hidden.png",
    "part9-panel-restored.png", "part9-awake.png",
}


def send_input(runner, vm, message):
    key = message.get("key")
    if key == "monitor":
        runner.type_command(vm, "Monitor")
    elif key == "wake":
        runner.keys(vm, "2a", "aa")
    else:
        raise RuntimeError(f"Unexpected Part 9 input request {key!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console-ready", required=True, action="store_true",
                        help="confirm active TTY 1 shows an idle guest shell")
    parser.add_argument("--vm", default="Arch Seaside Testing")
    parser.add_argument("--state", type=Path, default=Path.home() / ".local/state/seaside-vm")
    parser.add_argument("--tty-only", action="store_true")
    parser.add_argument("--pkg-config-path", type=Path)
    parser.add_argument("--runtime-prefix", type=Path)
    parser.add_argument("--renderer", choices=("auto", "pixman"), default="pixman")
    args = parser.parse_args()
    for path in (args.pkg_config_path, args.runtime_prefix):
        if path is not None and not path.is_absolute():
            parser.error("package and runtime paths must be absolute guest paths")
    shared = Path(__file__).with_name("part8_vm_guest.py").read_text()
    settings = (
        f"part9_artifacts = {tuple(sorted(ARTIFACTS))!r}\n"
        "helpers = {'__name__': 'seaside_guest_helpers', 'standard': standard, "
        f"'part8_pkg_config_path': {str(args.pkg_config_path or '')!r}, "
        f"'part8_runtime_prefix': {str(args.runtime_prefix or '')!r}, "
        f"'part8_renderer': {args.renderer!r}}}\n"
        f"exec(compile({shared!r}, 'part8_vm_guest.py', 'exec'), helpers)\n"
    )
    with revision_bundle() as (revision, bundle):
        return run(args, revision, bundle, scenario="part9", captures=ARTIFACTS,
                   send_input=send_input, settings=settings)


if __name__ == "__main__":
    raise SystemExit(main())
