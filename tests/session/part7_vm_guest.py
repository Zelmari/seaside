"""Compatibility, video sharing, and audio checks on the production Arch TTY."""

from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time


event = standard["event"]
wait_for = standard["wait_for"]
upload = standard["upload"]
run_logged = standard["run_logged"]
revision = standard["REVISION"]
base = standard["BASE"]
mode = standard["MODE"]
NATIVE_TITLE = "Part7 Native Target"
X11_TITLE = "Seaside Part 7 X11 capture"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def output(args, *, env=None, timeout=8):
    return subprocess.check_output(list(map(str, args)), env=env, text=True, timeout=timeout).strip()


def memory(pid):
    proc = Path(f"/proc/{pid}")
    status = (proc / "status").read_text()
    meminfo = Path("/proc/meminfo").read_text()
    fields = {name: int(value) for name, value in
              re.findall(r"^(VmRSS|VmHWM|VmData):\s+(\d+) kB$", status, re.MULTILINE)}
    available = re.search(r"^MemAvailable:\s+(\d+) kB$", meminfo, re.MULTILINE)
    return dict(fields, fds=sum(1 for _ in (proc / "fd").iterdir()),
                guest_available_kb=int(available[1]))


@contextmanager
def track_memory(session, path):
    stopped = threading.Event()

    def collect():
        with path.open("w", buffering=1) as stream:
            for _ in range(720):
                try:
                    sample = memory(session.process.pid)
                except (FileNotFoundError, ProcessLookupError):
                    return
                excessive = sample.get("VmRSS", 0) > 512 * 1024 or sample["guest_available_kb"] < 768 * 1024
                stream.write(json.dumps(dict(sample, recorded_at=time.time(), limit_reached=excessive)) + "\n")
                if excessive:
                    if session.process.poll() is None:
                        session.process.terminate()
                    return
                if stopped.wait(.5):
                    return

    thread = threading.Thread(target=collect, daemon=True)
    thread.start()
    try:
        yield
    finally:
        stopped.set()
        thread.join(timeout=2)


def environment(state):
    env = dict(os.environ)
    for key in ("PIPEWIRE_REMOTE", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SEASIDE_SOCKET",
                "DISPLAY", "XAUTHORITY"):
        env.pop(key, None)
    if part7_pkg_config_path:
        require(Path(part7_pkg_config_path).is_dir(), "Guest pkg-config path is unavailable")
        env["PKG_CONFIG_PATH"] = os.pathsep.join(filter(None, (part7_pkg_config_path,
                                                              env.get("PKG_CONFIG_PATH"))))
    if part7_runtime_prefix:
        root = Path(part7_runtime_prefix)
        require((root / "bin").is_dir(), "Guest runtime prefix is unavailable")
        for key, directory in (("PATH", root / "bin"), ("LD_LIBRARY_PATH", root / "lib")):
            env[key] = os.pathsep.join(filter(None, (str(directory), env.get(key))))
        manifest = root.parent / "manifest.json"
        if manifest.is_file():
            state["locally_staged_packages"] = json.loads(manifest.read_text())
    return env


def prepare_xkbcomp(env, root, state):
    if not part7_runtime_prefix or Path("/usr/bin/xkbcomp").is_file():
        return
    package_bin = Path(part7_runtime_prefix) / "bin"
    require((package_bin / "xkbcomp").is_file(), "The staged Xwayland requires xkbcomp")
    wrapper_dir = root / "xwayland-launcher"
    wrapper_dir.mkdir()
    wrapper = wrapper_dir / "Xwayland"
    # The official binary looks for /usr/bin/xkbcomp. Give only Xwayland a
    # private view of the signed compiler when it is missing from the guest.
    script = ('set -eu; packages=$1; shift; mount --make-rprivate /; '
              'mount -t overlay overlay -o "lowerdir=$packages:/usr/bin" /usr/bin; '
              'exec "$packages/Xwayland" "$@"')
    launcher = ["/usr/bin/unshare", "--user", "--map-root-user", "--mount", "--",
                "/bin/sh", "-c", script, "sh", str(package_bin)]
    wrapper.write_text("#!/bin/sh\nexec " + shlex.join(launcher) + ' "$@"\n')
    wrapper.chmod(0o700)
    env["PATH"] = str(wrapper_dir) + os.pathsep + env["PATH"]
    state["xkbcomp"] = "signed Arch package in Xwayland's private mount namespace"


