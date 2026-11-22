"""Exercise the production TTY startup path while the only virtual connector is off."""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


event = standard["event"]
wait_for = standard["wait_for"]
require = helpers["require"]
title = "Cold zero-output client"


class Blocked(RuntimeError):
    pass


def target_state(env):
    result = subprocess.run(["systemctl", "--user", "is-active", "graphical-session.target"],
                            env=env, capture_output=True, text=True, timeout=4, check=False)
    state = result.stdout.strip()
    require(state in {"active", "inactive"} and result.returncode in {0, 3},
            f"Could not read graphical-session.target: {result.stderr.strip() or state}")
    return state


def connector_command():
    require(re.fullmatch(r"card[0-9]+-Virtual-[0-9]+", zero_output_connector),
            "Refusing to change a non-VirtualBox connector")
    connector = Path("/sys/class/drm") / zero_output_connector / "status"
    connected = [path for path in connector.parent.parent.glob("card*-*/status")
                 if path.read_text().strip() == "connected"]
    if connected != [connector]:
        raise Blocked(f"The selected connector must be the only connected output: {connected}")
    command = ["/usr/bin/tee", "--", str(connector)]
    allowed = subprocess.run(["/usr/bin/sudo", "-n", "-l", "--", *command], capture_output=True,
                             text=True, timeout=4, check=False)
    if allowed.returncode:
        raise Blocked(f"The disposable guest needs noninteractive sudo for {shlex.join(command)}")
    return connector, ["/usr/bin/sudo", "-n", *command]


def write_status(command, value):
    status = subprocess.run(command, input=value + "\n", capture_output=True, text=True,
                            timeout=4, check=False)
    require(status.returncode == 0, f"Could not write connector status {value}: {status.stderr.strip()}")


def check_startup(repo, root, results, env, state):
    connector, command = connector_command()
    state["target_before"] = target_state(env)
    session = helpers["Session"](repo, root, results, env, label="zero-output", config="")
    # A source-built trial may run with the Arch portal packages installed
    # but without Seaside's system routing file. Use the candidate's routing
    # in the private config directory inherited by its own portal services.
    routing = Path(session.env["XDG_CONFIG_HOME"]) / "xdg-desktop-portal/seaside-portals.conf"
    routing.parent.mkdir()
    routing.write_bytes((repo / "packaging/seaside-portals.conf").read_bytes())
    starts = root / "autostart.txt"
    marker = shlex.quote(str(session.marker))
    autostart = (
        f"umask 077; printf 'started\\n' >> {shlex.quote(str(starts))} && "
        f"printf '%s\\n' \"$WAYLAND_DISPLAY\" \"$SEASIDE_SOCKET\" > {marker}.tmp && "
        f"mv -- {marker}.tmp {marker}"
    )
    session.base_config = f"[autostart]\ncommands = [{json.dumps(autostart)}]\n"
    session.edit()
    try:
        event("zero-output", "forcing the only virtual DRM connector off")
        write_status(command, "off")
        try:
            wait_for(lambda: connector.read_text().strip() == "disconnected", 5,
                     "the forced connector disconnection")
        except RuntimeError as error:
            raise Blocked(f"The virtual DRM driver did not expose the forced disconnect: {error}") from error
        with session:
            require("seaside READY outputs=none" in session.log.read_text(errors="replace") and
                    not any(row["enabled"] for row in session.ctl("outputs")["outputs"]) and
                    session.ctl("focus") == {"output_id": None, "window_id": None, "workspace_id": None},
                    "The cold session failed to report readiness without outputs")
            require(starts.read_text() == "started\n", "Autostart did not execute exactly once")
            wait_for(lambda: target_state(env) == "active", 5, "the passive graphical target")
            state["ready_without_outputs"] = True
            with helpers["fixture"](session, results / "zero-output-client.log",
                                    "seaside-part7-native-client", title, "316657") as client:
                original = wait_for(lambda: session.window(title), 8, "a cold-start Wayland window", client)
                with recovery["subscription"](session) as records:
                    pid = recovery["stop_owned_portal"](session, "org.freedesktop.portal.Desktop")
                    down = recovery["expect_integration"](records, "failed", "portal-frontend")
                    require(target_state(env) == "active" and session.ctl("status")["ready"],
                            "Restarting the owned frontend stopped the target or the session")
                    up = recovery["expect_integration"](records, "recovered", "portal-frontend")
                    require(up["seq"] > down["seq"], "Recovery preceded frontend failure")
                    state["frontend_restart"] = {"pid": pid, "failure": down["seq"], "recovery": up["seq"]}
                write_status(command, "detect")
                plane = f"output {zero_output_connector.split('-', 1)[1]} presented with cursor plane "
                output = session.wait(lambda: session.focused_output() if plane in
                                      session.log.read_text(errors="replace") else None,
                                      "a reconnected virtual hardware cursor plane")
                require(output["hardware_cursor"] is True and session.window(title)["id"] == original["id"] and
                        session.ctl("focus")["window_id"] == original["id"] and starts.read_text() == "started\n",
                        "Reconnection changed the client or ran autostart again")
                state.update(reconnected_output=output, surviving_window=original["id"], autostart_runs=1)
            session.close()
            require(wait_for(lambda: target_state(env) == state["target_before"], 8,
                             "the original graphical target state"), "The graphical target state changed")
    finally:
        write_status(command, "detect")
        wait_for(lambda: connector.read_text().strip() == "connected", 5, "connector restoration")
        state["connector_restored"] = True


def trial():
    tty, active = os.ttyname(0), standard["active"]()
    existing = subprocess.run(["pgrep", "-x", "-t", "tty1", "seaside"], capture_output=True, timeout=3)
    event("connected", f"stdin={tty} active={active}")
    if tty != "/dev/tty1" or active != "tty1" or os.tcgetpgrp(0) != os.getpgrp() or existing.returncode != 1:
        event("done", "An idle foreground shell on active TTY 1 is required", ok=False)
        return
    result_id = standard["BASE"].rsplit("/", 1)[-1][:12]
    results = Path.home() / "Projects/seaside/test-results" / f"vm-{result_id}"
    results.mkdir(mode=0o700, parents=True)
    state = {"revision": standard["REVISION"], "connector": zero_output_connector, "ok": False}
    try:
        env = helpers["environment"]()
        with tempfile.TemporaryDirectory(prefix="seaside-vm-source-") as source:
            repo = helpers["prepare"](Path(source), results, state, env,
                                      targets=("seaside", "seasidectl", "seaside-part7-native-client"))
            with tempfile.TemporaryDirectory(prefix="seaside-vm-", dir=env["XDG_RUNTIME_DIR"]) as root:
                check_startup(repo, Path(root), results, env, state)
        state.update(ok=True, status="pass")
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError,
            KeyboardInterrupt) as error:
        state.update(status="blocked" if isinstance(error, Blocked) else "fail",
                     error=str(error) or type(error).__name__)
        event("diagnostic", state["error"])
    finally:
        for name in zero_output_artifacts:
            standard["upload"](name, results / name)
        state["guest_evidence"] = str(results)
        standard["send"]("guest.json", json.dumps(state, indent=2).encode() + b"\n")
        event("done", state.get("error", "Zero-output VM trial passed"), ok=state["ok"])


if __name__ == "__main__":
    trial()
