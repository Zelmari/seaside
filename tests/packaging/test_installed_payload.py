#!/usr/bin/env python3
"""Exercise the production install target from a private staging directory."""

import configparser
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile


def run(*argv, **kwargs):
    return subprocess.run(argv, text=True, capture_output=True, timeout=20, **kwargs)


def installed_payload(source, build, stage):
    options = json.loads((build / "meson-info/intro-buildoptions.json").read_text())
    paths = {row["name"]: Path(row["value"]) for row in options
             if row["name"] in {"prefix", "bindir", "libexecdir", "datadir", "sysconfdir"}}

    def destination(option, filename):
        return (paths["prefix"] / paths[option] / filename).relative_to("/")

    binaries = {name: destination("bindir", name) for name in ("seaside", "seasidectl")}
    bridge = destination("libexecdir", "seaside/seaside-xwayland-satellite")
    data = {
        destination("datadir", "doc/seaside/xwayland-bridge/data-control.patch"):
            "packaging/xwayland-bridge/data-control.patch",
        destination("datadir", "doc/seaside/config.example.toml"):
            "Specification/SEASIDE_CONFIG_EXAMPLE.toml",
        destination("datadir", "licenses/seaside/LICENSE"): "LICENSE",
        destination("datadir", "seaside/wallpapers/black.png"): "packaging/black.png",
        destination("datadir", "xdg-desktop-portal/seaside-portals.conf"):
            "packaging/seaside-portals.conf",
        destination("sysconfdir", "xdg/xdg-desktop-portal-wlr/Seaside"):
            "packaging/xdg-desktop-portal-wlr/Seaside",
    }
    for name in ("LICENSE", "OFL.txt", "NOTICE.md", "THIRD-PARTY.txt"):
        data[destination("datadir", f"licenses/seaside/xwayland-satellite/{name}")] = (
            f"packaging/xwayland-bridge/{name}")
    # Even with DESTDIR, Meson replaces the build's install-log.txt. A real
    # system installation may own that log. Give the exact plan and its built
    # inputs a private working directory so this trial cannot overwrite it.
    install_build = stage.parent / "install-build"
    (install_build / "meson-private").mkdir(parents=True)
    (install_build / "meson-logs").mkdir()
    shutil.copyfile(build / "meson-private/install.dat", install_build / "meson-private/install.dat")
    for filename in json.loads((build / "meson-info/intro-installed.json").read_text()):
        path = Path(filename)
        if path.is_relative_to(build):
            target = install_build / path.relative_to(build)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target, follow_symlinks=False)
    result = run("meson", "install", "--no-rebuild", "-C", install_build, "--destdir", stage)
    assert result.returncode == 0, result.stderr or result.stdout
    actual = {path.relative_to(stage) for path in stage.rglob("*")
              if path.is_file() or path.is_symlink()}
    expected = set(data) | set(binaries.values()) | {bridge}
    assert actual == expected, f"Missing {expected - actual}; unexpected {actual - expected}"
    private = stage / bridge
    assert not private.is_symlink() and os.access(private, os.X_OK), bridge
    assert private.read_bytes() == (build / private.name).read_bytes(), bridge
    build_version = run(build / private.name, "-version", check=True).stdout.strip()
    assert "+seaside." in build_version, build_version
    assert run(private, "-version", check=True).stdout.strip() == build_version
    for target, original in data.items():
        installed = stage / target
        assert not installed.is_symlink(), target
        assert stat.S_IMODE(installed.stat().st_mode) & 0o111 == 0, target
        assert installed.read_bytes() == (source / original).read_bytes(), target

    route = configparser.ConfigParser()
    route.read(stage / destination("datadir", "xdg-desktop-portal/seaside-portals.conf"))
    assert route["preferred"]["default"] == "gtk"
    assert route["preferred"]["org.freedesktop.impl.portal.Screenshot"] == "wlr"
    assert route["preferred"]["org.freedesktop.impl.portal.ScreenCast"] == "wlr"

    chooser = configparser.ConfigParser()
    chooser.read(stage / destination("sysconfdir", "xdg/xdg-desktop-portal-wlr/Seaside"))
    assert chooser["screencast"]["chooser_type"] == "dmenu"
    command = chooser["screencast"]["chooser_cmd"].split()
    assert command[0] == "/usr/bin/wofi"
    assert "--dmenu" in command and "--no-custom-entry" in command
    assert "--normal-window" not in command
    return {name: stage / path for name, path in binaries.items()}


def check_entrypoints(binaries, home, source, build):
    for binary in binaries.values():
        assert not binary.is_symlink(), binary
        assert binary.open("rb").read(4) == b"\x7fELF", binary
        assert os.access(binary, os.X_OK), binary
    environment = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(home / "config"),
                       XDG_STATE_HOME=str(home / "state"))
    options = json.loads((build / "meson-info/intro-buildoptions.json").read_text())
    package_version = next(row["value"] for row in options if row["name"] == "package_version")
    project = json.loads((build / "meson-info/intro-projectinfo.json").read_text())
    version = package_version or project["version"]
    revision = (source / "packaging/source-revision").read_text().strip()
    if (source / ".git").exists():
        revision = run("git", "rev-parse", "--verify", "HEAD", cwd=source).stdout.strip()
    if not revision or revision.startswith("$Format:"):
        revision = "unknown"
    for name, binary in binaries.items():
        help_result = run(binary, "--help", stdin=subprocess.DEVNULL, env=environment)
        assert help_result.returncode == 0 and f"Usage: {name}" in help_result.stdout, help_result
        assert not help_result.stderr, help_result
        version_result = run(binary, "--version", stdin=subprocess.DEVNULL, env=environment)
        assert version_result.returncode == 0 and not version_result.stderr, version_result
        assert version_result.stdout.strip() == f"{name} {version} (revision {revision})", version_result
    entry = run(binaries["seaside"], stdin=subprocess.DEVNULL, env=environment)
    assert entry.returncode == 1 and "active Linux console" in entry.stderr, entry
    assert not list(home.iterdir()), "An off-console launch wrote persistent state"


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="seaside-install-") as directory:
        trial = Path(directory)
        home = trial / "home"
        home.mkdir()
        source, build = (Path(arg).resolve() for arg in sys.argv[1:])
        binaries = installed_payload(source, build, trial / "stage")
        check_entrypoints(binaries, home, source, build)