def prepare(source, results, env, state):
    bundle = source / "revision.bundle"
    limit = 64 * 1024 * 1024
    with standard["OPENER"].open(base + "/bundle", timeout=30) as response:
        bundle.write_bytes(response.read(limit + 1))
    require(0 < bundle.stat().st_size <= limit, "Invalid delivered Git bundle")
    repo = source / "checkout"
    log = results / "prepare.log"
    event("update", f"isolated checkout of {revision[:12]}")
    require(not run_logged(["git", "clone", "--quiet", str(bundle), str(repo)], source, log, 45),
            "Cloning the supplied revision failed")
    git = standard["git"]
    require(git(repo, "rev-parse", "HEAD") == revision and not git(repo, "status", "--porcelain"),
            "The isolated checkout does not match the requested revision")
    dependencies = ["wlroots-0.20", "wayland-protocols", "xau", "xcb", "libpipewire-0.3"]
    versions = output(["pkg-config", "--modversion", *dependencies], env=env).splitlines()
    state["dependencies"] = dict(zip(dependencies, versions, strict=True))
    installed = ["xdg-desktop-portal", "xdg-desktop-portal-wlr", "xdg-desktop-portal-gtk", "pipewire",
                 "pipewire-audio", "pipewire-pulse", "wireplumber", "grim", "wofi", "xwayland-satellite",
                 "xorg-xwayland", "wf-recorder", "obs-studio", "discord", "mesa"]
    packages = subprocess.run(["pacman", "-Q", *installed], capture_output=True, text=True, timeout=5)
    state["installed_packages"] = dict(line.split(maxsplit=1) for line in packages.stdout.splitlines())
    state["tools"] = {name: shutil.which(name, path=env.get("PATH")) for name in
                      ("wofi", "grim", "xwayland-satellite", "Xwayland", "wf-recorder", "obs",
                       "discord", "wpctl", "pactl", "pacat", "parec")}
    if mode == "tty":
        state["software_checks"] = "not-run"
        event("build", revision[:12])
        require(not run_logged(["meson", "setup", "build", "--buildtype=debug"], repo, log, 120,
                               env=env), "Guest configure failed")
        targets = ["seaside", "seasidectl", "seaside-portal-stream", "seaside-part7-native-client",
                   "seaside-part7-x11-client"]
        require(not run_logged(["meson", "compile", "-C", "build", "-j2", *targets], repo, log, 420,
                               env=env), "Guest compositor or fixture build failed")
    else:
        event("harness", revision[:12])
        reports = results / "harness"
        status = run_logged(["tools/run-tests", "--results-dir", str(reports), "--suite", "unit",
                             "--suite", "integration", "--suite", "runner", "--suite", "hardware",
                             "--jobs", "2"],
                            repo, results / "harness.log", 760, env=env)
        summaries = list(reports.glob("*/summary.json"))
        require(len(summaries) == 1, "The guest harness did not produce a unique summary")
        summary = summaries[0]
        upload("summary.json", summary)
        upload("summary.txt", summary.with_suffix(".txt"))
        report = json.loads(summary.read_text())
        state["software_checks"] = report["software_status"]
        tests = report["tests"]
        part7 = [test for test in tests if test["part"] == 7]
        require(status == 0 and state["software_checks"] == "pass" and part7 and
                all(test["status"] == "pass" for test in part7) and
                any(test["test_id"].endswith("part3-real-portals") and test["status"] == "pass"
                    for test in tests), f"Guest regression, Part 7, or real portal test failed ({status})")
    return repo


