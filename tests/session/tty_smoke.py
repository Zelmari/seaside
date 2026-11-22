"""Exercise the production terminal, VT round trip and missing PipeWire on one live seat."""

import json
import os
from pathlib import Path
import shlex
import subprocess


def tty_trial(standard, repo, temporary, results, state):
    env = dict(os.environ, XDG_CONFIG_HOME=str(temporary / "config"),
               XDG_STATE_HOME=str(temporary / "state"), WLR_RENDERER="pixman",
               PIPEWIRE_REMOTE="seaside-missing")
    for key in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SEASIDE_SOCKET"):
        env.pop(key, None)
    config = temporary / "config/seaside/config.toml"
    config.parent.mkdir(parents=True)
    socket = temporary / "session-socket"
    marker = shlex.quote(str(socket))
    command = f'printf "%s\\n" "$SEASIDE_SOCKET" > {marker}.tmp && mv {marker}.tmp {marker}'
    config.write_text(f"[autostart]\ncommands = [{json.dumps(command)}]\n")
    log = results / "normal.log"
    process = standard["start_seaside"](repo, env, log)
    wait = standard["wait_for"]

    def ctl(command):
        reply = subprocess.check_output([repo / "build/seasidectl", command], env=env, timeout=4)
        return json.loads(reply)["result"]

    def capture(label):
        standard["event"](label, "type in the running terminal", marker_dir=str(temporary))
        path = temporary / f"{label}.pid"
        wait(lambda: path.is_file() and path.stat().st_size, 14, label, process)
        state[label] = standard["read_capture"](temporary, label)
        standard["upload"](f"{label}.png", temporary / f"{label}.png")

    try:
        wait(socket.is_file, 15, "session autostart", process)
        env["SEASIDE_SOCKET"] = socket.read_text().strip()
        wait(lambda: ctl("status")["integrations"]["pipewire"]["state"] == "unavailable",
             15, "missing PipeWire diagnostic", process)
        status = ctl("status")
        if not status["ready"]:
            raise RuntimeError("Missing PipeWire prevented desktop readiness")
        state["degraded_startup"] = status["integrations"]["pipewire"]
        standard["event"]("open-foot", "open a terminal while PipeWire is unavailable")
        wait(lambda: any(w["mapped"] and "foot" in w["app_id"] for w in ctl("windows")["windows"]),
             12, "mapped terminal", process)
        capture("before")
        standard["event"]("to-tty2")
        wait(lambda: standard["active"]() == "tty2", 12, "TTY 2", process)
        standard["event"]("to-tty1")
        wait(lambda: standard["active"]() == "tty1" and
             log.read_text(errors="replace").count("presented with cursor plane") >= 2,
             12, "TTY 1 cursor presentation", process)
        capture("after")
        if state["before"]["shell_pid"] != state["after"]["shell_pid"]:
            raise RuntimeError("Terminal shell changed across VT return")
        state["vt_round_trip"] = True
        ctl("exit")
        if process.wait(timeout=10):
            raise RuntimeError(f"Session exit returned {process.returncode}")
        state["normal_exit"] = process.returncode
    finally:
        standard["stop"](process)

    # A launch without a foreground console must still fail before acquiring the seat.
    guard = subprocess.run([repo / "build/seaside"], stdin=subprocess.DEVNULL, capture_output=True,
                           env=env, timeout=5)
    (results / "failure.log").write_bytes(guard.stderr)
    if guard.returncode != 1 or b"active Linux console" not in guard.stderr:
        raise RuntimeError(f"Unexpected TTY guard result {guard.returncode}: {guard.stderr!r}")
    if os.tcgetpgrp(0) != os.getpgrp() or standard["active"]() != "tty1":
        raise RuntimeError("Session exit failed to return the original foreground TTY")
