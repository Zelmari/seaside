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
import threading
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
        self.assertEqual(report["tests"][0]["part"], 0)
        self.assertEqual(report["parts"], [])
        self.assertFalse(Path(runtime_path.read_text()).exists())
        self.assertEqual(os.environ.get("XDG_RUNTIME_DIR"), original_runtime)
        self.assertEqual(os.environ.get("HOME"), original_home)

    def test_preexisting_worker_thread_child_blocks_without_being_touched(self):
        ready = threading.Event()
        release = threading.Event()
        children = []

        def own_child():
            process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(300)"])
            children.append(process)
            ready.set()
            release.wait(timeout=5)
            process.wait(timeout=3)

        worker = threading.Thread(target=own_child)
        worker.start()
        try:
            self.assertTrue(ready.wait(timeout=3))
            result = self.command("raise AssertionError('must not execute')")
            self.assertEqual(result["status"], "blocked")
            self.assertIn("pre-existing children", result["error"])
            self.assertIsNone(children[0].poll())
        finally:
            if children:
                children[0].terminate()
            release.set()
            worker.join(timeout=4)
            self.assertFalse(worker.is_alive())

    def test_thread_spawned_new_session_descendant_receives_graceful_cleanup(self):
        leaf = (
            "import pathlib,signal,sys,time; "
            "signal.signal(signal.SIGTERM,lambda *_:(pathlib.Path('term-observed').write_text('yes'),sys.exit(0))); "
            "pathlib.Path('leaf-ready').write_text('yes'); time.sleep(300)"
        )
        parent = (
            "import pathlib,signal,subprocess,sys,threading,time\n"
            "signal.signal(signal.SIGTERM,signal.SIG_IGN)\n"
            "def child():\n"
            f"    p=subprocess.Popen([sys.executable,'-c',{leaf!r}],start_new_session=True)\n"
            "    pathlib.Path('pids').write_text(str(p.pid))\n"
            "    p.wait()\n"
            "threading.Thread(target=child).start()\n"
            "time.sleep(300)\n"
        )
        wrapper = (
            "import importlib.machinery,importlib.util,json,pathlib,sys; "
            f"loader=importlib.machinery.SourceFileLoader('runner',{str(ROOT / 'tools/run-tests')!r}); "
            "spec=importlib.util.spec_from_loader(loader.name,loader); "
            "runner=importlib.util.module_from_spec(spec); loader.exec_module(runner); "
            f"result=runner.run_command([sys.executable,'-c',{parent!r}],cwd=pathlib.Path.cwd(),"
            "log_path=pathlib.Path('threaded.log'),timeout=30,terminate_grace=.25); "
            "pathlib.Path('result.json').write_text(json.dumps(result))"
        )
        process = subprocess.Popen([sys.executable, "-c", wrapper], cwd=self.root, start_new_session=True)
        try:
            wait_for_file(self.root / "leaf-ready", process)
            process.send_signal(signal.SIGINT)
            self.assertEqual(process.wait(timeout=5), 0)
            result = json.loads((self.root / "result.json").read_text())
            self.assertEqual(result["status"], "blocked")
            self.assertIsNone(result["cleanup_error"])
            self.assertEqual((self.root / "term-observed").read_text(), "yes")
            self.assert_processes_gone([int((self.root / "pids").read_text())])
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)

    def test_late_adopted_zombie_is_refreshed_and_reaped_before_next_command(self):
        tree = runner.OwnedProcessTree()
        child = None
        process = None
        descriptor = None
        closed = False
        leaf = "import pathlib,time\nwhile not pathlib.Path('release').exists():\n    time.sleep(.01)\n"
        source = ("import pathlib,subprocess,sys; "
                  f"p=subprocess.Popen([sys.executable,'-c',{leaf!r}],start_new_session=True); "
                  "pathlib.Path('adopted-pid').write_text(str(p.pid))")
        try:
            process = subprocess.Popen([sys.executable, "-c", source], cwd=self.root)
            tree.root = process.pid
            wait_for_file(self.root / "adopted-pid", process)
            child = int((self.root / "adopted-pid").read_text())
            descriptor = os.pidfd_open(child)
            process.wait(timeout=3)
            (self.root / "release").touch()
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                identity = runner.process_identity(child)
                if identity is not None and identity[1] == "Z":
                    break
                time.sleep(.01)
            self.assertEqual(runner.process_identity(child)[1], "Z")
            # No refresh has run since the child was spawned/adopted. close()
            # itself must discover and reap this zombie before restoring state.
            tree.close()
            closed = True
            self.assertNotIn(child, runner.child_pids(os.getpid()))
            result = self.command("pass")
            self.assertEqual(result["status"], "pass")
        finally:
            (self.root / "release").touch()
            if descriptor is not None:
                try:
                    signal.pidfd_send_signal(descriptor, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.close(descriptor)
            if process is not None:
                process.wait(timeout=3)
            if not closed:
                tree.close()
            if child is not None:
                try:
                    os.waitpid(child, 0)
                except ChildProcessError:
                    pass

    def test_cleanup_uses_pinned_processes_without_numeric_process_group_signals(self):
        with patch.object(runner.os, "killpg", side_effect=AssertionError("unsafe numeric PGID")), \
             patch.object(runner.os, "kill", side_effect=AssertionError("unsafe numeric PID")):
            result = self.command("pass")
        self.assertEqual(result["status"], "pass")
        self.assertIsNone(result["cleanup_error"])

    def test_signal_error_does_not_abandon_other_descendant_cleanup(self):
        actual_signal = signal.pidfd_send_signal
        failed = False

        def once(descriptor, signum):
            nonlocal failed
            if not failed and signum == signal.SIGTERM:
                failed = True
                raise PermissionError("deliberate pinned-signal error")
            return actual_signal(descriptor, signum)

        with patch.object(runner.signal, "pidfd_send_signal", side_effect=once):
            result = self.command(self.tree_source(), timeout=.75)
        self.assertEqual(result["status"], "fail")
        self.assertIn("deliberate pinned-signal error", result["cleanup_error"])
        pids = [int(value) for value in (self.root / "pids").read_text().split()]
        self.assert_processes_gone(pids)

    def test_completed_descendants_do_not_accumulate_pinned_descriptors(self):
        tree = runner.OwnedProcessTree()
        process = None
        source = (
            "import pathlib,subprocess,sys,time\n"
            "for index in range(4):\n"
            "    p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(300)'])\n"
            "    pathlib.Path(f'ready-{index}').write_text(str(p.pid))\n"
            "    while not pathlib.Path(f'go-{index}').exists():\n"
            "        time.sleep(.01)\n"
            "    p.wait()\n"
            "    pathlib.Path(f'done-{index}').write_text('yes')\n"
        )
        try:
            process = subprocess.Popen([sys.executable, "-c", source], cwd=self.root)
            tree.root = process.pid
            for index in range(4):
                ready = self.root / f"ready-{index}"
                wait_for_file(ready, process)
                pid = int(ready.read_text())
                tree.refresh()
                self.assertIn(pid, tree.pidfds)
                signal.pidfd_send_signal(tree.pidfds[pid], signal.SIGKILL)
                (self.root / f"go-{index}").touch()
                wait_for_file(self.root / f"done-{index}", process)
                tree.refresh()
                self.assertNotIn(pid, tree.pidfds)
                # The parent may already have launched the next gated child.
                self.assertLessEqual(len(tree.pidfds), 2)
            process.wait(timeout=3)
        finally:
            tree.signal(signal.SIGKILL)
            if process is not None:
                process.wait(timeout=3)
            tree.close()


class ResultEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="seaside-results-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.gtest_dir = self.root / "gtest"
        self.gtest_dir.mkdir()
        self.metadata = [{
            "name": "configuration", "part": 2, "requirement_sections": ["8.1", "8.2"],
            "command": ["/build/config-tests"], "gtest_name": "config-tests",
        }]

    def manifest(self, rows, suites=None):
        path = self.root / "meson-info/intro-tests.json"
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps(rows))
        return runner.load_test_manifest(self.root, suites or [])

    def declaration(self, name="configuration", part="2", suite="unit", gtest="1"):
        return {
            "name": name, "cmd": ["/build/config-tests"], "suite": ["seaside:" + suite],
            "env": {"SEASIDE_GTEST": gtest, "SEASIDE_TEST_PART": part,
                    "SEASIDE_REQUIREMENTS": "8.1,8.2"},
        }

    def test_part3_discovery_keeps_its_requirements_and_suite(self):
        declaration = self.declaration(name="startup", part="3", suite="part3")
        declaration["env"]["SEASIDE_REQUIREMENTS"] = "3,12.2b"
        manifest = self.manifest([declaration], ["part3"])
        self.assertEqual(len(manifest), 1)
        self.assertEqual(manifest[0]["part"], 3)
        self.assertEqual(manifest[0]["requirement_sections"], ["3", "12.2b"])

    def test_part3_software_pass_does_not_qualify_the_real_graphical_session(self):
        self.metadata[0]["part"] = 3
        self.metadata[0]["requirement_sections"] = ["3", "12.2b"]
        rows = self.parse(self.xml([self.case()]))
        report = self.report(rows)
        self.assertEqual(report["parts"], [3])
        self.assertEqual(report["part_results"]["3"]["software_status"], "pass")
        self.assertEqual(report["part_results"]["3"]["qualification"],
                         "headless_milestone_only_pending_arch_vm_tty_seat_display_input_and_service_preflight")
        self.assertIn("Part 3 full TTY startup", (self.root / "summary.txt").read_text())

    def parse(self, xml, aggregate="OK", rows=None, log_limit=1024):
        if xml is not None:
            path = self.gtest_dir / "config-tests.xml"
            if isinstance(xml, bytes):
                path.write_bytes(xml)
            else:
                path.write_text(xml)
        rows = rows if rows is not None else [{
            "name": "unit - seaside:configuration", "result": aggregate,
            "stdout": "aggregate log", "returncode": 0 if aggregate == "OK" else 1,
        }]
        path = self.root / "meson.json"
        path.write_text("".join(json.dumps(row) + "\n" for row in rows))
        return runner.parse_meson_results(path, self.root, log_limit, self.metadata)

    def xml(self, cases, failures=0, errors=0):
        return (f'<testsuites tests="{len(cases)}" failures="{failures}" errors="{errors}">'
                '<testsuite name="ConfigTest">' + "".join(cases) + '</testsuite></testsuites>')

    def case(self, name="Defaults", contents="", status="run", result="completed", duration="0.001"):
        return (f'<testcase name="{name}" classname="ConfigTest" status="{status}" '
                f'result="{result}" time="{duration}">{contents}</testcase>')

    def report(self, rows, commands=None):
        report = {"environment": {"classification": "test-fixture", "kernel": "test-only"},
                  "commands": commands or [], "tests": rows}
        runner.write_report(self.root, report)
        return json.loads((self.root / "summary.json").read_text())

    def test_part_and_requirements_follow_discovered_test_metadata(self):
        result = self.manifest([self.declaration()])
        self.assertEqual(result[0]["part"], 2)
        self.assertEqual(result[0]["requirement_sections"], ["8.1", "8.2"])
        self.assertEqual(result[0]["gtest_name"], "config-tests")

    def test_suite_selection_does_not_require_unselected_tests(self):
        rows = [self.declaration(), self.declaration(name="drm", part="1", suite="hardware", gtest="0")]
        selected = self.manifest(rows, ["unit"])
        self.assertEqual([row["name"] for row in selected], ["configuration"])
        selected = self.manifest(rows, ["seaside:hardware"])
        self.assertEqual([row["name"] for row in selected], ["drm"])

    def test_empty_unknown_or_duplicate_discovery_is_rejected(self):
        for rows, suites in (([], []), ([self.declaration()], ["missing"]),
                             ([self.declaration(), self.declaration()], [])):
            with self.subTest(rows=rows, suites=suites), self.assertRaises(ValueError):
                self.manifest(rows, suites)

    def test_colliding_gtest_xml_paths_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "unique XML"):
            self.manifest([self.declaration(), self.declaration(name="other")])

    def test_declared_filters_shards_and_output_cannot_shrink_owned_discovery(self):
        for key, value in (("GTEST_FILTER", "OnlyOne.*"), ("GTEST_TOTAL_SHARDS", "2"),
                           ("TESTBRIDGE_TEST_ONLY", "OnlyOne.*"), ("TEST_TOTAL_SHARDS", "2"),
                           ("TEST_SHARD_INDEX", "1"), ("TEST_SHARD_STATUS_FILE", "/tmp/shard-status"),
                           ("XML_OUTPUT_FILE", "/tmp/result.xml"), ("TEST_PREMATURE_EXIT_FILE", "/tmp/exit-status"),
                           ("GTEST_OUTPUT", "xml:/elsewhere")):
            row = self.declaration()
            row["env"][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "run-owned discovery"):
                self.manifest([row])
        for argument in ("--gtest_filter=OnlyOne.*", "--gtest_repeat=0", "--gtest_output=x"):
            row = self.declaration()
            row["cmd"].append(argument)
            with self.subTest(argument=argument), self.assertRaises(ValueError):
                self.manifest([row])

    def test_missing_expected_test_result_blocks_even_with_other_passing_tests(self):
        self.metadata.append({"name": "other", "part": 1, "requirement_sections": ["2.2"],
                              "command": ["/build/other"], "gtest_name": None})
        result = self.report(self.parse(self.xml([self.case()])))
        self.assertEqual(result["status"], "blocked")
        self.assertEqual(result["tests"][1]["test_id"], "other")
        self.assertEqual(result["part_results"]["1"]["software_status"], "blocked")
        self.assertEqual(result["part_results"]["2"]["software_status"], "pass")
        self.assertEqual(result["parts"], [1, 2])
        self.assertIsNone(result["part"])

    def test_duplicate_or_unexpected_meson_results_cannot_pass(self):
        passing = {"name": "unit - seaside:configuration", "result": "OK"}
        for rows in ([passing, passing], [{"name": "unexpected", "result": "OK"}]):
            with self.subTest(rows=rows):
                result = self.report(self.parse(self.xml([self.case()]), rows=rows))
                self.assertEqual(result["status"], "blocked")

    def test_individual_cases_and_bounded_failures_override_successful_aggregate(self):
        failure = self.case("InvalidType", '<failure message="wrong type">' + "x" * 10000 + '</failure>')
        result = self.report(self.parse(self.xml([self.case(), failure], failures=1), log_limit=128))
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["counts"]["fail"], 1)
        self.assertEqual(result["case_counts"], {"pass": 1, "fail": 1, "skip": 0, "blocked": 0})
        cases = result["tests"][0]["cases"]
        self.assertEqual(cases[1]["case_name"], "ConfigTest.InvalidType")
        self.assertEqual(cases[1]["part"], 2)
        self.assertEqual(cases[1]["requirement_sections"], ["8.1", "8.2"])
        self.assertLessEqual(len(cases[1]["reason"].encode()), 128)
        self.assertIn("ConfigTest.InvalidType", (self.root / "summary.txt").read_text())

    def test_aggregate_crash_is_not_masked_by_completed_passing_cases(self):
        result = self.report(self.parse(self.xml([self.case()]), aggregate="FAIL"))
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["case_counts"]["pass"], 1)

    def test_missing_empty_malformed_and_oversized_xml_cannot_pass(self):
        samples = [None, "", "<testsuites>", '<testsuites tests="0"/>',
                   self.xml([self.case()]) + " " * (2 * 1024 * 1024)]
        for sample in samples:
            with self.subTest(size=None if sample is None else len(sample)):
                path = self.gtest_dir / "config-tests.xml"
                path.unlink(missing_ok=True)
                result = self.report(self.parse(sample))
                self.assertEqual(result["status"], "blocked")
                self.assertEqual(result["case_counts"]["pass"], 0)
                self.assertTrue(result["tests"][0]["collection_error"])

    def test_xml_count_mismatch_duplicates_unknown_outcomes_or_nonfinite_time_block(self):
        samples = [
            self.xml([self.case()], failures=1),
            self.xml([self.case(), self.case()]),
            self.xml([self.case(status="unknown")]),
            self.xml([self.case(duration="nan")]),
            self.xml([self.case()]).replace('tests="1"', 'tests="2"'),
            '<!DOCTYPE testsuites [<!ENTITY example "x">]>' + self.xml([self.case()]),
        ]
        for sample in samples:
            with self.subTest(sample=sample):
                result = self.report(self.parse(sample))
                self.assertEqual(result["status"], "blocked")
                self.assertEqual(result["tests"][0]["cases"], [])

    def test_suite_disabled_and_skipped_counts_must_agree_with_case_details(self):
        samples = [
            self.xml([self.case()]).replace('<testsuite name="ConfigTest">',
                                           '<testsuite name="ConfigTest" tests="1" failures="1">'),
            self.xml([self.case()]).replace('<testsuite name="ConfigTest">',
                                           '<testsuite name="ConfigTest" tests="2">'),
            self.xml([self.case()]).replace('errors="0"', 'errors="0" disabled="1"', 1),
            self.xml([self.case()]).replace('errors="0"', 'errors="0" skipped="1"', 1),
        ]
        for sample in samples:
            with self.subTest(xml=sample):
                result = self.report(self.parse(sample))
                self.assertEqual(result["status"], "blocked")
                self.assertEqual(result["tests"][0]["cases"], [])

    def test_xml_encoding_cannot_hide_entity_declarations(self):
        body = ('<!DOCTYPE testsuites [<!ENTITY name "Defaults">]>' +
                self.xml([self.case()]).replace('name="Defaults"', 'name="&name;"'))
        for encoding in ("utf-16", "utf-32"):
            xml = ('<?xml version="1.0" encoding="' + encoding + '"?>' + body).encode(encoding)
            with self.subTest(encoding=encoding):
                self.assertEqual(self.report(self.parse(xml))["status"], "blocked")
        mismatched = '<?xml version="1.0" encoding="UTF-16"?>' + self.xml([self.case()])
        self.assertEqual(self.report(self.parse(mismatched))["status"], "blocked")

    def test_explicit_process_exit_must_agree_with_successful_meson_outcome(self):
        for outcome, code in (("OK", 9), ("OK", 77), ("EXPECTEDFAIL", 0)):
            row = {"name": "unit - seaside:configuration", "result": outcome, "returncode": code}
            with self.subTest(row=row):
                self.assertEqual(self.report(self.parse(self.xml([self.case()]), rows=[row]))["status"],
                                 "blocked")

    def test_execution_must_match_independently_discovered_cases(self):
        self.metadata[0]["expected_cases"] = {"ConfigTest.Defaults": False,
                                              "ConfigTest.Bounds": False}
        self.assertEqual(self.report(self.parse(self.xml([self.case()])))["status"], "blocked")
        suppressed = self.case("Bounds", status="notrun", result="suppressed")
        self.assertEqual(self.report(self.parse(self.xml([self.case(), suppressed])))["status"],
                         "blocked")
        runtime_skip = self.case("Bounds", '<skipped message="unavailable"/>', result="skipped")
        result = self.report(self.parse(self.xml([self.case(), runtime_skip])))
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["case_counts"]["skip"], 1)
        self.metadata[0]["expected_cases"]["ConfigTest.DISABLED_Bounds"] = True
        del self.metadata[0]["expected_cases"]["ConfigTest.Bounds"]
        disabled = self.case("DISABLED_Bounds", status="notrun", result="suppressed")
        result = self.report(self.parse(self.xml([self.case(), disabled])))
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["case_counts"]["skip"], 1)

    def test_independent_discovery_rejects_empty_duplicate_or_inconsistent_case_lists(self):
        path = self.root / "discovered.xml"
        for sample in ('<testsuites tests="0"/>', self.xml([self.case(), self.case()]),
                       self.xml([self.case()]).replace('tests="1"', 'tests="2"')):
            path.write_text(sample)
            with self.subTest(sample=sample), self.assertRaises(ValueError):
                runner.discover_gtest_cases(path)
        path.write_text(self.xml([self.case(), self.case("DISABLED_Bounds")]))
        self.assertEqual(runner.discover_gtest_cases(path),
                         {"ConfigTest.Defaults": False, "ConfigTest.DISABLED_Bounds": True})

    def test_independent_discovery_uses_supervisor_deadline_and_cannot_pass_on_timeout(self):
        metadata = dict(self.metadata[0])
        metadata["command"] = [sys.executable, "-c", "import time; time.sleep(10)"]
        metadata["working_directory"] = str(self.root)
        observed = []

        def execute(name, command, *, env_override, cwd_override):
            result = runner.run_command(command, cwd=cwd_override,
                                        log_path=self.root / (name + ".log"),
                                        timeout=0.1, terminate_grace=0.1,
                                        log_limit=256, env=env_override)
            observed.append(result)
            return result

        failures = runner.prepare_gtest_discovery([metadata], self.root, os.environ.copy(), execute)
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["status"], "blocked")
        self.assertTrue(observed[0]["timed_out"])
        self.assertIsNone(observed[0]["cleanup_error"])
        self.assertNotIn("expected_cases", metadata)

    def test_disabled_or_skipped_only_cases_do_not_establish_pass(self):
        for case in (self.case(status="notrun", result="suppressed"),
                     self.case(contents='<skipped message="unavailable"/>', result="skipped")):
            with self.subTest(case=case):
                result = self.report(self.parse(self.xml([case])))
                self.assertEqual(result["status"], "blocked")
                self.assertEqual(result["case_counts"]["skip"], 1)
                self.assertTrue(any("::" in row["test_id"] for row in result["pending_checks"]))

    def test_successful_xml_artifact_retention_is_bounded_after_case_collection(self):
        result = self.parse(self.xml([self.case()]))
        self.assertEqual(result[0]["status"], "pass")
        artifacts = runner.retain_meson_logs(self.root, 64)
        xml = next(row for row in artifacts if "config-tests.xml" in row["path"])
        self.assertTrue(xml["truncated"])
        self.assertLessEqual(Path(xml["path"]).stat().st_size, 64)
        self.assertFalse((self.gtest_dir / "config-tests.xml").exists())

    def test_result_symlinks_and_fifos_are_rejected_without_reading_them(self):
        path = self.gtest_dir / "config-tests.xml"
        target = self.root / "other.xml"
        target.write_text(self.xml([self.case()]))
        path.symlink_to(target)
        result = self.report(self.parse(None))
        self.assertEqual(result["status"], "blocked")
        path.unlink()
        os.mkfifo(path)
        result = self.report(self.parse(None))
        self.assertEqual(result["status"], "blocked")
        self.assertEqual(target.read_text(), self.xml([self.case()]))

    def test_cleanup_error_cannot_be_hidden_by_successful_command_status(self):
        rows = self.parse(self.xml([self.case()]))
        command = {"id": "test", "status": "pass", "command": ["tests"],
                   "cleanup_error": "owned process still alive", "log": "test.log"}
        report = self.report(rows, [command])
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["part_results"]["2"]["software_status"], "fail")
        self.assertEqual(report["part1_qualification"], "pending_arch_vm_drm_and_interactive_checks")

    def test_full_runner_collects_run_owned_cases_and_isolates_ambient_gtest_filters(self):
        binary_dir = self.root / "bin"
        binary_dir.mkdir()
        meson = binary_dir / "meson"
        declaration = self.declaration()
        declaration["cmd"] = [str(binary_dir / "config-tests")]
        xml = self.xml([self.case(), self.case("Bounds")])
        gtest = binary_dir / "config-tests"
        gtest.write_text(
            f"#!{sys.executable}\nimport os,pathlib,sys\n"
            "assert sys.argv[1:] == ['--gtest_list_tests']\n"
            "assert not any((key.startswith('GTEST_') and key != 'GTEST_OUTPUT') or key in ('TESTBRIDGE_TEST_ONLY','TEST_TOTAL_SHARDS','TEST_SHARD_INDEX','TEST_SHARD_STATUS_FILE','XML_OUTPUT_FILE','TEST_PREMATURE_EXIT_FILE') for key in os.environ)\n"
            f"pathlib.Path(os.environ['GTEST_OUTPUT'][4:]).write_text({xml!r})\n"
        )
        gtest.chmod(0o700)
        meson.write_text(
            f"#!{sys.executable}\nimport json,os,pathlib,sys\n"
            "if sys.argv[1] == 'setup':\n"
            "    build=pathlib.Path(sys.argv[2]); (build/'meson-info').mkdir(parents=True)\n"
            f"    (build/'meson-info/intro-tests.json').write_text({json.dumps([declaration])!r})\n"
            "elif sys.argv[1] == 'test':\n"
            "    assert not any((key.startswith('GTEST_') and key != 'GTEST_OUTPUT') or key in ('TESTBRIDGE_TEST_ONLY','TEST_TOTAL_SHARDS','TEST_SHARD_INDEX','TEST_SHARD_STATUS_FILE','XML_OUTPUT_FILE','TEST_PREMATURE_EXIT_FILE') for key in os.environ)\n"
            "    output=os.environ['GTEST_OUTPUT']; assert output.startswith('xml:')\n"
            "    directory=pathlib.Path(output[4:]); assert directory.is_dir()\n"
            f"    (directory/'config-tests.xml').write_text({xml!r})\n"
            "    path=pathlib.Path(sys.argv[sys.argv.index('--logbase')+1]+'.json')\n"
            "    path.write_text(json.dumps({'name':'unit - seaside:configuration','result':'OK','stdout':'','returncode':0})+'\\n')\n"
        )
        meson.chmod(0o700)
        tools = self.root / "tools"
        tools.mkdir()
        (tools / "record-baseline").write_text(
            "import pathlib,sys\npathlib.Path(sys.argv[sys.argv.index('--output')+1]).write_text('{}')\n"
        )
        with patch.dict(os.environ, {"PATH": str(binary_dir) + os.pathsep + os.environ.get("PATH", ""),
                                     "GTEST_FILTER": "NoSuchCases.*", "GTEST_TOTAL_SHARDS": "4",
                                     "TESTBRIDGE_TEST_ONLY": "NoSuchCases.*", "TEST_TOTAL_SHARDS": "4",
                                     "TEST_SHARD_INDEX": "2", "TEST_SHARD_STATUS_FILE": str(self.root / "outside-shard-status"),
                                     "XML_OUTPUT_FILE": str(self.root / "outside.xml"),
                                     "TEST_PREMATURE_EXIT_FILE": str(self.root / "outside-premature")}), redirect_stdout(io.StringIO()):
            self.assertEqual(runner.main(["--suite", "unit"], repository_root=self.root), 0)
        path = next((self.root / "test-results").glob("*/summary.json"))
        report = json.loads(path.read_text())
        self.assertEqual(report["parts"], [2])
        self.assertEqual(report["part"], 2)
        self.assertEqual(report["case_counts"]["pass"], 2)
        self.assertEqual(report["tests"][0]["requirement_sections"], ["8.1", "8.2"])
        self.assertTrue(any("config-tests.xml" in row["path"] for row in report["artifacts"]))
        self.assertTrue(any(row["id"].startswith("discover-gtest-") for row in report["commands"]))
        self.assertTrue(any("gtest-discovery" in row["path"] for row in report["artifacts"]))

    def test_nonfatal_failure_followed_by_skip_retains_failure_diagnostics(self):
        mixed = self.case(contents='<failure message="failed assertion"/><skipped message="later skip"/>')
        xml = self.xml([mixed], failures=1).replace('<testsuite name="ConfigTest">',
            '<testsuite name="ConfigTest" tests="1" failures="1" skipped="0">')
        report = self.report(self.parse(xml))
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["case_counts"]["fail"], 1)
        self.assertIn("failed assertion", report["tests"][0]["cases"][0]["reason"])

    def test_global_interruption_does_not_invent_part_one_or_leave_part_two_passed(self):
        rows = self.parse(self.xml([self.case()]))
        rows.append(runner.collection_failure("test-run-interruption", "Run interrupted"))
        report = self.report(rows)
        self.assertEqual(report["parts"], [2])
        self.assertEqual(report["part_results"]["2"]["software_status"], "blocked")
        self.assertEqual(report["status"], "blocked")

    def test_exit_77_on_non_test_command_or_with_passing_rows_cannot_pass(self):
        passed = self.parse(self.xml([self.case()]))
        for name in ("baseline", "configure", "compile", "gtest-discovery-000", "test"):
            with self.subTest(command=name):
                command = {"id": name, "status": "skip", "returncode": 77,
                           "command": ["tool"], "log": "test.log"}
                self.assertEqual(self.report(passed, [command])["status"], "fail")
        skipped = [{"test_id": "hardware", "part": 2, "status": "skip", "reason": "unavailable"}]
        command = {"id": "test", "status": "skip", "returncode": 77,
                   "command": ["meson", "test"], "log": "test.log"}
        self.assertEqual(self.report(skipped, [command])["status"], "blocked")


class CodeCheckDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="seaside-code-check-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        loader = importlib.machinery.SourceFileLoader("seaside_code_check", str(ROOT / "tools/check-code"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        self.check = importlib.util.module_from_spec(spec)
        loader.exec_module(self.check)
        self.build = self.root / "build"
        self.build.mkdir()
        (self.root / "src").mkdir()
        (self.root / "src/main.cpp").write_text("int main() { return 0; }\n")

    def test_missing_empty_malformed_or_inapplicable_database_never_runs_tools(self):
        samples = [None, "[]", "{}", "[", '[{"file":"test.cpp"}]', json.dumps([
            {"directory": "/outside", "file": "test.cpp", "command": "g++ test.cpp"}])]
        for sample in samples:
            with self.subTest(sample=sample):
                path = self.build / "compile_commands.json"
                path.unlink(missing_ok=True)
                if sample is not None:
                    path.write_text(sample)
                with patch.object(self.check, "ROOT", self.root), patch.object(sys, "argv", ["check-code", "--build-dir", str(self.build)]), patch.object(self.check.subprocess, "run") as run, redirect_stdout(io.StringIO()):
                    self.assertEqual(self.check.main(), 1)
                    run.assert_not_called()

    def test_empty_format_targets_cannot_pass_with_an_applicable_compilation_database(self):
        (self.root / "src/main.cpp").unlink()
        source = self.root / "legacy/probe.c"
        source.parent.mkdir()
        source.write_text("int main(void) { return 0; }\n")
        (self.build / "compile_commands.json").write_text(json.dumps([
            {"directory": str(self.root), "file": str(source), "command": "cc probe.c"}]))
        with patch.object(self.check, "ROOT", self.root), patch.object(sys, "argv", ["check-code", "--build-dir", str(self.build)]), patch.object(self.check.subprocess, "run") as run, redirect_stdout(io.StringIO()):
            self.assertEqual(self.check.main(), 1)
            run.assert_not_called()


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
