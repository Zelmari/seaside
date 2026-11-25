"""Live config and IPC checks executed beside the Council guest TTY trial."""

import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import struct
import subprocess
import tempfile
import time
import tomllib
import zlib


event = standard["event"]
wait_for = standard["wait_for"]
upload = standard["upload"]
git = standard["git"]
run_logged = standard["run_logged"]
active = standard["active"]
base = standard["BASE"]
revision = standard["REVISION"]
mode = standard["MODE"]
baseline_config = "[window]\nlayout = 'dwindle'\n"
bad_config = "[window]\ngaps_outer = 'invalid'\n"
repaired_config = """[workspaces]
mode = 'per-monitor'
[window]
layout = 'dwindle'
gaps_outer = 28
[night_light]
enabled = true
[keybinds]
"Super+F2" = "exec foot"
"""


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def prepare(source, result_dir, state):
    size_limit = 64 * 1024 * 1024
    bundle = source / "revision.bundle"
    with standard["OPENER"].open(base + "/bundle", timeout=30) as response:
        bundle.write_bytes(response.read(size_limit + 1))
    require(bundle.stat().st_size <= size_limit, "The delivered bundle exceeds its size limit")
    repo = source / "checkout"
    log = result_dir / "prepare.log"
    event("update", f"isolated checkout of {revision[:12]}")
    require(not run_logged(["git", "clone", "--quiet", str(bundle), str(repo)], source, log, 45),
            "Could not clone the supplied revision bundle")
    require(git(repo, "rev-parse", "HEAD") == revision and not git(repo, "status", "--porcelain"),
            "The isolated checkout does not match the requested Git revision")

    env = os.environ.copy()
    if part6_pkg_config_path:
        require(Path(part6_pkg_config_path).is_dir(), "The guest pkg-config directory is missing")
        env["PKG_CONFIG_PATH"] = os.pathsep.join(filter(None, (part6_pkg_config_path,
                                                              env.get("PKG_CONFIG_PATH"))))
    packages = ["nlohmann_json", "pangocairo", "libpng", "libturbojpeg", "libwebp"]
    versions = subprocess.check_output(["pkg-config", "--modversion", *packages], env=env,
                                       text=True, timeout=5).splitlines()
    state["dependencies"] = dict(zip(packages, versions, strict=True))
    if mode == "tty":
        state["software_checks"] = "not-run"
        event("build", revision[:12])
        require(not run_logged(["meson", "setup", "build", "--buildtype=debug"], repo, log,
                               120, env=env), "Could not configure the guest checkout")
        require(not run_logged(["meson", "compile", "-C", "build", "-j2", "seaside",
                                "seasidectl"], repo, log, 360, env=env),
                "Could not build the guest compositor and CLI")
    else:
        event("harness", revision[:12])
        reports = result_dir / "harness"
        code = run_logged(["tools/run-tests", "--results-dir", str(reports), "--suite", "unit",
                           "--suite", "integration", "--suite", "runner", "--suite", "hardware"],
                          repo, result_dir / "harness.log", 760, env=env)
        summaries = list(reports.glob("*/summary.json"))
        require(len(summaries) == 1, "The guest harness did not produce a unique summary")
        summary = summaries[0]
        upload("summary.json", summary)
        upload("summary.txt", summary.with_suffix(".txt"))
        report = json.loads(summary.read_text())
        state["software_checks"] = report["software_status"]
        tests = report["tests"]
        part6 = [row for row in tests if row["part"] == 6]
        require(not code and state["software_checks"] == "pass" and part6 and
                all(row["status"] == "pass" for row in part6) and
                any(row["test_id"].endswith("part3-real-portals") and row["status"] == "pass"
                    for row in tests), f"Guest regression, Part 6, or real portal check failed ({code})")
    return repo


def replace(path, text):
    staged = path.with_suffix(".editor")
    staged.write_text(text)
    staged.replace(path)


def with_autostart(config, marker):
    command = f'''printf '%s\\n' "$SEASIDE_SOCKET" >> {shlex.quote(str(marker))}'''
    return config + f"\n[autostart]\ncommands = [{json.dumps(command)}]\n"


