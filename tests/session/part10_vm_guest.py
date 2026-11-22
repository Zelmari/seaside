"""Check the installed candidate through the real Arch VirtualBox console."""

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time
import tomllib


require = helpers["require"]
checked = helpers["checked"]
fixture = helpers["fixture"]
event = standard["event"]
run_logged = standard["run_logged"]
stop = standard["stop"]
upload = standard["upload"]
wait_for = standard["wait_for"]
REVISION = standard["REVISION"]
BASE = standard["BASE"]
SEASIDE = Path("/usr/bin/seaside")
CONTROL = Path("/usr/bin/seasidectl")
CONFIG_EXAMPLE = Path("/usr/share/doc/seaside/config.example.toml")
PACKAGE_FILES = (
    SEASIDE, CONTROL, Path("/usr/share/seaside/wallpapers/black.png"),
    Path("/usr/share/xdg-desktop-portal/seaside-portals.conf"),
    Path("/etc/xdg/xdg-desktop-portal-wlr/Seaside"),
    CONFIG_EXAMPLE,
    Path("/usr/share/licenses/seaside/LICENSE"),
)
TITLE = "Part 10 Package Pattern"
STARTS = 3
WORKSPACE_CYCLES = 48
MODE_CYCLES = 2
FULLSCREEN_SECONDS = 45


def download(name, path, limit):
    with standard["OPENER"].open(f"{BASE}/{name}", timeout=40) as response:
        data = response.read(limit + 1)
    require(0 < len(data) <= limit, f"Invalid {name} transfer size")
    path.write_bytes(data)


def package_metadata(archive, results, env):
    require(archive.stat().st_size == part10_package["size"] and
            hashlib.sha256(archive.read_bytes()).hexdigest() == part10_package["sha256"],
            "The delivered archive differs from the host candidate")
    contents = {}
    for entry, evidence in ((".PKGINFO", "part10-package-info.txt"),
                            (".BUILDINFO", "part10-build-info.txt")):
        metadata = checked(["/usr/bin/bsdtar", "-xOf", archive, entry], env=env)
        (results / evidence).write_bytes(metadata)
        fields = dict(line.split(" = ", 1) for line in metadata.decode().splitlines() if " = " in line)
        require(fields.get("pkgname") == "seaside" and fields.get("arch", fields.get("pkgarch")) == "x86_64",
                f"The delivered package has unexpected {entry} identity")
        version = fields.get("pkgver", "")
        require(version.endswith(f".g{REVISION}-1"),
                f"Candidate version {version!r} does not identify source revision {REVISION}")
        contents[entry] = version
    require(contents[".PKGINFO"] == contents[".BUILDINFO"], "Candidate package version metadata disagrees")
    return contents[".PKGINFO"]


def package_registered(env):
    return subprocess.run(["/usr/bin/pacman", "-Qq", "seaside"], env=env, capture_output=True,
                          check=False, timeout=8).returncode == 0


def pacman(args, log, env):
    status = run_logged(["/usr/bin/sudo", "-n", "/usr/bin/pacman", *map(str, args)], log.parent, log,
                        200, env=env)
    require(status == 0, f"Disposable guest pacman transaction failed with exit {status}. See {log.name}")


def verify_installed(archive, version, results, env):
    installed = checked(["/usr/bin/pacman", "-Q", "seaside"], env=env).decode().strip()
    require(installed == f"seaside {version}", f"Installed package differs from the candidate: {installed}")
    manifest = checked(["/usr/bin/pacman", "-Ql", "seaside"], env=env)
    (results / "part10-package-files.txt").write_bytes(manifest)
    for path in PACKAGE_FILES:
        owner = checked(["/usr/bin/pacman", "-Qoq", path], env=env).decode().strip()
        packaged = checked(["/usr/bin/bsdtar", "-xOf", archive, path.relative_to("/")], env=env)
        require(owner == "seaside" and path.read_bytes() == packaged,
                f"{path} differs from the pacman-owned candidate file")
    owner = checked(["/usr/bin/pacman", "-Qoq", "/usr/bin/wofi"], env=env).decode().strip()
    require(owner == "wofi", "The installed chooser does not belong to the Wofi package")


