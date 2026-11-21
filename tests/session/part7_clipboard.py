"""Transfer clipboard and primary-selection data between GTK Wayland and X11 clients."""

import ctypes as c
from contextlib import ExitStack
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import time


def bind(library, name, result, *arguments):
    function = getattr(library, name)
    function.restype = result
    function.argtypes = arguments
    return function


def client(title):
    gtk = c.CDLL("libgtk-3.so.0")
    gdk = c.CDLL("libgdk-3.so.0")
    glib = c.CDLL("libglib-2.0.so.0")
    ptr, word, num = c.c_void_p, c.c_char_p, c.c_int
    if not bind(gtk, "gtk_init_check", num, ptr, ptr)(None, None):
        raise RuntimeError(f"GTK could not open {os.environ['GDK_BACKEND']} display")
    window = bind(gtk, "gtk_window_new", ptr, num)(0)
    bind(gtk, "gtk_window_set_title", None, ptr, word)(window, title.encode())
    bind(gtk, "gtk_window_set_default_size", None, ptr, num, num)(window, 240, 120)
    bind(gtk, "gtk_widget_show_all", None, ptr)(window)
    clipboard_get = bind(gtk, "gtk_clipboard_get", ptr, ptr)
    intern = bind(gdk, "gdk_atom_intern_static_string", ptr, word)
    selections = [clipboard_get(intern(name)) for name in (b"CLIPBOARD", b"PRIMARY")]
    if not all(selections):
        raise RuntimeError("GTK did not offer both clipboard and primary-selection objects")
    active = bind(gtk, "gtk_window_is_active", num, ptr)
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
    def __init__(self, environment, backend, title):
        self.title = title
        self.process = subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve()), title],
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


def run_clipboard(environment, focus):
    """Verify both directions using the supplied session and focus-by-title callable."""
    with ExitStack() as clients:
        native = clients.enter_context(ClipboardClient(environment, "wayland", "Part 7 Wayland"))
        legacy = clients.enter_context(ClipboardClient(environment, "x11", "Part 7 X11"))
        transfer(focus, native, legacy, ["Wayland clipboard ✓", "Wayland PRIMARY ✓"])
        transfer(focus, legacy, native, ["X11 clipboard ✓", "X11 PRIMARY ✓"])
    return {"wayland_to_x11": "pass", "x11_to_wayland": "pass", "primary_selection": "pass"}


if __name__ == "__main__":
    client(sys.argv[1])