class Session:
    def __init__(self, repo, directory, result_dir, name, **environment):
        self.repo = repo
        self.log = result_dir / f"part6-{name}.log"
        self.environment = dict(os.environ, XDG_CONFIG_HOME=str(directory / "config"),
                                XDG_STATE_HOME=str(directory / "state"), WLR_RENDERER="pixman")
        for key in ("PIPEWIRE_REMOTE", "WAYLAND_DISPLAY", "SEASIDE_SOCKET"):
            self.environment.pop(key, None)
        self.environment.update(environment)
        self.socket_dir = Path(self.environment["XDG_RUNTIME_DIR"]) / "seaside"
        self.existing_sockets = set(self.socket_dir.glob("wayland-*.sock"))
        self.process = standard["start_seaside"](repo, self.environment, self.log)

    def __enter__(self):
        try:
            self.wait_for_socket()
            if "PIPEWIRE_REMOTE" not in self.environment:
                wait_for(lambda: re.search(r"^seaside READY cursor-plane=([1-9][0-9]*)$",
                                           self.log.read_text(errors="replace"), re.MULTILINE),
                         40, "Part 6 startup and hardware cursor", self.process)
                ready = self.ctl("status")["result"]
                require(ready["ready"] and ready["phase"] == "ready",
                        "IPC readiness disagrees with TTY")
            return self
        except Exception:
            self.__exit__(None, None, None)
            raise

    def wait_for_socket(self):
        sockets = wait_for(
            lambda: set(self.socket_dir.glob("wayland-*.sock")) - self.existing_sockets,
            8, "the session IPC socket", self.process)
        require(len(sockets) == 1, f"Expected one new Seaside socket, found {sockets}")
        self.path = sockets.pop()
        require(self.path.stat().st_mode & 0o777 == 0o600 and
                self.socket_dir.stat().st_mode & 0o777 == 0o700,
                "The session socket or directory has incorrect permissions")
        self.environment["WAYLAND_DISPLAY"] = self.path.stem

    def request(self, method, params=None):
        request = {"v": 1, "id": 1, "method": method, "params": params or {}}
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(5)
            connection.connect(str(self.path))
            connection.sendall((json.dumps(request) + "\n").encode())
            with connection.makefile("rb") as stream:
                line = stream.readline(1024 * 1024 + 1)
        require(line.endswith(b"\n") and len(line) <= 1024 * 1024, "Invalid IPC reply framing")
        response = json.loads(line)
        require(response["v"] == 1 and response["id"] == 1 and type(response["seq"]) is int,
                f"Invalid raw IPC response {response}")
        return response

    def __exit__(self, *_):
        try:
            standard["stop"](self.process)
        finally:
            upload(self.log.name, self.log)

    def ctl(self, *args):
        response = subprocess.run(["build/seasidectl", *args], cwd=self.repo, env=self.environment,
                                  timeout=5, text=True, capture_output=True, check=False)
        require(response.returncode == 0, f"seasidectl {' '.join(args)}: {response.stderr.strip()}")
        message = json.loads(response.stdout)
        require(message["v"] == 1 and message["ok"] is True and type(message["seq"]) is int,
                f"Invalid CLI response {message}")
        return message

    def config(self):
        return self.ctl("status")["result"]["config"]

    def night_light_enabled(self):
        return self.ctl("night-light", "status")["result"]["configured_enabled"]

    def windows(self):
        rows = self.ctl("windows")["result"]["windows"]
        ids = [row["id"] for row in rows]
        require(ids == sorted(set(ids)), "Window query returned duplicate or out-of-order IDs")
        for row in rows:
            require(type(row["id"]) is int and type(row["workspace_id"]) is int and
                    isinstance(row["app_id"], str) and isinstance(row["title"], str) and
                    all(type(row[key]) is bool for key in
                        ("mapped", "floating", "fullscreen", "maximized", "pinned")),
                    f"Unexpected window metadata: {row}")
        return [row for row in rows if row["mapped"]]

    def focused_output(self):
        outputs = self.ctl("outputs")["result"]["outputs"]
        return next(value for value in outputs if value["enabled"] and value["focused"])

    def key(self, key):
        event("part6-input", key, key=key)

    def wait(self, predicate, label):
        return wait_for(predicate, 8, label, self.process)

    def screenshot(self, result_dir, name):
        path = result_dir / f"part6-{name}.png"
        subprocess.run(["grim", str(path)], env=self.environment, timeout=5, check=True,
                       capture_output=True)
        with path.open("rb") as stream:
            header = stream.read(24)
        require(len(header) == 24 and header[:8] == standard["PNG_HEADER"] and
                header[12:16] == b"IHDR", f"Invalid PNG capture for {name}")
        upload(path.name, path)
        dimensions = [int.from_bytes(header[i:i + 4], "big") for i in (16, 20)]
        require(min(dimensions) >= 100, f"Unexpected screen size for {name}")
        return dimensions

    def strip(self, bounds):
        return subprocess.check_output(["grim", "-g", bounds, "-"], env=self.environment, timeout=5)

    def pixel(self, x, y):
        raw = subprocess.check_output(["grim", "-s", "1", "-t", "ppm", "-g",
                                       f"{x},{y} 1x1", "-"], env=self.environment, timeout=5)
        magic, width, height, depth, color = raw.split(maxsplit=4)
        require((magic, width, height, depth) == (b"P6", b"1", b"1", b"255") and len(color) == 3,
                "Expected one RGB pixel in the capture")
        return tuple(color)

    def exit(self):
        result = self.ctl("exit")["result"]
        require(result["accepted"] and self.process.wait(timeout=12) == 0,
                "The acknowledged exit did not stop Seaside cleanly")
        require(active() == "tty1" and os.tcgetpgrp(0) == os.getpgrp() and not self.path.exists(),
                "Seaside did not release its IPC socket and foreground console")