def test_environment(root):
    env = helpers["environment"]()
    for key in ("XDG_CONFIG_DIRS", "XDG_DATA_DIRS", "XDG_CURRENT_DESKTOP", "LD_PRELOAD", "LD_LIBRARY_PATH"):
        env.pop(key, None)
    # Runtime behavior must come from the installed files. Extra pkg-config
    # paths are allowed for building fixture clients only.
    env.update(PATH="/usr/bin", LC_ALL="C", XDG_CONFIG_HOME=str(root / "config"),
               XDG_STATE_HOME=str(root / "state"), XDG_DATA_HOME=str(root / "data"),
               XDG_CACHE_HOME=str(root / "cache"))
    return env


def prepare_fixtures(source, results, env):
    bundle, repo = source / "revision.bundle", source / "checkout"
    log = results / "prepare.log"
    download("bundle", bundle, 64 * 1024 * 1024)
    event("update", f"isolated fixture checkout {REVISION[:12]}")
    require(run_logged(["git", "clone", "--quiet", str(bundle), str(repo)], source, log, 45, env=env) == 0,
            "Could not clone the candidate's fixture sources")
    git = standard["git"]
    require(git(repo, "rev-parse", "HEAD") == REVISION and not git(repo, "status", "--porcelain"),
            "The fixture checkout differs from the candidate's exact Git revision")
    build_env = env.copy()
    if part10_pkg_config_path:
        require(Path(part10_pkg_config_path).is_dir(), "The guest fixture pkg-config directory is unavailable")
        build_env["PKG_CONFIG_PATH"] = part10_pkg_config_path
    event("build", "compile portal, Wayland and X11 test clients")
    require(run_logged(["meson", "setup", "build", "--buildtype=debug"], repo, log, 120,
                       env=build_env) == 0,
            "Fixture build could not configure against the guest toolchain. See prepare.log")
    targets = ["seaside-part7-native-client", "seaside-portal-stream", "seaside-part7-x11-client"]
    require(run_logged(["meson", "compile", "-C", "build", "-j2", *targets], repo, log, 150,
                       env=build_env) == 0,
            "Guest fixture build failed. See prepare.log")
    return repo


def owned_unix_sockets(process):
    proc = Path(f"/proc/{process.pid}")
    inodes = set()
    for entry in (proc / "fd").iterdir():
        try:
            match = re.fullmatch(r"socket:\[(\d+)\]", os.readlink(entry))
        except FileNotFoundError:
            continue
        if match:
            inodes.add(match[1])
    rows = (proc / "net/unix").read_text().splitlines()[1:]
    return [fields[7] for line in rows if len(fields := line.split(maxsplit=7)) == 8 and fields[6] in inodes]


def startup(env, log):
    with log.open("ab") as output:
        return subprocess.Popen([SEASIDE], env=env, stdout=output, stderr=subprocess.STDOUT)


def config_home(env):
    return Path(env["XDG_CONFIG_HOME"]) / "seaside/config.toml"