class TrialSession:
    def __init__(self, repo, root, results, env, chooser):
        self.repo = repo
        self.log = results / "part7-session.log"
        self.root = root
        self.env = dict(env, XDG_CONFIG_HOME=str(root / "config"),
                        XDG_STATE_HOME=str(root / "state"), WLR_RENDERER="pixman")
        self.marker = root / "launch-environment"
        config = root / "config/seaside/config.toml"
        config.parent.mkdir(parents=True)
        marker_command = ("umask 077; printf '%s\\n' \"$WAYLAND_DISPLAY\" \"$SEASIDE_SOCKET\" "
                          "\"$DISPLAY\" \"$XAUTHORITY\" > " + shlex.quote(str(self.marker)))
        config.write_text("[window]\ntiling = false\n[autostart]\ncommands = ["
                          + json.dumps(marker_command) + "]\n")
        portal = root / "config/xdg-desktop-portal-wlr/Seaside"
        portal.parent.mkdir()
        command = [chooser, "-d", "-n", "--no-custom-entry", f"--cache-file={root / 'wofi-cache'}"]
        if part7_runtime_prefix:
            command = ["/usr/bin/env", "LD_LIBRARY_PATH=" + str(Path(part7_runtime_prefix) / "lib"),
                       *command]
        chooser_script = root / "share-chooser"
        chooser_script.write_text("#!/bin/sh\nexec " + shlex.join(command) + "\n")
        chooser_script.chmod(0o700)
        portal.write_text(f"[screencast]\nchooser_type=dmenu\nchooser_cmd={chooser_script}\n")
        self.process = standard["start_seaside"](repo, self.env, self.log)

    def ready(self):
        wait_for(lambda: re.search(r"^seaside READY cursor-plane=([1-9][0-9]*)$", self.log_text(),
                                   re.MULTILINE), 25, "Part 7 startup and hardware cursor", self.process)
        def launch_environment():
            if not self.marker.is_file():
                return None
            text = self.marker.read_text()
            values = text.splitlines()
            return values if text.endswith("\n") and len(values) == 4 and all(values) else None

        lines = wait_for(launch_environment, 5, "autostart environment", self.process)
        self.env.update(zip(("WAYLAND_DISPLAY", "SEASIDE_SOCKET", "DISPLAY", "XAUTHORITY"), lines,
                            strict=True))
        require(Path(lines[3]).stat().st_mode & 0o777 == 0o600,
                "Satellite authority file has incorrect permissions")
        require(self.ctl("status")["ready"], "The Part 7 session is not ready over IPC")

    def log_text(self):
        return self.log.read_text(errors="replace")

    def ctl(self, *args):
        return json.loads(output([self.repo / "build/seasidectl", *args], env=self.env))["result"]

    def windows(self):
        return self.ctl("windows")["windows"]

    def window(self, title):
        return next((row for row in self.windows() if row["title"] == title and row["mapped"]), None)

    def focus(self, title):
        target = wait_for(lambda: self.window(title), 8, f"{title} window", self.process)
        for _ in range(4):
            if self.ctl("focus")["window_id"] == target["id"]:
                return
            event("part7-input", f"cycle focus to {title}", key="cycle")
            deadline = time.monotonic() + .8
            while time.monotonic() < deadline:
                if self.ctl("focus")["window_id"] == target["id"]:
                    return
                time.sleep(.06)
        raise RuntimeError(f"Could not focus {title}")

    def close(self):
        if self.process.poll() is None:
            self.ctl("exit")
        require(self.process.wait(timeout=12) == 0, "Part 7 clean exit failed")


@contextmanager
def process(session, path, executable, *args, stdin=False):
    with path.open("w") as log:
        child = subprocess.Popen([session.repo / "build" / executable, *args], env=session.env,
                                 stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                                 stdout=log, stderr=subprocess.STDOUT, text=True)
        try:
            yield child
        finally:
            standard["stop"](child)
            if child.stdin is not None:
                child.stdin.close()


def command(child, line):
    child.stdin.write(line + "\n")
    child.stdin.flush()


def choose(session, child, *, choice=None):
    def chooser_window():
        return next((row for row in session.windows()
                     if "wofi" in (row.get("app_id", "") or "").casefold()
                     or "wofi" in (row.get("title", "") or "").casefold()), None)

    window = wait_for(chooser_window, 10, "the real Wofi chooser window", child)
    wait_for(lambda: session.ctl("focus")["window_id"] == window["id"], 4,
             "keyboard focus on the chooser", child)
    if choice:
        event("part7-input", f"select {choice} in Wofi", choice=choice)
    else:
        event("part7-input", "cancel the Wofi chooser", key="cancel")


def frame_rows(path):
    return [tuple(line.split()[1:]) for line in path.read_text(errors="replace").splitlines()
            if line.startswith("frame ")]


def wait_color(path, child, color, size=None):
    return wait_for(lambda: next((row for row in frame_rows(path)
                                  if all(pixel == color for pixel in row[2:]) and
                                  (not size or row[:2] == size)), None),
                    8, f"portal frame with {color} pixels", child)


def first_live_frame(path, child, repaint, color=None):
    next_paint = 0.

    def read_frame():
        nonlocal next_paint
        for row in frame_rows(path):
            if color is None or all(pixel == color for pixel in row[2:]):
                return row
        if child.poll() is not None:
            raise RuntimeError(f"Portal receiver exited {child.returncode}: {path.read_text(errors='replace')}")
        now = time.monotonic()
        if now >= next_paint and "PipeWire stream state streaming" in path.read_text(errors="replace"):
            repaint()
            next_paint = now + 1.
        return None

    return wait_for(read_frame, 8, "the first live PipeWire frame")


