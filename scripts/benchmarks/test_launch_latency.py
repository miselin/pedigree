#!/usr/bin/env python3
"""Focused checks for launch benchmark trace attribution."""

import importlib.util
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
