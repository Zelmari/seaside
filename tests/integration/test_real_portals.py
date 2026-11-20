#!/usr/bin/env python3
"""Run the packaged portals and grim on a private bus and headless Seaside display."""

import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time


def run(fixture, routing, package_root):
    lib = package_root / "lib"
    data = package_root / "share"
    definitions = [
        ("permissions", lib / "xdg-permission-store", "org.freedesktop.impl.portal.PermissionStore"),
        ("wlr", lib / "xdg-desktop-portal-wlr", "org.freedesktop.impl.portal.desktop.wlr"),
        ("gtk", lib / "xdg-desktop-portal-gtk", "org.freedesktop.impl.portal.desktop.gtk"),
        ("frontend", lib / "xdg-desktop-portal", "org.freedesktop.portal.Desktop"),
    ]
    grim = package_root / "bin/grim"
    if not all(binary.is_file() for binary in [grim, *(row[1] for row in definitions)]):
        print("Real portal smoke requires the Arch frontend, wlr, GTK and grim packages")
        return 77
    with tempfile.TemporaryDirectory(prefix="seaside-portals-") as name:
        root = Path(name)
        address = f"unix:path={root / 'bus'}"
        env = dict(os.environ)
        for key in list(env):
            if key.startswith(("XDG_", "PIPEWIRE_", "SPA_", "DBUS_")):
                del env[key]
        for key in ("DISPLAY", "WAYLAND_SOCKET", "LD_PRELOAD"):
            env.pop(key, None)
        portal_dir = root / "portals"
        portal_dir.mkdir()
        for source in (data / "xdg-desktop-portal/portals").glob("*.portal"):
            if source.name in ("gtk.portal", "wlr.portal"):
                shutil.copyfile(source, portal_dir / source.name)
        shutil.copyfile(routing, portal_dir / "seaside-portals.conf")
        env.update(HOME=name, XDG_RUNTIME_DIR=name, XDG_CURRENT_DESKTOP="Seaside",
                   XDG_SESSION_TYPE="wayland", XDG_DESKTOP_PORTAL_DIR=str(portal_dir),
                   XDG_DATA_DIRS=str(data), GDK_BACKEND="wayland", NO_AT_BRIDGE="1",
                   GSETTINGS_BACKEND="memory", DBUS_SESSION_BUS_ADDRESS=address,
                   DBUS_SYSTEM_BUS_ADDRESS=f"unix:path={root / 'no-system-bus'}",
                   WAYLAND_DISPLAY="seaside-portal-smoke")

        package_env = dict(env)
        if package_root != Path("/usr"):
            package_env.update(LD_LIBRARY_PATH=str(lib),
                               GSETTINGS_SCHEMA_DIR=str(data / "glib-2.0/schemas"))
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
        children = []

        def start(label, command, *, child_env=env, handshake=False):
            log = root / f"{label}.log"
            with log.open("w") as stream:
                child = subprocess.Popen(command, env=child_env, stdout=stream,
                                         stderr=subprocess.STDOUT,
                                         stdin=subprocess.PIPE if handshake else subprocess.DEVNULL)
            children.append((child, log))
            return child

        def wait_for(check, *required):
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline and all(p.poll() is None for p in required):
                if check():
                    return
                time.sleep(.025)
            raise RuntimeError("Timed out waiting for a fixture service")

        def name_exists(service):
            reply = subprocess.run(
                ["busctl", "--address", address, "call", "org.freedesktop.DBus",
                 "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", "s", service],
                env=env, capture_output=True, text=True, timeout=1)
            return reply.stdout.strip() == "b true"

        try:
            bus = start("bus", ["dbus-daemon", "--nofork", f"--config-file={root / 'bus.conf'}"])
            pw = start("pipewire", ["pipewire"], child_env=dict(env, PIPEWIRE_CONFIG_DIR=name))
            wait_for(lambda: (root / "bus").is_socket() and (root / "pipewire-0").is_socket(), bus, pw)
            compositor = start("compositor", [fixture, address], handshake=True)
            wait_for(lambda: (root / env["WAYLAND_DISPLAY"]).is_socket(), compositor)
            for label, executable, service in definitions:
                child = start(label, [executable], child_env=package_env)
                wait_for(lambda: name_exists(service), child)
            screenshot = root / "cursor.png"
            subprocess.run([grim, "-c", screenshot], env=package_env, check=True, timeout=4)
            png = screenshot.read_bytes()
            if png[:8] != b"\x89PNG\r\n\x1a\n" or struct.unpack(">II", png[16:24]) != (160, 90):
                raise RuntimeError("grim produced an unexpected PNG size")
            compositor.stdin.write(b"\n")
            compositor.stdin.flush()
            if compositor.wait(timeout=9):
                raise RuntimeError("Real portal readiness check failed")
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
    sys.exit(run(sys.argv[1], sys.argv[2], Path(sys.argv[3]) if len(sys.argv) > 3 else Path("/usr")))
