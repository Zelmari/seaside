#!/usr/bin/env python3
"""Collect bounded, read-only display evidence inside a running Seaside session."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
LIMIT = 1024 * 1024


def capture(*command):
    try:
        result = subprocess.run(command, capture_output=True, timeout=8, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"command": list(map(str, command)), "error": str(error)}
    return {"command": list(map(str, command)), "returncode": result.returncode,
            "stdout": result.stdout[:LIMIT].decode(errors="replace"),
            "stderr": result.stderr[:LIMIT].decode(errors="replace")}


def read(path):
    try:
        with path.open() as stream:
            return stream.read(LIMIT).strip()
    except OSError as error:
        return {"error": str(error)}


def drm_state():
    drm = Path("/sys/class/drm")
    cards = [card for card in sorted(drm.glob("card*")) if card.name[4:].isdigit()]
    return {
        "card_devices": {card.name: read(card / "device/uevent") for card in cards},
        "connectors": {connector.name: {field: read(connector / field)
                                       for field in ("status", "enabled", "modes")}
                       for connector in sorted(drm.glob("card*-*")) if (connector / "status").is_file()},
        "atomic_state": {card.name: read(Path("/sys/kernel/debug/dri") / card.name[4:] / "state")
                         for card in cards},
        "gpu_busy_percent": {path.parent.parent.name: read(path)
                             for path in sorted(drm.glob("card*/device/gpu_busy_percent"))},
    }


def query(ctl, *args):
    result = capture(ctl, *args)
    if result.get("returncode") != 0:
        raise RuntimeError(result.get("error") or result.get("stderr") or "seasidectl failed")
    reply = json.loads(result["stdout"])
    if not reply["ok"]:
        raise RuntimeError(f"seasidectl did not acknowledge {args}")
    return reply["result"]


def sample(ctl):
    start = time.monotonic_ns()
    render = query(ctl, "debug", "render")
    end = time.monotonic_ns()
    return {"monotonic_ns": [start, end], "render": render}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", help="description of the current workload, such as idle or fullscreen recording")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--seconds", choices=range(1, 31), type=int, default=5, metavar="1–30")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    ctl = build / "seasidectl"
    now = datetime.now(timezone.utc)
    results = ROOT / "test-results" / f"native-{now.strftime('%Y%m%dT%H%M%S.%fZ')}"
    manifest = {"scenario": args.scenario, "date": now.isoformat(), "kernel": os.uname().release,
                "checkout": capture("git", "-C", ROOT, "rev-parse", "HEAD"),
                "packages": capture("pacman", "-Q", "mesa", "libdrm", "wlroots0.20", "wayland-protocols",
                                    "pipewire", "xdg-desktop-portal", "xdg-desktop-portal-wlr",
                                    "xwayland-satellite", "xorg-xwayland"),
                "drm_probe": capture(build / "seaside-probe-drm")}
    status = 0
    try:
        before = sample(ctl)
        manifest["before"] = {**before, "outputs": query(ctl, "outputs"), "drm": drm_state(),
                              "night_light": query(ctl, "night-light", "status")}
        time.sleep(args.seconds)
        after = sample(ctl)
        manifest["after"] = {**after, "outputs": query(ctl, "outputs"), "drm": drm_state(),
                             "night_light": query(ctl, "night-light", "status")}
    except (RuntimeError, ValueError, KeyError) as error:
        manifest["sampling_error"] = str(error)
        status = 1
    results.mkdir(mode=0o700, parents=True)
    report = results / "display.json"
    report.write_text(json.dumps(manifest, indent=2) + "\n")
    print(report)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