class Events:
    def __init__(self, session):
        self.connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.connection.settimeout(8)
        self.connection.connect(str(session.path))
        self.stream = self.connection.makefile("rb")
        self.connection.sendall(b'{"v":1,"id":1,"method":"events.subscribe","params":{}}\n')
        acknowledgement = self.read()
        require(acknowledgement["id"] == 1 and acknowledgement["ok"] and
                acknowledgement["result"]["subscribed"], "Subscription acknowledgement was invalid")
        self.seq = acknowledgement["seq"]
        self.acknowledged = self.seq

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stream.close()
        self.connection.close()

    def read(self):
        line = self.stream.readline(1024 * 1024 + 1)
        require(line.endswith(b"\n") and len(line) <= 1024 * 1024, "Invalid IPC event framing")
        return json.loads(line)

    def notification(self, message):
        require(message["v"] == 1 and type(message["seq"]) is int and self.seq < message["seq"],
                f"Invalid event sequence after {self.seq}: {message}")
        self.seq = message["seq"]
        return message

    def expect(self, name, predicate=lambda _: True, *, unexpected=None):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            message = self.notification(self.read())
            require(message["event"] != unexpected,
                    f"Unexpected event while waiting for {name}: {message}")
            if message["event"] == name and predicate(message["data"]):
                return message
        raise RuntimeError(f"Timed out waiting for {name}")

    def query_watermark(self):
        self.connection.sendall(b'{"v":1,"id":2,"method":"query.config","params":{}}\n')
        result = self.read()
        while "event" in result:
            self.notification(result)
            result = self.read()
        require(result["id"] == 2 and result["ok"] and result["seq"] >= self.seq,
                f"Invalid config query watermark {result}")
        return result["seq"]


