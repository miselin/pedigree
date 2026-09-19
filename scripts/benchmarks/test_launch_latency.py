#!/usr/bin/env python3
"""Focused checks for launch benchmark trace attribution."""

import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "launch_latency", Path(__file__).with_name("run-launch-latency.py"))
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)

BOUNDARY = 'qmp_enter_query_blockstats {}'


def issue(port=0, tag=0):
    return (f"process_ncq_command ahci(0xa075e4ae0)[{port}][tag:{tag}]: "
            "NCQ op 0x60 on sectors [8,15]")


def read(port=0, tag=0):
    return (f"execute_ncq_command_read ahci(0xa075e4ae0)[{port}][tag:{tag}]: "
            "NCQ reading 8 sectors from LBA 8")


def finish(port=0, tag=0):
    return (f"ncq_finish ahci(0xa075e4ae0)[{port}][tag:{tag}]: "
            "NCQ transfer finished")


class TracePhasesTest(unittest.TestCase):
    def parse(self, lines, phases=("launch-0",)):
        return RUNNER.trace_phases("\n".join(lines) + "\n", list(phases))

    def test_installed_qemu_read_format_records_byte_size(self):
        phase, = self.parse([BOUNDARY, issue(), read(), finish(), BOUNDARY])
        self.assertEqual(phase["read_sizes"], {"4096": 1})
        self.assertEqual(phase["read_commands"], 1)
        self.assertEqual(phase["ncq_submissions"], 1)
        self.assertEqual(phase["ncq_completions"], 1)
        self.assertEqual(phase["maximum_ncq"], 1)
        self.assertEqual(phase["active_at_start"], 0)
        self.assertEqual(phase["active_at_end"], 0)
        self.assertTrue(phase["clean_boundaries"])

    def test_overlapping_tags_are_distinct_across_ports(self):
        phase, = self.parse([
            BOUNDARY, issue(), read(), issue(tag=1), read(tag=1),
            issue(port=1), read(port=1), finish(tag=1), finish(port=1),
            finish(), BOUNDARY,
        ])
        self.assertEqual(phase["maximum_ncq"], 3)
        self.assertEqual(phase["ncq_submissions"], 3)
        self.assertEqual(phase["ncq_completions"], 3)
        self.assertEqual(phase["read_sizes"], {"4096": 3})
        self.assertEqual(phase["active_at_end"], 0)

    def test_duplicate_active_tag_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "tag reused"):
            self.parse([BOUNDARY, issue(), issue(), finish(), BOUNDARY])

    def test_orphan_completion_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "completion without submission"):
            self.parse([BOUNDARY, finish(), BOUNDARY])

    def test_missing_boundary_is_rejected(self):
        for lines in ([], [BOUNDARY], [BOUNDARY, issue(), read(), finish()]):
            with self.subTest(lines=lines):
                with self.assertRaisesRegex(ValueError, "missing.*boundaries"):
                    self.parse(lines)

    def test_carryover_activity_remains_visible_at_both_boundaries(self):
        first, second = self.parse([
            issue(), BOUNDARY, issue(tag=1), read(tag=1), finish(), BOUNDARY,
            BOUNDARY, finish(tag=1), BOUNDARY,
        ], ("launch-0", "launch-1"))
        self.assertEqual((first["active_at_start"], first["active_at_end"]), (1, 1))
        self.assertEqual((second["active_at_start"], second["active_at_end"]), (1, 0))
        self.assertFalse(first["clean_boundaries"])
        self.assertFalse(second["clean_boundaries"])
        self.assertEqual(first["maximum_ncq"], 2)
        self.assertEqual(second["maximum_ncq"], 1)
        self.assertEqual((second["ncq_submissions"], second["ncq_completions"]), (0, 1))

    def test_unrecognized_read_size_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unrecognized NCQ read size"):
            self.parse([
                BOUNDARY, issue(),
                "execute_ncq_command_read ahci(0xa075e4ae0)[0][tag:0]: unexpected format",
                finish(), BOUNDARY,
            ])


