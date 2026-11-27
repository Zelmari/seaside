#!/usr/bin/env python3
"""Prepare isolated browser stubs and verify them in a running Seaside session."""

import argparse
import ctypes as c
import json
import os
from pathlib import Path
import shlex
import subprocess
import time


HTTP = "x-scheme-handler/http"
HTTPS = "x-scheme-handler/https"
HTML = "text/html"
XHTML = "application/xhtml+xml"
APP_A = "firefox.desktop"
APP_B = "seaside-browser-b.desktop"
HTTPS_URL = "https://example.test/seaside?one=a%20b&two=%2F#fragment"
HTTP_URL = HTTPS_URL.replace("https:", "http:")
AUTOSTART_URL = HTTPS_URL.replace("/seaside?", "/autostart?")
MIME = "seaside-mimeapps.list"


def prepare(root):
    root.mkdir()
    applications = root / "data/applications"
    applications.mkdir(parents=True)
    (root / "config/seaside").mkdir(parents=True)
    stub = root / "browser-stub"
    stub.write_text("""#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
fd = os.open(Path(__file__).parent / "launches.jsonl", os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
os.write(fd, (json.dumps(sys.argv[1:]) + "\\n").encode())
os.close(fd)
""")
    stub.chmod(0o700)
    for desktop_id, label, types in (
        (APP_A, "A", (HTTP, HTTPS, HTML, XHTML)),
        (APP_B, "B", (HTTP, HTTPS, HTML, "text/plain")),
    ):
        executable = str(stub)
        if any(char not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_.-"
               for char in executable):
            raise ValueError("Use a fixture path made of letters, digits, underscores, dots and hyphens")
        (applications / desktop_id).write_text(
            f'[Desktop Entry]\nType=Application\nName=Seaside Browser {label}\n'
            f'Exec={executable} {label} %U\nMimeType={";".join(types)};\n'
        )
    subprocess.run(["update-desktop-database", str(applications)], check=True, timeout=5)
    (root / "config" / MIME).write_text(
        f"# Existing desktop defaults\n[Default Applications]\n{HTTPS}={APP_B};\n"
        f"text/plain={APP_B};\n"
    )
    (root / "config/mimeapps.list").write_text(
        f"[Default Applications]\n{HTTP}={APP_B};\n{HTTPS}={APP_B};\n{HTML}={APP_B};\n"
    )
    (root / "page with spaces.html").write_text("<!doctype html><title>Seaside browser test</title>\n")
    (root / "page.xhtml").write_text('<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"/>')
    set_config(root, "firefox")


def set_config(root, browser):
    file = root / "config/seaside/config.toml"
    temporary = file.with_suffix(".new")
    autostart = json.dumps(f"xdg-open {shlex.quote(AUTOSTART_URL)}")
    temporary.write_text(
        f"[autostart]\ncommands = [{autostart}]\n[applications]\ndefault_browser = {json.dumps(browser)}\n"
    )
    temporary.replace(file)


