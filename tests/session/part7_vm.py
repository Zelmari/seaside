#!/usr/bin/env python3
"""Run Part 7 on the Arch TTY with background VirtualBox controls."""

import argparse
from pathlib import Path

from part5_vm import revision_bundle, run


ARTIFACTS = {"part7-session.log", "part7-native.log", "part7-x11.log", "part7-monitor.log",
             "part7-window.log", "part7-cancel.log", "part7-x11-window.log", "part7-grim.png",
             "part7-portals.log", "part7-pipewire.log", "part7-memory.jsonl", "part7-recorder.log",
             "part7-monitor.mkv"}

KEYS = {
    "cycle": ("38", "0f", "8f", "b8"),
    "cancel": ("01", "81"),
    "down": ("e0", "2e", "e0", "ae"),
    "up": ("e0", "30", "e0", "b0"),
    "mute": ("e0", "20", "e0", "a0"),
}
CHOICES = {"Monitor", "Part7 Native Target", "Seaside Part 7 X11 capture"}


def send_input(runner, vm, message):
    if (choice := message.get("choice")) in CHOICES:
        runner.type_command(vm, choice)
    elif (key := message.get("key")) in KEYS:
        runner.keys(vm, *KEYS[key])
    else:
        raise RuntimeError(f"Unexpected Part 7 input request {message!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console-ready", required=True, action="store_true",
                        help="confirm active TTY 1 shows an idle guest shell")
    parser.add_argument("--vm", default="Arch Seaside Testing")
    parser.add_argument("--state", type=Path, default=Path.home() / ".local/state/seaside-vm")
    parser.add_argument("--tty-only", action="store_true")
    parser.add_argument("--require-background-clipboard", action="store_true",
                        help="require unfocused X11 transfers in both directions for both selections")
    parser.add_argument("--pkg-config-path", type=Path)
    parser.add_argument("--runtime-prefix", type=Path,
                        help="guest-local signed packages with bin/ and lib/ directories")
    parser.add_argument("--recorder-prefix", type=Path,
                        help="guest-local wf-recorder and FFmpeg runtime directory")
    args = parser.parse_args()
    for path in (args.pkg_config_path, args.runtime_prefix, args.recorder_prefix):
        if path is not None and not path.is_absolute():
            parser.error("package and runtime paths must be absolute guest paths")
    settings = (f"part7_require_background_clipboard = {args.require_background_clipboard!r}\n"
                f"part7_pkg_config_path = {str(args.pkg_config_path or '')!r}\n"
                f"part7_runtime_prefix = {str(args.runtime_prefix or '')!r}\n"
                f"part7_recorder_prefix = {str(args.recorder_prefix or '')!r}\n")
    with revision_bundle() as (revision, bundle):
        return run(args, revision, bundle, scenario="part7", captures=ARTIFACTS,
                   send_input=send_input, settings=settings)


if __name__ == "__main__":
    raise SystemExit(main())