@contextmanager
def start_stream(session, results, state, log, source, lifetime, choice, first_frame):
    for attempt in range(1, 4):
        with process(session, log, "seaside-portal-stream", source, lifetime) as reader:
            choose(session, reader, choice=choice)
            try:
                first = first_frame(reader)
            except RuntimeError:
                diagnostic = log.read_text(errors="replace")
                if (attempt == 3 or frame_rows(log) or reader.poll() is not None or
                        "PipeWire stream state streaming" not in diagnostic):
                    raise
                retries = state.setdefault("portal_start_retries", [])
                retries.append({"source": choice, "attempt": attempt, "receiver_log": diagnostic})
                event("part7", f"retry {choice} after a linked stream delivered no frames")
                if len(retries) == 1:
                    graph = results / "part7-pipewire.log"
                    for args in (["systemctl", "--user", "show", "-p", "MainPID,SubState", "wireplumber"],
                                 ["pw-cli", "ls", "Node"], ["pw-link", "-l"]):
                        run_logged(args, session.repo, graph, 3, env=session.env)
                continue
            yield reader, first
            return


def run_monitor(session, results, state):
    tools = state["tools"]
    grab = results / "part7-grim.png"
    output([tools["grim"], "-c", grab], env=session.env)
    png = grab.read_bytes()[:24]
    require(png[:8] == b"\x89PNG\r\n\x1a\n" and png[12:16] == b"IHDR", "grim PNG is invalid")
    monitor_size = tuple(map(str, struct.unpack(">II", png[16:24])))
    state["grim_dimensions"] = monitor_size
    monitor_log = results / "part7-monitor.log"

    title = "Part7 Monitor Pattern"
    with process(session, results / "part7-monitor-pattern.log", "seaside-part7-native-client", title,
                 "664422", stdin=True) as pattern:
        wait_for(lambda: session.window(title), 8, "the monitor's live pattern window", pattern)
        colors = ("227744", "662255")
        next_color = 0

        def repaint():
            nonlocal next_color
            command(pattern, colors[next_color])
            next_color = 1 - next_color

        with start_stream(session, results, state, monitor_log, "monitor", "frame", "Monitor",
                          lambda reader: first_live_frame(monitor_log, reader, repaint)) as (reader, first):
            require(reader.wait(timeout=8) == 0, f"Monitor stream failed: {monitor_log.read_text()}")
            require(first[:2] == monitor_size, "Portal monitor dimensions differ from grim")
            state["monitor_frames"] = frame_rows(monitor_log)


def run_video(session, results, state):
    native_log = results / "part7-native.log"
    cast_log = results / "part7-window.log"
    with process(session, native_log, "seaside-part7-native-client", NATIVE_TITLE, "b04e29",
                 stdin=True) as native:
        target = wait_for(lambda: session.window(NATIVE_TITLE), 8, "native target window", native)
        cancel_log = results / "part7-cancel.log"
        with process(session, cancel_log, "seaside-portal-stream", "window", "cancel") as reader:
            choose(session, reader)
            require(reader.wait(timeout=12) == 0 and "cancelled" in cancel_log.read_text(),
                    f"Chooser did not cancel: {cancel_log.read_text()}")
        with start_stream(session, results, state, cast_log, "window", "closed", NATIVE_TITLE,
                          lambda reader: first_live_frame(cast_log, reader, lambda: command(native, "b04e29"),
                                                          "b04e29")) as (reader, first):
            require(first[:2] != tuple(state.get("grim_dimensions", ())), "The window stream has monitor dimensions")
            with process(session, results / "part7-overlap.log", "seaside-part7-native-client",
                         "Part7 Overlap", "fa2277", stdin=True) as overlap:
                wait_for(lambda: session.window("Part7 Overlap"), 8, "overlap window", overlap)
                command(native, "ad286c")
                wait_color(cast_log, reader, "ad286c", first[:2])
                session.ctl("workspace", "2")
                focus = session.ctl("focus")
                require(focus["workspace_id"] == 2 and focus["window_id"] is None,
                        "The captured window followed the workspace switch")
                command(native, "1177b1")
                wait_color(cast_log, reader, "1177b1", first[:2])
                command(native, "23ad52")
                wait_color(cast_log, reader, "23ad52", first[:2])
                require(session.ctl("focus") == focus, "Hidden capture changed keyboard focus")
                session.ctl("workspace", "1")
                require(session.window(NATIVE_TITLE)["id"] == target["id"], "The target ID changed")
                command(native, "close")
                require(native.wait(timeout=8) == 0, "The native fixture did not close")
                require(reader.wait(timeout=10) == 0 and "closed frames " in cast_log.read_text(),
                        f"Target loss failed to close its stream: {cast_log.read_text()}")
            state["native_window_frames"] = frame_rows(cast_log)


