#!/usr/bin/env python3
"""Qualify the installed Arch package on an isolated VirtualBox guest."""

import argparse
import hashlib
from pathlib import Path
import re

from part5_vm import revision_bundle, run
from part7_vm import KEYS
from part9_vm import guest_source


PACKAGE_LIMIT = 64 * 1024 * 1024
ARTIFACTS = {
    "part10-package.log", "part10-package-info.txt", "part10-build-info.txt",
    "part10-package-files.txt", "part10-first-use.toml", "part10-session.log",
    "part10-first-use.png", "part10-portal-monitor.log", "part10-portal-window.log",
    "part10-portal-cancel.log", "part10-pattern.log", "part10-events.log",
    "part10-restarts.log", "part10-x11.log", "part10-x11-restart.log", "part10-stability-client.log",
}


def send_input(runner, vm, message):
    key = message.get("key")
    if key in {"monitor", "window"}:
        runner.type_command(vm, "Monitor" if key == "monitor" else "Part 10 Package Pattern")
    elif key in {"up", "down", "mute", "cancel"}:
        runner.keys(vm, *KEYS[key])
    else:
        raise RuntimeError(f"Unexpected Part 10 input request {key!r}")


def helper(revision, source, namespace, context):
    code = guest_source(revision, source)
    return (f"{namespace} = dict(__name__='seaside_guest_helpers', {context})\n"
            f"exec(compile({code!r}, {source!r}, 'exec'), {namespace})\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm", required=True, help="the isolated Arch package-test guest")
    parser.add_argument("--console-ready", required=True, action="store_true",
                        help="confirm the guest shows an idle foreground shell on TTY 1")
    parser.add_argument("--disposable", action="store_true",
                        help="confirm the selected guest is disposable and pacman writes are safe")
    parser.add_argument("--preinstalled", action="store_true",
                        help="verify the candidate against a package already installed on the guest")
    parser.add_argument("--package", type=Path, required=True, help="host path to the candidate archive")
    parser.add_argument("--state", type=Path, default=Path.home() / ".local/state/seaside-vm")
    parser.add_argument("--pkg-config-path", type=Path, help="guest path for test fixture builds only")
    parser.add_argument("--renderer", default="pixman", choices=("pixman", "auto"))
    args = parser.parse_args()
    if not (args.disposable or args.preinstalled):
        parser.error("installation requires --disposable; verification can use --preinstalled")
    if args.pkg_config_path is not None and not args.pkg_config_path.is_absolute():
        parser.error("the fixture pkg-config path must be an absolute guest path")
    if not re.fullmatch(r"[\w.+-]+\.pkg\.tar\.zst", args.package.name):
        parser.error("--package must name one Arch .pkg.tar.zst archive")
    args.tty_only = True
    try:
        with args.package.open("rb") as stream:
            archive = stream.read(PACKAGE_LIMIT + 1)
        if not 0 < len(archive) <= PACKAGE_LIMIT:
            parser.error("the package is empty or exceeds the transfer limit")
        package = dict(name=args.package.name, sha256=hashlib.sha256(archive).hexdigest(),
                       size=len(archive), preinstalled=args.preinstalled)
        with revision_bundle() as (revision, bundle):
            settings = (
                f"part10_package = {package!r}\n"
                f"part10_artifacts = {tuple(sorted(ARTIFACTS))!r}\n"
                f"part10_pkg_config_path = {str(args.pkg_config_path or '')!r}\n"
                f"part10_renderer = {args.renderer!r}\n"
                + helper(revision, "part8_vm_guest.py", "helpers",
                         "standard=standard, part8_runtime_prefix='', part8_pkg_config_path='', "
                         "part8_renderer=part10_renderer")
                + helper(revision, "part9_vm_guest.py", "recovery", "standard=standard, helpers=helpers")
                + helper(revision, "part7_vm_guest.py", "compatibility", "standard=standard")
            )
            return run(args, revision, bundle, scenario="part10", captures=ARTIFACTS,
                       send_input=send_input, settings=settings,
                       scenario_source=guest_source(revision, "part10_vm_guest.py"),
                       downloads={"package": archive})
    except (OSError, RuntimeError) as error:
        parser.exit(1, f"Part 10 VM test failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
