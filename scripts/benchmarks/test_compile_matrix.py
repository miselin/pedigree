#!/usr/bin/env python3
"""Check RAM matrix cleanup and rejection of incomplete disk-I/O evidence."""

import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


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


class TraceLinkProtocolTest(unittest.TestCase):
    def arguments(self, *extra):
        argv = ["run-compile-matrix.py", "--image", "fixture.qcow2", "--output", "trace",
                "--os", "pedigree", "--firmware-code", "firmware.fd",
                "--mode", "trace-link", "--storage", "ramfs", *extra]
        with patch("sys.argv", argv), patch("sys.stderr", new_callable=io.StringIO):
            return RUNNER.arguments()

    def test_two_phases_are_distinct_from_normal_matrix(self):
        self.assertEqual(RUNNER.phases("trace-link"), ["warm-link", "trace-link"])
        self.assertEqual(len(RUNNER.phases("run")), 38)
        self.assertNotIn("trace-link", RUNNER.phases("run"))

    def test_quick_mode_rejects_full_matrix_profiles_and_checks_inputs(self):
        self.assertEqual(self.arguments("--mode", "quick").mode, "quick")
        self.assertEqual(len(RUNNER.phases("quick")), 6)
        self.assertEqual(self.arguments("--mode", "quick", "--profile-phase", "r1-full")
                         .profile_phase, "r1-full")
        with self.assertRaises(SystemExit):
            self.arguments("--mode", "quick", "--profile-phase", "r2-full")
        identities = report()["identities"]
        RUNNER.validate_identities(identities, "quick")
        with self.assertRaisesRegex(ValueError, "incomplete or changed"):
            RUNNER.validate_identities(identities[:-1], "quick")

    def test_trace_requires_pedigree_ramfs(self):
        self.assertEqual(self.arguments().mode, "trace-link")
        for extra in (("--storage", "disk"),
                      ("--os", "linux", "--linux-root", "linux.img",
                       "--linux-kernel", "vmlinuz", "--linux-initrd", "initrd")):
            with self.subTest(arguments=extra), self.assertRaises(SystemExit):
                self.arguments(*extra)

    def test_profile_must_match_one_of_the_two_phases(self):
        for phase in RUNNER.phases("trace-link"):
            self.assertEqual(self.arguments("--profile-phase", phase).profile_phase, phase)
        with self.assertRaises(SystemExit):
            self.arguments("--profile-phase", "r1-link")

    def test_reports_distinguish_plain_links_from_instrumented_runs(self):
        cases = (("trace-link", (), True), ("run", (), False), ("link", (), False),
                 ("link", ("--plugin", "profile.so"), True),
                 ("link", ("--profile-phase", "r5-link"), True))
        for mode, extra, expected in cases:
            with self.subTest(mode=mode, extra=extra), tempfile.TemporaryDirectory(
                    prefix="compile-trace-test-") as directory:
                args = self.arguments("--mode", mode, *extra)
                args.image = Path(directory) / "fixture.qcow2"
                args.image.touch()
                args.output = Path(directory) / "report"
                with patch.object(RUNNER, "arguments", return_value=args), patch.object(
                        RUNNER.subprocess, "check_output", side_effect=RuntimeError("no guest")), \
                        patch("sys.stdout", new_callable=io.StringIO):
                    self.assertEqual(RUNNER.main(), 1)
                data = json.loads((args.output / "report.json").read_text())
                self.assertIs(data["instrumented"], expected)

    def test_link_modes_require_all_unchanged_frozen_inputs(self):
        for mode in ("trace-link", "link"):
            with self.subTest(mode=mode):
                identities = report("pedigree")["identities"]
                RUNNER.validate_identities(identities, mode)
                with self.assertRaisesRegex(ValueError, "incomplete or changed"):
                    RUNNER.validate_identities(identities[:-1], mode)
                identities[-1]["fnv1a64"] = "aaaaaaaaaaaaaaaa"
                with self.assertRaisesRegex(ValueError, "incomplete or changed"):
                    RUNNER.validate_identities(identities, mode)

    def test_trace_is_rejected_by_timing_comparison(self):
        data = report("pedigree")
        data["mode"] = "trace-link"
        data["instrumented"] = True
        with tempfile.TemporaryDirectory(prefix="compile-trace-test-") as directory:
            path = Path(directory) / "report.json"
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "uninstrumented"):
                SUMMARY.load_report(path, "pedigree")


class LinkOnlyProtocolTest(unittest.TestCase):
    def arguments(self, os_name="pedigree", *extra):
        argv = ["run-compile-matrix.py", "--image", "fixture.qcow2", "--output", "link",
                "--os", os_name, "--mode", "link", "--storage", "ramfs",
                "--firmware-code", "firmware.fd", "--linux-root", "linux.img",
                "--linux-kernel", "vmlinuz", "--linux-initrd", "initrd", *extra]
        with patch("sys.argv", argv), patch("sys.stderr", new_callable=io.StringIO):
            return RUNNER.arguments()

    def test_warmup_and_five_links_have_exact_order(self):
        self.assertEqual(RUNNER.phases("link"),
                         ["warm-link", "r1-link", "r2-link", "r3-link", "r4-link", "r5-link"])
        self.assertEqual(RUNNER.phases("trace-link"), ["warm-link", "trace-link"])

    def test_link_supports_both_kernels_but_requires_ramfs(self):
        for os_name in ("linux", "pedigree"):
            with self.subTest(os=os_name):
                self.assertEqual(self.arguments(os_name).mode, "link")
                with self.assertRaises(SystemExit):
                    self.arguments(os_name, "--storage", "disk")
                with self.assertRaises(SystemExit):
                    self.arguments(os_name, "--mode", "prepare")

    def test_link_profiles_are_limited_to_its_six_phases(self):
        for phase in RUNNER.phases("link"):
            self.assertEqual(self.arguments("pedigree", "--profile-phase", phase).profile_phase,
                             phase)
        for phase in ("trace-link", "r1-full", "cpu-before"):
            with self.subTest(phase=phase), self.assertRaises(SystemExit):
                self.arguments("pedigree", "--profile-phase", phase)

    def test_linux_link_bootstrap_uses_marker_aware_ram_driver(self):
        script = RUNNER.linux_bootstrap(self.arguments("linux"))
        self.assertIn("/root/compile-bench/compile-matrix-ramroot --stdio", script)
        self.assertNotIn(" --link", script)
        self.assertIn("umount /mnt/pedigree/tmp/compile-matrix-root/proc", script)


if __name__ == "__main__":
    unittest.main()
