"""Exercise Part 9 using the production entrypoint and temporary guest services."""

from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import tempfile


require = helpers["require"]
checked = helpers["checked"]
fixture = helpers["fixture"]
wait_for = standard["wait_for"]
event = standard["event"]
upload = standard["upload"]
stop = standard["stop"]
TITLE = "Part 9 Survival Pattern"
PATTERN_PIXEL = (100, 72, 44)
PANEL_PIXEL = (42, 128, 96)


class PrivatePipeWire:
    def __init__(self, root, env, log):
        self.root = root
        root.mkdir(mode=0o700)
        self.socket = root / "part9-pw"
        self.config = root / "server.conf"
        self.config.write_text(
            "context.properties = { core.daemon = true core.name = part9-pw "
            "support.dbus = false mem.allow-mlock = false }\n"
            "context.spa-libs = { support.* = support/libspa-support }\n"
            "context.modules = [ { name = libpipewire-module-protocol-native } "
            "{ name = libpipewire-module-access args = { access.socket = { "
            "part9-pw = unrestricted } } } ]\n"
        )
        self.env = {key: value for key, value in env.items() if not key.startswith(("PIPEWIRE_", "SPA_"))}
        self.env.update(PIPEWIRE_RUNTIME_DIR=str(root), XDG_RUNTIME_DIR=str(root),
                        PIPEWIRE_CONFIG_DIR=str(root))
        self.log = log
        self.process = None

    def start(self):
        with self.log.open("ab") as output:
            self.process = subprocess.Popen(["/usr/bin/pipewire", "-c", self.config], env=self.env,
                                            stdin=subprocess.DEVNULL, stdout=output,
                                            stderr=subprocess.STDOUT)
        try:
            wait_for(self.socket.is_socket, 8, "the private PipeWire socket", self.process)
        except BaseException:
            self.stop()
            raise

    def stop(self):
        if self.process is not None:
            stop(self.process)
            self.process = None


@contextmanager
def private_pipewire(root, env, log):
    server = PrivatePipeWire(root, env, log)
    server.start()
    try:
        yield server
    finally:
        server.stop()


@contextmanager
def subscription(session):
    log = session.results / "part9-events.log"
    with log.open("wb") as output:
        proc = subprocess.Popen([session.repo / "build/seasidectl", "events"], env=session.env,
                                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.DEVNULL)
        try:
            def records():
                return [json.loads(line) for line in log.read_bytes().splitlines(keepends=True)
                        if line.endswith(b"\n")]

            wait_for(lambda: records() and records()[0]["result"]["subscribed"] is True,
                     5, "an IPC subscription acknowledgement", proc)
            yield records
        finally:
            stop(proc)


def expect_integration(records, state, service, limit=16):
    name = f"integration.{state}"

    def observed():
        return next((row for row in records() if row.get("event") == name and
                     row["data"]["name"] == service), None)

    return wait_for(observed, limit, f"{name} for {service}")


