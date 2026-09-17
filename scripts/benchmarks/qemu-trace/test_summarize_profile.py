#!/usr/bin/env python3
"""Aggregate profile contracts without building or running a guest."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location("profile_summary", Path(__file__).with_name("summarize-profile.py"))
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)
HEADER = SUMMARY.HEADER + "\n"
BEGIN = "B\t400100\t3000\t8000\n"
FIRST = "P\tffffffff80001000\t7\t90\tnop\n"
SECOND = "P\tffffffff80001001\t3\t0f32\trdmsr\n"
USER = "U\t19\n"
SYSCALL = "S\t102\t2\n"
DISRUPTION = "D\t1\tffffffff80001000\tffffffff80001001\t4\n"
END = "E\tcomplete\t10\t19\n"
PROFILE = HEADER + BEGIN + FIRST + SECOND + USER + SYSCALL + DISRUPTION + END


class RecordsTest(unittest.TestCase):
    def parse(self, text):
        return list(SUMMARY.records(text.splitlines(keepends=True)))

    def test_complete_profile_preserves_decimal_counts_and_disassembly_tabs(self):
        rows = self.parse(PROFILE.replace("nop", "nop\t "))
        self.assertEqual(rows[1]["count"], 7)
        self.assertEqual(rows[1]["pc"], 0xffffffff80001000)
        self.assertEqual(rows[1]["disassembly"], "nop\t ")
        self.assertEqual(rows[-1]["kernel_dispatches"], 10)
        self.assertEqual(rows[-1]["user_dispatches"], 19)

    def test_rejects_incomplete_brackets_and_wrong_totals(self):
        cases = [
            (PROFILE.removeprefix(HEADER), "missing format header"),
            (HEADER + PROFILE, "duplicate or late format header"),
            (PROFILE.replace(BEGIN, ""), "outside the workload bracket"),
            (PROFILE.replace(BEGIN, BEGIN + BEGIN), "duplicate begin"),
            (PROFILE.removesuffix(END), "complete B/E bracket"),
            (PROFILE.rstrip("\n"), "unterminated record"),
            (PROFILE + USER, "record after terminal E"),
            (PROFILE.replace(USER, ""), "missing user total"),
            (PROFILE.replace(END, "E\tcomplete\t11\t19\n"), "terminal totals"),
            (PROFILE.replace(END, "E\tcomplete\t10\t20\n"), "terminal totals"),
            (PROFILE.replace(END, "E\tlimit\t10\t19\n"), "unknown terminal status"),
        ]
        for text, error in cases:
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                self.parse(text)

    def test_rejects_duplicates_and_nonpositive_counted_rows(self):
        for row, error in [(FIRST, "duplicate PC"), (USER, "duplicate user total"),
                           (SYSCALL, "duplicate syscall number"), (DISRUPTION, "duplicate discontinuity")]:
            with self.subTest(row=row), self.assertRaisesRegex(ValueError, error):
                self.parse(PROFILE.replace(row, row + row))
        for before, after in [(FIRST, FIRST.replace("\t7\t", "\t0\t")),
                              (SYSCALL, "S\t102\t0\n"),
                              (DISRUPTION, DISRUPTION.replace("\t4\n", "\t0\n"))]:
            with self.subTest(after=after), self.assertRaisesRegex(ValueError, "positive"):
                self.parse(PROFILE.replace(before, after))

    def test_rejects_invalid_kernel_address_bytes_and_notification_mask(self):
        for before, after, error in [
            (FIRST, FIRST.replace("ffffffff80001000", "400100"), "high-half kernel"),
            (FIRST, FIRST.replace("\t90\t", "\t9\t"), "instruction bytes"),
            (FIRST, FIRST.replace("\t7\t", "\t-1\t"), "invalid base-10"),
            (DISRUPTION, DISRUPTION.replace("D\t1", "D\t8"), "discontinuity type"),
        ]:
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                self.parse(PROFILE.replace(before, after))

    def test_exit_and_explicit_zero_user_total_are_representable(self):
        rows = self.parse(PROFILE.replace(USER, "U\t0\n").replace(END, "E\texit\t10\t0\n"))
        self.assertEqual(rows[-1]["status"], "exit")
        self.assertEqual(rows[-1]["user_dispatches"], 0)

    def test_same_address_with_distinct_observed_bytes_is_not_a_duplicate(self):
        rows = self.parse(PROFILE.replace(SECOND, SECOND.replace("ffffffff80001001", "ffffffff80001000")))
        self.assertEqual(sum(row["count"] for row in rows if row["kind"] == "P"), 10)


class ReportTest(unittest.TestCase):
    def report(self, output, text=PROFILE, verify=lambda event: "matched"):
        profile = output / "profile.tsv"
        profile.write_text(text)
        return SUMMARY.summarize(profile, lambda pc: ("kernel", "function"), verify)

    def test_counts_are_weighted_and_flat_output_excludes_user_aggregate(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = self.report(output)
            report["symbols"] = {"module_mapping": {"status": "anchored", "reason": "test anchors"}}
            SUMMARY.write_reports(report, output)
            self.assertTrue(report["validation"]["complete"])
            self.assertEqual(report["kernel_dispatches"], 10)
            self.assertEqual(report["user_dispatches"], 19)
            self.assertEqual(report["byte_validation"]["matched"], 2)
            self.assertEqual(report["byte_validation"]["dispatches_by_result"], {"matched": 10})
            self.assertEqual(report["special_instructions"], {"rdmsr": 3})
            self.assertEqual(report["notification_counts"], {"interrupt_notification": 4})
            self.assertEqual(report["syscalls"], [{"number": 102, "name": "getuid", "calls": 2}])
            self.assertEqual(report["functions"][0]["dispatches"], 10)
            callgrind = (output / "callgrind.out").read_text()
            self.assertIn("summary: 10\n", callgrind)
            self.assertNotIn("calls=", callgrind)
            self.assertIn("no chronology", (output / "summary.md").read_text())
            self.assertEqual(json.loads((output / "summary.json").read_text())["event"], "InsnDispatch")

    def test_mismatched_bytes_fail_and_unmapped_bytes_remain_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = self.report(output, verify=lambda event: "mismatched" if event["bytes"] == "90" else "unmapped")
            self.assertTrue(report["validation"]["format_valid"])
            self.assertFalse(report["validation"]["valid"])
            self.assertFalse(report["validation"]["complete"])
            self.assertEqual(report["byte_validation"]["mismatched"], 1)
            self.assertEqual(report["byte_validation"]["unmapped"], 1)
            self.assertEqual(report["byte_validation"]["dispatches_by_result"], {"mismatched": 7, "unmapped": 3})

    def test_early_exit_and_bad_totals_do_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            exited = self.report(output, PROFILE.replace("E\tcomplete", "E\texit"))
            self.assertTrue(exited["validation"]["valid"])
            self.assertFalse(exited["validation"]["complete"])
            invalid = self.report(output, PROFILE.replace(END, "E\tcomplete\t9\t19\n"))
            self.assertFalse(invalid["validation"]["format_valid"])
            self.assertFalse(invalid["validation"]["complete"])
            self.assertEqual(invalid["kernel_dispatches"], 10)


if __name__ == "__main__":
    unittest.main()
