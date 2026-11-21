"""Exercise Part 8 using an isolated checkout and the production TTY entrypoint."""

from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import tempfile
import time


event = standard["event"]
wait_for = standard["wait_for"]
upload = standard["upload"]
run_logged = standard["run_logged"]
stop = standard["stop"]
revision = standard["REVISION"]
base = standard["BASE"]
TITLE = "Part 8 Fullscreen Pattern"
PIXEL = (100, 72, 44)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def checked(args, *, env):
    result = subprocess.run(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=8, check=False)
    require(result.returncode == 0,
            f"{shlex.join(map(str, args))}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def environment():
    env = os.environ.copy()
    for key in ("PIPEWIRE_REMOTE", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SEASIDE_SOCKET",
                "DISPLAY", "XAUTHORITY", "WLR_RENDERER"):
        env.pop(key, None)
    if part8_renderer != "auto":
        env["WLR_RENDERER"] = part8_renderer
    if part8_pkg_config_path:
        require(Path(part8_pkg_config_path).is_dir(), "The guest pkg-config directory is unavailable")
        env["PKG_CONFIG_PATH"] = os.pathsep.join(filter(None, (
            part8_pkg_config_path, env.get("PKG_CONFIG_PATH"))))
    if part8_runtime_prefix:
        root = Path(part8_runtime_prefix)
        require((root / "bin").is_dir(), "The guest runtime prefix is unavailable")
        for key, path in (("PATH", root / "bin"), ("LD_LIBRARY_PATH", root / "lib")):
            env[key] = os.pathsep.join(filter(None, (str(path), env.get(key))))
    return env


def prepare(source, results, state, env):
    limit = 64 * 1024 * 1024
    bundle = source / "revision.bundle"
    with standard["OPENER"].open(base + "/bundle", timeout=30) as response:
        bundle.write_bytes(response.read(limit + 1))
    require(0 < bundle.stat().st_size <= limit, "Invalid delivered Git bundle")
    repo = source / "checkout"
    log = results / "prepare.log"
    event("update", f"isolated checkout of {revision[:12]}")
    require(not run_logged(["git", "clone", "--quiet", str(bundle), str(repo)], source, log, 45),
            "Could not clone the supplied revision bundle")
    git = standard["git"]
    require(git(repo, "rev-parse", "HEAD") == revision and not git(repo, "status", "--porcelain"),
            "The isolated checkout does not match the requested revision")

    configured = ["wlroots-0.20", "wayland-protocols", "libdrm", "libpipewire-0.3"]
    versions = checked(["pkg-config", "--modversion", *configured], env=env).decode().splitlines()
    state["dependencies"] = dict(zip(configured, versions, strict=True))
    packages = ["virtualbox-guest-utils", "mesa", "pipewire", "xdg-desktop-portal",
                "xdg-desktop-portal-wlr", "xwayland-satellite", "xorg-xwayland", "grim", "wofi"]
    installed = subprocess.run(["pacman", "-Q", *packages], env=env, capture_output=True,
                               text=True, timeout=8, check=False)
    state["installed_packages"] = dict(line.split(maxsplit=1) for line in installed.stdout.splitlines())
    if standard["MODE"] == "tty":
        state["software_checks"] = "not-run"
        event("build", revision[:12])
        require(not run_logged(["meson", "setup", "build", "--buildtype=debug"], repo, log, 120,
                               env=env), "Guest configure failed")
        targets = ["seaside", "seasidectl", "seaside-part7-native-client", "seaside-portal-stream"]
        require(not run_logged(["meson", "compile", "-C", "build", "-j2", *targets], repo, log,
                               420, env=env), "Guest compositor or fixture build failed")
    else:
        event("harness", revision[:12])
        reports = results / "harness"
        suites = ["unit", "integration", "runner", "hardware", "part8"]
        command = ["tools/run-tests", "--jobs", "2", "--timeout", "420", "--results-dir", str(reports)]
        for suite in suites:
            command.extend(("--suite", suite))
        status = run_logged(command, repo, results / "harness.log", 810, env=env)
        summaries = list(reports.glob("*/summary.json"))
        require(len(summaries) == 1, "The guest harness did not produce a unique summary")
        summary = summaries[0]
        for path in (summary, summary.with_suffix(".txt"), summary.with_name("baseline.json")):
            upload(path.name, path)
        report = json.loads(summary.read_text())
        state["software_checks"] = report["software_status"]
        part8 = [row for row in report["tests"] if row["part"] == 8]
        real_portals = [row for row in report["tests"] if row["test_id"].endswith("part3-real-portals")]
        require(status == 0 and state["software_checks"] == "pass" and part8 and
                all(row["status"] == "pass" for row in part8) and len(real_portals) == 1 and
                real_portals[0]["status"] == "pass",
                f"Guest regression, Part 8, or real portal test failed ({status})")
    return repo


class Session:
    def __init__(self, repo, root, results, env):
        self.repo, self.results = repo, results
        self.log = results / "part8-session.log"
        self.state_dir = root / "state"
        self.env = dict(env, XDG_CONFIG_HOME=str(root / "config"), XDG_STATE_HOME=str(self.state_dir))
        self.config = root / "config/seaside/config.toml"
        self.config.parent.mkdir(parents=True)
        self.marker = root / "environment.txt"
        marker = shlex.quote(str(self.marker))
        command = (f"umask 077; printf '%s\\n' \"$WAYLAND_DISPLAY\" \"$SEASIDE_SOCKET\" > {marker}.tmp"
                   f" && mv -- {marker}.tmp {marker}")
        self.base_config = ("[window]\ntiling = false\n[keybinds]\n"
                            '"Super+N" = "night_light toggle"\n'
                            f"[autostart]\ncommands = [{json.dumps(command)}]\n")
        self.chooser = shutil.which("wofi", path=self.env.get("PATH"))
        if self.chooser:
            portal = root / "config/xdg-desktop-portal-wlr/Seaside"
            portal.parent.mkdir()
            chooser = root / "chooser"
            invocation = [self.chooser, "-d", "-n", "--no-custom-entry", f"--cache-file={root / 'wofi-cache'}"]
            if part8_runtime_prefix:
                invocation = ["/usr/bin/env", f"LD_LIBRARY_PATH={Path(part8_runtime_prefix) / 'lib'}",
                              *invocation]
            chooser.write_text("#!/bin/sh\nexec " + shlex.join(invocation) + "\n")
            chooser.chmod(0o700)
            portal.write_text(f"[screencast]\nchooser_type=dmenu\nchooser_cmd={chooser}\n")
        self.edit()

    def edit(self, *settings):
        staged = self.config.with_suffix(".editor")
        staged.write_text(self.base_config + "\n".join(settings))
        staged.replace(self.config)

    def __enter__(self):
        self.process = standard["start_seaside"](self.repo, self.env, self.log)
        try:
            wait_for(self.marker.is_file, 25, "production readiness and test autostart", self.process)
            wayland, socket = self.marker.read_text().splitlines()
            require(wayland and socket, "The TTY session did not provide its connection sockets")
            self.env.update(WAYLAND_DISPLAY=wayland, SEASIDE_SOCKET=socket)
            require(self.ctl("status")["ready"], "The Part 8 session is not ready over IPC")
            return self
        except BaseException:
            stop(self.process)
            raise

    def __exit__(self, *_):
        stop(self.process)

    def ctl(self, *args):
        reply = json.loads(checked([self.repo / "build/seasidectl", *args], env=self.env))
        require(reply["ok"] is True and reply["v"] == 1, f"Invalid seasidectl reply: {reply}")
        return reply["result"]

    def focused_output(self):
        return next((row for row in self.ctl("outputs")["outputs"] if row["focused"] and row["enabled"]),
                    None)

    def window(self, title):
        return next((row for row in self.ctl("windows")["windows"] if row["mapped"] and
                     row["title"] == title), None)

    def diagnostics(self, output):
        data = self.ctl("debug", "render")
        return next(row for row in data["outputs"] if row["output_id"] == output["id"])

    def grab(self, output, name):
        path = self.results / f"part8-{name}.png"
        require(not checked(["grim", "-s", "1", "-o", output["connector"], path], env=self.env),
                "grim wrote unexpected data to standard output")
        with path.open("rb") as stream:
            header = stream.read(24)
        require(len(header) == 24 and header[:8] == standard["PNG_HEADER"] and
                header[12:16] == b"IHDR", f"Malformed capture for {name}")
        return struct.unpack(">II", header[16:24])

    def pixel(self, x, y):
        data = checked(["grim", "-s", "1", "-t", "ppm", "-g", f"{x},{y} 1x1", "-"], env=self.env)
        magic, width, height, depth, pixel = data.split(maxsplit=4)
        require((magic, width, height, depth) == (b"P6", b"1", b"1", b"255") and len(pixel) == 3,
                "grim did not return one RGB pixel")
        return tuple(pixel)

    def wait(self, predicate, label):
        return wait_for(predicate, 8, label, self.process)

    def close(self):
        require(self.ctl("exit")["accepted"] and self.process.wait(timeout=12) == 0,
                "The Part 8 session did not exit cleanly")
        require(not Path(self.env["SEASIDE_SOCKET"]).exists() and standard["active"]() == "tty1",
                "The Part 8 session did not release IPC and return to TTY 1")


@contextmanager
def fixture(session, log, executable, *args, interactive=False):
    with log.open("wb") as stream:
        child = subprocess.Popen([session.repo / "build" / executable, *args], env=session.env,
                                 stdin=subprocess.PIPE if interactive else subprocess.DEVNULL,
                                 stdout=stream, stderr=subprocess.STDOUT, text=True)
        try:
            yield child
        finally:
            try:
                stop(child)
            finally:
                if child.stdin is not None:
                    child.stdin.close()


def run_geometry(session, state, original):
    physical = original["mode"]
    for rotation, scale in ((90, 1.25), (180, 1.5), (270, 1.25), (0, 1.0)):
        session.edit(f"[monitors]\nrotation = {rotation}\nscale = {scale}\n")

        def transformed():
            current = session.focused_output()
            return current if current and (current["rotation"], current["scale"]) == (rotation, scale) else None

        output = session.wait(transformed, f"rotation {rotation} and scale {scale}")
        require(output["id"] == original["id"] and output["mode"] == physical and output["hardware_cursor"],
                f"Transform {rotation} changed physical mode, identity, or cursor availability")
        width, height = physical["width"], physical["height"]
        if rotation in (90, 270):
            width, height = height, width
        expected = (int(width / scale), int(height / scale))
        size = session.grab(output, f"rotated-{rotation}")
        require(size == expected, f"The rotated and scaled capture was {size}, expected {expected}")
        state.setdefault("geometry", []).append({"rotation": rotation, "scale": scale, "logical_capture": size})

    snapshot = session.state_dir / "seaside/last-good-config.toml"
    current = session.config.read_text()
    session.wait(lambda: snapshot.is_file() and snapshot.read_text() == current, "the accepted configuration snapshot")
    session.edit("[monitors]\nresolution = '313x271'\n")
    error = session.wait(lambda: session.ctl("status")["config"]["error"], "the unsupported mode diagnostic")
    retained = session.focused_output()
    geometry = ("id", "mode", "position", "rotation", "scale", "hardware_cursor")
    require(error["field"] == "monitors.resolution" and retained and
            all(retained[key] == output[key] for key in geometry) and snapshot.read_text() == current,
            "An unsupported mode changed the output or recovery snapshot")
    session.edit("[monitors]\nrotation = 0\nscale = 1.0\n")
    session.wait(lambda: session.ctl("status")["config"]["error"] is None, "invalid-mode recovery")
    state["mode_rollback"] = "pass"

    modes = set()
    for path in Path("/sys/class/drm").glob(f"card*-{original['connector']}/modes"):
        modes.update(path.read_text().split())
    original_size = f"{physical['width']}x{physical['height']}"
    alternatives = [size for size in modes - {original_size} if re.fullmatch(r"[0-9]+x[0-9]+", size)]
    if not alternatives:
        state["modeset"] = {"status": "skip", "reason": "Guest connector advertises only its active mode"}
        return
    size = min(alternatives, key=lambda value: tuple(map(int, value.split("x"))))
    w, h = map(int, size.split("x"))
    session.edit(f"[monitors]\nresolution = '{size}'\n")

    def modeset():
        active = session.focused_output()
        return active if active and (active["mode"]["width"], active["mode"]["height"]) == (w, h) else None

    changed = session.wait(modeset, f"the alternate guest mode {size}")
    require(changed["id"] == original["id"] and changed["hardware_cursor"],
            "A modeset lost output identity or the hardware cursor")
    session.edit()
    session.wait(lambda: (session.focused_output() or {}).get("mode") == physical, "the preferred mode restored")
    state["modeset"] = {"status": "pass", "alternate": changed["mode"], "restored": physical}


def run_night_light(session, state, output):
    def status():
        return session.ctl("night-light", "status")

    def capability():
        return next(row for row in status()["outputs"] if row["output_id"] == output["id"])

    before = status()
    require(not before["enabled"] and not before["configured_enabled"] and before["temperature_kelvin"] == 4500,
            "Night light must default to off at 4500 K")
    event("part8-input", "toggle night light with the real binding", key="night-light")
    session.wait(lambda: status()["enabled"], "night light binding")
    on = capability()
    if on["supported"] is True:
        session.wait(lambda: capability()["applied"] is True, "night light applied to the guest output")
        on = capability()
    else:
        require(on["applied"] is False and on["reason"], "The guest failed to report its night light limit")

    session.edit("[night_light]\ntemperature_kelvin = 5000\n")
    session.wait(lambda: status()["temperature_kelvin"] == 5000, "live night light temperature")
    require(status()["enabled"], "A temperature-only reload lost the runtime night light override")
    require(session.ctl("night-light", "off")["accepted"], "Night light off was not accepted")
    session.wait(lambda: not status()["enabled"] and capability()["applied"] is False, "night light off")
    require(session.focused_output()["mode"] == output["mode"] and session.focused_output()["hardware_cursor"],
            "Night light changed output availability, mode, or cursor policy")
    state["night_light"] = {"status": "pass", "on": on, "off": capability()}
    session.edit()
    session.wait(lambda: status()["temperature_kelvin"] == 4500, "restored night light temperature")


def run_adaptive_sync(session, state, output):
    snapshot = session.state_dir / "seaside/last-good-config.toml"
    outcomes = {}
    for policy in ("always", "fullscreen", "off"):
        session.edit(f'[render]\nadaptive_sync = "{policy}"\n')
        expected = session.config.read_text()
        session.wait(lambda: snapshot.is_file() and snapshot.read_text() == expected,
                     f"accepted {policy} adaptive-sync policy")
        current = session.focused_output()
        require(current and current["id"] == output["id"] and current["mode"] == output["mode"] and
                current["hardware_cursor"], f"The {policy} adaptive-sync policy changed the output")
        capability = current["adaptive_sync"]
        outcomes[policy] = capability
        if policy == "always" and not capability["applied"]:
            require(capability["reason"], "The guest did not report why it remained at fixed refresh")
        if policy != "always":
            require(capability["applied"] is False, f"The {policy} policy applied VRR to the idle desktop")
    state["adaptive_sync"] = {"status": "pass", "policies": outcomes}


def run_portal(session, pattern, output, state):
    if not session.chooser:
        state["fullscreen_portal"] = {"status": "blocked", "reason": "Wofi is unavailable in the guest trial"}
        return
    log = session.results / "part8-portal.log"
    for attempt in range(1, 4):
        with fixture(session, log, "seaside-portal-stream", "monitor", "frame") as reader:
            def chooser_focused():
                focus = session.ctl("focus")["window_id"]
                return any(row["id"] == focus and "wofi" in
                           f"{row['app_id']} {row['title']}".casefold()
                           for row in session.ctl("windows")["windows"])

            wait_for(chooser_focused, 10, "the focused portal chooser", reader)
            event("part8-input", "choose the real monitor for the portal receiver", key="monitor")
            next_paint = 0.0

            def repaint_running():
                nonlocal next_paint
                contents = log.read_text(errors="replace")
                if "\nframe " in contents or reader.poll() is not None:
                    return True
                if "PipeWire stream state streaming" in contents and time.monotonic() >= next_paint:
                    pattern.stdin.write("64482c\n")
                    pattern.stdin.flush()
                    next_paint = time.monotonic() + 0.8
                return None

            try:
                wait_for(repaint_running, 8, "an active fullscreen monitor stream")
            except RuntimeError:
                if attempt == 3 or "PipeWire stream state streaming" not in log.read_text(errors="replace"):
                    raise
                event("part8", f"retrying linked portal stream without frames, attempt {attempt}")
                state["portal_linked_retries"] = attempt
                continue
            code = reader.wait(timeout=8)
            contents = log.read_text(errors="replace")
            require(code == 0, f"The monitor stream did not complete: {contents}")
            frame = next((line.split()[1:] for line in contents.splitlines() if line.startswith("frame ")), [])
            require(tuple(map(int, frame[:2])) == (output["mode"]["width"], output["mode"]["height"]) and
                    set(frame[2:]) == {"64482c"}, f"Unexpected fullscreen portal frame {frame}")
            state["fullscreen_portal"] = {"status": "pass", "frame": frame}
            return


def run_fullscreen(session, state, output):
    mode = output["mode"]
    width, height = mode["width"], mode["height"]
    x, y = output["position"]["x"], output["position"]["y"]
    center = (x + width // 2, y + height // 2)
    # The fixed 3:2 surface yields either letterboxing or pillarboxing. Use a
    # point on the actual long axis so both guest monitor aspect ratios work.
    require(width * 2 != height * 3, "A non-3:2 output is required for fullscreen aspect verification")
    edge = (x + 2, center[1]) if width * 2 > height * 3 else (center[0], y + 2)
    initial = session.diagnostics(output)
    with fixture(session, session.results / "part8-pattern.log", "seaside-part7-native-client",
                 TITLE, "64482c", "--fixed-size", interactive=True) as pattern:
        target = wait_for(lambda: session.window(TITLE), 8, "the fixed-size Wayland fixture", pattern)
        require(session.ctl("focus")["window_id"] == target["id"], "The fullscreen test did not own focus")
        require(session.ctl("window", "toggle-fullscreen")["changed"], "Fullscreen did not activate")
        session.wait(lambda: session.window(TITLE)["fullscreen"] and session.pixel(*center) == PIXEL and
                     session.pixel(*edge) == (0, 0, 0), "fullscreen content and black bars")
        require(session.grab(output, "preserve") == (width, height), "Preserve changed the capture dimensions")
        session.edit('[render]\nfullscreen_aspect = "stretch"\nallow_tearing = true\n')
        session.wait(lambda: session.pixel(*edge) == PIXEL and session.pixel(*center) == PIXEL,
                     "fullscreen stretched pixels")
        require(session.grab(output, "stretch") == (width, height), "Stretch changed the capture dimensions")
        diagnostic = session.diagnostics(output)
        require(diagnostic["presented_frames"] > initial["presented_frames"] and
                diagnostic["presentation_mode"] == "composed" and diagnostic["last_presentation_ns"],
                f"Missing actual composition evidence: {diagnostic}")
        tearing = session.focused_output()["tearing"]
        require(tearing["applied"] is False, "The default synchronized fullscreen client was allowed to tear")
        run_portal(session, pattern, output, state)
        require(session.window(TITLE)["id"] == target["id"] and session.ctl("focus")["window_id"] == target["id"],
                "Capture changed the fullscreen client's identity or focus")
        require(session.ctl("window", "toggle-fullscreen")["changed"], "Fullscreen did not exit")
        session.wait(lambda: not session.window(TITLE)["fullscreen"] and session.pixel(*edge) == (0, 0, 0),
                     "restored floating window")
        require(session.focused_output()["mode"] == mode, "Fullscreen altered the physical mode")
        pattern.stdin.write("close\n")
        pattern.stdin.flush()
        require(pattern.wait(timeout=8) == 0, "The fullscreen fixture did not close cleanly")
    state["fullscreen"] = {"status": "pass", "center": center, "edge": edge, "mode": mode,
                           "presentation": diagnostic, "tearing_without_hint": tearing}


def trial():
    tty, active = os.ttyname(0), standard["active"]()
    existing = subprocess.run(["pgrep", "-x", "-t", "tty1", "seaside"], capture_output=True, timeout=3)
    event("connected", f"stdin={tty} active={active}")
    if tty != "/dev/tty1" or active != "tty1" or os.tcgetpgrp(0) != os.getpgrp() or existing.returncode != 1:
        event("done", "An idle foreground shell on active TTY 1 is required", ok=False)
        return
    results = Path.home() / "Projects/seaside/test-results" / f"vm-{base.rsplit('/', 1)[-1][:12]}"
    results.mkdir(mode=0o700, parents=True)
    state = {"revision": revision, "kernel": os.uname().release, "requested_renderer": part8_renderer,
             "ok": False}
    try:
        env = environment()
        runtime = env.get("XDG_RUNTIME_DIR")
        require(runtime, "The guest did not provide XDG_RUNTIME_DIR")
        with tempfile.TemporaryDirectory(prefix="seaside-vm-source-") as source:
            repo = prepare(Path(source), results, state, env)
            with tempfile.TemporaryDirectory(prefix="seaside-vm-", dir=runtime) as temp:
                root = Path(temp)
                original_env = os.environ.copy()
                try:
                    os.environ.clear()
                    os.environ.update(env)
                    standard["tty_trial"](repo, root, results, state)
                finally:
                    os.environ.clear()
                    os.environ.update(original_env)

                with Session(repo, root / "part8", results, env) as session:
                    output = session.wait(session.focused_output, "the focused guest output")
                    require(output["hardware_cursor"], "The virtual DRM output has no working cursor plane")
                    info = session.diagnostics(output)
                    state.update(renderer=info["renderer"], backend=info["backend"], initial_output=output)
                    event("part8", f"{info['backend']}/{info['renderer']} {output['connector']}")
                    run_geometry(session, state, output)
                    run_adaptive_sync(session, state, output)
                    run_night_light(session, state, output)
                    run_fullscreen(session, state, output)
                    session.close()
        state["ok"] = True
    except (OSError, RuntimeError, ValueError, KeyError, StopIteration, subprocess.SubprocessError,
            KeyboardInterrupt) as error:
        state["error"] = str(error) or type(error).__name__
        event("diagnostic", state["error"])
    finally:
        for name in ("prepare.log", "harness.log", "normal.log", "failure.log", *part8_artifacts):
            upload(name, results / name)
        state["guest_evidence"] = str(results)
        standard["send"]("guest.json", json.dumps(state, indent=2).encode() + b"\n")
        event("done", state.get("error", "Part 8 VM trial passed"), ok=state["ok"])


trial()