def run_x11(session, results, state):
    require("xwayland-satellite started pid=" not in session.log_text(),
            "Native Wayland and portal use started Xwayland before an X11 connection")
    x11_log = results / "part7-x11.log"
    cast_log = results / "part7-x11-window.log"
    with process(session, x11_log, "seaside-part7-x11-client", stdin=True) as client:
        wait_for(lambda: session.window(X11_TITLE), 12, "first satellite-backed X11 window", client)
        first_pid = wait_for(lambda: re.search(r"xwayland-satellite started pid=([0-9]+)", session.log_text()),
                             5, "the owned satellite child", session.process)[1]
        with start_stream(session, results, state, cast_log, "window", "closed", X11_TITLE,
                          lambda reader: first_live_frame(cast_log, reader, lambda: command(client, "color 2496d0"),
                                                          "2496d0")) as (reader, first):
            session.ctl("workspace", "2")
            before = session.ctl("focus")
            command(client, "color c24122")
            wait_color(cast_log, reader, "c24122", first[:2])
            require(session.ctl("focus") == before, "The hidden X11 stream changed focus")
            command(client, "quit")
            require(client.wait(timeout=8) == 0, "The X11 fixture did not close")
            require(reader.wait(timeout=10) == 0 and "closed frames " in cast_log.read_text(),
                    f"X11 target loss failed to close its stream: {cast_log.read_text()}")
        state["x11_window_frames"] = frame_rows(cast_log)
    session.ctl("workspace", "1")
    previous = int(first_pid)
    children = Path(f"/proc/{session.process.pid}/task/{session.process.pid}/children")
    require(str(previous) in children.read_text().split(), "Satellite PID is not a child of this trial")
    os.kill(previous, signal.SIGTERM)
    wait_for(lambda: str(previous) not in children.read_text().split(), 8, "satellite child reaping",
             session.process)
    with process(session, results / "part7-x11-restart.log", "seaside-part7-x11-client",
                 stdin=True) as client:
        wait_for(lambda: session.window(X11_TITLE), 12, "X11 window after satellite restart", client)
        pids = re.findall(r"xwayland-satellite started pid=([0-9]+)", session.log_text())
        require(len(pids) == 2 and pids[0] != pids[1], "Satellite did not restart exactly once")
        command(client, "quit")
        require(client.wait(timeout=8) == 0, "The restarted X11 client did not exit")
        state["satellite_pids"] = pids


def volume(session):
    value = output(["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"], env=session.env)
    match = re.fullmatch(r"Volume: ([0-9.]+)( \[MUTED\])?", value)
    require(match is not None, "wpctl returned an unexpected volume result")
    return float(match[1]), bool(match[2])