def stop_owned_backend(session):
    service = "org.freedesktop.impl.portal.desktop.wlr"
    pid_output = checked(["busctl", "--user", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus", "GetConnectionUnixProcessID", "s", service],
                         env=session.env)
    match = re.fullmatch(rb"u ([1-9][0-9]*)\s*", pid_output)
    require(match is not None, "The portal backend has no known process owner")
    pid = int(match[1])
    proc = Path(f"/proc/{pid}")
    descriptor = os.pidfd_open(pid)
    try:
        require("/seaside-portal-" in (proc / "cgroup").read_text() and
                f"WAYLAND_DISPLAY={session.env['WAYLAND_DISPLAY']}".encode() in
                (proc / "environ").read_bytes().split(b"\0"),
                "Refusing to stop a portal backend outside the trial's units")
        signal.pidfd_send_signal(descriptor, signal.SIGTERM)
    finally:
        os.close(descriptor)
    return pid


def run_layer_shell(session, state, output):
    point = (output["position"]["x"] + output["mode"]["width"] // 2,
             output["position"]["y"] + 8)
    with fixture(session, session.results / "part9-panel.log", "seaside-part9-panel") as panel:
        wait_for(lambda: session.pixel(*point) == PANEL_PIXEL, 8, "the top layer's pixels", panel)
        session.grab(output, "panel-visible")
        require(session.ctl("window", "toggle-fullscreen")["changed"], "Fullscreen did not start")
        session.wait(lambda: session.pixel(*point) == PATTERN_PIXEL, "fullscreen panel suppression")
        session.grab(output, "panel-hidden")
        require(session.ctl("window", "toggle-fullscreen")["changed"], "Fullscreen did not stop")
        session.wait(lambda: session.pixel(*point) == PANEL_PIXEL, "restored panel pixels")
        session.grab(output, "panel-restored")
    state["layer_shell"] = {"status": "pass", "panel": PANEL_PIXEL, "fullscreen": PATTERN_PIXEL}


def run_trace(session, output, pattern, state):
    require(session.ctl("trace", "start")["trace"]["state"] == "running", "Trace did not start")
    pattern.stdin.write("3968b0\n")
    pattern.stdin.flush()
    session.wait(lambda: session.ctl("debug", "render")["trace"]["entries"] >= 2,
                 "committed and presented trace samples")
    stopped = session.ctl("trace", "stop")["trace"]
    require(stopped["state"] == "stopped" and stopped["stop_reason"] == "requested", "Trace did not stop")
    path = Path(session.ctl("trace", "export")["path"])
    require(path.parent == session.state_dir / "seaside/traces" and path.is_file(),
            f"The exported trace is outside its private directory: {path}")
    require(stat.S_IMODE(path.stat().st_mode) == 0o600 and
            stat.S_IMODE(path.parent.stat().st_mode) == 0o700, "Incorrect trace permissions")
    raw = path.read_bytes()
    rows = [json.loads(line) for line in raw.splitlines()]
    require(len(rows) == stopped["entries"] and len(raw) == stopped["bytes"] and
            {"commit", "present"} <= {row["event"] for row in rows} and
            all(row["output_id"] == output["id"] and row["observed_ns"].isdecimal() for row in rows),
            "Export did not contain the requested output observations")
    shutil.copyfile(path, session.results / "part9-trace.jsonl")
    state["trace"] = {"status": "pass", **stopped}
    pattern.stdin.write("64482c\n")
    pattern.stdin.flush()


def run_recovery(session, output, pattern, client, server, state):
    focus = session.ctl("focus")
    with subscription(session) as records:
        server.stop()
        pipewire_down = expect_integration(records, "failed", "pipewire")
        require(session.ctl("status")["ready"] and session.ctl("focus") == focus and
                session.window(TITLE)["id"] == client["id"],
                "The client or session was lost on private PipeWire disconnection")
        server.start()
        pipewire_up = expect_integration(records, "recovered", "pipewire")
        backend = stop_owned_backend(session)
        portal_down = expect_integration(records, "failed", "capture-backend")
        require(session.ctl("status")["ready"] and session.ctl("focus") == focus and
                session.window(TITLE)["id"] == client["id"],
                "The client or session was lost on portal backend disconnection")
        portal_up = expect_integration(records, "recovered", "capture-backend")
        require(pipewire_down["seq"] < pipewire_up["seq"] and portal_down["seq"] < portal_up["seq"],
                "An integration recovery event preceded its failure")
        state["recovery"] = {"status": "pass", "window": client["id"], "stopped_backend": backend,
                             "event_sequences": [row["seq"] for row in
                                                 (pipewire_down, pipewire_up, portal_down, portal_up)]}
    require(session.ctl("window", "toggle-fullscreen")["changed"], "Fullscreen did not start for sharing")
    mode = output["mode"]
    helpers["run_portal"](session, pattern, "monitor", (mode["width"], mode["height"]), state)
    require(session.ctl("focus")["window_id"] == client["id"], "Sharing did not return focus to its client")
    require(session.ctl("window", "toggle-fullscreen")["changed"], "Fullscreen did not end after sharing")


def run_idle(session, output, client, state):
    focus = session.ctl("focus")
    session.edit('[idle]\ndisplay_off_after_minutes = 1\n[render]\nfullscreen_aspect = "stretch"\n')
    snapshot = session.state_dir / "seaside/last-good-config.toml"
    wanted = session.config.read_text()
    session.wait(lambda: snapshot.read_text() == wanted, "the one-minute idle setting")
    event("part9-input", "reset the idle clock with a guest key", key="wake")
    event("part9", "waiting for the one-minute DRM idle deadline")
    off = f"output {output['connector']} display power off"
    on = f"output {output['connector']} display power on"
    text = lambda: session.log.read_text(errors="replace")
    wait_for(lambda: off in text() and session.diagnostics(output)["frame_state"] == "inactive",
             85, "the idle DRM output powered off", session.process)
    asleep = session.focused_output()
    require(asleep["id"] == output["id"] and asleep["mode"] == output["mode"] and
            asleep["hardware_cursor"] is None and session.ctl("focus") == focus and
            session.window(TITLE)["id"] == client["id"],
            "Idle power-off changed the output assignment or client identity")
    event("part9-input", "wake the sleeping output with a guest key", key="wake")
    plane = f"TTY DRM: output {output['connector']} presented with cursor plane "
    session.wait(lambda: plane in text().partition(on)[2] and
                 session.diagnostics(output)["frame_state"] != "inactive" and
                 session.focused_output()["hardware_cursor"], "DRM output and hardware cursor wake")
    session.grab(output, "awake")
    require(session.focused_output()["id"] == output["id"] and session.ctl("focus") == focus and
            session.window(TITLE)["id"] == client["id"],
            "Input wake changed the output assignment or client identity")
    state["idle_power"] = {"status": "pass", "timeout_minutes": 1, "sleep": off, "wake": on}


def trial():
    tty, active = os.ttyname(0), standard["active"]()
    existing = subprocess.run(["pgrep", "-x", "-t", "tty1", "seaside"], capture_output=True, timeout=3)
    event("connected", f"stdin={tty} active={active}")
    if tty != "/dev/tty1" or active != "tty1" or os.tcgetpgrp(0) != os.getpgrp() or existing.returncode != 1:
        event("done", "An idle foreground shell on active TTY 1 is required", ok=False)
        return
    results = Path.home() / "Projects/seaside/test-results" / f"vm-{standard['BASE'].rsplit('/', 1)[-1][:12]}"
    results.mkdir(mode=0o700, parents=True)
    state = {"revision": standard["REVISION"], "kernel": os.uname().release, "ok": False}
    try:
        env = helpers["environment"]()
        runtime = env.get("XDG_RUNTIME_DIR")
        require(runtime, "The guest did not provide XDG_RUNTIME_DIR")
        with tempfile.TemporaryDirectory(prefix="seaside-vm-source-") as source:
            repo = helpers["prepare"](Path(source), results, state, env, part=9,
                                      extra_targets=("seaside-part9-panel",))
            with tempfile.TemporaryDirectory(prefix="seaside-vm-", dir=runtime) as temporary:
                root = Path(temporary)
                original = os.environ.copy()
                try:
                    os.environ.clear()
                    os.environ.update(env)
                    standard["tty_trial"](repo, root, results, state)
                finally:
                    os.environ.clear()
                    os.environ.update(original)
                with private_pipewire(root / "pw", env, results / "part9-private-pipewire.log") as server:
                    session = helpers["Session"](repo, root / "part9", results,
                                                 dict(env, PIPEWIRE_REMOTE=str(server.socket)), label="part9")
                    session.edit('[idle]\ndisplay_off_after_minutes = 0\n[render]\nfullscreen_aspect = "stretch"\n')
                    with session:
                        # The compositor retains the private remote. Real portal clients
                        # and the external backend use the guest's existing audio stack.
                        del session.env["PIPEWIRE_REMOTE"]
                        output = session.wait(session.focused_output, "the focused guest output")
                        info = session.diagnostics(output)
                        state.update(renderer=info["renderer"], backend=info["backend"], initial_output=output)
                        require(output["hardware_cursor"], "No working virtual cursor plane")
                        require(not session.ctl("windows")["windows"], "The empty session has an application window")
                        event("part9", f"{info['backend']}/{info['renderer']} {output['connector']}")
                        with fixture(session, results / "part9-pattern.log", "seaside-part7-native-client",
                                     TITLE, "64482c", "--fixed-size", interactive=True) as pattern:
                            target = wait_for(lambda: session.window(TITLE), 8, "the test window", pattern)
                            run_layer_shell(session, state, output)
                            run_trace(session, output, pattern, state)
                            run_recovery(session, output, pattern, target, server, state)
                            run_idle(session, output, target, state)
                        session.close()
        state["suspend"] = {"status": "skip", "reason": "The guest runner has no verified targeted system wake path"}
        state["ok"] = True
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        state["error"] = str(error) or type(error).__name__
        event("diagnostic", state["error"])
    finally:
        for name in ("prepare.log", "harness.log", "normal.log", "failure.log", *part9_artifacts):
            upload(name, results / name)
        state["guest_evidence"] = str(results)
        standard["send"]("guest.json", json.dumps(state, indent=2).encode() + b"\n")
        event("done", state.get("error", "Part 9 VM trial passed"), ok=state["ok"])


trial()
