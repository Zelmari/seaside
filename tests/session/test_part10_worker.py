"""Verify the packaged-session worker can assemble its shared trial code."""

from pathlib import Path
import unittest
from unittest.mock import patch

import part10_vm


class Part10WorkerTests(unittest.TestCase):
    def test_shared_trials_load_without_console_or_process_io(self):
        directory = Path(__file__).parent
        revision = "a" * 40
        package = dict(name="seaside.pkg.tar.zst", sha256="1" * 64, size=123, preinstalled=True)
        placeholders = dict.fromkeys(("event", "wait_for", "upload", "run_logged", "stop"))
        standard = dict(placeholders, __name__="seaside_vm_guest", BASE="test", REVISION=revision, MODE="tty")
        worker = dict(__name__="seaside_guest_helpers", standard=standard)

        def guest_source(requested, name):
            self.assertEqual(requested, revision)
            return (directory / name).read_text()

        with (patch.object(part10_vm, "guest_source", side_effect=guest_source),
              patch("os.ttyname", side_effect=AssertionError("Loading a helper must not open the console")),
              patch("subprocess.Popen", side_effect=AssertionError("Loading a helper must not start a process"))):
            source = part10_vm.worker_settings(revision, package, None, "pixman")
            source += guest_source(revision, "part10_vm_guest.py")
            exec(compile(source, "part10_vm_guest.py", "exec"), worker)

        self.assertTrue(callable(worker["trial"]))
        self.assertTrue(callable(worker["compatibility"]["run_audio"]))
        self.assertTrue(callable(worker["helpers"]["run_portal"]))
        self.assertTrue(callable(worker["recovery"]["stop_owned_portal"]))
        self.assertEqual(worker["part10_package"], package)


if __name__ == "__main__":
    unittest.main()
