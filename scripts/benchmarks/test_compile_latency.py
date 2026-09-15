#!/usr/bin/env python3
"""Focused checks for compile benchmark gate handshakes."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "compile_latency", Path(__file__).with_name("run-compile-latency.py"))
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class PhaseHandshakeTest(unittest.TestCase):
    def setUp(self):
        self.current = {"phase": "compile-cold", "gate_acknowledged": False}

    def test_matching_ack_allows_metric_then_done(self):
        RUNNER.validate_phase_event(self.current, "compile-cold", "ACK")
        self.current["gate_acknowledged"] = True
        RUNNER.validate_phase_event(self.current, "compile-cold", "metric")
        self.current["metric"] = {"rc": 0}
        RUNNER.validate_phase_event(self.current, "compile-cold", "DONE")

    def test_metric_and_done_require_ack(self):
        for event in ("metric", "DONE"):
            with self.subTest(event=event):
                with self.assertRaisesRegex(RuntimeError, f"{event} before ACK"):
                    RUNNER.validate_phase_event(self.current, "compile-cold", event)

    def test_ack_must_match_the_active_phase(self):
        with self.assertRaisesRegex(RuntimeError, "does not match active phase"):
            RUNNER.validate_phase_event(self.current, "compile-warm-1", "ACK")

    def test_duplicate_ack_is_rejected(self):
        self.current["gate_acknowledged"] = True
        with self.assertRaisesRegex(RuntimeError, "duplicate ACK"):
            RUNNER.validate_phase_event(self.current, "compile-cold", "ACK")

    def test_done_requires_metric_after_ack(self):
        self.current["gate_acknowledged"] = True
        with self.assertRaisesRegex(RuntimeError, "DONE before metric"):
            RUNNER.validate_phase_event(self.current, "compile-cold", "DONE")


class ReportStateTest(unittest.TestCase):
    def test_running_report_persists_ready_and_ack_state_atomically(self):
        with tempfile.TemporaryDirectory(prefix="compile-latency-test-") as directory:
            output = Path(directory)
            report = {"result": "FAIL", "phases": []}
            current = {
                "phase": "compile-cold",
                "gate_acknowledged": False,
                "ready_host_s": 1.0,
                "go_host_s": 1.1,
                "started_monotonic": 123.0,
                "go_monotonic": 123.0,
            }

            RUNNER.write_report(output, report, result="RUNNING", current=current)

            saved = json.loads((output / "report.json").read_text())
            self.assertEqual(saved["result"], "RUNNING")
            self.assertEqual(saved["incomplete_phase"]["phase"], "compile-cold")
            self.assertFalse(saved["incomplete_phase"]["gate_acknowledged"])
            self.assertEqual(saved["incomplete_phase"]["ready_host_s"], 1.0)
            self.assertNotIn("ack_host_s", saved["incomplete_phase"])

            current["gate_acknowledged"] = True
            current["ack_host_s"] = 1.2
            current["gate_ack_wall_s"] = 0.1
            RUNNER.write_report(output, report, result="RUNNING", current=current)

            saved = json.loads((output / "report.json").read_text())
            self.assertTrue(saved["incomplete_phase"]["gate_acknowledged"])
            self.assertEqual(saved["incomplete_phase"]["ack_host_s"], 1.2)
            self.assertNotIn("started_monotonic", saved["incomplete_phase"])
            self.assertNotIn("go_monotonic", saved["incomplete_phase"])
            self.assertFalse((output / "report.json.tmp").exists())


if __name__ == "__main__":
    unittest.main()
