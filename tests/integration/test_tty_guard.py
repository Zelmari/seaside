#!/usr/bin/env python3
"""The opt-in DRM fixture must refuse pipes and pseudoterminals before taking a seat."""

import os
import pty
import subprocess
import sys


def expect(executable, stdin, args, code, diagnostic):
    result = subprocess.run([executable, *args], stdin=stdin, text=True,
                            capture_output=True, check=False, timeout=3)
    if result.returncode != code or diagnostic not in result.stderr:
        raise AssertionError(f"Expected {code} and {diagnostic!r}, got {result!r}")


if __name__ == "__main__":
    fixture = sys.argv[1]
    expect(fixture, subprocess.DEVNULL, ["/dev/dri/renderD128"], 2, "Usage:")
    expect(fixture, subprocess.DEVNULL, [], 77, "active Linux console")
    expect(fixture, subprocess.DEVNULL, ["--terminal"], 77, "active Linux console")
    master, slave = pty.openpty()
    try:
        expect(fixture, slave, [], 77, "active Linux console")
    finally:
        os.close(slave)
        os.close(master)
