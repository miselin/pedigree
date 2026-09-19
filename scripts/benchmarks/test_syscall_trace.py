#!/usr/bin/env python3
"""Focused checks for structured syscall trace reconstruction."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("summarize-syscall-trace.py")
SPEC = importlib.util.spec_from_file_location("syscall_trace", SCRIPT)
TRACE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRACE)
NAMES = TRACE.syscall_names()
NUMBERS = {name: number for number, name in NAMES.items()}


def record(kind, ident, **fields):
    values = {"id": ident, **fields}
    return "[123] STRACE " + kind + " " + " ".join(
        f"{key}={value if isinstance(value, str) else hex(value)}" for key, value in values.items())


def entry(ident, name="read", pid=10, tid=11, args=None):
    args = [0] * 6 if args is None else args
    return [record("E", ident, pid=pid, tid=tid, abi="L", nr=NUMBERS[name]),
            record("A", ident, n=0, x=args[0], y=args[1], z=args[2]),
            record("A", ident, n=3, x=args[3], y=args[4], z=args[5])]


def path_records(ident, data=b"/tmp/input.o", arg=0, status="ok"):
    records = [record("P", ident, arg=arg, off=offset, data=data[offset:offset+32].hex())
               for offset in range(0, len(data), 32)]
    return records + [record("S", ident, arg=arg, status=status)]


def leave(ident, raw=0, err=0, ns=100):
    return record("X", ident, raw=raw & ((1 << 64) - 1), err=err, ns=ns)


def call(ident, name="read", args=None, raw=0, err=0, paths=None, ns=100):
    result = entry(ident, name, args=args)
    for arg, data in (paths or {}).items():
        result += path_records(ident, data, arg)
    return result + [leave(ident, raw, err, ns)]


def workload(records):
    return "\n".join(["COMPILEBENCH ACK phase=trace-link", *records,
                      "COMPILEBENCH DONE phase=trace-link", "COMPILEBENCH PASS END"])


class SyscallTraceTest(unittest.TestCase):
    def test_local_linux_mapping_and_hex_numbers(self):
        self.assertEqual(NAMES[257], "openat")
        kind, fields = TRACE.decode_record("STRACE E id=10 pid=a tid=f abi=L nr=101")
        self.assertEqual((kind, fields["id"], fields["pid"], fields["nr"]), ("E", 16, 10, 257))

    def test_interleaved_calls_preserve_entry_and_exit_order(self):
        records = entry(1, "open") + path_records(1)
        records += entry(2, pid=20, tid=21) + [leave(2, raw=9, ns=50), leave(1, raw=3, ns=1000)]
        report = TRACE.parse_trace(workload(records))
        self.assertTrue(report["complete"], report["issues"])
        self.assertEqual([item["id"] for item in report["calls"]], [1, 2])
        self.assertGreater(report["calls"][0]["exit_line"], report["calls"][1]["exit_line"])
        self.assertEqual(report["processes"][0]["key"], 10)
        self.assertIn('text="/tmp/input.o"', TRACE.timeline(report))

    def test_signed_dispatch_results_and_linux_errno(self):
        records = call(1, raw=-1, err=2) + call(2, raw=-13) + call(3, raw=5)
        report = TRACE.parse_trace(workload(records))
        results = [item["exit"] for item in report["calls"]]
        self.assertEqual(results[0]["raw_signed"], -1)
        self.assertEqual([result["linux_visible"] for result in results], [-2, -13, 5])
        self.assertEqual(report["syscalls"][0]["errors"], 2)

    def test_exec_and_exit_are_dispatch_actions_only_on_success(self):
        records = call(1, "execve", paths={0: b"/bin/ld"})
        records += call(2, "execve", raw=-1, err=2, paths={0: b"/missing"})
        records += call(3, "exit_group")
        results = [item["exit"] for item in TRACE.parse_trace(workload(records))["calls"]]
        self.assertEqual([result["kind"] for result in results], ["dispatch_action", "return", "dispatch_action"])
        self.assertEqual([result["linux_visible"] for result in results], [None, -2, None])

    def test_hex_paths_preserve_whitespace_non_utf8_and_empty(self):
        data = b"/tmp/with space\n" + b"a" * 40 + b"\xff"
        records = call(1, "open", paths={0: data}) + call(2, "open", paths={0: b""})
        report = TRACE.parse_trace(workload(records))
        self.assertTrue(report["complete"], report["issues"])
        path = report["calls"][0]["paths"][0]
        self.assertEqual(path["hex"], data.hex())
        self.assertEqual(path["text"], data.decode("utf-8", errors="backslashreplace"))
        self.assertIn("\\n", TRACE.timeline(report))
        self.assertEqual(report["calls"][1]["paths"][0]["text"], "")

    def test_path_truncation_and_invalid_prefix_are_explicit(self):
        records = entry(1, "open") + path_records(1, b"x" * 256, status="truncated") + [leave(1)]
        records += entry(2, "open") + path_records(2, b"/prefix", status="invalid") + [leave(2, raw=-1, err=14)]
        report = TRACE.parse_trace(workload(records))
        self.assertTrue(report["call_records_complete"], report["issues"])
        self.assertFalse(report["paths_complete"])
        self.assertEqual(report["calls"][1]["paths"][0]["text"], "/prefix")

    def test_missing_duplicate_out_of_order_and_bad_chunks(self):
        complete_open = entry(1, "open") + path_records(1) + [leave(1)]
        variants = {
            "missing exit": entry(1),
            "missing args": entry(1)[:1] + [leave(1)],
            "missing path": entry(1, "open") + [leave(1)],
            "missing status": entry(1, "open") + path_records(1)[:-1] + [leave(1)],
            "duplicate entry": entry(1) + entry(1) + [leave(1)],
            "duplicate exit": call(1) + [leave(1)],
            "out of order": [leave(1)] + entry(1),
            "missing call IDs": call(1) + call(3),
            "path gap": entry(1, "open") + [record("P", 1, arg=0, off=32, data="61"), record("S", 1, arg=0, status="ok"), leave(1)],
            "truncated line": complete_open + ["STRACE X id=0x2 raw="],
        }
        for name, records in variants.items():
            with self.subTest(name=name):
                report = TRACE.parse_trace(workload(records))
                self.assertFalse(report["complete"])
                self.assertFalse(report["call_records_complete"])
                self.assertTrue(report["issues"])

    def test_workload_boundaries_are_required_and_enforced(self):
        bare = "\n".join(call(1))
        report = TRACE.parse_trace(bare)
        self.assertTrue(report["call_records_complete"])
        self.assertFalse(report["complete"])
        self.assertFalse(report["workload_complete"])
        for text in (workload(call(1)).replace("COMPILEBENCH PASS END", ""),
                     bare + "\n" + workload(call(2)),
                     workload(call(1)) + "\nCOMPILEBENCH DONE phase=trace-link"):
            self.assertFalse(TRACE.parse_trace(text)["workload_complete"])

    def test_descriptor_attribution_openat_dup_close_and_mmap(self):
        records = call(1, "openat", args=[(1 << 64)-100, 1, 0, 0, 0, 0], raw=3, paths={1: b"/tmp/input.o"})
        records += call(2, "dup", args=[3, 0, 0, 0, 0, 0], raw=8)
        records += call(3, "close", args=[3, 0, 0, 0, 0, 0])
        records += call(4, "read", args=[8, 1000, 100, 0, 0, 0], raw=100)
        records += call(5, "mmap", args=[0, 4096, 1, 2, 8, 0], raw=4096)
        records += call(6, "read", args=[3, 1000, 100, 0, 0, 0], raw=-1, err=9)
        report = TRACE.parse_trace(workload(records))
        self.assertTrue(report["complete"], report["issues"])
        self.assertEqual(report["calls"][0]["fd_paths"][0]["fd"], -100)
        for index in (3, 4):
            self.assertEqual(report["calls"][index]["fd_paths"][0]["path"], "/tmp/input.o")
        self.assertIsNone(report["calls"][5]["fd_paths"][0]["path"])
        group = next(row for row in report["fd_path_syscalls"] if row["key"] == (10, "read", 8, "/tmp/input.o"))
        self.assertEqual(group["count"], 1)

    def test_elapsed_statistics_use_nearest_rank_and_count_unpaired(self):
        records = []
        for ident in range(1, 21):
            records += call(ident, ns=ident)
        records += entry(21)
        stats = TRACE.parse_trace(workload(records))["syscalls"][0]
        self.assertEqual((stats["count"], stats["completed"]), (21, 20))
        self.assertEqual((stats["total_ns"], stats["median_ns"], stats["p95_ns"], stats["max_ns"]), (210, 10.5, 19, 20))

    def test_cli_preserves_raw_log_and_emits_incomplete_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = workload(entry(1)).encode()
            source = root / "serial.log"
            source.write_bytes(raw)
            result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(source), "--output", str(root/"report")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(source.read_bytes(), raw)
            self.assertFalse(json.loads((root/"report/summary.json").read_text())["complete"])
            self.assertIn("UNPAIRED", (root/"report/timeline.txt").read_text())


if __name__ == "__main__":
    unittest.main()
