"""Transfer clipboard and primary-selection data between GTK Wayland and X11 clients."""

import ctypes as c
from contextlib import contextmanager
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import tempfile
import time


def bind(library, name, result, *arguments):
    function = getattr(library, name)
    function.restype = result
    function.argtypes = arguments
    return function


def client(title, mapped=True):
    gtk = c.CDLL("libgtk-3.so.0")
    gdk = c.CDLL("libgdk-3.so.0")
    glib = c.CDLL("libglib-2.0.so.0")
    ptr, word, num = c.c_void_p, c.c_char_p, c.c_int
    if not bind(gtk, "gtk_init_check", num, ptr, ptr)(None, None):
        raise RuntimeError(f"GTK could not open {os.environ['GDK_BACKEND']} display")
    window = bind(gtk, "gtk_window_new", ptr, num)(0)
    bind(gtk, "gtk_window_set_title", None, ptr, word)(window, title.encode())
    bind(gtk, "gtk_window_set_default_size", None, ptr, num, num)(window, 240, 120)
    bind(gtk, "gtk_window_set_decorated", None, ptr, num)(window, 0)
    if mapped:
        bind(gtk, "gtk_widget_show_all", None, ptr)(window)
    clipboard_get = bind(gtk, "gtk_clipboard_get", ptr, ptr)
    intern = bind(gdk, "gdk_atom_intern_static_string", ptr, word)
    selections = [clipboard_get(intern(name)) for name in (b"CLIPBOARD", b"PRIMARY")]
    if not all(selections):
        raise RuntimeError("GTK did not offer both clipboard and primary-selection objects")
    active = bind(gtk, "gtk_window_is_active", num, ptr)
    # Detect a transient focus loss even if the native window regains focus
    # before the external clipboard process finishes its transfer.
    changes = 0
    notify = c.CFUNCTYPE(None, ptr, ptr, ptr)

    @notify
    def activation_changed(widget, _spec, _data):
        nonlocal changes
        if not active(widget):
            changes += 1

    gobject = c.CDLL("libgobject-2.0.so.0")
    bind(gobject, "g_signal_connect_data", c.c_ulong, ptr, word, notify, ptr, ptr, num)(
        window, b"notify::is-active", activation_changed, None, None, 0)
    set_text = bind(gtk, "gtk_clipboard_set_text", None, ptr, word, num)
    wait_text = bind(gtk, "gtk_clipboard_wait_for_text", ptr, ptr)
    g_free = bind(glib, "g_free", None, ptr)
    display = bind(gdk, "gdk_display_get_default", ptr)()
    sync = bind(gdk, "gdk_display_sync", None, ptr)
    quit_loop = bind(gtk, "gtk_main_quit", None)
    watch = c.CFUNCTYPE(num, num, c.c_uint, ptr)
    pending = b""

    @watch
    def on_command(fd, _condition, _data):
        nonlocal pending
        try:
            chunk = os.read(fd, 4096)
            if not chunk:
                quit_loop()
                return 0
            pending += chunk
            line, newline, rest = pending.partition(b"\n")
            if not newline:
                return 1
            pending = rest
            operation, *values = json.loads(line)
            if operation == "active":
                result = bool(active(window))
            elif operation == "focus_status":
                result = [bool(active(window)), changes]
            elif operation == "offer":
                for selection, text in zip(selections, values, strict=True):
                    set_text(selection, text.encode(), -1)
                sync(display)
                result = True
            elif operation == "read":
                result = []
                for selection in selections:
                    text = wait_text(selection)
                    try:
                        result.append(c.string_at(text).decode() if text else None)
                    finally:
                        g_free(text)
            else:
                raise RuntimeError(f"Unknown clipboard operation {operation!r}")
            print(json.dumps(result), flush=True)
            return 1
        except Exception as error:
            print(json.dumps({"error": str(error)}), flush=True)
            quit_loop()
            return 0

    # G_IO_IN | G_IO_HUP. A single request is outstanding at any time.
    bind(glib, "g_unix_fd_add", c.c_uint, num, c.c_uint, watch, ptr)(0, 17, on_command, None)
    print(json.dumps("ready"), flush=True)
    bind(gtk, "gtk_main", None)()