def run_audio(session, state):
    def pulse(*args):
        return output(["pactl", *args], env=session.env)

    default_key = "default.configured.audio.sink"
    history_key = "node.restore-default-targets"

    def metadata(name, *args):
        return output(["pw-metadata", "-n", name, *args], env=session.env)

    def read_metadata(name, key):
        prefix = f"update: id:0 key:'{key}' value:'"
        for row in metadata(name, "0", key).splitlines():
            if row.startswith(prefix):
                value, marker, kind = row[len(prefix):].rpartition("' type:'")
                require(bool(marker) and kind.endswith("'"), f"Could not parse audio metadata {key}")
                return value, kind[:-1]
        return None

    previous = read_metadata("default", default_key)
    history = read_metadata("sm-settings", history_key)
    require(history and history[0] in ("true", "false"), "WirePlumber history setting is unavailable")

    def set_history(value):
        # Without --save this applies only to the running session manager.
        output(["wpctl", "settings", history_key, value], env=session.env)
        wait_for(lambda: read_metadata("sm-settings", history_key) == (value, history[1]),
                 5, "WirePlumber history setting " + value)

    modules = []
    sink_name = f"seaside_part7_{session.process.pid}"
    try:
        if history[0] == "true":
            set_history("false")
        for suffix in ("_a", "_b"):
            modules.append(pulse("load-module", "module-null-sink", "sink_name=" + sink_name + suffix))
        first = sink_name + "_a"
        second = sink_name + "_b"

        def select_sink(name):
            pulse("set-default-sink", name)
            wait_for(lambda: name in output(["wpctl", "inspect", "@DEFAULT_AUDIO_SINK@"],
                                           env=session.env), 6, f"default audio node {name}")

        select_sink(first)
        output(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", ".45"], env=session.env)
        pulse("set-sink-mute", first, "0")

        def key(name, expected):
            event("part7-input", f"test the {name} media key", key=name)
            return wait_for(lambda: volume(session) == expected and expected, 6,
                            f"default audio state {expected}", session.process)

        wait_for(lambda: volume(session) == (.45, False), 6, "first audio node selection", session.process)
        changes = [key("up", (.50, False)), key("down", (.45, False)), key("mute", (.45, True)),
                   key("mute", (.45, False))]
        select_sink(second)
        output(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", ".98"], env=session.env)
        pulse("set-sink-mute", second, "0")
        wait_for(lambda: volume(session) == (.98, False), 6, "changed audio node selection",
                 session.process)
        changes.append(key("up", (1., False)))
        changes.append(key("down", (.95, False)))
        state["media_key_volume_mute"] = changes

        monitor_name = second + ".monitor"
        sources = json.loads(pulse("-f", "json", "list", "sources"))
        monitor = next((row["index"] for row in sources if row["name"] == monitor_name), None)
        require(monitor is not None, f"PulseAudio did not expose {monitor_name}")
        sample_format = ["--raw", "--format=s16le", "--channels=1", "--rate=48000", "--latency-msec=25"]
        evidence = {"source": monitor_name, "source_index": monitor}
        with (tempfile.TemporaryFile(dir=session.root) as pcm,
              tempfile.TemporaryFile(dir=session.root) as log):
            record = subprocess.Popen(["parec", *sample_format, "--device=" + monitor_name],
                                      env=session.env, stdout=pcm, stderr=log)

            def receiver_ready():
                streams = json.loads(pulse("-f", "json", "list", "source-outputs"))
                return next((row for row in streams if row["source"] == monitor and
                             row["properties"].get("application.process.id") == str(record.pid) and
                             not row["corked"]), None)

            def recorded_pattern():
                samples = os.pread(pcm.fileno(), 2 * 1024 * 1024, 0)
                values = struct.unpack(f"<{len(samples) // 2}h", samples[:len(samples) // 2 * 2])
                peak = [min(values), max(values)] if values else [0, 0]
                evidence.update(sample_count=len(values), range=peak)
                return peak[0] < -1000 and peak[1] > 1000

            tone = struct.pack("<480h", *([9216] * 120 + [-9216] * 120) * 2) * 100
            try:
                stream = wait_for(receiver_ready, 6, "uncorked receiver on " + monitor_name, record)
                evidence["source_output_index"] = stream["index"]
                subprocess.run(["pacat", "--playback", *sample_format, "--device=" + second],
                               input=tone, env=session.env, timeout=8, check=True)
                # Playback can drain before the recorder receives its last
                # fragments. Read without changing the recorder's file offset.
                wait_for(recorded_pattern, 6, "playback pattern on " + monitor_name, record)
            finally:
                standard["stop"](record)
                log.seek(0)
                evidence["receiver_diagnostic"] = log.read(4096).decode(errors="replace")
                state["playback_monitor_evidence"] = evidence
        state["external_playback_monitor"] = evidence
    finally:
        try:
            if previous is None:
                metadata("default", "-d", "0", default_key)
            else:
                metadata("default", "0", default_key, *previous)
            wait_for(lambda: read_metadata("default", default_key) == previous,
                     5, "restored configured audio sink")
        finally:
            try:
                for module in reversed(modules):
                    pulse("unload-module", module)
            finally:
                if history[0] == "true":
                    set_history("true")


def run_portal_loss(session, results, state):
    service = "org.freedesktop.impl.portal.desktop.wlr"
    raw = output(["busctl", "--user", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                  "org.freedesktop.DBus", "GetConnectionUnixProcessID", "s", service], env=session.env)
    match = re.fullmatch(r"u ([1-9][0-9]*)", raw)
    require(match is not None, "The portal service did not have a known owner")
    pid = int(match[1])
    proc = Path(f"/proc/{pid}")
    require("/seaside-portal-" in (proc / "cgroup").read_text() and
            f"WAYLAND_DISPLAY={session.env['WAYLAND_DISPLAY']}".encode() in
            (proc / "environ").read_bytes().split(b"\0"),
            "The portal service is outside the current trial's owned units")
    with process(session, results / "part7-survivor.log", "seaside-part7-native-client",
                 "Part7 Survivor", "338866", stdin=True) as client:
        target = wait_for(lambda: session.window("Part7 Survivor"), 7, "native survival window", client)
        os.kill(pid, signal.SIGTERM)
        wait_for(lambda: "service fault service=capture-backend" in session.log_text(), 8,
                 "portal loss diagnostic", session.process)
        require(session.ctl("status")["ready"] and session.window("Part7 Survivor")["id"] == target["id"],
                "The owned portal disconnection disrupted the running session")
        command(client, "close")
        require(client.wait(timeout=8) == 0, "The native survivor did not exit")
        state["portal_loss_survived"] = True


def run_capture_soak(session, state):
    shot = session.root / "soak.png"
    args = [state["tools"]["grim"], "-c", shot]
    for _ in range(3):
        output(args, env=session.env)
    before = memory(session.process.pid)
    for _ in range(24):
        output(args, env=session.env)
    after = memory(session.process.pid)
    state["capture_soak"] = {"captures_after_warmup": 24, "before_kb": before, "after_kb": after}
    require(after["fds"] <= before["fds"] + 2, "Capture retained file descriptors after disconnect")
    require(after["VmRSS"] <= before["VmRSS"] + 32 * 1024, "Capture kept over 32 MiB after disconnect")


def run_recorder(session, results, state):
    env = session.env.copy()
    if part7_recorder_prefix:
        prefix = Path(part7_recorder_prefix)
        libraries = (str(prefix / "lib"), str(prefix / "lib/pulseaudio"))
        env["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (*libraries, env.get("LD_LIBRARY_PATH"))))
        recorder, probe = (prefix / "bin" / tool for tool in ("wf-recorder", "ffprobe"))
        require(all(path.is_file() and os.access(path, os.X_OK) for path in (recorder, probe)),
                "The supplied recorder directory lacks executable wf-recorder or ffprobe")
    else:
        recorder, probe = (shutil.which(tool, path=env.get("PATH")) for tool in ("wf-recorder", "ffprobe"))
        if not recorder or not probe:
            state["external_recorder"] = {"status": "blocked", "reason": "wf-recorder or ffprobe absent"}
            return
    log_path = results / "part7-recorder.log"
    recording = results / "part7-monitor.mkv"
    width, height = (int(value) & ~1 for value in state["grim_dimensions"])
    before = memory(session.process.pid)
    with log_path.open("w") as log:
        child = subprocess.Popen([recorder, "--no-dmabuf", "-D", "-r", "30", "-g", f"0,0 {width}x{height}",
                                  "-c", "libx264", "-x", "yuv420p",
                                  "-p", "threads=2", "-f", recording], stdin=subprocess.DEVNULL,
                                 stdout=log, stderr=subprocess.STDOUT, env=env)
        try:
            wait_for(lambda: "Output #0" in log_path.read_text(errors="replace"), 8,
                     "wf-recorder encoding its first frame", child)
            # Capture multiple frames even if the compositor is otherwise idle.
            time.sleep(2)
            child.send_signal(signal.SIGINT)
            require(child.wait(timeout=8) == 0, "wf-recorder failed to finalize its recording")
        finally:
            standard["stop"](child)
    info = json.loads(output([probe, "-v", "error", "-count_frames", "-select_streams", "v:0",
                              "-show_entries", "stream=codec_name,width,height,nb_read_frames",
                              "-of", "json", recording], env=env))
    stream = info["streams"][0]
    require(stream["codec_name"] == "h264" and int(stream["nb_read_frames"]) > 1,
            "ffprobe did not decode multiple H.264 frames")
    require([stream["width"], stream["height"]] == [width, height],
            "The decoded recording does not match the requested even-pixel region")
    after = memory(session.process.pid)
    require(after["VmRSS"] <= before["VmRSS"] + 16 * 1024 and after["fds"] <= before["fds"] + 2,
            "Recording retained memory or file descriptors after the client disconnected")
    state["external_recorder"] = {"status": "pass", **stream, "before_kb": before, "after_kb": after}


def publish_state(results, state, artifacts):
    for name in artifacts:
        upload(name, results / name)
    state["guest_evidence"] = str(results)
    standard["send"]("guest.json", json.dumps(state, indent=2).encode() + b"\n")


def trial():
    tty = os.ttyname(0)
    current = standard["active"]()
    existing = subprocess.run(["pgrep", "-x", "-t", "tty1", "seaside"], capture_output=True,
                              text=True, timeout=3)
    event("connected", f"stdin={tty} active={current}")
    if tty != "/dev/tty1" or current != "tty1" or os.tcgetpgrp(0) != os.getpgrp() or existing.returncode != 1:
        event("done", "An idle foreground shell on active TTY 1 is required", ok=False)
        return
    results = Path.home() / "Projects/seaside/test-results" / f"vm-{base.rsplit('/', 1)[-1][:12]}"
    results.mkdir(mode=0o700, parents=True)
    started = int(time.time())
    artifacts = ("prepare.log", "harness.log", "normal.log", "failure.log", "part7-session.log",
                 "part7-native.log", "part7-x11.log", "part7-monitor.log", "part7-window.log",
                 "part7-cancel.log", "part7-x11-window.log", "part7-grim.png", "part7-portals.log",
                 "part7-pipewire.log", "part7-memory.jsonl", "part7-recorder.log", "part7-monitor.mkv")
    state = {"revision": revision, "kernel": os.uname().release, "renderer": "pixman", "ok": False}
    try:
        env = environment(state)
        with tempfile.TemporaryDirectory(prefix="seaside-vm-source-") as source:
            repo = prepare(Path(source), results, env, state)
            for tool in ("wofi", "grim", "xwayland-satellite", "Xwayland", "wpctl", "pactl", "pacat", "parec"):
                require(state["tools"][tool], f"Required guest trial tool {tool} is unavailable")
            with tempfile.TemporaryDirectory(prefix="seaside-vm-", dir=env["XDG_RUNTIME_DIR"]) as path:
                root = Path(path)
                prepare_xkbcomp(env, root, state)
                # The earlier TTY regression still checks VT and failure handling with the
                # new mandatory portal capabilities. Launch with the same qualified packages.
                old_env = dict(os.environ)
                os.environ.update(env)
                try:
                    standard["tty_trial"](repo, root, results, state)
                finally:
                    os.environ.clear()
                    os.environ.update(old_env)
                session = TrialSession(repo, root / "part7", results, env, state["tools"]["wofi"])
                try:
                    with track_memory(session, results / "part7-memory.jsonl"):
                        session.ready()
                        sys.path.insert(0, str(repo / "tests/session"))
                        from part7_clipboard import run_clipboard

                        def clipboard():
                            state["cross_client_clipboard"] = run_clipboard(session.env, session.focus)

                        checks = {
                            "capture-soak": lambda: run_capture_soak(session, state),
                            "monitor": lambda: run_monitor(session, results, state),
                            "video": lambda: run_video(session, results, state),
                            "recorder": lambda: run_recorder(session, results, state),
                            "x11": lambda: run_x11(session, results, state),
                            "clipboard": clipboard,
                            "portal-loss": lambda: run_portal_loss(session, results, state),
                            "audio": lambda: run_audio(session, state),
                        }
                        outcomes = state["tty_checks"] = {}
                        for name, check in checks.items():
                            event("part7", name)
                            try:
                                session.ctl("workspace", "1")
                                check()
                                outcomes[name] = state["external_recorder"]["status"] if name == "recorder" else "pass"
                            except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
                                outcomes[name] = str(error) or type(error).__name__
                                event("diagnostic", f"{name}: {outcomes[name]}")
                            publish_state(results, state, artifacts)
                            if session.process.poll() is not None:
                                break
                        session.close()
                        failed = [name for name, result in outcomes.items() if result != "pass"]
                        require(not failed, "Part 7 TTY checks failed: " + ", ".join(failed))
                finally:
                    standard["stop"](session.process)
        state["ok"] = True
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError,
            KeyboardInterrupt) as error:
        state["error"] = str(error) or type(error).__name__
        event("diagnostic", state["error"])
    finally:
        try:
            journal = subprocess.run(["journalctl", "--user", "--no-pager", "-o", "cat", "-n", "120",
                                      "--user-unit=seaside-portal-*", f"--since=@{started}"],
                                     capture_output=True, timeout=10, check=False)
            (results / "part7-portals.log").write_bytes(journal.stdout)
        except (OSError, subprocess.SubprocessError) as error:
            state["portal_journal_error"] = str(error)
        publish_state(results, state, artifacts)
        event("done", state.get("error", "Part 7 VM trial passed"), ok=state["ok"])


if __name__ == "__main__":
    trial()
