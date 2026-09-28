#!/usr/bin/env python3
"""Focused checks for compile benchmark gate handshakes."""

import importlib.util
import json
import os
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


def metrics_text(uptime=10, count=9007199254740993, cpus=1):
    return (
        "# HELP pedigree_metrics_enabled Whether cheap counters are enabled.\n"
        "# TYPE pedigree_metrics_enabled gauge\npedigree_metrics_enabled 1\n"
        f"# TYPE pedigree_cpus gauge\npedigree_cpus {cpus}\n"
        f"# TYPE pedigree_uptime_seconds gauge\npedigree_uptime_seconds {uptime}\n"
        "# TYPE pedigree_spinlock_acquires_total counter\n" +
        "".join(f'pedigree_spinlock_acquires_total{{policy="no_irq",cpu="{cpu}"}} {count}\n'
                for cpu in range(cpus)) + "# EOF\n")


class MetricsSnapshotTest(unittest.TestCase):
    def test_integer_counter_precision_labels_and_rate(self):
        before = RUNNER.parse_metrics_snapshot(metrics_text(cpus=4), 4)
        after = RUNNER.parse_metrics_snapshot(metrics_text(12, 9007199254740999, 4), 4)
        key = 'pedigree_spinlock_acquires_total{cpu="0",policy="no_irq"}'
        self.assertEqual(before["values"][key], 9007199254740993)
        self.assertEqual(before["raw"], metrics_text(cpus=4))
        result = RUNNER.metrics_delta(before, after)
        self.assertEqual(result["window_seconds"], 2)
        self.assertEqual(result["counter_deltas"][key], 6)
        self.assertEqual(result["counter_rates_per_second"][key], 3)
        self.assertNotIn("pedigree_uptime_seconds", result["counter_deltas"])

    def test_unavailable_incomplete_or_ambiguous_samples_fail(self):
        original = metrics_text()
        invalid = [
            original.removesuffix("# EOF\n"),
            original.replace("pedigree_metrics_enabled 1", "pedigree_metrics_enabled 0"),
            original.replace('cpu="0"', 'cpu="1"'),
            original.replace('cpu="0"', 'cpu="0",cpu="0"'),
            original.replace('cpu="0"', 'thread="0"'),
            original.replace("9007199254740993", "9.5"),
            original.replace("# EOF\n", "pedigree_cpus 1\n# EOF\n"),
            original.replace("# EOF\n", "# TYPE absent_total counter\n# EOF\n"),
        ]
        for raw in invalid:
            with self.subTest(raw=raw), self.assertRaises(RuntimeError):
                RUNNER.parse_metrics_snapshot(raw, 1)

    def test_decreasing_counters_stalled_clock_and_changed_cpu_set_fail(self):
        before = RUNNER.parse_metrics_snapshot(metrics_text(), 1)
        for raw, cpus in ((metrics_text(11, 1), 1), (metrics_text(), 1),
                          (metrics_text(11, cpus=4), 4)):
            with self.subTest(raw=raw), self.assertRaises(RuntimeError):
                RUNNER.metrics_delta(before, RUNNER.parse_metrics_snapshot(raw, cpus))

    def test_blocks_pair_after_done_before_next_phase(self):
        report = {"metrics_stats": True, "cpus": 1, "phases": [{"phase": "cpu"}]}
        capture = None
        for edge, raw in (("before", metrics_text()), ("after", metrics_text(12, 9007199254740999))):
            lines = [f"METRICS BEGIN phase=cpu edge={edge}", *raw.splitlines(),
                     f"METRICS END phase=cpu edge={edge}"]
            for line in lines:
                capture, consumed = RUNNER.collect_metrics_line(line, report, None, capture)
                self.assertTrue(consumed)
        RUNNER.require_complete_metrics(report, capture)
        self.assertEqual(report["phases"][0]["metrics"]["window_seconds"], 2)
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            RUNNER.collect_metrics_line("METRICS BEGIN phase=cpu edge=before", report, None, None)

    def test_missing_mismatched_and_interrupted_blocks_fail(self):
        report = {"metrics_stats": True, "cpus": 1, "phases": [{"phase": "cpu"}]}
        with self.assertRaisesRegex(RuntimeError, "missing metrics pair"):
            RUNNER.require_complete_metrics(report, None)
        with self.assertRaisesRegex(RuntimeError, "unexpected"):
            RUNNER.collect_metrics_line("METRICS BEGIN phase=idle edge=before", report, None, None)
        with self.assertRaisesRegex(RuntimeError, "out-of-order"):
            RUNNER.collect_metrics_line("METRICS BEGIN phase=cpu edge=after", report, None, None)
        capture, _ = RUNNER.collect_metrics_line(
            "METRICS BEGIN phase=cpu edge=before", report, None, None)
        with self.assertRaisesRegex(RuntimeError, "mismatched"):
            RUNNER.collect_metrics_line("METRICS END phase=cpu edge=after", report, None, capture)
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            RUNNER.collect_metrics_line("COMPILEBENCH READY phase=run", report, None, capture)

    def test_old_fixtures_do_not_require_metrics(self):
        report = {"cpus": 1, "phases": [{"phase": "cpu"}]}
        RUNNER.require_complete_metrics(report, None)
        self.assertEqual(RUNNER.collect_metrics_line(
            "COMPILEBENCH READY phase=run", report, None, None), (None, False))
        with self.assertRaisesRegex(RuntimeError, "unexpected"):
            RUNNER.collect_metrics_line("METRICS BEGIN phase=cpu edge=before", report, None, None)


class SerialTransportTest(unittest.TestCase):
    def test_fifo_transport_bridges_host_and_qemu_ends(self):
        with tempfile.TemporaryDirectory(prefix="compile-latency-fifo-") as directory:
            output = Path(directory)
            base, input_fd, output_fd = RUNNER.open_serial_fifo(output)
            qemu_input = os.open(f"{base}.in", os.O_RDONLY | os.O_NONBLOCK)
            qemu_output = os.open(f"{base}.out", os.O_WRONLY | os.O_NONBLOCK)
            try:
                self.assertEqual(os.write(input_fd, b"g"), 1)
                self.assertEqual(os.read(qemu_input, 1), b"g")
                self.assertEqual(os.write(qemu_output, b"o"), 1)
                self.assertEqual(os.read(output_fd, 1), b"o")
            finally:
                os.close(qemu_input)
                os.close(qemu_output)
                RUNNER.close_serial_fifo(base, input_fd, output_fd)
            self.assertFalse(Path(f"{base}.in").exists())
            self.assertFalse(Path(f"{base}.out").exists())


if __name__ == "__main__":
    unittest.main()
