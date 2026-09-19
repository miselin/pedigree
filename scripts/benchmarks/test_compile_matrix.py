#!/usr/bin/env python3
"""Check RAM matrix cleanup and rejection of incomplete disk-I/O evidence."""

import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUNNER = load("matrix_runner", "run-compile-matrix.py")
SUMMARY = load("matrix_summary", "summarize-compile-matrix.py")


def snapshot(reads=17):
    return [{"device": "benchmark", "stats": {
        "rd_operations": reads, "wr_operations": 3, "flush_operations": 1}}]


def report(os_name="linux"):
    phases = SUMMARY.expected_phases()
    return {
        "result": "PASS", "os": os_name, "mode": "run", "storage": "ramfs",
        "instrumented": False, "profile_phase": None, "cpus": 1,
        "configuration": {"mode": "run", "profile": "none"},
        "expected_phases": phases, "ramroot_settled_blocks": snapshot(),
        "ramroot": {"files": 5, "bytes": 50, "fnv1a64": "1234567890abcdef"},
        "identities": [{"stage": stage, "path": path, "bytes": 10,
                        "fnv1a64": "0123456789abcdef"}
                       for stage in ("before", "after") for path in sorted(SUMMARY.INPUTS)],
        "phases": [{"phase": phase, "gate_acknowledged": True, "host_wall_s": 0.2,
                    "metric": {"rc": 0, "total_us": 100000, "user_us": 50000,
                               "system_us": 40000, "checksum": 7},
                    "blocks_before": snapshot(), "blocks_after": snapshot()}
                   for phase in phases],
    }


class RamMatrixEvidenceTest(unittest.TestCase):
    def read_report(self, data):
        with tempfile.TemporaryDirectory(prefix="compile-matrix-test-") as directory:
            path = Path(directory) / "report.json"
            path.write_text(json.dumps(data))
            return SUMMARY.load_report(path, data["os"])

    def test_linux_cleanup_unmounts_children_before_parents(self):
        script = RUNNER.linux_bootstrap(SimpleNamespace(
            mode="run", storage="ramfs", setup_iso=None, profile_phase=None))
        paths = ["/mnt/pedigree/tmp/compile-matrix-root/proc",
                 "/mnt/pedigree/tmp/compile-matrix-root", "/mnt/pedigree/tmp",
                 "/mnt/pedigree"]
        offsets = [script.index(f"umount {path} ||") for path in paths]
        self.assertEqual(offsets, sorted(offsets))
        self.assertGreater(script.index("echo MATRIX-LINUX-CLEAN-END"), offsets[-1])

    def test_install_only_populates_fixture_without_running_preparation(self):
        script = RUNNER.linux_bootstrap(SimpleNamespace(
            mode="install", storage="disk", setup_iso=Path("setup.iso"), profile_phase=None))
        self.assertIn("cp -a /mnt/setup/root/. /mnt/pedigree/", script)
        self.assertIn("configuration mode=install profile=none", script)
        self.assertIn("MATRIX-LINUX-CLEAN-END", script)
        self.assertNotIn("chroot", script)
        self.assertNotIn("--prepare", script)
        self.assertEqual(RUNNER.phases("install"), [])

    def test_block_requests_requires_device_counters(self):
        incomplete = snapshot()
        del incomplete[0]["stats"]["flush_operations"]
        for value in ([], incomplete):
            with self.subTest(snapshot=value), self.assertRaises((ValueError, KeyError)):
                RUNNER.block_requests(value)

    def test_complete_reports_with_quiet_disks_are_accepted(self):
        for os_name in ("linux", "pedigree"):
            with self.subTest(os=os_name):
                data, inputs = self.read_report(report(os_name))
                self.assertEqual(len(data["phases"]), 38)
                self.assertEqual(set(inputs), SUMMARY.INPUTS)

    def test_phase_io_is_rejected(self):
        data = report()
        data["phases"][1]["blocks_after"] = snapshot(reads=18)
        with self.assertRaisesRegex(ValueError, "disk requests"):
            self.read_report(data)

    def test_io_between_phases_is_rejected_even_when_each_phase_is_quiet(self):
        data = report()
        for phase in data["phases"][1:]:
            phase["blocks_before"] = snapshot(reads=18)
            phase["blocks_after"] = snapshot(reads=18)
        with self.assertRaisesRegex(ValueError, "disk requests"):
            self.read_report(data)

    def test_missing_settling_or_phase_counters_are_rejected(self):
        data = report()
        del data["ramroot_settled_blocks"]
        with self.assertRaises((ValueError, KeyError)):
            self.read_report(data)
        data = report()
        del data["phases"][1]["blocks_before"][0]["stats"]["wr_operations"]
        with self.assertRaises((ValueError, KeyError)):
            self.read_report(data)


if __name__ == "__main__":
    unittest.main()
