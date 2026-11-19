#!/usr/bin/env python3
"""Exercise observable process ownership, evidence and fixture lifecycle contracts."""

from contextlib import redirect_stdout
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader("seaside_test_runner", str(ROOT / "tools/run-tests"))
spec = importlib.util.spec_from_loader(loader.name, loader)
runner = importlib.util.module_from_spec(spec)
loader.exec_module(runner)


def process_running(pid):
    try:
        # An orphan may briefly await reaping by the guest's init process.
        return Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()[0] != "Z"
    except FileNotFoundError:
        return False


def wait_for_file(path, process, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            return
        if process.poll() is not None:
            raise AssertionError(f"Process exited before readiness: {process.returncode}")
        time.sleep(0.01)
    raise AssertionError("Process did not publish readiness")


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="seaside-runner-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def command(self, source, **kwargs):
        return runner.run_command(
            [sys.executable, "-c", source], cwd=self.root,
            log_path=self.root / "command.log", timeout=kwargs.pop("timeout", 3),
            terminate_grace=0.1, **kwargs,
        )

    def assert_processes_gone(self, pids):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if not any(process_running(pid) for pid in pids):
                return
            time.sleep(0.01)
        self.fail(f"Owned processes remain alive: {[pid for pid in pids if process_running(pid)]}")

    def test_command_failure_retains_diagnostic(self):
        result = self.command("import sys; print('the concrete failure', flush=True); sys.exit(9)")
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["returncode"], 9)
        self.assertIn("the concrete failure", Path(result["log"]).read_text())

    def test_unavailable_command_is_blocked(self):
        result = runner.run_command(
            [str(self.root / "not-installed")], cwd=self.root,
            log_path=self.root / "missing.log", timeout=1,
        )
        self.assertEqual(result["status"], "blocked")
        self.assertIsNone(result["returncode"])
        self.assertTrue(result["error"])

    def test_output_flood_is_drained_with_bounded_retention(self):
        result = self.command(
            "import os; os.write(1, b'first\\n' + b'x' * 500000); os.write(2, b'y' * 500000 + b'\\nlast\\n')",
            log_limit=1024,
        )
        self.assertEqual(result["status"], "pass")
        self.assertGreater(result["output_bytes"], 1000000)
        self.assertTrue(result["log_truncated"])
        output = Path(result["log"]).read_bytes()
        self.assertLessEqual(len(output), 1024)
        self.assertTrue(output.startswith(b"first\n"))
        self.assertTrue(output.endswith(b"last\n"))
        self.assertIn(b"[output truncated]", output)

    def test_tiny_log_limits_still_bound_a_noisy_failed_command(self):
        for limit in (1, len(b"\n[output truncated]\n")):
            with self.subTest(limit=limit):
                result = self.command("import sys; print('x' * 5000); sys.exit(3)", log_limit=limit)
                self.assertEqual(result["status"], "fail")
                self.assertTrue(result["log_truncated"])
                self.assertLessEqual(Path(result["log"]).stat().st_size, limit)

    def tree_source(self):
        # The descendants deliberately ignore TERM to exercise escalation.
        grandchild = "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(300)"
        child = (
            "import os,signal,subprocess,sys,time; "
            "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
            f"p=subprocess.Popen([sys.executable,'-c',{grandchild!r}]); "
            "open('pids','w').write(f'{os.getpid()} {p.pid}'); "
            "time.sleep(300)"
        )
        return f"import subprocess,sys; p=subprocess.Popen([sys.executable,'-c',{child!r}]); p.wait()"

    def test_timeout_kills_child_and_grandchild(self):
        result = self.command(self.tree_source(), timeout=0.75)
        self.assertTrue(result["timed_out"])
        self.assertEqual(result["status"], "fail")
        self.assertIsNone(result["cleanup_error"])
        pids = [int(pid) for pid in (self.root / "pids").read_text().split()]
        self.assertEqual(len(pids), 2)
        self.assert_processes_gone(pids)

    def test_success_also_cleans_background_processes(self):
        child = "import time; time.sleep(300)"
        result = self.command(
            "import subprocess,sys; "
            f"p=subprocess.Popen([sys.executable,'-c',{child!r}], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
            "open('pids','w').write(str(p.pid))"
        )
        self.assertEqual(result["status"], "pass")
        self.assert_processes_gone([int((self.root / "pids").read_text())])

    def test_short_lived_session_leader_cannot_hide_env_cleared_orphan(self):
        leaf = "import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(300)"
        intermediary = (
            "import os,subprocess,sys; os.setsid(); "
            f"p=subprocess.Popen([sys.executable,'-c',{leaf!r}], env={{}}); "
            "open('pids','w').write(str(p.pid)); os._exit(0)"
        )
        result = self.command(
            f"import subprocess,sys; subprocess.run([sys.executable,'-c',{intermediary!r}],check=True)",
            timeout=0.5,
        )
        self.assertTrue(result["timed_out"])
        self.assertIsNone(result["cleanup_error"])
        self.assert_processes_gone([int((self.root / "pids").read_text())])

    def test_preexisting_child_blocks_command_without_being_touched(self):
        process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(300)"], start_new_session=True)
        try:
            os.kill(process.pid, signal.SIGSTOP)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                if runner.process_identity(process.pid)[1] == "T":
                    break
                time.sleep(0.01)
            self.assertEqual(runner.process_identity(process.pid)[1], "T")
            result = self.command("raise AssertionError('must not execute')")
            self.assertEqual(result["status"], "blocked")
            self.assertIn("no pre-existing children", result["error"])
            self.assertEqual(runner.process_identity(process.pid)[1], "T")
            os.kill(process.pid, signal.SIGCONT)
            self.assertIsNone(process.poll(), "Runner touched an unrelated child")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2)

    @unittest.skipUnless(shutil.which("meson"), "Meson is required to test its separate-session cleanup")
    def test_outer_timeout_allows_meson_to_clean_its_test_session(self):
        (self.root / "meson.build").write_text(
            "project('runner-cancellation-check')\n"
            "test('owned-tree', find_program('python3'), args: files('tree.py'), timeout: 30)\n"
        )
        (self.root / "tree.py").write_text(self.tree_source())
        build = self.root / "build"
        configured = runner.run_command(
            ["meson", "setup", str(build), str(self.root)], cwd=self.root,
            log_path=self.root / "setup.log", timeout=15,
        )
        self.assertEqual(configured["status"], "pass", Path(configured["log"]).read_text())
        result = runner.run_command(
            ["meson", "test", "-C", str(build), "--no-rebuild"], cwd=self.root,
            log_path=self.root / "meson-command.log", timeout=1,
        )
        self.assertTrue(result["timed_out"])
        self.assertEqual(result["status"], "fail")
        self.assertIsNone(result["cleanup_error"])
        self.assert_processes_gone([int(pid) for pid in (build / "pids").read_text().split()])

    def test_interrupt_cleans_tree_and_returns_blocked_evidence(self):
        wrapper = (
            "import importlib.machinery,importlib.util,json,pathlib,sys; "
            f"loader=importlib.machinery.SourceFileLoader('runner',{str(ROOT / 'tools/run-tests')!r}); "
            "spec=importlib.util.spec_from_loader(loader.name,loader); "
            "runner=importlib.util.module_from_spec(spec); loader.exec_module(runner); "
            f"result=runner.run_command([sys.executable,'-c',{self.tree_source()!r}], "
            "cwd=pathlib.Path.cwd(),log_path=pathlib.Path('interrupt.log'),timeout=30,terminate_grace=.1); "
            "pathlib.Path('interrupted.json').write_text(json.dumps(result))"
        )
        process = subprocess.Popen([sys.executable, "-c", wrapper], cwd=self.root, start_new_session=True)
        try:
            wait_for_file(self.root / "pids", process)
            os.kill(process.pid, signal.SIGINT)
            self.assertEqual(process.wait(timeout=4), 0)
            result = json.loads((self.root / "interrupted.json").read_text())
            self.assertEqual(result["status"], "blocked")
            self.assertEqual(result["interrupted_signal"], signal.SIGINT)
            self.assertIsNone(result["cleanup_error"])
            self.assert_processes_gone([int(pid) for pid in (self.root / "pids").read_text().split()])
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)

    def meson_results(self, rows):
        path = self.root / "meson.json"
        path.write_text("".join(json.dumps(row) + "\n" for row in rows))
        return runner.parse_meson_results(path, self.root, 1024)

    def report(self, tests, commands=None):
        report = {
            "environment": {"classification": "test-fixture", "kernel": "test-only"},
            "commands": commands or [], "tests": tests,
        }
        runner.write_report(self.root, report)
        return json.loads((self.root / "summary.json").read_text())

    def test_meson_skip_is_distinct_and_remains_pending(self):
        tests = self.meson_results([
            {"name": "headless", "result": "OK", "stdout": "", "returncode": 0},
            {"name": "hardware", "result": "SKIP", "stdout": "No DRM device", "returncode": 77},
        ])
        report = self.report(tests)
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["counts"], {"pass": 1, "fail": 0, "skip": 1, "blocked": 0})
        self.assertEqual(report["pending_checks"][0]["test_id"], "hardware")
        self.assertEqual(report["pending_checks"][0]["reason"], "No DRM device")

    def test_empty_or_malformed_results_cannot_pass(self):
        empty = self.report(self.meson_results([]))
        self.assertEqual(empty["status"], "blocked")
        path = self.root / "meson.json"
        path.write_text('{"name": "broken",')
        malformed = self.report(runner.parse_meson_results(path, self.root, 1024))
        self.assertEqual(malformed["status"], "blocked")
        self.assertEqual(malformed["tests"][0]["test_id"], "meson-result-collection")

    def test_only_skipped_tests_do_not_establish_a_pass(self):
        report = self.report(self.meson_results([{"name": "hardware", "result": "SKIP"}]))
        self.assertEqual(report["status"], "blocked")

    def test_failed_command_is_not_masked_by_passed_tests(self):
        tests = self.meson_results([{"name": "headless", "result": "OK"}])
        command = {"id": "compile", "status": "fail", "command": ["meson", "compile"], "log": "compile.log"}
        self.assertEqual(self.report(tests, [command])["status"], "fail")

    def test_retained_raw_meson_artifacts_are_bounded_and_identified(self):
        source = self.root / "meson.json"
        source.write_bytes(b"start" + b"x" * 10000 + b"end")
        artifacts = runner.retain_meson_logs(self.root, 256)
        self.assertFalse(source.exists())
        retained = Path(artifacts[0]["path"])
        self.assertTrue(artifacts[0]["truncated"])
        self.assertTrue(retained.name.endswith(".truncated.log"))
        self.assertLessEqual(retained.stat().st_size, 256)

    def test_configuration_failure_still_reports_and_removes_private_state(self):
        binary_dir = self.root / "bin"
        binary_dir.mkdir()
        meson = binary_dir / "meson"
        runtime_path = self.root / "runtime-path"
        meson.write_text(
            f"#!{sys.executable}\nimport os,pathlib,sys\n"
            "assert 'WAYLAND_SOCKET' not in os.environ\n"
            f"pathlib.Path({str(runtime_path)!r}).write_text(os.environ['XDG_RUNTIME_DIR'])\n"
            "print('Missing deliberate test dependency')\nsys.exit(4)\n"
        )
        meson.chmod(0o700)
        tools = self.root / "tools"
        tools.mkdir()
        (tools / "record-baseline").write_text(
            "import json,pathlib,sys\n"
            "pathlib.Path(sys.argv[sys.argv.index('--output')+1]).write_text(json.dumps({'test_fixture': True}))\n"
        )
        original_runtime = os.environ.get("XDG_RUNTIME_DIR")
        original_home = os.environ.get("HOME")
        with patch.dict(os.environ, {
            "PATH": str(binary_dir) + os.pathsep + os.environ.get("PATH", ""),
            "WAYLAND_SOCKET": "999999",
        }):
            with redirect_stdout(io.StringIO()):
                status = runner.main([], repository_root=self.root)
        self.assertEqual(status, 1)
        report_path = next((self.root / "test-results").glob("*/summary.json"))
        report = json.loads(report_path.read_text())
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["commands"][0]["id"], "configure")
        self.assertEqual(report["commands"][0]["returncode"], 4)
        self.assertTrue(Path(report["baseline"]).is_file())
        self.assertEqual(report["tests"][0]["status"], "blocked")
        self.assertFalse(Path(runtime_path.read_text()).exists())
        self.assertEqual(os.environ.get("XDG_RUNTIME_DIR"), original_runtime)
        self.assertEqual(os.environ.get("HOME"), original_home)


class HeadlessFixtureTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("SEASIDE_HEADLESS_FIXTURE"), "Meson supplies the built headless fixture")
    def test_ready_then_signal_shutdown_releases_private_wayland_runtime(self):
        fixture = os.environ["SEASIDE_HEADLESS_FIXTURE"]
        with tempfile.TemporaryDirectory(prefix="seaside-fixture-test-") as temporary:
            runtime_root = Path(temporary)
            env = os.environ.copy()
            env["XDG_RUNTIME_DIR"] = str(runtime_root)
            env.pop("WAYLAND_DISPLAY", None)
            env.pop("DISPLAY", None)
            process = subprocess.Popen(
                [fixture, "--serve"], env=env, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, start_new_session=True,
            )
            selector = selectors.DefaultSelector()
            selector.register(process.stdout, selectors.EVENT_READ)
            readiness = None
            output = bytearray()
            try:
                deadline = time.monotonic() + 5
                while readiness is None and time.monotonic() < deadline:
                    for key, _ in selector.select(0.05):
                        chunk = os.read(key.fd, 4096)
                        if not chunk:
                            self.fail(f"Fixture exited before readiness: {output.decode(errors='replace')}")
                        output.extend(chunk)
                        self.assertLessEqual(len(output), 65536, "Fixture startup output is unbounded")
                        for line in output.splitlines():
                            try:
                                record = json.loads(line)
                            except (ValueError, UnicodeError):
                                continue
                            if isinstance(record, dict) and record.get("event") == "ready":
                                readiness = record
                self.assertIsNotNone(readiness, output.decode(errors="replace"))
                private_runtime = Path(readiness["runtime_dir"])
                self.assertEqual(private_runtime.parent, runtime_root)
                self.assertTrue((private_runtime / readiness["socket"]).is_socket())
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=3)
                self.assertEqual(process.returncode, 0)
                self.assertFalse(private_runtime.exists())
                self.assertTrue(runtime_root.is_dir())
            finally:
                selector.close()
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=2)
                process.stdout.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
