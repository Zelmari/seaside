#!/usr/bin/env python3
"""Run the packaged portals on a private bus and headless Seaside display."""

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time


def run(fixture, routing, package_root, receiver=None, native_client=None):
    lib = package_root / "lib"
    data = package_root / "share"
    definitions = [
        ("permissions", lib / "xdg-permission-store", "org.freedesktop.impl.portal.PermissionStore"),
        ("wlr", lib / "xdg-desktop-portal-wlr", "org.freedesktop.impl.portal.desktop.wlr"),
        ("gtk", lib / "xdg-desktop-portal-gtk", "org.freedesktop.impl.portal.desktop.gtk"),
        ("frontend", lib / "xdg-desktop-portal", "org.freedesktop.portal.Desktop"),
    ]
    grim = package_root / "bin/grim"
    wireplumber = package_root / "bin/wireplumber"
    required = [grim, *(row[1] for row in definitions)]
    if receiver:
        required.append(wireplumber)
    missing = [str(binary) for binary in required if not binary.is_file()]
    if missing:
        print(f"Real portal checks need the installed Arch tools {', '.join(missing)}")
        return 77
    with tempfile.TemporaryDirectory(prefix="seaside-portals-") as name:
        root = Path(name)
        address = f"unix:path={root / 'bus'}"
        env = dict(os.environ)
        for key in list(env):
            if key.startswith(("XDG_", "PIPEWIRE_", "SPA_", "DBUS_", "WIREPLUMBER_")):
                del env[key]
        for key in ("DISPLAY", "WAYLAND_SOCKET", "LD_PRELOAD"):
            env.pop(key, None)
        portal_dir = root / "portals"
        portal_dir.mkdir()
        for source in (data / "xdg-desktop-portal/portals").glob("*.portal"):
            if source.name in ("gtk.portal", "wlr.portal"):
                shutil.copyfile(source, portal_dir / source.name)
        shutil.copyfile(routing, portal_dir / "seaside-portals.conf")
        env.update(HOME=name, XDG_RUNTIME_DIR=name, XDG_CONFIG_HOME=str(root / "config"),
                   XDG_CURRENT_DESKTOP="Seaside",
                   XDG_SESSION_TYPE="wayland", XDG_DESKTOP_PORTAL_DIR=str(portal_dir),
                   XDG_DATA_DIRS=str(data), GDK_BACKEND="wayland", NO_AT_BRIDGE="1",
                   GSETTINGS_BACKEND="memory", DBUS_SESSION_BUS_ADDRESS=address,
                   DBUS_SYSTEM_BUS_ADDRESS=f"unix:path={root / 'no-system-bus'}",
                   WAYLAND_DISPLAY="seaside-portal-smoke")

        package_env = dict(env)
        if package_root != Path("/usr"):
            package_env.update(LD_LIBRARY_PATH=str(lib),
                               GSETTINGS_SCHEMA_DIR=str(data / "glib-2.0/schemas"),
                               WIREPLUMBER_MODULE_DIR=str(lib / "wireplumber-0.5"),
                               WIREPLUMBER_CONFIG_DIR=str(data / "wireplumber"),
                               WIREPLUMBER_DATA_DIR=str(data / "wireplumber"))
        (root / "bus.conf").write_text(f"""<busconfig><type>session</type>
  <listen>unix:path={root / 'bus'}</listen>
  <policy context="default">
    <allow send_destination="*" send_requested_reply="false"/>
    <allow receive_sender="*"/>
    <allow own="*"/>
  </policy>
</busconfig>
""")
        (root / "pipewire.conf").write_text("""context.properties = {
    core.daemon = true core.name = pipewire-0 support.dbus = false mem.allow-mlock = false
}
context.spa-libs = { support.* = support/libspa-support }
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-access args = { access.socket = { pipewire-0 = unrestricted } } }
]
""")
        if receiver:
            # This offscreen check chooses the one candidate for the requested
            # source type. The TTY trial exercises interactive chooser consent.
            chooser = root / "config/xdg-desktop-portal-wlr/Seaside"
            chooser.parent.mkdir(parents=True)
            chooser.write_text("[screencast]\nchooser_type=dmenu\nchooser_cmd=sed -n '1p'\n")
        children = []

        def start(label, command, *, child_env=env, handshake=False):
            log = root / f"{label}.log"
            with log.open("w") as stream:
                child = subprocess.Popen(command, env=child_env, stdout=stream,
                                         stderr=subprocess.STDOUT,
                                         stdin=subprocess.PIPE if handshake else subprocess.DEVNULL)
            children.append((child, log))
            return child

        def wait_for(check, *required, description="a fixture service", timeout=4):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline and all(p.poll() is None for p in required):
                if check():
                    return
                time.sleep(.025)
            raise RuntimeError(f"Timed out waiting for {description}")

        def name_exists(service):
            reply = subprocess.run(
                ["busctl", "--address", address, "call", "org.freedesktop.DBus",
                 "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", "s", service],
                env=env, capture_output=True, text=True, timeout=1)
            return reply.stdout.strip() == "b true"

        def log_has(label, marker):
            return marker in (root / f"{label}.log").read_text(errors="replace")

        def send(child, command):
            child.stdin.write(command)
            child.stdin.flush()

        def check_frames():
            monitor = start("monitor", [receiver, "monitor", "frame"])
            if monitor.wait(timeout=8) or not log_has("monitor", "frame 160 90"):
                raise RuntimeError("The monitor receiver did not read a 160 by 90 frame")

            native = start("native", [native_client, "Selected window", "2244cc"], handshake=True)
            wait_for(lambda: log_has("native", "color 2244cc 264 176"), native,
                     description="the selected fixture window")
            window = start("window", [receiver, "window", "closed"])

            def expect_color(color):
                expected = "frame 264 176 " + " ".join([color] * 5)
                wait_for(lambda: log_has("window", expected), window, native,
                         description=f"selected-window frame {color}", timeout=6)

            expect_color("2244cc")
            overlap = start("overlap", [native_client, "Overlapping window", "aa5533"],
                            handshake=True)
            wait_for(lambda: log_has("overlap", "color aa5533 264 176"), overlap,
                     description="the overlapping fixture window")
            send(native, b"33bb55\n")
            expect_color("33bb55")
            send(native, b"close\n")
            if native.wait(timeout=4) or window.wait(timeout=6) or not log_has("window", "closed frames "):
                raise RuntimeError("The selected stream did not close with the window")

        try:
            bus = start("bus", ["dbus-daemon", "--nofork", f"--config-file={root / 'bus.conf'}"])
            pw = start("pipewire", ["pipewire"], child_env=env if receiver else
                       dict(env, PIPEWIRE_CONFIG_DIR=name))
            wait_for(lambda: (root / "bus").is_socket() and (root / "pipewire-0").is_socket(), bus, pw)
            if receiver:
                start("wireplumber", [wireplumber], child_env=package_env)
            compositor = start("compositor", [fixture, address, "--stream"] if receiver else
                               [fixture, address], handshake=True)
            wait_for(lambda: (root / env["WAYLAND_DISPLAY"]).is_socket(), compositor)
            for label, executable, service in definitions:
                command = [executable, "-l", "TRACE"] if receiver and label == "wlr" else [executable]
                child = start(label, command, child_env=package_env)
                wait_for(lambda: name_exists(service), child)
            screenshot = root / "cursor.png"
            subprocess.run([grim, "-c", screenshot], env=package_env, check=True, timeout=4)
            png = screenshot.read_bytes()
            if png[:8] != b"\x89PNG\r\n\x1a\n" or struct.unpack(">II", png[16:24]) != (160, 90):
                raise RuntimeError("grim produced an unexpected PNG size")
            send(compositor, b"\n")
            if receiver:
                wait_for(lambda: log_has("compositor", "portal smoke PASS"), compositor,
                         description="monitor and window portal readiness", timeout=9)
                check_frames()
                send(compositor, b"\n")
            if compositor.wait(timeout=9):
                raise RuntimeError("Real portal readiness check failed")
            if receiver:
                print("Real headless portal monitor, window updates and target closure PASS")
            else:
                print("Real Arch portal readiness and grim capture PASS")
            return 0
        except (OSError, RuntimeError, struct.error, subprocess.SubprocessError) as error:
            print(error, file=sys.stderr)
            for _, log in children:
                print(f"{log.name}\n{log.read_text(errors='replace')[-4000:]}", file=sys.stderr)
            return 1
        finally:
            for child, _ in reversed(children):
                if child.poll() is None:
                    child.terminate()
            end = time.monotonic() + 2
            for child, _ in reversed(children):
                try:
                    child.wait(timeout=max(0.05, end - time.monotonic()))
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("routing", type=Path)
    parser.add_argument("package_root", type=Path, nargs="?", default=Path("/usr"))
    parser.add_argument("--stream-receiver", type=Path)
    parser.add_argument("--native-client", type=Path)
    args = parser.parse_args()
    if (args.stream_receiver is None) != (args.native_client is None):
        parser.error("--stream-receiver and --native-client must be used together")
    sys.exit(run(args.fixture, args.routing, args.package_root, args.stream_receiver,
                 args.native_client))
