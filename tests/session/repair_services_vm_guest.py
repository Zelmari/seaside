"""Check session repair behavior on an idle Arch guest TTY using the Part 8 helpers."""

from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import signal
import stat
import sys
import subprocess
import threading
import tempfile
import time
import tomllib


PORTALS = ("org.freedesktop.impl.portal.desktop.wlr", "org.freedesktop.impl.portal.desktop.gtk",
           "org.freedesktop.portal.Desktop")
ENV_KEYS = ("WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "DISPLAY", "XAUTHORITY")


def cmd(*args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, timeout=10, **kwargs)


def manager_environment():
    output = cmd("systemctl", "--user", "show-environment", check=True).stdout.splitlines()
    return {k: v for row in output for k, _, v in [row.partition("=")] if k in ENV_KEYS}


def owned_unit(session, name):
    owner = cmd("busctl", "--user", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetConnectionUnixProcessID", "s", name)
    if owner.returncode:
        return None
    pid = int(owner.stdout.split()[1])
    try:
        proc = Path("/proc") / str(pid)
        unit = re.search(r"/(seaside-portal-[0-9a-f]+-(?:wlr|gtk|frontend)\.service)$",
                         (proc / "cgroup").read_text().strip())
        if not unit or f"WAYLAND_DISPLAY={session.env['WAYLAND_DISPLAY']}".encode() not in (
                proc / "environ").read_bytes().split(b"\0"):
            return None
        invocation = unit_invocation(unit[1])
        return (unit[1], invocation, pid) if invocation else None
    except FileNotFoundError:
        return None


def unit_invocation(unit):
    query = cmd("systemctl", "--user", "show", unit, "--property=InvocationID", "--value")
    return query.stdout.strip() if query.returncode == 0 else None


def release_trial_units(units):
    for unit, invocation, _ in reversed(units):
        if invocation == unit_invocation(unit):
            cmd("systemctl", "--user", "stop", unit, check=True)


def until(check, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(.08)
    return None


@contextmanager
def reserve_default_display(runtime):
    path = runtime / "wayland-0.lock"
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_CLOEXEC, 0o600)
    except FileExistsError:
        yield
        return
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        if path.exists() and path.stat().st_ino == os.fstat(fd).st_ino:
            path.unlink()
        os.close(fd)


def activated_app(root, runtime):
    # Use the standard per-user runtime D-Bus service directory. Reloading its
    # catalogue never restarts the manager, broker, or another user's services.
    name = f"org.seaside.RepairProbe.P{os.getpid()}"
    app = root / "activated.py"
    marker = root / "activated.json"
    wayland = root / "activated-window"
    app.write_text("""import ctypes, json, os, subprocess, sys
from pathlib import Path
mark, wayland, name = sys.argv[1:]
result = subprocess.run(['/usr/bin/foot', '/usr/bin/touch', wayland],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=6)
xcb = ctypes.CDLL('libxcb.so.1')
xcb.xcb_connect.restype = ctypes.c_void_p
xcb.xcb_connection_has_error.argtypes = [ctypes.c_void_p]
xcb.xcb_disconnect.argtypes = [ctypes.c_void_p]
connection = xcb.xcb_connect(None, None)
connected = bool(connection and not xcb.xcb_connection_has_error(connection))
if connection:
    xcb.xcb_disconnect(connection)
keys = ('WAYLAND_DISPLAY', 'XDG_CURRENT_DESKTOP', 'XDG_SESSION_TYPE', 'DISPLAY', 'XAUTHORITY')
Path(mark).write_text(json.dumps(dict(wayland=Path(wayland).exists(), x11=connected,
    environment={k: os.environ[k] for k in keys if k in os.environ})))
os.execl('/usr/bin/busctl', 'busctl', '--user', 'call', 'org.freedesktop.DBus',
    '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName', 'su', name, '4')
""")
    directory = runtime / "dbus-1/services"
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    service = directory / f"{name}.service"
    bus = ("busctl", "--user", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
           "org.freedesktop.DBus")
    with service.open("x") as stream:
        stream.write(f"[D-BUS Service]\nName={name}\nExec=/usr/bin/python3 {app} {marker} {wayland} {name}\n")
    try:
        cmd(*bus, "ReloadConfig", check=True)
        response = cmd(*bus, "StartServiceByName", "su", name, "0")
        return {"activation": response.returncode,
                **(json.loads(marker.read_text()) if marker.exists() else {"error": response.stderr})}
    finally:
        service.unlink()
        cmd(*bus, "ReloadConfig", check=True)


def exercise_foreign(session, root):
    name = PORTALS[-1]
    marker = root / "foreign-owner"
    # Keep our own connection alive without starting/stopping any stock units.
    owner = subprocess.Popen([sys.executable, "-c", """import ctypes as c, signal, sys
from pathlib import Path
bus = c.c_void_p()
sd = c.CDLL('libsystemd.so.0')
if sd.sd_bus_open_user(c.byref(bus)) < 0 or sd.sd_bus_request_name(bus, sys.argv[1].encode(), 0) < 0:
    sys.exit(1)
Path(sys.argv[2]).touch()
signal.pause()
""", name, str(marker)])
    evidence = {}
    before = manager_environment()
    try:
        if not until(marker.exists, 4):
            raise RuntimeError("Could not acquire disposable foreign portal name")
        with session:
            status = until(lambda: fault_status(session, "portal-frontend"), 12)
            evidence["fault"] = status
            session.ctl("workspace", "2")
            evidence["untouched"] = (owner.poll() is None and manager_environment() == before
                                     and not any(owned_unit(session, item) for item in PORTALS))
            owner.terminate()
            owner.wait(timeout=3)
            evidence["recovered"] = bool(until(lambda: all_available(session), 24))
            session.close()
    except RuntimeError as error:
        evidence["error"] = str(error)
    finally:
        if owner.poll() is None:
            owner.terminate()
        owner.wait(timeout=3)
    return evidence


def fault_status(session, component):
    integrations = session.ctl("status").get("integrations", {})
    return integrations if integrations.get(component, {}).get("state") == "unavailable" else None


def all_available(session):
    services = session.ctl("status").get("integrations", {})
    return bool(services) and all(item["state"] == "available" for item in services.values())


@contextmanager
def hide_login_bus(root, env):
    # Only the test compositor's socket check sees ENOENT. The user's actual
    # login bus, portal services and all unrelated clients continue running.
    marker = root / "hide-bus"
    marker.touch()
    source = root / "login_bus_fixture.c"
    library = root / "login_bus_fixture.so"
    source.write_text("""#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int lstat(const char *path, struct stat *info) {
    const char *socket = getenv("SEASIDE_TEST_LOGIN_SOCKET");
    const char *flag = getenv("SEASIDE_TEST_LOGIN_FLAG");
    if (socket && flag && strcmp(path, socket) == 0 && access(flag, F_OK) == 0) {
        errno = ENOENT;
        return -1;
    }
    return fstatat(AT_FDCWD, path, info, AT_SYMLINK_NOFOLLOW);
}
""")
    cmd("cc", "-O2", "-shared", "-fPIC", str(source), "-o", str(library), check=True)
    preload = str(library)
    if "ASAN_OPTIONS" in env:
        preload = cmd("cc", "-print-file-name=libasan.so", check=True).stdout.strip() + ":" + preload
    yield dict(env, LD_PRELOAD=preload, SEASIDE_TEST_LOGIN_SOCKET=f"/run/user/{os.getuid()}/bus",
               SEASIDE_TEST_LOGIN_FLAG=str(marker)), marker


def exercise_bus(session, marker):
    evidence = {}
    try:
        with session:
            evidence["fault"] = until(lambda: fault_status(session, "systemd"), 8)
            session.ctl("workspace", "2")
            evidence["pipewire"] = bool(until(lambda: session.ctl("status").get(
                "integrations", {}).get("pipewire", {}).get("state") == "available", 4))
            marker.unlink()
            evidence["recovered"] = bool(until(lambda: all_available(session), 25))
            session.close()
    except RuntimeError as error:
        evidence["error"] = str(error)
    return evidence


@contextmanager
def manager_overrides():
    before = manager_environment()
    keys = ("XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE")
    values = (f"seaside-vm-prior-{os.getpid()}", f"seaside-vm-later-{os.getpid()}")
    cmd("systemctl", "--user", "set-environment", f"{keys[0]}={values[0]}", check=True)
    try:
        yield dict(zip(keys, values, strict=True))
    finally:
        current = manager_environment()
        for key, value in zip(keys, values, strict=True):
            if current.get(key) == value:
                command = ("set-environment", f"{key}={before[key]}") if key in before else (
                    "unset-environment", key)
                cmd("systemctl", "--user", *command, check=True)


def observe_shutdown(session, change_env=None):
    samples = []
    stop = threading.Event()
    executable = session.repo / "build/seasidectl"

    def monitor():
        while not stop.is_set():
            result = cmd(executable, "debug", "render", env=session.env)
            samples.append(result.returncode)
            if result.returncode or stop.wait(.02):
                break

    with session:
        until(lambda: owned_unit(session, PORTALS[-1]), 12)
        if change_env:
            cmd("systemctl", "--user", "set-environment",
                f"XDG_SESSION_TYPE={change_env['XDG_SESSION_TYPE']}", check=True)
        worker = threading.Thread(target=monitor)
        worker.start()
        try:
            until(lambda: len(samples) >= 2, 3)
            result = cmd(executable, "exit", env=session.env)
            code = session.process.wait(timeout=12)
        finally:
            stop.set()
            worker.join(timeout=12)
    return {"exit": code, "acknowledged": result.returncode == 0, "samples": samples}

def x11_reservations(session, marker):
    display, auth = marker.read_text().splitlines()
    if not re.fullmatch(r":[0-9]+", display):
        raise RuntimeError("Unexpected X11 test display")
    number = display[1:]
    authority = Path(auth)
    if (authority.parent != Path(session.env["XDG_RUNTIME_DIR"]) or
            not re.fullmatch(r"seaside-xauth-[A-Za-z0-9]+", authority.name)):
        raise RuntimeError("Unexpected test authority path")
    paths = (Path(f"/tmp/.X{number}-lock"), Path(f"/tmp/.X11-unix/X{number}"), authority)
    entries = tuple(path.lstat() for path in paths)
    if (any(info.st_uid != os.getuid() for info in entries) or
            not stat.S_ISREG(entries[0].st_mode) or not stat.S_ISSOCK(entries[1].st_mode) or
            not stat.S_ISREG(entries[2].st_mode) or int(paths[0].read_text()) != session.process.pid):
        raise RuntimeError("X11 reservations are not owned by this test process")
    return tuple(zip(paths, entries, strict=True))


def clean_crashed_x11(process, entries):
    if process.returncode is None:
        raise RuntimeError("Test compositor is still running")
    lock, sock = entries[0][0], str(entries[1][0])

    def inactive():
        for line in Path("/proc/net/unix").read_text().splitlines()[1:]:
            row = line.split(maxsplit=7)
            if len(row) == 8 and row[-1] in (sock, "@" + sock):
                return False
        return True

    if not until(inactive, 4):
        return False
    # Preserve a replacement's files even if it chose this display number.
    for path, recorded in entries:
        if path.exists() and not os.path.samestat(path.lstat(), recorded):
            return False
    if lock.exists() and int(lock.read_text()) != process.pid:
        return False
    for path, recorded in reversed(entries):
        if path.exists() and os.path.samestat(path.lstat(), recorded):
            path.unlink()
    return True


def exercise(repo, root, results, env):
    before = manager_environment()
    evidence = {"environment_before": before}
    Session = helpers["Session"]
    first = Session(repo, root / "healthy", results, env, label="repair-healthy")
    x11_marker = root / "x11-environment"
    commands = tomllib.loads(first.base_config)["autostart"]["commands"]
    extra = 'printf "%s\\n" "$DISPLAY" "$XAUTHORITY" > ' + shlex.quote(str(x11_marker))
    first.base_config = first.base_config.replace(json.dumps(commands[0]), json.dumps(extra + " && " + commands[0]))
    first.edit()
    units = []
    try:
        with first:
            for name in PORTALS:
                result = until(lambda: owned_unit(first, name), 15)
                if not result:
                    raise RuntimeError(f"No verified session unit for {name}")
                units.append(result)
            evidence["environment_during"] = manager_environment()
            evidence["wayland_display"] = first.env["WAYLAND_DISPLAY"]
            evidence["activated_app"] = activated_app(root, Path(env["XDG_RUNTIME_DIR"]))
            marker = root / "systemd-window"
            r = cmd("systemd-run", "--user", "--wait", "--collect", "--quiet", "foot",
                    "--app-id=seaside-activation-trial", "/usr/bin/touch", str(marker))
            evidence["systemd_window"] = {"exit": r.returncode, "opened": marker.exists()}
            gtk, invocation, pid = units[1]
            if unit_invocation(gtk) != invocation:
                raise RuntimeError("GTK unit changed before its recovery trial")
            cmd("systemctl", "--user", "stop", gtk, check=True)
            replacement = until(lambda: owned_unit(first, PORTALS[1]), 13)
            evidence["gtk_restarted"] = bool(replacement and replacement[2] != pid)
            if replacement:
                units[1] = replacement
                def restored_gtk():
                    args = ("busctl", "--user", "--auto-start=no")
                    obj = "/org/freedesktop/portal/desktop"
                    chooser = cmd(*args, "introspect", PORTALS[1], obj, "org.freedesktop.impl.portal.FileChooser")
                    settings = cmd(*args, "call", PORTALS[1], obj, "org.freedesktop.impl.portal.Settings",
                                   "ReadAll", "as", "0")
                    evidence["gtk_errors"] = [r.stderr.strip() for r in (chooser, settings) if r.returncode]
                    return (chooser.returncode == settings.returncode == 0 and ".OpenFile" in chooser.stdout
                            and settings.stdout.startswith("a{sa{sv}}"))
                evidence["gtk_filechooser"] = bool(until(restored_gtk, 8))
            # The guest compositor belongs to this trial. Record the surviving
            # service invocations and retry a new session before cleanup.
            reservations = x11_reservations(first, x11_marker)
            first.process.kill()
            evidence["crash_exit"] = first.process.wait(timeout=10)
        second = Session(repo, root / "restart", results, env, label="repair-restart")
        try:
            with second:
                evidence["restart_ready"] = second.ctl("status")["ready"]
                evidence["restart_portals"] = bool(until(lambda: all(owned_unit(second, name)
                                                                     for name in PORTALS), 10))
                second.close()
        except RuntimeError as error:
            evidence["restart_error"] = str(error)
            evidence["restart_log"] = second.log.read_text(errors="replace")[-1200:]
        evidence["orphan_units"] = [unit for unit, invocation, _ in units
                                    if unit_invocation(unit) == invocation]
    finally:
        release_trial_units(units)
        if "crash_exit" in evidence:
            evidence["trial_x11_cleanup"] = clean_crashed_x11(first.process, reservations)
    missing = root / "repair-pipewire"
    pw = Session(repo, root / "unavailable", results, dict(env, PIPEWIRE_REMOTE=str(missing)),
                 label="repair-unavailable")
    try:
        with pw:
            evidence["missing_pipewire"] = pw.ctl("status")
            pw.ctl("workspace", "2")
            missing.symlink_to(Path(env["XDG_RUNTIME_DIR"]) / "pipewire-0")
            evidence["pipewire_recovered"] = bool(until(
                lambda: "service recovered service=pipewire" in pw.log.read_text(errors="replace"), 20))
            pw.process.send_signal(signal.SIGPIPE)
            evidence["sigpipe_ready"] = pw.ctl("status")["ready"]
            pw.process.send_signal(signal.SIGHUP)
            evidence["hangup_exit"] = pw.process.wait(timeout=12)
            evidence["hangup_cleanup"] = not Path(pw.env["SEASIDE_SOCKET"]).exists()
    except RuntimeError as error:
        evidence["missing_pipewire_error"] = str(error)
        evidence["missing_pipewire_log"] = pw.log.read_text(errors="replace")[-1200:]
    shutdown = Session(repo, root / "shutdown", results, env, label="repair-shutdown")
    foreign = Session(repo, root / "foreign", results, env, label="repair-foreign")
    evidence["foreign"] = exercise_foreign(foreign, root)
    with hide_login_bus(root, env) as (isolated, flag):
        bus = Session(repo, root / "bus", results, isolated, label="repair-bus")
        evidence["bus"] = exercise_bus(bus, flag)
    with manager_overrides() as changed:
        evidence["shutdown"] = observe_shutdown(shutdown, changed)
        after = manager_environment()
        evidence["preserved_values"] = all(after.get(key) == value for key, value in changed.items())
    evidence["environment_after"] = manager_environment()
    return evidence


def trial():
    standard["event"]("connected", f"stdin={os.ttyname(0)} active={standard['active']()}")
    if (os.ttyname(0) != "/dev/tty1" or standard["active"]() != "tty1" or
            os.tcgetpgrp(0) != os.getpgrp() or
            cmd("pgrep", "-x", "-t", "tty1", "seaside").returncode != 1):
        standard["event"]("done", "An idle foreground shell on TTY 1 is required", ok=False)
        return
    results = Path.home() / "Projects/seaside/test-results" / f"vm-{standard['BASE'].rsplit('/', 1)[-1][:12]}"
    results.mkdir(parents=True, mode=0o700, exist_ok=True)
    state = {"revision": standard["REVISION"], "ok": False}
    try:
        env = helpers["environment"]()
        with tempfile.TemporaryDirectory(prefix="seaside-repair-source-") as source:
            repo = helpers["prepare"](Path(source), results, state, env, part=9)
            if globals().get("repair_sanitize", False):
                log = results / "sanitize-build.log"
                for argv in (["meson", "configure", "build", "-Db_sanitize=address,undefined"],
                             ["meson", "compile", "-C", "build", "-j2", "seaside", "seasidectl"]):
                    helpers["require"](standard["run_logged"](argv, repo, log, 420, env) == 0,
                                       "Could not build the sanitized TTY compositor")
                env.update(ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
                state["sanitizers"] = "address,undefined"
            with tempfile.TemporaryDirectory(prefix="seaside-repair-", dir=env["XDG_RUNTIME_DIR"]) as root:
                with reserve_default_display(Path(env["XDG_RUNTIME_DIR"])):
                    state["services"] = exercise(repo, Path(root), results, env)
        if not repair_baseline:
            checks = state["services"]
            assert checks["systemd_window"]["opened"] and checks["gtk_restarted"] and checks["gtk_filechooser"], checks
            for case in ("foreign", "bus"):
                detail = checks[case]
                assert detail["fault"] and detail["recovered"], detail
                assert all(detail["fault"][k]["state"] == "unavailable" for k in
                           ("portal-frontend", "capture-backend", "gtk-backend")), detail
            assert checks["bus"]["pipewire"] and checks["foreign"]["untouched"], checks
            assert checks["preserved_values"] and checks["trial_x11_cleanup"], checks
            assert checks["wayland_display"] != "wayland-0", checks
            assert checks["environment_during"]["WAYLAND_DISPLAY"] == checks["wayland_display"], checks
            app = checks["activated_app"]
            assert app["wayland"] and app["x11"] and app["environment"] == checks["environment_during"], app
            assert checks["restart_ready"] and checks["restart_portals"] and not checks["orphan_units"], checks
            assert checks["missing_pipewire"]["ready"] and checks["pipewire_recovered"], checks
            assert checks["sigpipe_ready"] and checks["hangup_exit"] == 0 and checks["hangup_cleanup"], checks
            assert checks["shutdown"]["exit"] == 0 and checks["shutdown"]["acknowledged"], checks
            assert checks["shutdown"]["samples"].count(0) >= 2, checks
            assert checks["environment_before"] == checks["environment_after"], checks
        state["ok"] = True
    except (OSError, RuntimeError, ValueError, KeyError, AssertionError, subprocess.SubprocessError) as e:
        state["error"] = str(e) or type(e).__name__
    finally:
        for log in ("prepare.log", "harness.log", "repair-healthy-session.log",
                    "repair-restart-session.log", "repair-unavailable-session.log", "repair-shutdown-session.log",
                    "repair-foreign-session.log", "repair-bus-session.log", "sanitize-build.log"):
            standard["upload"](log, results / log)
        standard["send"]("guest.json", json.dumps(state, indent=2).encode() + b"\n")
        standard["event"]("done", state.get("error", "Repair service observations complete"), ok=state["ok"])


if __name__ == "__main__":
    trial()