class SerialLinesTest(unittest.TestCase):
    def test_diagnostic_mode_accepts_kernel_records(self):
        self.assertEqual(RUNNER.serial_lines(
            bytearray(), b"(NN) [10.2] STRACE E id=0x1\n", allow_kernel_log=True),
            ["(NN) [10.2] STRACE E id=0x1"])

    def test_observed_kernel_log_inside_metric_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "kernel serial logging is enabled"):
            RUNNER.serial_lines(bytearray(),
                b"LAUNCHBENCH metric phase=launch-1 first_us=768(NN) [1789010912.0] "
                b"ZombiePipe: freeing 0xffff9000c4b11030\n")

    def test_fragmented_kernel_prefix_is_rejected(self):
        wire = bytearray()
        self.assertEqual(RUNNER.serial_lines(wire, b"LAUNCHBENCH RE(N"), [])
        with self.assertRaisesRegex(ValueError, "kernel serial logging is enabled"):
            RUNNER.serial_lines(wire, b"N) [10.2] log\nADY phase=launch-0\n")

    def test_clean_fragmented_metric_preserves_digits(self):
        wire = bytearray()
        self.assertEqual(RUNNER.serial_lines(wire, b"LAUNCHBENCH metric first_us=768"), [])
        self.assertEqual(RUNNER.serial_lines(wire, b"57\n"),
                         ["LAUNCHBENCH metric first_us=76857"])


@unittest.skipUnless(shutil.which("cc"), "native C compiler unavailable")
class BenchmarkDriverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="launch-latency-test-")
        cls.directory = Path(cls.temporary.name)
        cls.binary = cls.directory / "launch-latency"
        subprocess.run([
            shutil.which("cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            str(Path(__file__).with_name("launch-latency.c")), "-o", str(cls.binary),
        ], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_fixture(self, operation, order, size=8 * 1024 * 1024, *, corrupt=False,
                    read_size=None):
        data = bytearray((i * 37 + (i >> 8) * 17 + 0x53) & 255 for i in range(size))
        if corrupt:
            data[4096 + 123] ^= 1
        fixture = self.directory / "launch-input.bin"
        fixture.write_bytes(data)
        command = [str(self.binary), "--iterations", "3"]
        if read_size is not None:
            command += ["--read-size", str(read_size)]
        command += [operation, str(fixture), order]
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        metrics = re.findall(
            r"LAUNCHBENCH metric phase=(\S+) first_us=(\d+) total_us=(\d+) "
            r"bytes=(\d+) checksum=(\d+)", result.stdout)
        return result, metrics, size + sum(data)

    def assert_phases(self, result, metrics, phase, size, checksum):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LAUNCHBENCH PASS END", result.stdout)
        self.assertEqual([metric[0] for metric in metrics], [f"{phase}-{i}" for i in range(3)])
        for name, first, total, covered, actual_checksum in metrics:
            self.assertIn(f"LAUNCHBENCH READY phase={name}", result.stdout)
            self.assertIn(f"LAUNCHBENCH DONE phase={name}", result.stdout)
            self.assertLessEqual(int(first), int(total))
            self.assertEqual(int(covered), size)
            self.assertEqual(int(actual_checksum), checksum)

    def test_mmap_orders_validate_full_8mib_span_and_warm_repeats(self):
        for order in ("sequential", "permuted"):
            with self.subTest(order=order):
                result, metrics, checksum = self.run_fixture("mmap", order)
                self.assert_phases(result, metrics, f"mmap-{order}", 8 * 1024 * 1024, checksum)

    def test_mmap_validation_detects_corruption_between_touched_bytes(self):
        result, metrics, _ = self.run_fixture("mmap", "permuted", 16 * 4096, corrupt=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("operation=fixture-pattern", result.stderr)
        self.assertEqual(metrics, [])
        self.assertNotIn("LAUNCHBENCH PASS END", result.stdout)

    def test_only_permuted_mapping_requires_power_of_two_page_count(self):
        result, metrics, checksum = self.run_fixture("mmap", "sequential", 3 * 4096)
        self.assert_phases(result, metrics, "mmap-sequential", 3 * 4096, checksum)
        result, metrics, _ = self.run_fixture("mmap", "permuted", 3 * 4096)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("operation=fixture-power-of-two", result.stderr)
        self.assertEqual(metrics, [])

    def test_scalar_read_size_option_remains_independent_of_mmap_page_touches(self):
        result, metrics, checksum = self.run_fixture(
            "read", "sequential", 2 * 65536, read_size=65536)
        self.assert_phases(result, metrics, "read-sequential", 2 * 65536, checksum)
        result, metrics, checksum = self.run_fixture(
            "mmap", "permuted", 16 * 4096, read_size=131072)
        self.assert_phases(result, metrics, "mmap-permuted", 16 * 4096, checksum)


if __name__ == "__main__":
    unittest.main()