class ClipboardClient:
    def __init__(self, environment, backend, title, *, mapped=True):
        self.title = title
        command = [sys.executable, str(Path(__file__).resolve()), title]
        if not mapped:
            command.append("--unmapped")
        self.process = subprocess.Popen(
            command,
            env=dict(environment, GDK_BACKEND=backend), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, bufsize=0)

    def __enter__(self):
        try:
            if self.receive() != "ready":
                raise RuntimeError(f"{self.title} failed to initialize its clipboard")
            return self
        except Exception:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)
        finally:
            self.process.stdout.close()

    def receive(self):
        deadline = time.monotonic() + 5
        reply = b""
        with selectors.DefaultSelector() as ready:
            ready.register(self.process.stdout, selectors.EVENT_READ)
            while not reply.endswith(b"\n") and ready.select(max(0, deadline - time.monotonic())):
                chunk = os.read(self.process.stdout.fileno(), 4096)
                if not chunk:
                    raise RuntimeError(f"{self.title} exited before replying")
                reply += chunk
                if len(reply) > 4096:
                    raise RuntimeError(f"{self.title} returned an oversized clipboard reply")
        if not reply.endswith(b"\n"):
            raise RuntimeError(f"{self.title} did not service its clipboard within five seconds")
        result = json.loads(reply)
        if isinstance(result, dict):
            raise RuntimeError(f"{self.title}: {result['error']}")
        return result

    def command(self, *parameters):
        self.process.stdin.write(json.dumps(parameters).encode() + b"\n")
        return self.receive()


def select(focus, client):
    focus(client.title)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if client.command("active"):
            return
        time.sleep(.025)
    raise RuntimeError(f"{client.title} never received keyboard focus")


def transfer(focus, source, target, payload):
    latest = None
    for _ in range(3):
        select(focus, source)
        if not source.command("offer", *payload):
            raise RuntimeError(f"{source.title} could not offer its selection")
        select(focus, target)
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            latest = target.command("read")
            if latest == payload:
                return
            time.sleep(.025)
    raise RuntimeError(f"{source.title} to {target.title} selection transfer returned {latest!r}")


