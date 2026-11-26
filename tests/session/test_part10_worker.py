"""Verify the packaged-session worker can assemble its shared trial code."""

import hashlib
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import part10_vm


class Part10WorkerTests(unittest.TestCase):
    def load_worker(self):
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
        return worker, package

    def test_shared_trials_load_without_console_or_process_io(self):
        worker, package = self.load_worker()
        self.assertTrue(callable(worker["trial"]))
        self.assertTrue(callable(worker["compatibility"]["run_audio"]))
        self.assertTrue(callable(worker["helpers"]["run_portal"]))
        self.assertTrue(callable(worker["recovery"]["stop_owned_portal"]))
        self.assertEqual(worker["part10_package"], package)

    def test_package_metadata_accepts_release_and_matching_development_versions(self):
        worker, _ = self.load_worker()
        package = worker["part10_package"]
        revision = worker["REVISION"]
        with TemporaryDirectory() as temporary:
            results = Path(temporary)
            archive = results / package["name"]
            payload = b"candidate payload"
            archive.write_bytes(payload)
            package.update(size=len(payload), sha256=hashlib.sha256(payload).hexdigest())
            for version, accepted in (("1.0.0-1", True),
                                      (f"1.0.0dev.r17.g{revision}-1", True),
                                      (f"1.0.0dev.r17.g{'b' * 40}-1", False),
                                      ("", False)):
                with self.subTest(version=version):
                    def checked(args, *, env):
                        arch_field = "arch" if args[-1] == ".PKGINFO" else "pkgarch"
                        return f"pkgname = seaside\n{arch_field} = x86_64\npkgver = {version}\n".encode()

                    worker["checked"] = checked
                    metadata = worker["package_metadata"]
                    if accepted:
                        self.assertEqual(metadata(archive, results, {}), version)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "Candidate version"):
                            metadata(archive, results, {})

    def test_installed_version_requires_exact_candidate_identity_in_both_commands(self):
        worker, package = self.load_worker()
        revision = worker["REVISION"]
        version = "1.0.0-1"
        self.assertIn(Path("/usr/bin/seaside-screenshot"), worker["PACKAGE_FILES"])
        with TemporaryDirectory() as temporary:
            results = Path(temporary)
            archive = results / package["name"]
            binaries = [results / name for name in ("seaside", "seasidectl")]
            worker.update(SEASIDE=binaries[0], CONTROL=binaries[1], PACKAGE_FILES=tuple(binaries))
            payload = b"pacman-owned candidate file"
            for binary in binaries:
                binary.write_bytes(payload)
            for stale in (None, *binaries):
                with self.subTest(stale=stale):
                    queried = []

                    def checked(args, *, env):
                        if args[:2] == ["/usr/bin/pacman", "-Q"]:
                            return f"seaside {version}\n".encode()
                        if args[:2] == ["/usr/bin/pacman", "-Ql"]:
                            return b"candidate manifest"
                        if args[:2] == ["/usr/bin/pacman", "-Qoq"]:
                            return (b"wofi\n" if args[-1] == "/usr/bin/wofi" else b"seaside\n")
                        if args[:2] == ["/usr/bin/bsdtar", "-xOf"]:
                            return payload
                        binary, flag = args
                        self.assertEqual(flag, "--version")
                        queried.append(binary)
                        built_revision = "b" * 40 if binary == stale else revision
                        return f"{binary.name} {version} (revision {built_revision})\n".encode()

                    worker["checked"] = checked
                    verify = worker["verify_installed"]
                    if stale is None:
                        verify(archive, version, results, {})
                        self.assertEqual(queried, binaries)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "does not report candidate version"):
                            verify(archive, version, results, {})
                        self.assertEqual(queried[-1], stale)

    def test_restored_mode_checks_each_dma_pool_other_descriptors_and_rss(self):
        worker, _ = self.load_worker()
        check = worker["restored_mode_resources"]
        cold = dict(dma_buffers={"drm:16384": 1, "drm:4096000": 2}, other_fds=81, VmRSS=30000)
        warm = dict(cold, dma_buffers={"drm:16384": 3, "drm:4096000": 2})
        self.assertTrue(check(cold, warm, warm))
        for leaking in (dict(warm, dma_buffers={"drm:16384": 4, "drm:4096000": 1}),
                        dict(warm, dma_buffers={"drm:1228800": 1}),
                        dict(warm, other_fds=84), dict(warm, VmRSS=30000 + 32 * 1024 + 1)):
            with self.subTest(sample=leaking):
                self.assertFalse(check(cold, warm, leaking))

    def test_playback_requires_recorded_pcm_when_producer_stays_active(self):
        for send_pcm in (True, False):
            with self.subTest(send_pcm=send_pcm), TemporaryDirectory() as temporary:
                compatibility = self.load_worker()[0]["compatibility"]
                processes = []
                stopped = []
                diagnostics = b"simulated producer still draining\n"
                state = {}

                def start(args, *, env, stdout, stderr, stdin=None):
                    process = SimpleNamespace(pid=len(processes) + 40, returncode=None,
                                              poll=lambda: None, stdout=stdout)
                    if args[0] == "pacat":
                        data = stdin.read(960)
                        self.assertEqual(len(data), 960)
                        os.write(stderr.fileno(), diagnostics)
                        if send_pcm:
                            os.write(processes[0].stdout.fileno(), data)
                    processes.append(process)
                    return process

                def stop(process):
                    process.returncode = -15
                    stopped.append(process)

                def wait_for(check, _, label, process):
                    if (result := check()):
                        return result
                    raise RuntimeError(f"Timed out waiting for {label}")

                def pulse(*args):
                    return json.dumps([dict(
                        source=4, index=7, corked=False,
                        properties={"application.process.id": str(processes[0].pid)})])

                compatibility["wait_for"] = wait_for
                compatibility["standard"]["stop"] = stop
                record = compatibility["record_playback_monitor"]
                session = SimpleNamespace(root=temporary, env={})
                with patch("subprocess.Popen", side_effect=start):
                    if send_pcm:
                        record(session, pulse, "fixture", "fixture.monitor", 4, state)
                        self.assertTrue(state["external_playback_monitor"]["pcm_observed"])
                    else:
                        with self.assertRaisesRegex(RuntimeError, "playback PCM was not observed"):
                            record(session, pulse, "fixture", "fixture.monitor", 4, state)
                        self.assertNotIn("external_playback_monitor", state)
                evidence = state["playback_monitor_evidence"]
                self.assertEqual(evidence["sample_count"], 480 if send_pcm else 0)
                self.assertEqual(evidence["producer_diagnostic"], diagnostics.decode())
                self.assertIsNone(evidence["producer_exit_before_cleanup"])
                self.assertEqual(stopped, processes[::-1])


if __name__ == "__main__":
    unittest.main()
