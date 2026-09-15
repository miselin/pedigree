#!/usr/bin/env python3
"""Focused checks for compile benchmark diagnostic summaries."""

import importlib.util
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location(
    "summarize_compile", Path(__file__).with_name("summarize-compile.py"))
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


class ActivityDiagnosticsTest(unittest.TestCase):
    def test_user_return_and_entry_diagnostics_are_structured(self):
        metric = {
            "activity_interrupts": 3,
            "activity_ur_sample_period": 64,
            "activity_ur_interrupt_tail_samples": 2,
            "activity_ur_interrupt_tail_total_ns": 1234,
            "activity_ur_interrupt_tail_h4": 2,
            "activity_ur_fault_handled_samples": 1,
            "activity_ur_interrupt_affinity_waited_samples": 1,
            "activity_ue_sample_period": 256,
            "activity_ue_capture_calls": 1024,
            "activity_ue_capture_samples": 4,
            "activity_ue_capture_tsc_total": 9876,
            "activity_ue_capture_tsc_h7": 4,
            "activity_ue_empty_tsc_samples": 8,
        }

        activity = SUMMARY.activity_diagnostics(metric)

        user_return = activity["user_return"]
        self.assertEqual(user_return["sample_period"], 64)
        self.assertEqual(user_return["stages"]["interrupt_tail"]["samples"], 2)
        self.assertEqual(user_return["stages"]["interrupt_tail"]["total_ns"], 1234)
        self.assertEqual(user_return["stages"]["interrupt_tail"]["duration_buckets"][4], 2)
        self.assertEqual(user_return["fault_handled_samples"], 1)
        self.assertEqual(user_return["interrupt_affinity_waited_samples"], 1)
        self.assertEqual(user_return["stages"]["syscall_tail"]["samples"], 0)

        user_entry = activity["user_entry"]
        self.assertEqual(user_entry["sample_period"], 256)
        self.assertEqual(user_entry["capture_calls"], 1024)
        self.assertEqual(user_entry["capture_samples"], 4)
        self.assertEqual(user_entry["capture_tsc_total"], 9876)
        self.assertEqual(user_entry["capture_tsc_buckets"][7], 4)
        self.assertEqual(user_entry["empty_tsc_samples"], 8)


if __name__ == "__main__":
    unittest.main()