class InstalledSession(helpers["Session"]):
    def __init__(self, repo, results, env, label="part10"):
        self.repo, self.results, self.env = repo, results, env.copy()
        self.root = Path(env["XDG_CONFIG_HOME"]).parent
        self.state_dir = Path(env["XDG_STATE_HOME"])
        self.config = config_home(env)
        self.log = results / f"{label}-session.log"
        self.label = label
        self.chooser = Path("/usr/bin/wofi")

    def __enter__(self):
        self.process = startup(self.env, self.log)
        try:
            wait_for(lambda: re.search(r"^seaside READY cursor-plane=([1-9][0-9]*)$",
                                       self.log.read_text(errors="replace"), re.MULTILINE),
                     25, "the installed session's mandatory startup gate", self.process)
            require(Path(f"/proc/{self.process.pid}/exe").resolve() == SEASIDE,
                    "The running compositor is not the package-owned executable")
            runtime = Path(self.env["XDG_RUNTIME_DIR"])
            sockets = owned_unix_sockets(self.process)
            paths = [Path(item) for item in sockets]
            ipc = [path for path in paths if path.parent == runtime / "seaside" and path.suffix == ".sock"]
            require(len(ipc) == 1 and runtime / ipc[0].stem in paths,
                    "The installed process does not own matching control and Wayland sockets")
            self.env.update(WAYLAND_DISPLAY=ipc[0].stem, SEASIDE_SOCKET=str(ipc[0]))
            displays = {match[1] for path in sockets if
                        (match := re.fullmatch(r"@?/tmp/\.X11-unix/X([0-9]+)", path))}
            require(len(displays) == 1, "The installed process does not own an unambiguous X11 socket")
            self.env["DISPLAY"] = f":{displays.pop()}"
            require(self.ctl("status")["ready"], "The packaged CLI does not report a ready session")
            return self
        except BaseException:
            stop(self.process)
            raise

    def ctl(self, *args):
        reply = json.loads(checked([CONTROL, *args], env=self.env))
        require(reply["ok"] is True and reply["v"] == 1, f"Invalid packaged seasidectl response: {reply}")
        return reply["result"]

    def replace(self, contents):
        temporary = self.config.with_suffix(".tmp")
        temporary.write_bytes(contents)
        temporary.replace(self.config)


def first_use(session, temporary, state):
    expected_config = session.repo / "Specification/SEASIDE_CONFIG_EXAMPLE.toml"
    generated = session.config.read_bytes()
    require(generated.rstrip(b"\n") == expected_config.read_bytes().rstrip(b"\n"),
            "First-use configuration differs from the committed example")
    require(generated.rstrip(b"\n") == CONFIG_EXAMPLE.read_bytes().rstrip(b"\n"),
            "First-use configuration differs from the package's installed example")
    require(not tomllib.loads(generated.decode())["autostart"]["commands"] and
            not session.ctl("windows")["windows"] and not standard["foot_started"](session.process),
            "Empty autostart unexpectedly opened a window")
    (session.results / "part10-first-use.toml").write_bytes(generated)
    output = session.focused_output()
    require(output and output["hardware_cursor"], "No packaged session output exposes a hardware cursor")
    x, y = output["position"]["x"], output["position"]["y"]
    require(session.pixel(x + 1, y + 1) == (0, 0, 0), "First-use wallpaper is not black")
    session.grab(output, "first-use")
    render = session.diagnostics(output)
    plane = re.search(r"^seaside READY cursor-plane=([1-9][0-9]*)$",
                      session.log.read_text(errors="replace"), re.MULTILINE)
    state["first_use"] = {"config": "matches canonical example", "default_autostart": "empty",
                          "wallpaper": "black", "connector": output["connector"], "mode": output["mode"],
                          "renderer": render["renderer"], "backend": render["backend"],
                          "cursor_plane": int(plane[1])}

    event("open-foot", "launch the packaged default terminal shortcut")
    wait_for(lambda: standard["foot_started"](session.process), 12, "foot launched from the clean config",
             session.process)
    session.wait(lambda: any(window["mapped"] and "foot" in window["app_id"]
                             for window in session.ctl("windows")["windows"]), "a mapped foot window")
    runtime = Path(session.env["XDG_RUNTIME_DIR"])
    children = Path(f"/proc/{session.process.pid}/task/{session.process.pid}/children")
    for pid in children.read_text().split():
        process = Path("/proc") / pid
        if (process / "exe").resolve() == Path("/usr/bin/foot"):
            values = dict(record.split(b"=", 1) for record in (process / "environ").read_bytes().split(b"\0")
                          if b"=" in record)
            require(values.get(b"DISPLAY") == session.env["DISPLAY"].encode(),
                    "The installed terminal inherited a different X11 display")
            auth = Path(os.fsdecode(values.get(b"XAUTHORITY", b"")))
            require(auth.parent == runtime and auth.name.startswith("seaside-xauth-") and auth.is_file(),
                    "The packaged terminal did not inherit the session's private X11 authority path")
            session.env["XAUTHORITY"] = str(auth)
            break
    require("XAUTHORITY" in session.env, "The default terminal did not run the installed foot executable")
    for name in ("before", "after"):
        if name == "after":
            event("to-tty2")
            wait_for(lambda: standard["active"]() == "tty2", 12, "TTY 2", session.process)
            event("to-tty1")
            wait_for(lambda: standard["active"]() == "tty1", 12, "TTY 1", session.process)
            wait_for(lambda: session.log.read_text(errors="replace").count("presented with cursor plane") >= 2,
                     12, "hardware cursor presentation after VT return", session.process)
        event(name, "type and capture in the package-launched terminal", marker_dir=str(temporary))
        marker = temporary / f"{name}.pid"
        wait_for(lambda: marker.is_file() and marker.stat().st_size, 14, f"{name} capture", session.process)
        state[name] = standard["read_capture"](temporary, name)
        upload(f"{name}.png", temporary / f"{name}.png")
    require(state["before"]["shell_pid"] == state["after"]["shell_pid"], "foot shell changed across VT return")
    state["vt_round_trip"] = True
    return generated


