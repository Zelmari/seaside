#!/usr/bin/env python3
"""Exercise the real command's foreground-console guard before config I/O."""

import os
from pathlib import Path
import pty
import subprocess
import sys
import tempfile


def check(executable, stdin, args, diagnostic, environment):
    result = subprocess.run([executable, *args], stdin=stdin, text=True, capture_output=True,
                            check=False, timeout=3, env=environment)
    if result.returncode != 1 or diagnostic not in result.stderr:
        raise AssertionError(f"Expected exit 1 and {diagnostic!r}, got {result!r}")


if __name__ == "__main__":
    seaside = sys.argv[1]
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        environment = {**os.environ, "HOME": directory, "XDG_CONFIG_HOME": str(root / "config"),
                       "XDG_STATE_HOME": str(root / "state")}
        check(seaside, subprocess.DEVNULL, ["--skip-preflight"], "Usage:", environment)
        check(seaside, subprocess.DEVNULL, [], "active Linux console", environment)
        master, slave = pty.openpty()
        try:
            check(seaside, slave, [], "active Linux console", environment)
        finally:
            os.close(slave)
            os.close(master)
        if list(root.iterdir()):
            raise AssertionError("console rejection wrote config or recovery state")