def check_completed_saves(session, events, target, snapshot, initial, selected):
    def accepted(text, night_light):
        events.expect("settings.changed")
        session.wait(lambda: session.config()["recovery_persisted"], "completed save snapshot")
        status = session.config()
        require(not status["error"] and session.night_light_enabled() == night_light and
                snapshot.read_text() == text, "Completed save lost live state or snapshot bytes")

    stage = target.with_suffix(".stage")
    stage.write_text(selected)
    target.unlink()
    os.link(stage, target)
    stage.unlink()
    accepted(selected, True)
    os.truncate(target, len(initial.encode()))
    accepted(initial, False)

    # Pause only this trial's child so its real inotify queue deterministically
    # overflows. Every rename contributes two independent, noncoalescing records.
    limit = int(Path("/proc/sys/fs/inotify/max_queued_events").read_text())
    require(0 < limit <= 100_000, f"Impractical inotify queue size {limit}")
    noise_a, noise_b = target.with_suffix(".a"), target.with_suffix(".b")
    noise_a.touch()

    def overflow(close_writer=None):
        nonlocal noise_a, noise_b
        session.process.send_signal(signal.SIGSTOP)
        try:
            _, status = os.waitpid(session.process.pid, os.WUNTRACED)
            require(os.WIFSTOPPED(status), "Could not pause the trial compositor")
            for _ in range(limit // 2 + 2):
                noise_a.rename(noise_b)
                noise_a, noise_b = noise_b, noise_a
            if close_writer:
                close_writer()
        finally:
            session.process.send_signal(signal.SIGCONT)

    finished = selected + "# complete\n"
    with target.open("w") as writer:
        writer.write(selected)
        writer.flush()
        overflow()
        deadline = time.monotonic() + .65
        while time.monotonic() < deadline:
            status = session.config()
            require(not status["error"] and not session.night_light_enabled() and
                    snapshot.read_text() == initial, "Overflow published an open writer's prefix")
            time.sleep(.05)
        writer.write("# complete\n")
        writer.flush()
        overflow(writer.close)
    accepted(finished, True)
    target.write_text(initial)
    accepted(initial, False)
    return {"hard_link_unlink": True, "pathname_truncate": True, "open_prefix_retained": True,
            "lost_close_recovered": True, "queue_limit": limit}


def run_live(repo, directory, result_dir, state):
    config = directory / "config/seaside/config.toml"
    snapshot = directory / "state/seaside/last-good-config.toml"
    first_autostart = directory / "initial-autostart.txt"
    second_autostart = directory / "next-autostart.txt"
    initial = with_autostart(baseline_config, first_autostart)
    repair = with_autostart(repaired_config, second_autostart)
    unsupported_config = repair + "\n[monitors]\nresolution = '313x271'\n"
    moved_config = repair + "\n[monitors]\nposition = [64, 32]\n"
    config.parent.mkdir(parents=True)
    profiles = directory / "profiles"
    files = directory / "files"
    profiles.mkdir()
    files.mkdir()
    selected = initial + "\n[workspaces]\nmode = 'per-monitor'\n[night_light]\nenabled = true\n"
    (files / "work").write_text(initial)
    (files / "home").write_text(selected)
    (profiles / "active").symlink_to("../files/work")
    config.symlink_to("../../profiles/active")
    with Session(repo, directory, result_dir, "live") as session:
        session.wait(lambda: session.config()["recovery_persisted"], "first recovery snapshot")
        require(snapshot.read_text() == initial, "Startup snapshot changed the supplied TOML")
        session.wait(first_autostart.is_file, "the startup command")
        require(first_autostart.read_text().splitlines() == [str(session.path)],
                "The launch command did not inherit the precise session socket")
        with Events(session) as events:
            (profiles / "next").symlink_to("../files/home")
            (profiles / "next").replace(profiles / "active")
            events.expect("settings.changed")
            session.wait(lambda: session.config()["recovery_persisted"], "switched profile snapshot")
            require(session.night_light_enabled() and snapshot.read_text() == selected,
                    "Retargeting the intermediate link did not apply the selected profile")
            (files / "home").write_text(bad_config)
            events.expect("config.error")
            require(session.config()["error"]["field"] == "window.gaps_outer" and
                    snapshot.read_text() == selected, "Invalid profile edit replaced last-good TOML")
            (files / "home").write_text(initial)
            events.expect("config.recovered")
            events.expect("settings.changed")
            session.wait(lambda: session.config()["recovery_persisted"], "repaired profile snapshot")
            require(not session.night_light_enabled() and snapshot.read_text() == initial,
                    "A subsequent edit of the new target did not repair the profile")
            (profiles / "next").symlink_to(".")
            (profiles / "next").replace(profiles / "active")
            events.expect("config.error")
            require(snapshot.read_text() == initial, "A directory target replaced last-good TOML")
            (profiles / "next").symlink_to("../files/home")
            (profiles / "next").replace(profiles / "active")
            events.expect("config.recovered")
            events.expect("settings.changed")
            session.wait(lambda: session.config()["recovery_persisted"], "repaired profile link")
            require(not session.config()["error"] and snapshot.read_text() == initial,
                    "The profile link could not recover from pointing at its own directory")
            state["profile_switch"] = {"retargeted": True, "invalid_retained": True,
                                       "repaired": True, "directory_target_repaired": True}
            state["completed_saves"] = check_completed_saves(session, events, files / "home",
                                                            snapshot, initial, selected)
            session.key("open")
            session.wait(session.windows, "the default Super+Return terminal")
            mapped = events.expect("window.mapped")
            first = mapped["data"]["window_id"]
            require(session.ctl("window", "toggle-fullscreen")["result"]["changed"],
                    "The default terminal could not enter fullscreen")
            fullscreen = events.expect("window.changed", lambda data: data["window_id"] == first and
                                       data["fullscreen"])["data"]
            window = next(window for window in session.windows() if window["id"] == first)
            require(all(fullscreen[field] == window[field] for field in
                        ("workspace_id", "mapped", "floating", "fullscreen", "maximized", "pinned")),
                    "Fullscreen event disagrees with the current window state")
            output = session.focused_output()
            workspaces = session.ctl("workspaces")["result"]["workspaces"]
            require([workspace["id"] for workspace in workspaces] == list(range(1, 11)) and
                    all(workspace["output_id"] == output["id"] for workspace in workspaces),
                    "The single display did not own workspaces 1–10")
            state["workspace_count"] = len(workspaces)
            position = output["position"]
            region = f'{position["x"] + output["mode"]["width"] - 132},{position["y"]} 128x12'
            clean = session.strip(region)
            state["screen"] = session.screenshot(result_dir, "clean")

            session.ctl("night-light", "on")
            events.expect("settings.changed", unexpected="config.recovered")
            session.wait(lambda: session.config()["recovery_persisted"], "night-light recovery snapshot")
            night_light_saved = config.read_text()
            require(session.night_light_enabled() and config.is_symlink() and
                    tomllib.loads(night_light_saved)["night_light"]["enabled"] and
                    snapshot.read_text() == night_light_saved,
                    "Night-light change did not update the linked user file and the recovery snapshot")
            replace(config, bad_config)
            invalid = events.expect("config.error")
            rejected = session.config()
            require(rejected["error"]["field"] == "window.gaps_outer" and
                    rejected["error"]["line"] == 2 and snapshot.read_text() == night_light_saved,
                    "Invalid edit lost its diagnostic or replaced last-good TOML")
            session.wait(lambda: session.strip(region) != clean, "visible fullscreen error banner")
            session.screenshot(result_dir, "error")

            config.unlink()
            missing = events.expect("config.error", lambda data: data["message"] ==
                                    "configuration is missing")
            prefix, bindings = repair.split("[keybinds]", 1)
            with config.open("w") as writer:
                writer.write(prefix)
                writer.flush()
                # The prefix is valid TOML with a different gaps setting.
                # Hold it open across the debounce window and query live state.
                deadline = time.monotonic() + .65
                while time.monotonic() < deadline:
                    staged = session.config()
                    require(staged["error"] and staged["error"]["message"] ==
                            missing["data"]["message"] and
                            snapshot.read_text() == night_light_saved,
                            "An unfinished recreated config changed active state or last-good TOML")
                    time.sleep(.05)
                writer.write("[keybinds]" + bindings)
            recovered = events.expect("config.recovered")
            events.expect("settings.changed")
            session.wait(lambda: session.config()["recovery_persisted"], "new recovery snapshot")
            accepted = session.config()
            require(not accepted["error"] and snapshot.read_text() == repair,
                    "Valid edit lost live state or snapshot bytes")
            state["partial_recreation"] = "held-until-close"
            require(session.night_light_enabled(), "The repaired file did not retain night light")
            session.wait(lambda: session.strip(region) == clean, "banner removal after repair")
            session.screenshot(result_dir, "recovered")

            replace(config, unsupported_config)
            mode_rejected = events.expect("config.error")
            retained = session.focused_output()
            require(session.config()["error"]["field"] == "monitors.resolution" and
                    all(retained[key] == output[key] for key in ("id", "mode", "position", "scale", "rotation")) and
                    snapshot.read_text() == repair, "Unsupported mode changed the realized output or snapshot")

            replace(config, moved_config)
            moved = events.expect("config.recovered")
            events.expect("settings.changed")
            session.wait(lambda: session.focused_output()["position"] == {"x": 64, "y": 32},
                         "live output position")
            actual = session.focused_output()
            require(actual["id"] == output["id"] and actual["mode"] == output["mode"] and
                    actual["hardware_cursor"] is True, "A position edit lost output identity, mode or cursor")
            session.wait(lambda: session.config()["recovery_persisted"], "output recovery snapshot")
            require(snapshot.read_text() == moved_config, "Valid output edit did not update the snapshot")
            state["output_edit"] = {"mode": actual["mode"], "before": output["position"],
                                    "applied": actual["position"]}

            session.ctl("workspace", "4")
            workspace = events.expect("workspace.changed", lambda data: data["workspace_id"] == 4 and
                                        data["visible"])
            session.key("open")
            deadline = time.monotonic() + .65
            while time.monotonic() < deadline:
                require({window["id"] for window in session.windows()} == {first},
                        "Removed Super+Return binding launched another terminal")
                time.sleep(.05)
            session.key("remapped")
            session.wait(lambda: any(window["id"] != first and window["workspace_id"] == 4
                                     for window in session.windows()), "replacement Super+F2 shortcut")
            remapped = events.expect("window.mapped", lambda data: data["window_id"] != first)
            focused = events.expect("window.focused", lambda data: data["window_id"] ==
                                    remapped["data"]["window_id"])
            state["event_sequence"] = {"subscribed": events.acknowledged, "first_window": mapped["seq"],
                                       "invalid": invalid["seq"], "missing_file": missing["seq"],
                                       "repaired": recovered["seq"],
                                       "mode_rejected": mode_rejected["seq"], "position_applied": moved["seq"],
                                       "workspace": workspace["seq"], "remapped": remapped["seq"],
                                       "focused": focused["seq"], "query": events.query_watermark()}
            require(first_autostart.read_text().splitlines() == [str(session.path)] and
                    not second_autostart.exists(), "Reload reran autostart commands")
            target = remapped["data"]["window_id"]
            moved_window = session.request("action.window_to_workspace",
                                           {"window_id": target, "workspace_id": 5})
            require(moved_window["ok"] and moved_window["result"]["changed"], "Window move failed")
            changed = events.expect("window.changed", lambda data: data["window_id"] == target and
                                    data["workspace_id"] == 5)["data"]
            require(changed["mapped"], "A moved window was reported as unmapped")
            close = session.request("action.window_close", {"window_id": target})
            require(close["ok"] and close["result"] == {"accepted": True, "changed": False},
                    "Close should acknowledge the request while the client handles it")
            events.expect("window.unmapped", lambda data: data["window_id"] == target)
            events.expect("window.destroyed", lambda data: data["window_id"] == target)
            stale = session.request("action.window_toggle_tiling", {"window_id": target})
            require(not stale["ok"] and stale["error"]["code"] == "not_found" and
                    all(window["id"] != target for window in session.windows()),
                    "A destroyed window ID remained usable")
            state["stale_window_result"] = stale["error"]["code"]
            session.exit()

    replace(config, unsupported_config)
    with Session(repo, directory, result_dir, "restart") as session:
        recovered = session.config()
        require(recovered["source"] == "last_good" and
                recovered["error"]["field"] == "monitors.resolution" and
                snapshot.read_text() == moved_config and config.read_text() == unsupported_config,
                "An unsupported boot mode did not recover the last-good configuration")
        require(session.night_light_enabled(), "Restart did not restore the saved night-light setting")
        session.wait(second_autostart.is_file, "the restart autostart command")
        require(second_autostart.read_text().splitlines() == [str(session.path)],
                "Recovery did not start the persisted launch command with the current socket")
        output = session.focused_output()
        position = output["position"]
        require(position == {"x": 64, "y": 32}, "Restart did not restore the applied output position")
        right = position["x"] + output["mode"]["width"] - 132
        top = position["y"]
        session.wait(lambda: session.strip(f"{right},{top} 128x12") !=
                     session.strip(f"{right},{top + 150} 128x12"), "visible startup error banner")
        session.screenshot(result_dir, "startup-error")
        state["night_light_persisted"] = True
        state["startup_source"] = recovered["source"]
        session.exit()


def solid_png(path, rgb):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))

    header = struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)
    rows = (b"\0" + bytes(rgb) * 2) * 2
    path.write_bytes(standard["PNG_HEADER"] + chunk(b"IHDR", header) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def run_appearance(repo, directory, result_dir, state):
    config = directory / "config/seaside/config.toml"
    snapshot = directory / "state/seaside/last-good-config.toml"
    config.parent.mkdir(parents=True)
    wallpaper = directory / "color.png"
    damaged = directory / "damaged.png"
    color = (20, 104, 217)
    solid_png(wallpaper, color)
    damaged.write_bytes(standard["PNG_HEADER"])
    replace(config, baseline_config)

    with Session(repo, directory, result_dir, "appearance") as session:
        mode = session.focused_output()["mode"]
        x = mode["width"] * 4 // 5
        y = mode["height"] * 3 // 4
        require(session.pixel(x, y) == (0, 0, 0), "Expected the default black wallpaper")
        with Events(session) as events:
            wallpaper_config = f"[wallpaper]\npath = {json.dumps(str(wallpaper))}\n"
            replace(config, baseline_config + wallpaper_config)
            events.expect("settings.changed")
            session.wait(lambda: session.pixel(x, y) == color, "live wallpaper pixels")
            session.wait(lambda: session.config()["recovery_persisted"], "wallpaper snapshot")
            reference = snapshot.read_text()

            replace(config, baseline_config + f"[wallpaper]\npath = {json.dumps(str(damaged))}\n")
            events.expect("config.error")
            require(session.config()["error"]["field"] == "wallpaper.path" and
                    session.pixel(x, y) == color and snapshot.read_text() == reference,
                    "Rejected wallpaper edit changed the wallpaper or recovery snapshot")

            scaled = reference + "\n[monitors]\nscale = 1.25\n"
            replace(config, scaled)
            events.expect("config.recovered")
            events.expect("settings.changed")
            session.wait(lambda: session.focused_output()["scale"] == 1.25, "real fractional scale")
            output = session.focused_output()
            require(output["hardware_cursor"] and output["mode"] == mode,
                    "Scale changed the mode or lost the hardware cursor")
            dimensions = subprocess.check_output(["grim", "-s", "1", "-"], env=session.environment,
                                                 timeout=5)[16:24]
            require(struct.unpack(">II", dimensions) ==
                    (int(mode["width"] / 1.25), int(mode["height"] / 1.25)),
                    "Composed logical output bounds disagree with the configured scale")

            replace(config, reference)
            events.expect("settings.changed")
            session.wait(lambda: session.focused_output()["scale"] == 1, "restored scale")
            session.key("open")
            session.wait(lambda: len(session.windows()) == 1, "first tile")
            session.key("open")
            session.wait(lambda: len(session.windows()) == 2, "second tile")
            session.wait(lambda: session.pixel(x, y) != color, "Dwindle tile covering the wallpaper")
            tile_color = session.pixel(x, y)
            require(tile_color != (0, 0, 0), "Dwindle tile was not presenting client pixels")

            scrolling = ("[window]\nlayout = 'scrolling'\n[scrolling]\n"
                         "default_column_width = 0.30\n" + wallpaper_config)
            replace(config, scrolling)
            events.expect("settings.changed")
            session.wait(lambda: session.pixel(x, y) == color, "scrolling layout revealing wallpaper")
            session.wait(lambda: session.config()["recovery_persisted"], "scrolling snapshot")
            replace(config, scrolling.replace("0.30", "'invalid'"))
            events.expect("config.error")
            require(session.pixel(x, y) == color and snapshot.read_text() == scrolling,
                    "Rejected layout edit changed presentation or the recovery snapshot")
            replace(config, reference)
            events.expect("config.recovered")
            events.expect("settings.changed")
            session.wait(lambda: session.pixel(x, y) == tile_color, "restored Dwindle client pixels")
            require(len(session.windows()) == 2, "Live layout replaced client windows")
            state["appearance"] = {"wallpaper_rgb": color, "scale": 1.25,
                                   "scrolling_exposed_wallpaper": True, "rejected_edits_retained": True}
            session.exit()


def run_preflight(repo, directory, result_dir, state):
    config = directory / "config/seaside/config.toml"
    config.parent.mkdir(parents=True)
    marker = directory / "autostart.txt"
    replace(config, with_autostart(baseline_config, marker))
    # Give the production PipeWire client a reachable test socket with no
    # replies. Ordinary actions and autostart must be usable during the probe.
    silent_path = directory / "silent-pipewire"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as silent:
        silent.bind(str(silent_path))
        silent.listen(1)
        silent.settimeout(8)
        with Session(repo, directory, result_dir, "preflight", PIPEWIRE_REMOTE=silent_path.name,
                     PIPEWIRE_RUNTIME_DIR=str(directory)) as session:
            connection, _ = silent.accept()
            with connection, Events(session):
                status = session.ctl("status")["result"]
                require(status["ready"] and status["integrations"]["pipewire"]["state"] == "checking",
                        "The Wayland session did not become usable during its PipeWire probe")
                mutation = session.request("action.workspace_select", {"workspace_id": 9})
                require(mutation["ok"] and session.ctl("focus")["result"]["workspace_id"] == 9,
                        "Workspace selection did not work during the silent PipeWire probe")
                session.wait(marker.is_file, "autostart during the PipeWire probe")
                session.exit()
                require(len(marker.read_text().splitlines()) == 1, "Autostart did not run exactly once")
                state["preflight_ipc"] = {"queries": True, "subscription": True, "actions": True,
                                          "clean_exit": True}


def trial():
    tty = os.ttyname(0)
    current = active()
    existing = subprocess.run(["pgrep", "-x", "-t", "tty1", "seaside"], capture_output=True,
                              text=True, timeout=3)
    event("connected", f"stdin={tty} active={current}")
    if (tty != "/dev/tty1" or current != "tty1" or os.tcgetpgrp(0) != os.getpgrp() or
            existing.returncode != 1):
        event("done", "An idle foreground shell on active TTY 1 is required", ok=False)
        return
    result_dir = Path.home() / "Projects/seaside/test-results" / f"vm-{base.rsplit('/', 1)[-1][:12]}"
    result_dir.mkdir(mode=0o700, parents=True)
    state = {"revision": revision, "kernel": os.uname().release, "renderer": "pixman", "ok": False}
    try:
        runtime = os.environ.get("XDG_RUNTIME_DIR")
        require(runtime, "The guest did not provide XDG_RUNTIME_DIR")
        with tempfile.TemporaryDirectory(prefix="seaside-vm-source-") as source:
            repo = prepare(Path(source), result_dir, state)
            with tempfile.TemporaryDirectory(prefix="seaside-vm-", dir=runtime) as name:
                temporary = Path(name)
                standard["tty_trial"](repo, temporary, result_dir, state)
                run_live(repo, temporary / "live", result_dir, state)
                run_appearance(repo, temporary / "appearance", result_dir, state)
                run_preflight(repo, temporary / "preflight", result_dir, state)
        state["ok"] = True
    except (OSError, RuntimeError, ValueError, KeyError, StopIteration, subprocess.SubprocessError,
            KeyboardInterrupt) as error:
        state["error"] = str(error) or type(error).__name__
        event("diagnostic", state["error"])
    finally:
        for name in ("prepare.log", "harness.log", "normal.log", "failure.log"):
            upload(name, result_dir / name)
        state["guest_evidence"] = str(result_dir)
        standard["send"]("guest.json", json.dumps(state, indent=2).encode() + b"\n")
        event("done", state.get("error", "Part 6 VM trial passed"), ok=state["ok"])


trial()