def recover_config(session, generated, state):
    def config():
        return session.ctl("status")["config"]

    initial = config()
    session.replace(b"[window\n")
    session.wait(lambda: config()["error"], "a reported bad package configuration")
    require(config()["source"] == initial["source"],
            "A rejected configuration replaced the active source")
    session.replace(generated)
    session.wait(lambda: not config()["error"], "the repaired configuration")
    state["config_recovery"] = "invalid edit rejected and accepted bytes restored"


def media_keys(session, state):
    def route(phase, detail, **fields):
        event("part10-input" if phase == "part7-input" else phase, detail, **fields)

    compatibility["event"] = route
    compatibility["run_audio"](session, state)


@contextmanager
def integration_events(session):
    path = session.results / "part10-events.log"
    with path.open("wb") as stream:
        process = subprocess.Popen([CONTROL, "events"], env=session.env, stdin=subprocess.DEVNULL,
                                   stdout=stream, stderr=subprocess.STDOUT)
        try:
            def records():
                return [json.loads(line) for line in path.read_bytes().splitlines(keepends=True)
                        if line.endswith(b"\n")]

            wait_for(lambda: records() and records()[0]["result"]["subscribed"], 5,
                     "an installed CLI event subscription", process)
            yield records
        finally:
            stop(process)


def overlay_chooser(session):
    previous_windows = session.ctl("windows")["windows"]

    def chooser_focused():
        if session.ctl("focus")["window_id"] is not None:
            return False
        for exe in Path("/proc").glob("[0-9]*/exe"):
            try:
                if exe.parent.stat().st_uid != os.getuid() or exe.readlink() != session.chooser:
                    continue
            except OSError:
                continue
            require(session.ctl("windows")["windows"] == previous_windows,
                    "The share picker changed the list or state of application windows")
            return True
        return False

    return chooser_focused


def verify_chooser_cancel(session):
    chooser_focused = overlay_chooser(session)
    log = session.results / "part10-portal-cancel.log"
    with fixture(session, log, "seaside-portal-stream", "window", "cancel") as client:
        wait_for(chooser_focused, 10, "the package's focused portal chooser", client)
        event("part10-input", "cancel the package's window chooser", key="cancel")
        require(client.wait(timeout=12) == 0 and "cancelled" in log.read_text(errors="replace"),
                "The installed chooser did not cancel the portal request")


