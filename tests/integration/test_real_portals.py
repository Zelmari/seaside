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


def run(fixture, routing, package_root, receiver=None, native_client=None, no_outputs=False):
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
    required = [row[1] for row in definitions]
    if not no_outputs:
        required.append(grim)
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
            while time.monotonic() < deadline:
                if check():
                    return
                if any(p.poll() is not None for p in required):
                    break
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
            native = start("native", [native_client, "Selected window", "2244cc"], handshake=True)
            wait_for(lambda: log_has("native", "color 2244cc 264 176"), native,
                     description="the selected fixture window")

            def paint(color):
                send(native, f"{color}\n".encode("ascii"))
                wait_for(lambda: log_has("native", f"color {color} 264 176"), native,
                         description=f"native client commit {color}")

            def start_damaged_stream(label, source, lifetime, colors):
                child = start(label, [receiver, source, lifetime])
                wait_for(lambda: log_has(label, "PipeWire stream state streaming"), child,
                         description=f"the {source} PipeWire stream", timeout=6)
                # A still desktop may only produce the initial Wayland frame.
                # Supply real damage while the new PipeWire link starts.
                for color in colors:
                    paint(color)
                return child

            def frame_present(label, size, color):
                expected = f"frame {size} " + " ".join([color] * 5)
                return log_has(label, expected)

            monitor = start_damaged_stream("monitor", "monitor", "frame", ("119966", "7755bb"))
            if monitor.wait(timeout=8) or not any(
                    frame_present("monitor", "160 90", color)
                    for color in ("2244cc", "119966", "7755bb")):
                raise RuntimeError("The monitor receiver did not read a 160 by 90 marker frame")

            window = start_damaged_stream("window", "window", "closed", ("4422cc", "33bb55"))

            def expect_color(color):
                wait_for(lambda: frame_present("window", "264 176", color), window, native,
                         description=f"selected-window frame {color}", timeout=6)

            expect_color("33bb55")
            overlap = start("overlap", [native_client, "Overlapping window", "aa5533"],
                            handshake=True)
            wait_for(lambda: log_has("overlap", "color aa5533 264 176"), overlap,
                     description="the overlapping fixture window")
            paint("22a088")
            expect_color("22a088")
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
            fixture_command = [fixture, address]
            if receiver:
                fixture_command.append("--stream")
            elif no_outputs:
                fixture_command.append("--no-outputs")
            compositor = start("compositor", fixture_command, handshake=True)
            wait_for(lambda: (root / env["WAYLAND_DISPLAY"]).is_socket(), compositor)
            for label, executable, service in definitions:
                command = [executable, "-l", "TRACE"] if receiver and label == "wlr" else [executable]
                child = start(label, command, child_env=package_env)
                wait_for(lambda: name_exists(service), child)
            if not no_outputs:
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
            elif no_outputs:
                print("Real Arch portal monitor and window readiness with zero outputs PASS")
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
    parser.add_argument("--no-outputs", action="store_true")
    args = parser.parse_args()
    if (args.stream_receiver is None) != (args.native_client is None):
        parser.error("--stream-receiver and --native-client must be used together")
    if args.no_outputs and args.stream_receiver is not None:
        parser.error("--no-outputs requires the readiness-only check")
    receiver = args.stream_receiver.resolve() if args.stream_receiver is not None else None
    native = args.native_client.resolve() if args.native_client is not None else None
    sys.exit(run(args.fixture.resolve(), args.routing.resolve(), args.package_root.resolve(),
                 receiver, native, args.no_outputs))