def launches(root):
    log = root / "launches.jsonl"
    return [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []


def wait_until(check, description, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if result := check():
            return result
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {description}")


def expect_launch(root, expected, command):
    before = len(launches(root))
    command()
    wait_until(lambda: len(launches(root)) > before, f"browser argument {expected!r}")
    actual = launches(root)[before:]
    assert actual == [expected], (actual, expected)
    print(f"Launched {actual[0]!r}", flush=True)


def open_via(command, target):
    subprocess.run([*command, str(target)], check=True, timeout=5)


def status():
    output = subprocess.check_output(["seasidectl", "status"], text=True, timeout=5)
    return json.loads(output)["result"]


def expect_browser(requested, state, https_app):
    def matches():
        current = status()["applications"]["default_browser"]
        if current["requested"] == requested and current["state"] == state:
            return current
        return None

    current = wait_until(matches, f"browser {requested} ({state})")
    assert current["effective"][HTTPS] == https_app, current
    print(f"Browser status {current!r}", flush=True)


def verify(root, portal):
    assert os.environ.get("XDG_CURRENT_DESKTOP") == "Seaside"
    assert os.environ.get("XDG_CONFIG_HOME") == str(root / "config")
    expect_browser("firefox", "applied", APP_A)
    wait_until(lambda: ["A", AUTOSTART_URL] in launches(root), "autostart xdg-open")
    print(f"Autostart launched {launches(root)!r}", flush=True)
    expect_launch(root, ["A", HTTP_URL], lambda: open_via(["xdg-open"], HTTP_URL))
    expect_launch(root, ["A", HTTPS_URL], lambda: open_via(["gio", "open"], HTTPS_URL))
    local = root / "page with spaces.html"
    expect_launch(root, ["A", str(local)], lambda: open_via(["gio", "open"], local))
    if portal:
        expect_launch(root, ["A", HTTPS_URL], lambda: portal_open(HTTPS_URL, False))
    set_config(root, APP_B)
    expect_browser(APP_B, "applied", APP_B)
    expect_launch(root, ["B", HTTPS_URL], lambda: open_via(["xdg-open"], HTTPS_URL))
    expect_launch(root, ["B", str(local)], lambda: open_via(["xdg-open"], local))
    if portal:
        expect_launch(root, ["B", HTTPS_URL], lambda: portal_open(HTTPS_URL, False))
    set_config(root, "seaside-browser-missing.desktop")
    expect_browser("seaside-browser-missing.desktop", "unavailable", APP_B)
    original = root / "config" / MIME
    assert f"{HTTPS}={APP_B};" in original.read_text()
    assert f"text/plain={APP_B};" in original.read_text()
    set_config(root, "firefox")
    expect_browser("firefox", "applied", APP_A)
    set_config(root, "firefox --flag")
    wait_until(lambda: status()["config"]["error"], "rejected browser configuration")
    expect_browser("firefox", "applied", APP_A)
    set_config(root, APP_B)
    expect_browser(APP_B, "applied", APP_B)
    wait_until(lambda: not status()["config"]["error"], "config error recovery")
    other = {**os.environ, "XDG_CURRENT_DESKTOP": "Other"}
    generic = subprocess.check_output(["xdg-mime", "query", "default", HTML], env=other, text=True,
                                      timeout=5).strip()
    assert generic == APP_B, generic
    print("Browser session checks passed", flush=True)


def portal_open(uri, ask):
    # Keep one D-Bus connection open from the method call through the Response
    # signal. A one-shot gdbus call closes the requester connection too early.
    gio = c.CDLL("libgio-2.0.so.0")
    ptr, string, uint = c.c_void_p, c.c_char_p, c.c_uint
    callback_type = c.CFUNCTYPE(None, ptr, string, string, string, string, ptr, ptr)

    def symbol(name, result, *arguments):
        call = getattr(gio, name)
        call.restype = result
        call.argtypes = arguments
        return call

    connect = symbol("g_bus_get_sync", ptr, c.c_int, ptr, ptr)
    unique_name = symbol("g_dbus_connection_get_unique_name", string, ptr)
    subscribe = symbol("g_dbus_connection_signal_subscribe", uint, ptr, string, string, string,
                       string, string, uint, callback_type, ptr, ptr)
    call = symbol("g_dbus_connection_call_sync", ptr, ptr, string, string, string, string, ptr,
                  ptr, uint, c.c_int, ptr, ptr)
    parse = symbol("g_variant_parse", ptr, ptr, string, ptr, ptr, ptr)
    take_variant = symbol("g_variant_take_ref", ptr, ptr)
    child = symbol("g_variant_get_child_value", ptr, ptr, c.c_size_t)
    get_uint = symbol("g_variant_get_uint32", uint, ptr)
    unref_variant = symbol("g_variant_unref", None, ptr)
    iterate = symbol("g_main_context_iteration", c.c_int, ptr, c.c_int)
    unref = symbol("g_object_unref", None, ptr)
    unsubscribe = symbol("g_dbus_connection_signal_unsubscribe", None, ptr, uint)
    response = []

    @callback_type
    def received(_connection, _sender, _path, _interface, _signal, parameters, _data):
        code = child(parameters, 0)
        response.append(get_uint(code))
        unref_variant(code)

    connection = connect(2, None, None)
    if not connection:
        raise RuntimeError("Could not connect to the session bus")
    destination = b"org.freedesktop.portal.Desktop"
    sender = unique_name(connection)[1:].replace(b".", b"_")
    token = f"seaside_browser_{time.monotonic_ns()}"
    request = b"/org/freedesktop/portal/desktop/request/" + sender + b"/" + token.encode()
    subscription = subscribe(connection, destination, b"org.freedesktop.portal.Request",
                             b"Response", request, None, 0, received, None, None)
    try:
        parameters = parse(
            None, (f"('', {json.dumps(uri)}, {{'handle_token': <'{token}'>, "
                   f"'ask': <{'true' if ask else 'false'}>}})").encode(), None, None, None
        )
        if not parameters:
            raise RuntimeError("Could not encode the OpenURI request")
        take_variant(parameters)
        print(f"Waiting for OpenURI with ask={ask}", flush=True)
        try:
            reply = call(connection, destination, b"/org/freedesktop/portal/desktop",
                         b"org.freedesktop.portal.OpenURI", b"OpenURI", parameters,
                         None, 0, 5000, None, None)
        finally:
            unref_variant(parameters)
        if not reply:
            raise RuntimeError("OpenURI call failed")
        unref_variant(reply)

        def poll():
            while iterate(None, 0):
                pass
            return response

        wait_until(poll, "OpenURI Response signal", timeout=45)
        assert response == [0], f"OpenURI returned {response}"
        print("OpenURI returned success", flush=True)
    finally:
        unsubscribe(connection, subscription)
        unref(connection)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "verify", "portal"))
    parser.add_argument("root", type=Path)
    parser.add_argument("--portal", action="store_true", help="exercise unprompted OpenURI in verify")
    parser.add_argument("--ask", action="store_true", help="request the portal app chooser")
    parser.add_argument("--expected", choices=("A", "B"), default="A")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.mode == "prepare":
        prepare(root)
        print(f"XDG_CONFIG_HOME={shlex.quote(str(root / 'config'))} "
              f"XDG_DATA_HOME={shlex.quote(str(root / 'data'))} "
              f"XDG_STATE_HOME={shlex.quote(str(root / 'state'))} WLR_RENDERER=pixman seaside")
    elif args.mode == "verify":
        verify(root, args.portal)
    else:
        expect_launch(root, [args.expected, HTTPS_URL], lambda: portal_open(HTTPS_URL, args.ask))


if __name__ == "__main__":
    main()
