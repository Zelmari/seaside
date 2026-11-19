#!/usr/bin/env python3
"""CLI and simulated snapshot checks; never opens a DRM device."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--probe", required=True, type=Path)
parser.add_argument("--fixture", required=True, type=Path)
options, remaining = parser.parse_known_args()
probe_path = str(options.probe.resolve())
fixture_path = str(options.fixture.resolve())


class ProbeTests(unittest.TestCase):
    def run_probe(self, *args, pass_fds=()):
        return subprocess.run(
            [probe_path, *args],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
            pass_fds=pass_fds,
        )

    def report(self, result, expected_exit):
        self.assertEqual(result.returncode, expected_exit, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report["read_only"])
        self.assertEqual(report["qualification"], "capability_only")
        self.assertFalse(report["hardware_cursor_qualified"])
        self.assertFalse(report["actual_cursor_use_tested"])
        return report

    def test_missing_device_is_unavailable_and_cursor_unknown(self):
        with tempfile.TemporaryDirectory(prefix="seaside-probe-") as directory:
            result = self.run_probe("--device", str(Path(directory) / "card0"))
        report = self.report(result, 77)
        self.assertEqual(report["status"], "blocked")
        device = report["devices"][0]
        self.assertEqual(device["cursor_capability"], "unknown")
        self.assertIsNone(device["cursor_plane_count"])
        self.assertFalse(device["enumerated"])
        self.assertIn("cannot stat device", result.stderr)

    def test_regular_file_is_rejected(self):
        with tempfile.NamedTemporaryFile(prefix="seaside-probe-") as file:
            report = self.report(self.run_probe("--device", file.name), 1)
        self.assertEqual(report["devices"][0]["status"], "error")
        self.assertEqual(report["devices"][0]["cursor_capability"], "unknown")

    def test_non_drm_borrowed_fd_is_rejected(self):
        read_fd, write_fd = os.pipe()
        try:
            report = self.report(
                self.run_probe("--fd", str(read_fd), pass_fds=(read_fd,)), 1
            )
            self.assertIn("not a DRM primary", report["devices"][0]["reason"])
        finally:
            os.close(read_fd)
            os.close(write_fd)

    def test_invalid_descriptor_is_query_failure(self):
        report = self.report(self.run_probe("--fd", "2147483647"), 1)
        self.assertIn("cannot duplicate", report["devices"][0]["reason"])

    def test_invalid_arguments(self):
        cases = (
            ("--universal-planes-prepared",),
            ("--fd", "-1"),
            ("--fd", "garbage"),
            ("--fd", "2147483648"),
            ("--fd", "3", "--device", "/dev/null"),
            ("--device",),
            ("--device", "x", "--device", "y"),
            ("--unexpected",),
        )
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = self.run_probe(*arguments)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")
                self.assertIn("Usage:", result.stderr)

    def test_simulated_classification_and_associations(self):
        result = subprocess.run(
            [fixture_path], capture_output=True, text=True, timeout=5, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        fixture = json.loads(result.stdout)
        self.assertTrue(fixture["simulated"])
        self.assertFalse(fixture["hardware_cursor_qualified"])
        self.assertFalse(fixture["actual_cursor_use_tested"])
        observed, absent, unprepared, blocked, failed = fixture["devices"]
        self.assertEqual(observed["path"], 'simulated:"driver\\path\n')
        self.assertEqual(observed["drm_driver"], 'fixture\t"driver')
        self.assertEqual(observed["cursor_capability"], "cursor_planes_observed")
        self.assertEqual(observed["cursor_plane_count"], 1)
        self.assertEqual(observed["cursor_size_caps"], {"width": 64, "height": None})
        connector = observed["connectors"][0]
        self.assertEqual(connector["possible_crtc_ids"], [107, 317])
        self.assertEqual(connector["candidate_cursor_crtc_ids"], [107])
        self.assertEqual(observed["planes"][0]["possible_crtc_ids"], [107])
        self.assertEqual(observed["planes"][1]["possible_crtc_ids"], [317])
        self.assertEqual(observed["planes"][0]["current_fb_id"], 81)
        self.assertEqual(observed["planes"][0]["properties"][0]["name"], "type")
        self.assertEqual(absent["status"], "complete")
        self.assertEqual(absent["cursor_capability"], "no_cursor_planes_observed")
        self.assertEqual(absent["cursor_plane_count"], 0)
        self.assertEqual(unprepared["status"], "blocked")
        self.assertEqual(unprepared["cursor_capability"], "unknown")
        self.assertEqual(blocked["status"], "blocked")
        self.assertIsNone(blocked["cursor_plane_count"])
        self.assertEqual(failed["status"], "error")
        self.assertEqual(failed["cursor_capability"], "unknown")


unittest.main(argv=[sys.argv[0], *remaining])