def background_selections(environment, native, *, cold=False):
    """Transfer both X11 selections while a native Wayland window holds keyboard focus."""
    missing = [tool for tool in ("xclip", "wl-copy", "wl-paste")
               if not shutil.which(tool, path=environment.get("PATH"))]
    if missing:
        return {"status": "unavailable", "reason": "Missing clipboard tools: " + ", ".join(missing)}

    def matches(command, expected, status=0):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                read = subprocess.run(command, env=environment, capture_output=True, timeout=1)
                if read.returncode == status and read.stdout == expected:
                    return True
            except subprocess.TimeoutExpired:
                pass
            time.sleep(.025)
        return False

    @contextmanager
    def offer(command, payload):
        with tempfile.TemporaryFile() as data:
            data.write(payload)
            data.seek(0)
            writer = subprocess.Popen(command, env=environment, stdin=data,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                yield
            finally:
                if writer.poll() is None:
                    writer.terminate()
                try:
                    writer.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    writer.kill()
                    writer.wait(timeout=2)

    initial_focus = native.command("focus_status")
    if not initial_focus[0]:
        raise RuntimeError("The native window did not have keyboard focus before the transfer")
    results = {}
    selections = (("clipboard", []), ("primary", ["--primary"]))
    for name, options in selections:
        for kind in (("text",) if cold else ("text", "binary")):
            mime = ["--type", "application/octet-stream"] if kind == "binary" else []
            target = ["-target", "application/octet-stream"] if mime else []
            data = bytes(range(256)) * 2048 if mime else " ✓".encode()
            xwrite = ["xclip", "-quiet", "-selection", name, *target, "-in"]
            xread = ["xclip", "-selection", name, *target, "-out"]
            wlwrite = ["wl-copy", "--foreground", *options, *mime]
            wlread = ["wl-paste", "--no-newline", *options, *mime]
            first = f"Wayland {name} ".encode() + data
            second = f"X11 {name} ".encode() + data
            third = f"Wayland replacement {name} ".encode() + data
            transfers = {}
            with offer(wlwrite, first):
                if not matches(wlread, first):
                    raise RuntimeError(f"Native Wayland {name} {kind} control failed")
                transfers["wayland_to_x11"] = matches(xread, first)
                with offer(xwrite, second):
                    if not matches(xread, second):
                        raise RuntimeError(f"Native X11 {name} {kind} control failed")
                    transfers["x11_to_wayland"] = matches(wlread, second)
                    # Replace a live X11 owner. A stale bridge must not keep
                    # serving that owner's bytes after the new Wayland offer.
                    with offer(wlwrite, third):
                        if not matches(wlread, third):
                            raise RuntimeError(f"Replacing the native {name} {kind} offer failed")
                        transfers["replaced_x11_owner"] = matches(xread, third)
            # Wait for source destruction to reach both protocols before
            # publishing the next offer. This catches an X11 ownership claim
            # with an older timestamp than the preceding release.
            transfers["cleared_wayland"] = matches(["wl-paste", "--list-types", *options], b"", 1)
            transfers["cleared_x11"] = matches(["xclip", "-selection", name, "-out", "-target", "TARGETS"], b"", 1)
            results[f"{name}_{kind}"] = transfers
    if not cold:
        # A never-mapped GTK/X11 client must be able to read and offer while
        # the native window remains active. Check both GTK operations against
        # an X11-only peer before attributing a failure to the bridge.
        with ClipboardClient(environment, "x11", "Part 7 unmapped X11", mapped=False) as gtk:
            for index, (name, options) in enumerate(selections):
                row = {}
                offers = ["GTK clipboard", "GTK primary"]
                gtk.command("offer", *offers)
                xread = ["xclip", "-selection", name, "-out"]
                wlread = ["wl-paste", "--no-newline", *options]
                row["gtk_to_xclip"] = matches(xread, offers[index].encode())
                row["gtk_to_wayland"] = matches(wlread, offers[index].encode())
                for provider, command in (("xclip", ["xclip", "-quiet", "-selection", name, "-in"]),
                                          ("wayland", ["wl-copy", "--foreground", *options])):
                    payload = f"{provider} to unmapped GTK {name}"
                    with offer(command, payload.encode()):
                        if not matches(xread, payload.encode()):
                            row[provider + "_to_gtk"] = False
                            continue
                        deadline = time.monotonic() + 2
                        received = gtk.command("read")[index]
                        while received != payload and time.monotonic() < deadline:
                            time.sleep(.025)
                            received = gtk.command("read")[index]
                        row[provider + "_to_gtk"] = received == payload
                results[f"{name}_unmapped_gtk"] = row
    final_focus = native.command("focus_status")
    if final_focus != initial_focus:
        raise RuntimeError(f"Native focus changed during the background transfer: {initial_focus} -> {final_focus}")
    return {"status": "pass" if all(all(row.values()) for row in results.values()) else "failed",
            "transfers": results, "keyboard_focus_unchanged": True}


def run_clipboard(environment, focus):
    """Verify both directions using the supplied session and focus-by-title callable."""
    with ClipboardClient(environment, "wayland", "Part 7 Wayland") as native:
        with ClipboardClient(environment, "x11", "Part 7 X11") as legacy:
            transfer(focus, native, legacy, ["Wayland clipboard ✓", "Wayland PRIMARY ✓"])
            transfer(focus, legacy, native, ["X11 clipboard ✓", "X11 PRIMARY ✓"])
        select(focus, native)
        background = background_selections(environment, native)
    return {"wayland_to_x11": "pass", "x11_to_wayland": "pass", "primary_selection": "pass",
            "background": background}


if __name__ == "__main__":
    client(sys.argv[1], "--unmapped" not in sys.argv[2:])