def portal_recovery_and_capture(session, generated, state):
    # Configure only the compositor's aspect policy. The chooser and portal
    # routing remain the system files verified against the candidate archive.
    preserve = b'fullscreen_aspect = "preserve"'
    require(generated.count(preserve) == 1, "Expected one canonical fullscreen aspect default")
    session.replace(generated.replace(preserve, b'fullscreen_aspect = "stretch"'))
    snapshot = session.state_dir / "seaside/last-good-config.toml"
    session.wait(lambda: snapshot.is_file() and snapshot.read_bytes() == session.config.read_bytes(),
                 "the installed config transaction to persist the fullscreen test policy")
    with fixture(session, session.results / "part10-pattern.log", "seaside-part7-native-client", TITLE,
                 "64482c", "--fixed-size", interactive=True) as pattern:
        target = wait_for(lambda: session.window(TITLE), 8, "the packaged session's colored window", pattern)
        with integration_events(session) as records:
            transitions = {}
            for name, service in (("capture-backend", "org.freedesktop.impl.portal.desktop.wlr"),
                                  ("portal-frontend", "org.freedesktop.portal.Desktop")):
                recovery["stop_owned_portal"](session, service)
                down = recovery["expect_integration"](records, "failed", name)
                up = recovery["expect_integration"](records, "recovered", name)
                require(down["seq"] < up["seq"] and session.ctl("status")["ready"] and
                        session.window(TITLE)["id"] == target["id"],
                        f"Installed session did not preserve client identity across {name} recovery")
                transitions[name] = {"failed_seq": down["seq"], "recovered_seq": up["seq"]}
        verify_chooser_cancel(session)
        session.wait(lambda: session.ctl("focus")["window_id"] == target["id"],
                     "focus return after chooser cancellation")
        require(session.ctl("window", "toggle-fullscreen")["changed"], "The package did not enter fullscreen")
        mode = session.focused_output()["mode"]
        for source, size in (("monitor", (mode["width"], mode["height"])), ("window", (264, 176))):
            helpers["run_portal"](session, pattern, source, size, state,
                                  chooser_ready=overlay_chooser(session))
            require(state["fullscreen_portal"][source]["status"] == "pass" and
                    session.window(TITLE)["id"] == target["id"],
                    f"Post-recovery {source} share did not retain the chosen content")
            session.wait(lambda: session.ctl("focus")["window_id"] == target["id"],
                         f"focus return after {source} selection")
        require(session.ctl("window", "toggle-fullscreen")["changed"], "The package did not leave fullscreen")
        pattern.stdin.write("close\n")
        pattern.stdin.flush()
        require(pattern.wait(timeout=8) == 0, "The portal target could not shut down")
        state["portal_recovery"] = {"service_transitions": transitions, "chooser_cancel": True}
    session.replace(generated)
    session.wait(lambda: snapshot.read_bytes() == generated, "restored defaults after the capture checks")


def satellite_restart(session, state):
    title = compatibility["X11_TITLE"]

    def launch(log):
        return fixture(session, session.results / log, "seaside-part7-x11-client", interactive=True)

    def note(pid):
        return str(pid) in children.read_text().split()

    require("xwayland-satellite started" not in session.log.read_text(),
            "The packaged session started Xwayland without an X11 client")
    children = Path(f"/proc/{session.process.pid}/task/{session.process.pid}/children")
    with launch("part10-x11.log") as client:
        previous_window = wait_for(lambda: session.window(title), 12,
                                   "lazy X11 startup via installed packages", client)
        found = re.search(r"xwayland-satellite started pid=([0-9]+)", session.log.read_text())
        require(found is not None, "The package did not log an owned satellite startup")
        previous = int(found[1])
        descriptor = os.pidfd_open(previous)
        try:
            require(note(previous), "Refusing to stop an Xwayland satellite outside this trial")
            signal.pidfd_send_signal(descriptor, signal.SIGTERM)
        finally:
            os.close(descriptor)
        wait_for(lambda: not note(previous), 8, "reaping the terminated Xwayland satellite", session.process)
    session.wait(lambda: not session.window(title), "unmapping the original X11 window")
    with launch("part10-x11-restart.log") as client:
        def restarted_window():
            window = session.window(title)
            return window and window["id"] != previous_window["id"]

        wait_for(restarted_window, 12, "a new X11 client after satellite restart", client)
        pids = re.findall(r"xwayland-satellite started pid=([0-9]+)", session.log.read_text())
        require(len(pids) == 2 and pids[0] != pids[1], "Xwayland did not restart exactly once")
        client.stdin.write("quit\n")
        client.stdin.flush()
        require(client.wait(timeout=8) == 0, "The restarted X11 client did not exit")
    state["satellite_restart"] = {"processes": pids, "status": "pass"}


