#!/usr/bin/env python3
"""Exercise desktop-entry and MIME resolution inside isolated XDG directories."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "session"))
from part12_browser_vm import (APP_A, APP_B, HTML, HTTP, HTTP_URL, HTTPS, HTTPS_URL, MIME, XHTML,
                              expect_launch, prepare, set_config)


def check(executable, root):
    prepare(root)
    config = root / "config/seaside/config.toml"
    mime = root / "config" / MIME
    generic = root / "config/mimeapps.list"
    initial = mime.read_text()
    generic_initial = generic.read_bytes()
    desktop_entries = root / "data/applications"
    vendor_entries = desktop_entries / "vendor"
    vendor_entries.mkdir()
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    shared = root / "share"
    shared.mkdir()
    (shared / "mime").symlink_to("/usr/share/mime", target_is_directory=True)
    env = {**os.environ, "HOME": str(root), "XDG_CONFIG_HOME": str(root / "config"),
           "XDG_DATA_HOME": str(root / "data"), "XDG_CONFIG_DIRS": str(root / "etc"),
           "XDG_DATA_DIRS": str(root / "share"), "XDG_RUNTIME_DIR": str(runtime),
           "DISPLAY": ":77", "XDG_CURRENT_DESKTOP": "Seaside", "GIO_USE_VFS": "local"}
    for key in ("BROWSER", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "GIO_USE_PORTALS",
                "DE", "DESKTOP_SESSION"):
        env.pop(key, None)

    def launch(expected, command, *args):
        expect_launch(root, expected, lambda: subprocess.run([*command, *map(str, args)], env=env,
                                                            check=True, timeout=5))

    def assert_default(mime_type, expected, environment=env):
        result = subprocess.check_output(["xdg-mime", "query", "default", mime_type], env=environment,
                                         text=True, timeout=5).strip()
        assert result == expected, (mime_type, result, expected)

    with subprocess.Popen([executable, config, mime], env=env, text=True, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE) as session:
        def apply(requested=None):
            if requested is not None:
                set_config(root, requested)
            session.stdin.write("apply\n")
            session.stdin.flush()
            return json.loads(session.stdout.readline())

        def expect(state, expected_https, requested=None):
            status = apply(requested)
            assert status["state"] == state, status
            assert status["effective"][HTTPS] == expected_https, status
            return status

        expect("applied", APP_A)
        assert f"{HTTPS}={APP_A};{APP_B};" in mime.read_text()
        assert f"text/plain={APP_B};" in mime.read_text()
        assert "# Existing desktop defaults" in mime.read_text()
        assert_default(HTTP, APP_A)
        assert_default(HTTPS, APP_A)
        assert_default(HTML, APP_A)
        assert_default(XHTML, APP_A)
        host_mime_env = {**env, "XDG_CURRENT_DESKTOP": "Other"}
        assert_default(HTTPS, APP_B, host_mime_env)
        assert_default(HTML, APP_B, host_mime_env)
        html = root / "page with spaces.html"
        launch(["A", HTTPS_URL], ["xdg-open"], HTTPS_URL)
        launch(["A", HTTP_URL], ["gio", "open"], HTTP_URL)
        launch(["A", str(html)], ["gio", "open"], html)

        expect("applied", APP_B, APP_B)
        assert f"{XHTML}=" not in mime.read_text()
        launch(["B", HTTPS_URL], ["gio", "open"], HTTPS_URL)
        launch(["B", str(html)], ["xdg-open"], html)

        before = mime.read_bytes()
        assert apply("firefox --private-window") == {"error": "applications.default_browser"}
        assert mime.read_bytes() == before
        expect("applied", APP_A, "firefox")
        expect("unavailable", APP_B, "missing.desktop")
        assert mime.read_text().strip() == initial.strip()

        expect("applied", APP_A, APP_A)
        # A newer external change to one of our keys must survive another
        # reload of the same selection and its eventual release.
        mime.write_text(mime.read_text().replace(f"{HTTPS}={APP_A};{APP_B};\n",
                                                f"{HTTPS}={APP_B};\n", 1))
        expect("external-override", APP_B)
        expect("unavailable", APP_B, "missing.desktop")
        assert mime.read_text().strip() == initial.strip()
        assert generic.read_bytes() == generic_initial

        # An app installed in an existing subdirectory must resolve on the
        # next reload even when the session has already looked up defaults.
        (vendor_entries / "browser.desktop").write_bytes((desktop_entries / APP_A).read_bytes())
        expect("applied", "vendor-browser.desktop", "vendor-browser.desktop")

        mime.unlink()
        mime.symlink_to(generic)
        expect("mimeapps-error", APP_B, APP_A)
        assert mime.is_symlink()
        assert generic.read_bytes() == generic_initial
        session.stdin.close()
        assert session.wait(timeout=5) == 0


if __name__ == "__main__":
    if any(shutil.which(command) is None for command in ("gio", "xdg-open", "xdg-mime",
                                                       "update-desktop-database")):
        sys.exit(77)
    with tempfile.TemporaryDirectory() as directory:
        check(sys.argv[1], Path(directory) / "browser")
