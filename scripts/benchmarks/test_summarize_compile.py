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
            "benchmark_user_return_ablation": 3,
            "activity_ur_interrupt_ablation_eligible": 17,
            "activity_ur_interrupt_ablation_fast": 16,
            "activity_ur_interrupt_ablation_fallback": 1,
            "activity_ur_syscall_ablation_eligible": 9,
            "activity_ur_syscall_ablation_fast": 9,
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
        self.assertEqual(user_return["ablation"]["mask"], 3)
        self.assertEqual(user_return["ablation"]["interrupt"], {
            "eligible": 17, "fast": 16, "fallback": 1})
        self.assertEqual(user_return["ablation"]["syscall"], {
            "eligible": 9, "fast": 9, "fallback": 0})

        user_entry = activity["user_entry"]
        self.assertEqual(user_entry["sample_period"], 256)
        self.assertEqual(user_entry["capture_calls"], 1024)
        self.assertEqual(user_entry["capture_samples"], 4)
        self.assertEqual(user_entry["capture_tsc_total"], 9876)
        self.assertEqual(user_entry["capture_tsc_buckets"][7], 4)
        self.assertEqual(user_entry["empty_tsc_samples"], 8)

    def test_syscall_timing_is_sorted_and_accounted(self):
        metric = {
            "system_us": 1000,
            "syscall_timing_calls": 5,
            "syscall_timing_kernel_ns": 400000,
            "sc0_calls": 4,
            "sc0_kernel_ns": 100000,
            "sc9_calls": 1,
            "sc9_kernel_ns": 300000,
        }

        timing = SUMMARY.syscall_timing(metric)

        self.assertEqual(timing["calls"], 5)
        self.assertEqual(timing["kernel_ns"], 400000)
        self.assertEqual(timing["system_percent"], 40)
        self.assertEqual([entry["name"] for entry in timing["entries"]], ["mmap", "read"])
        self.assertEqual(timing["entries"][0]["kernel_ns_per_call"], 300000)

    def test_vm_diagnostics_extracts_only_vm_counters(self):
        metric = {
            "vm_publish_calls": 10,
            "vm_publish_overlap_probe_visits": 550,
            "system_us": 1000,
        }

        self.assertEqual(SUMMARY.vm_diagnostics(metric), {
            "publish_calls": 10,
            "publish_overlap_probe_visits": 550,
        })
        self.assertIsNone(SUMMARY.vm_diagnostics({"system_us": 1000}))


if __name__ == "__main__":
    unittest.main()