def change_modes(session, generated):
    output = session.focused_output()
    original = output["mode"]
    original_size = (original["width"], original["height"])
    modes = {tuple(map(int, size.split("x")))
             for file in Path("/sys/class/drm").glob(f"card*-{output['connector']}/modes")
             for size in file.read_text().split() if re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", size)}
    if not (alternates := modes - {original_size}):
        return {"status": "skip", "reason": "Only the active guest mode was advertised"}
    alternate = min(alternates)
    default = b'resolution = "auto"\n'
    require(generated.count(default) == 1, "Expected the canonical monitor resolution default")
    changed = generated.replace(default, f'resolution = "{alternate[0]}x{alternate[1]}"\n'.encode())
    cursor_log = f"output {output['connector']} presented with cursor plane "

    def apply(contents, size):
        previous = session.log.read_text(errors="replace").count(cursor_log)
        session.replace(contents)

        def matches():
            observed = session.focused_output()
            mode = observed["mode"]
            return (observed["id"] == output["id"] and observed["hardware_cursor"] and
                    (mode["width"], mode["height"]) == size and
                    session.log.read_text(errors="replace").count(cursor_log) > previous)

        session.wait(matches, f"real guest modeset and cursor restoration for {size}")

    for _ in range(MODE_CYCLES):
        apply(changed, alternate)
        apply(generated, original_size)
    return {"status": "pass", "cycles": MODE_CYCLES, "alternate": alternate, "restored": original_size}


def stability(session, generated, state):
    def present_after(previous):
        return session.diagnostics(session.focused_output())["presented_frames"] > previous

    before = compatibility["memory"](session.process.pid)
    regressions = {"mode_changes": change_modes(session, generated)}
    first = time.monotonic()
    with fixture(session, session.results / "part10-stability-client.log", "seaside-part7-native-client",
                 "Part 10 Stability Pattern", "112244", interactive=True) as pattern:
        mapped = wait_for(lambda: session.window("Part 10 Stability Pattern"), 8,
                          "the installed stability workload", pattern)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as slow:
            slow.settimeout(5)
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.connect(session.env["SEASIDE_SOCKET"])
            slow.sendall(b'{"v":1,"id":1,"method":"events.subscribe","params":{}}\n')
            with slow.makefile("rb", buffering=0) as stream:
                ack = json.loads(stream.readline(4096))
                require(ack["ok"] and ack["result"]["subscribed"], "The slow client could not subscribe")

            # Leave this subscriber unread while a responsive CLI client
            # toggles visibility and the fixture submits new Wayland buffers.
            for cycle in range(WORKSPACE_CYCLES):
                require(session.ctl("workspace", "2")["changed"] and
                        session.ctl("workspace", "1")["changed"], "A workspace cycle did not apply")
                if cycle % 8 == 0:
                    old = session.diagnostics(session.focused_output())["presented_frames"]
                    pattern.stdin.write(f"{cycle:06x}\n")
                    pattern.stdin.flush()
                    session.wait(lambda: present_after(old), "a presentation under slow IPC load")
                    require(session.ctl("status")["ready"] and
                            session.window("Part 10 Stability Pattern")["id"] == mapped["id"],
                            "Slow IPC churn disrupted the mapped client")
        require(session.ctl("window", "toggle-fullscreen")["changed"], "The stability client did not fullscreen")
        deadline = time.monotonic() + FULLSCREEN_SECONDS
        frames = 0
        while time.monotonic() < deadline:
            old = session.diagnostics(session.focused_output())["presented_frames"]
            pattern.stdin.write(f"{frames % 0xffffff:06x}\n")
            pattern.stdin.flush()
            session.wait(lambda: present_after(old), "a fullscreen workload presentation")
            frames += 1
            time.sleep(.15)
        require(session.ctl("window", "toggle-fullscreen")["changed"], "The stability client stayed fullscreen")
        pattern.stdin.write("close\n")
        pattern.stdin.flush()
        require(pattern.wait(timeout=8) == 0, "The stability fixture did not stop")
    session.wait(lambda: not session.window("Part 10 Stability Pattern"), "removal of the stability client")
    after = compatibility["memory"](session.process.pid)
    require(after["fds"] <= before["fds"] + 2 and
            after["VmRSS"] <= before["VmRSS"] + 32 * 1024,
            "The bounded stability cycle retained more descriptors or memory than expected")
    regressions.update(status="pass", workspace_cycles=WORKSPACE_CYCLES, fullscreen_frames=frames,
                       fullscreen_seconds=FULLSCREEN_SECONDS,
                       seconds=round(time.monotonic() - first, 2), before=before, after=after)
    state["stability"] = regressions


def starts_and_failure(repo, results, env, state):
    log = results / "part10-restarts.log"
    for index in range(STARTS):
        # Separate logs avoid selecting an earlier session's readiness marker.
        session = InstalledSession(repo, results, env, label=f"restart-{index}")
        try:
            with session:
                require(not session.ctl("windows")["windows"], "A restart unexpectedly launched a client")
                session.close()
                require(not (Path(env["XDG_RUNTIME_DIR"]) / session.env["WAYLAND_DISPLAY"]).exists(),
                        "A stopped installed session retained its Wayland socket")
        finally:
            if session.log.is_file():
                with log.open("ab") as output:
                    output.write(session.log.read_bytes())
    state["repeated_starts"] = STARTS

    failure_log = results / "failure.log"
    failure = startup(dict(env, PIPEWIRE_REMOTE="seaside-part10-missing"), failure_log)
    try:
        code = failure.wait(timeout=25)
        output = failure_log.read_text(errors="replace")
        error = re.search(r"^seaside FAIL service=pipewire reason=missing\((-\d+)\)", output, re.MULTILINE)
        require(code == 1 and error is not None and "portal cleanup incomplete" not in output and
                "seaside READY" not in output and standard["active"]() == "tty1" and
                os.tcgetpgrp(0) == os.getpgrp(), "Installed strict startup did not fail cleanly to the same TTY")
        state["forced_failure"] = {"exit": code, "line": error[0]}
    finally:
        stop(failure)


def missing_audio_device(repo, runtime, results, state):
    env = test_environment(runtime / "without-audio-device")
    private_log = results / "part10-device-pipewire.log"
    with recovery["private_pipewire"](runtime / "pw", env, private_log) as server:
        env["PIPEWIRE_REMOTE"] = str(server.socket)
        with InstalledSession(repo, results, env, label="part10-device") as session:
            probe = subprocess.run(["/usr/bin/wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"],
                                   env=session.env, timeout=7, text=True, capture_output=True, check=False)
            # WirePlumber can report an unavailable default node with exit status zero.
            require(not probe.stdout.strip() and "Translate ID error" in probe.stderr,
                    f"Could not establish the private PipeWire server has no default sink: {probe!r}")
            event("part10-input", "try a package media key without a playback device", key="up")
            diagnostic = session.wait(
                lambda: re.search(r"^seaside: command pid=\d+ wait-status=[1-9][0-9]*$",
                                  session.log.read_text(errors="replace"), re.MULTILINE),
                "the missing-device external command failure")
            require(session.ctl("status")["ready"], "Missing playback device disrupted the compositor")
            session.close()
            state["missing_audio_device"] = {"status": "pass", "diagnostic": diagnostic[0],
                                             "probe": probe.stderr.strip()}


def installed_trial(source, runtime, results, state):
    env = test_environment(runtime)
    archive = source / part10_package["name"]
    download("package", archive, 64 * 1024 * 1024)
    version = package_metadata(archive, results, env)
    state["candidate"] = dict(part10_package, version=version)
    installing = not part10_package["preinstalled"]
    package_log = results / "part10-package.log"
    if installing:
        require(not package_registered(env), "The disposable guest already has a Seaside package registered")
    config = config_home(env)
    require(not config.exists(), "The first-use config directory is not clean")
    try:
        if installing:
            event("package", "pacman install on the disposable guest")
            pacman(["-U", "--noconfirm", archive], package_log, env)
        verify_installed(archive, version, results, env)
        packages = checked(["/usr/bin/pacman", "-Q", "systemd", "polkit", "rtkit", "mesa", "wlroots0.20",
                            "wayland-protocols", "pipewire", "wireplumber", "xdg-desktop-portal",
                            "xdg-desktop-portal-wlr", "wofi", "foot", "xwayland-satellite"], env=env)
        state["guest_packages"] = dict(line.split(maxsplit=1) for line in packages.decode().splitlines())
        require(not config.exists(), "Package installation created a user configuration")
        repo = prepare_fixtures(source, results, env)
        with InstalledSession(repo, results, env) as session:
            generated = first_use(session, runtime, state)
            event("part10", "verify live configuration recovery")
            recover_config(session, generated, state)
            event("part10", "verify packaged media keys and playback")
            media_keys(session, state)
            event("part10", "verify portal recovery and installed capture routes")
            portal_recovery_and_capture(session, generated, state)
            event("part10", "verify lazy X11 startup and satellite restart")
            satellite_restart(session, state)
            event("part10", "exercise mode changes, slow IPC and fullscreen stability")
            stability(session, generated, state)
            session.close()
        event("part10", "verify restart paths and missing playback device")
        starts_and_failure(repo, results, env, state)
        missing_audio_device(repo, runtime, results, state)
        if installing:
            event("package", "reinstalling the candidate with the generated config still present")
            pacman(["-U", "--noconfirm", archive], package_log, env)
            verify_installed(archive, version, results, env)
            state["package_reinstall"] = "candidate matched and user configuration preserved"
        require(config.read_bytes() == generated, "The session or package reinstall changed the user config")
    finally:
        if installing and package_registered(env):
            saved = config.read_bytes() if config.exists() else None
            pacman(["-R", "--noconfirm", "seaside"], package_log, env)
            require(not package_registered(env) and not SEASIDE.exists(),
                    "The disposable guest kept an installed Seaside binary after removal")
            require((config.read_bytes() if config.exists() else None) == saved,
                    "Package removal changed the user configuration")
            state["package_removal"] = "removed; user configuration preserved"


def trial():
    tty, active = os.ttyname(0), standard["active"]()
    existing = subprocess.run(["pgrep", "-x", "-t", "tty1", "seaside"], capture_output=True, timeout=3)
    event("connected", f"stdin={tty} active={active}")
    if tty != "/dev/tty1" or active != "tty1" or os.tcgetpgrp(0) != os.getpgrp() or existing.returncode != 1:
        event("done", "An idle foreground shell on active TTY 1 is required", ok=False)
        return
    directory = Path.home() / ".local/state/seaside-vm" / f"package-{BASE.rsplit('/', 1)[-1][:12]}"
    directory.mkdir(mode=0o700, parents=True)
    state = {"revision": REVISION, "kernel": os.uname().release,
             "renderer_setting": part10_renderer, "guest_evidence": str(directory), "ok": False}
    try:
        with (tempfile.TemporaryDirectory(prefix="seaside-package-source-") as source,
              tempfile.TemporaryDirectory(prefix="seaside-vm-", dir=os.environ["XDG_RUNTIME_DIR"]) as runtime):
            installed_trial(Path(source), Path(runtime), directory, state)
        state["ok"] = True
    except (OSError, RuntimeError, KeyError, ValueError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        state["error"] = str(error) or type(error).__name__
        event("diagnostic", state["error"])
    finally:
        for name in ("prepare.log", "failure.log", *part10_artifacts):
            upload(name, directory / name)
        standard["send"]("guest.json", json.dumps(state, indent=2).encode() + b"\n")
        event("done", state.get("error", "Part 10 installed-package trial passed"), ok=state["ok"])


if __name__ == "__main__":
    trial()
