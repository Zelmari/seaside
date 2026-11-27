#!/usr/bin/env python3
"""Check CLI wire mapping, discovery, JSON output, and errors over Unix sockets."""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile


def exchange(binary, listener, env, args, response, event=None):
    command = [binary, *args]
    with subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as cli:
        try:
            with listener.accept()[0] as peer:
                peer.settimeout(2)
                with peer.makefile("rb") as requests:
                    request = json.loads(requests.readline(65536))
                payloads = [response]
                if event is not None:
                    payloads.append(event)
                peer.sendall(b"".join(json.dumps(payload).encode() + b"\n" for payload in payloads))
            stdout, stderr = cli.communicate(timeout=5)
            assert [json.loads(line) for line in stdout.splitlines()] == payloads, (command, stdout)
            return request, cli.returncode, stderr
        finally:
            if cli.poll() is None:
                cli.kill()


def main():
    binary = sys.argv[1]
    commands = [
        (["status"], "query.status", {}),
        (["bindings"], "query.bindings", {}),
        (["outputs"], "query.outputs", {}),
        (["workspaces"], "query.workspaces", {}),
        (["windows"], "query.windows", {}),
        (["focus"], "query.focus", {}),
        (["focus", "left"], "action.focus", {"direction": "left"}),
        (["workspace", "3"], "action.workspace_select", {"workspace_id": 3}),
        (["scratchpad", "toggle"], "action.scratchpad_toggle", {}),
        (["window", "toggle-tiling"], "action.window_toggle_tiling", {}),
        (["window", "toggle-pin"], "action.window_toggle_pin", {}),
        (["window", "toggle-maximize"], "action.window_toggle_maximize", {}),
        (["window", "toggle-fullscreen"], "action.window_toggle_fullscreen", {}),
        (["window", "close"], "action.window_close", {}),
        (["window", "move", "left"], "action.window_move", {"direction": "left"}),
        (["window", "swap", "left"], "action.window_swap", {"direction": "left"}),
        (["window", "resize", "right", "32"], "action.window_resize",
         {"direction": "right", "delta": 32}),
        (["window", "resize", "right"], "action.window_resize", {"direction": "right"}),
        (["window", "to-workspace", "3"], "action.window_to_workspace", {"workspace_id": 3}),
        (["window", "to-scratchpad"], "action.window_to_scratchpad", {}),
        (["night-light", "status"], "query.night_light", {}),
        (["night-light", "on"], "action.night_light_set", {"enabled": True}),
        (["night-light", "off"], "action.night_light_set", {"enabled": False}),
        (["night-light", "toggle"], "action.settings_update",
         {"setting": "night_light.enabled", "value": "toggle"}),
        (["settings"], "query.settings", {}),
        (["settings", "set", "night_light.temperature_kelvin", "5100"], "action.settings_update",
         {"setting": "night_light.temperature_kelvin", "value": 5100}),
        (["settings", "set", "power.profile", "system"], "action.settings_update",
         {"setting": "power.profile", "value": None}),
        (["settings", "set", "window.allow_edge_drag", "true"], "action.settings_update",
         {"setting": "window.allow_edge_drag", "value": True}),
        (["settings", "set", "idle.display_off_after_minutes", "15"], "action.settings_update",
         {"setting": "idle.display_off_after_minutes", "value": 15}),
        (["keybind", "set", "Super+N", "night_light toggle"], "action.settings_update",
         {"setting": "keybinds", "chord": "Super+N", "value": "night_light toggle"}),
        (["keybind", "unset", "Super+N"], "action.settings_update",
         {"setting": "keybinds", "chord": "Super+N", "value": None}),
        (["keybind", "reset"], "action.settings_update",
         {"setting": "keybinds", "value": None}),
        (["debug", "render"], "query.diagnostics", {}),
        (["trace", "start"], "trace.start", {}),
        (["trace", "stop"], "trace.stop", {}),
        (["trace", "export"], "trace.export", {}),
        (["exit"], "action.exit", {}),
    ]
    with tempfile.TemporaryDirectory(prefix="ipc-") as runtime:
        directory = Path(runtime, "seaside")
        directory.mkdir(mode=0o700)
        path = str(directory / "wayland-cli.sock")
        env = dict(os.environ, SEASIDE_SOCKET=path, XDG_RUNTIME_DIR=runtime, WAYLAND_DISPLAY="unused")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(path)
            listener.listen()
            listener.settimeout(5)
            success = {"v": 1, "id": 1, "seq": 4, "ok": True, "result": {}}
            for args, method, params in commands:
                request, status, stderr = exchange(binary, listener, env, args, success)
                assert request == {"v": 1, "id": 1, "method": method, "params": params}, request
                assert status == 0 and not stderr, (args, status, stderr)

            del env["SEASIDE_SOCKET"]
            env["WAYLAND_DISPLAY"] = "wayland-cli"
            failure = {"v": 1, "id": 1, "seq": 4, "ok": False,
                       "error": {"code": "no_target", "message": "No focused window"}}
            request, status, stderr = exchange(binary, listener, env, ["window", "close"], failure)
            assert request["method"] == "action.window_close"
            assert status != 0 and b"no_target" in stderr, (status, stderr)

            env["WAYLAND_DISPLAY"] = runtime + "/wayland-cli"
            ack = dict(success, result={"subscribed": True})
            event = {"v": 1, "event": "window.destroyed", "seq": 5, "data": {"window_id": 3}}
            request, status, stderr = exchange(binary, listener, env, ["events"], ack, event)
            assert request["method"] == "events.subscribe"
            assert status != 0 and b"closed the connection" in stderr, (status, stderr)

        invalid = subprocess.run([binary, "workspace", "0"], env=env, capture_output=True, timeout=5)
        assert invalid.returncode != 0 and not invalid.stdout and b"workspace_id" in invalid.stderr
        missing = subprocess.run([binary, "status"], env=env, capture_output=True, timeout=5)
        assert missing.returncode != 0 and not missing.stdout and missing.stderr


if __name__ == "__main__":
    main()
