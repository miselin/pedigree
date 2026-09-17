#!/usr/bin/env python3
"""Trace-format and reporting contracts independent of a guest or QEMU build."""

import importlib.util
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("trace_summary", Path(__file__).with_name("summarize.py"))
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)
HEADER = SUMMARY.HEADER + "\n"
BEGIN = "B\t1\t1000\t2000\t8000\t3000\n"
FIRST = "I\t1\t1\t1000\t8000\t3000\t202\t90\tnop\n"
END = "E\t1\tcomplete\t1\t2000\t0\n"
FOOTER = "# finished traces=1 requested=1 matching_starts=1\n"


class RecordsTest(unittest.TestCase):
    def parse(self, text):
        return [r for r in SUMMARY.records((text + FOOTER).splitlines(keepends=True)) if r["kind"] != "F"]

    def test_disassembly_tabs_and_metadata(self):
        result = self.parse(HEADER + "# ignored metadata\n" + BEGIN + FIRST.replace("nop", "nop\t ") + END)
        self.assertEqual(result[1]["disassembly"], "nop\t ")
        self.assertEqual(result[-1]["count"], 1)
        self.assertEqual(result[0]["start_cr3"], 0x3000)

    def test_rejects_incomplete_and_inconsistent_records(self):
        cases = {
            "missing format header": BEGIN + FIRST + END,
            "duplicate or late": HEADER + HEADER + BEGIN + FIRST + END,
            "without a terminal": HEADER + BEGIN + FIRST,
            "sequence is not contiguous": HEADER + BEGIN + FIRST.replace("\t1\t1\t", "\t1\t2\t") + END,
            "terminal count differs": HEADER + BEGIN + FIRST + END.replace("complete\t1", "complete\t2"),
            "return PC": HEADER + BEGIN + FIRST + END.replace("\t2000\t", "\t2001\t"),
            "outside an active": HEADER + BEGIN + FIRST + END + FIRST,
            "first instruction differs": HEADER + BEGIN + FIRST.replace("1000", "1001") + END,
            "invalid x86 instruction bytes": HEADER + BEGIN + FIRST.replace("\t90\t", "\t9\t") + END,
            "duplicate trace ID": HEADER + BEGIN + FIRST + END + BEGIN,
        }
        for expected, text in cases.items():
            with self.subTest(expected=expected), self.assertRaisesRegex(ValueError, expected):
                self.parse(text)

    def test_truncated_footer_and_missing_captures_are_rejected(self):
        for text, error in [
            (HEADER + BEGIN + FIRST + END, "missing capture-count footer"),
            (HEADER + BEGIN + FIRST + END + FOOTER.rstrip("\n"), "unterminated record"),
            (HEADER + BEGIN + FIRST + END + FOOTER.replace("requested=1", "requested=2"), "footer capture count"),
        ]:
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                list(SUMMARY.records(text.splitlines(keepends=True)))

    def test_software_interrupt_has_two_notifications_at_same_sequence(self):
        result = self.parse(HEADER + BEGIN + FIRST + "D\t1\t1\t2\t1000\t1000\n"
                            + "D\t1\t1\t1\t1000\t1100\n" + END)
        self.assertEqual([r["type"] for r in result if r["kind"] == "D"], [2, 1])

    def test_limit_is_valid_terminal_but_not_complete(self):
        result = self.parse(HEADER + BEGIN + FIRST + END.replace("complete", "limit").replace("2000", "1001"))
        self.assertEqual(result[-1]["status"], "limit")

    def test_discontinuity_sequence_and_mask(self):
        for row in ["D\t1\t2\t1\t1000\t1100\n", "D\t1\t1\t8\t1000\t1100\n"]:
            with self.subTest(row=row), self.assertRaisesRegex(ValueError, "invalid discontinuity"):
                self.parse(HEADER + BEGIN + FIRST + row + END)


class ReportTest(unittest.TestCase):
    def test_byte_mismatch_fails_validation_and_checks_distinct_observations(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            trace = output / "trace.tsv"
            trace.write_text(HEADER + BEGIN + FIRST + FIRST.replace("\t1\t1\t", "\t1\t2\t")
                             + FIRST.replace("\t1\t1\t", "\t1\t3\t").replace("\t90\t", "\t91\t")
                             + END.replace("complete\t1", "complete\t3") + FOOTER)
            checked = []

            def verify(event):
                checked.append(event["bytes"])
                return "matched" if event["bytes"] == "90" else "mismatched"

            report = SUMMARY.summarize(trace, output, lambda pc: ("kernel", "entry"), verify)
            self.assertEqual(checked, ["90", "91"])
            self.assertFalse(report["validation"]["valid"])
            self.assertFalse(report["validation"]["complete"])
            self.assertEqual(report["byte_validation"]["verdict"], "fail")
            self.assertEqual(report["byte_validation"]["matched"], 1)
            self.assertEqual(report["byte_validation"]["mismatched"], 1)

    def test_flat_counts_preserve_interrupt_and_cr3_evidence(self):
        text = (HEADER + BEGIN + FIRST + "D\t1\t1\t1\t1000\t1100\n"
                + "I\t1\t2\t1100\t7ff0\t4000\t2\tfa\tcli\n"
                + "I\t1\t3\t1101\t7ff0\t4000\t2\t0f32\trdmsr\n"
                + END.replace("complete\t1", "complete\t3") + FOOTER)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            trace = output / "trace.tsv"
            trace.write_text(text)
            report = SUMMARY.summarize(trace, output, lambda pc: ("kernel", "handler"))
            report["symbols"] = {"module_mapping": {"status": "unverified", "reason": "no anchors"},
                                 "runtime_identity": "not runtime verified"}
            SUMMARY.write_reports(report, output)
            saved = json.loads((output / "summary.json").read_text())
            self.assertEqual(saved["instruction_dispatches"], 3)
            self.assertTrue(saved["validation"]["complete"])
            self.assertEqual(saved["special_instructions"], {"cli": 1, "rdmsr": 1})
            self.assertEqual(saved["discontinuities"], {"interrupt_notification": 1})
            self.assertEqual(saved["captures"][0]["cr3_changes"], 1)
            self.assertFalse(saved["captures"][0]["uninterrupted"])
            callgrind = (output / "callgrind.out").read_text()
            self.assertIn("events: InsnDispatch", callgrind)
            self.assertNotIn("calls=", callgrind)
            self.assertIn("CR3-CHANGED", (output / "symbolized-trace.txt").read_text())

    def test_partial_report_is_marked_invalid(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            trace = output / "trace.tsv"
            trace.write_text(HEADER + BEGIN + FIRST)
            report = SUMMARY.summarize(trace, output, lambda pc: ("unknown", "unknown"))
            self.assertFalse(report["validation"]["valid"])
            self.assertFalse(report["validation"]["complete"])
            self.assertEqual(report["instruction_dispatches"], 1)

    def test_prefix_and_control_register_detection(self):
        self.assertEqual(SUMMARY.special_instructions("lock cmpxchgq %rax, (%rbx)"), ["lock_prefix"])
        self.assertEqual(SUMMARY.special_instructions("movq %cr3, %rax"), ["mov_cr3"])
        self.assertEqual(SUMMARY.special_instructions("iretw "), ["iretw"])


class MappingTest(unittest.TestCase):
    def test_unsized_assembly_does_not_absorb_cpp_source(self):
        symbols = SUMMARY.Symbolizer.__new__(SUMMARY.Symbolizer)
        symbols.tables = {"kernel": SimpleNamespace(
            addresses=[0x1000], entries={0x1000: (0, "memzero")}, path=Path("kernel.debug"),
            executable_ranges=[(0x1000, 0x1100)], lookup=lambda pc: "memzero")}
        symbols.ranges, symbols.provenance, symbols.source_overrides = {}, {}, {}
        symbols.unsized_ends = {}
        output = "memzero\n??:0\nmemzero\n/project/SlamAllocator.cc:876\n"
        with patch.object(SUMMARY.subprocess, "run", return_value=SimpleNamespace(stdout=output)):
            symbols.resolve_unsized_sources([0x1000, 0x1020], "nm", "addr2line")
        self.assertIn("memzero", symbols.lookup(0x1000)[1])
        self.assertEqual(symbols.lookup(0x1020)[1], "SlamAllocator.cc [DWARF source; unresolved function]")
        self.assertNotIn("memzero", symbols.lookup(0x1030)[1])
        self.assertEqual(symbols.provenance["unsized_source_check"]["source_overrides"],
                         {"kernel:0x1020": "/project/SlamAllocator.cc:876"})

    def test_assembly_without_nm_size_stops_at_executable_section_end(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "kernel"
            path.write_bytes(b"mock ELF")
            table = SimpleNamespace(entries={}, addresses=[], executable_ranges=[(0x1000, 0x1020)],
                                    lookup=lambda address: None)
            symbols = SUMMARY.Symbolizer.__new__(SUMMARY.Symbolizer)
            symbols.tables, symbols.byte_ranges, symbols.byte_provenance, symbols.ranges = {}, {}, {}, {}
            with patch.object(SUMMARY.COMPILE, "Symbols", return_value=table), \
                    patch.object(SUMMARY.subprocess, "check_output", return_value="0000000000001000 T entry\n"):
                symbols.add_table("kernel", path, "unused")
            self.assertEqual(table.entries[0x1000], (0, "entry"))
            self.assertIn("entry (zero-size symbol; next-symbol/section bound)", symbols.lookup(0x101f)[1])
            self.assertEqual(symbols.lookup(0x1020)[0], "unknown")

    def test_byte_comparison_stays_within_executable_file_bytes(self):
        symbols = SUMMARY.Symbolizer.__new__(SUMMARY.Symbolizer)
        symbols.ranges = {}
        symbols.tables = {"kernel": SimpleNamespace(executable_ranges=[(0x1000, 0x1004)])}
        symbols.byte_ranges = {"kernel": [(0x1000, 0x1004, b"\x90\x0f\x32\xc3")]}
        self.assertEqual(symbols.verify({"pc": 0x1001, "bytes": "0f32"}), "matched")
        self.assertEqual(symbols.verify({"pc": 0x1001, "bytes": "0f33"}), "mismatched")
        self.assertEqual(symbols.verify({"pc": 0x1003, "bytes": "c390"}), "unmapped")
        self.assertEqual(symbols.verify({"pc": 0x2000, "bytes": "90"}), "unmapped")

    def test_retained_anchors_do_not_prove_current_runtime_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            initrd = root / "initrd.tar"
            data = b"test module bytes"
            with tarfile.open(initrd, "w") as archive:
                member = tarfile.TarInfo("posix.so")
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
            base = 0xffffffff90000000
            manifest = {
                "initrd_sha256": hashlib.sha256(initrd.read_bytes()).hexdigest(),
                "modules": {"posix": {"base": base, "end": base + 4096}},
                "verified_anchors": {"old1": {}, "old2": {}},
                "archive_entries": [{"module_name_from_elf": "posix", "archive_path": "posix.so",
                                     "sha256": hashlib.sha256(data).hexdigest()}],
            }
            mapping = root / "mapping.json"
            mapping.write_text(json.dumps(manifest))
            args = SimpleNamespace(kernel=root / "kernel", initrd=initrd, module_map=mapping,
                                   serial=None, user=None, nm="unused")
            args.kernel.write_bytes(b"test kernel bytes")
            table = SimpleNamespace(provenance=lambda: {"path": "temporary"},
                                    lookup=lambda address: "posix_getuid", executable_ranges=[(0, 4096)],
                                    entries={}, addresses=[])
            with patch.object(SUMMARY.COMPILE, "Symbols", return_value=table), \
                    patch.object(SUMMARY.subprocess, "check_output", return_value=""):
                result = SUMMARY.Symbolizer(args, root)
                self.assertEqual(result.mapping["status"], "unverified")
                self.assertIn("layout unverified", result.lookup(base)[1])
                self.assertEqual(result.provenance["posix"]["path"], f"{initrd.resolve()}::posix")
                manifest["initrd_sha256"] = "0" * 64
                mapping.write_text(json.dumps(manifest))
                with self.assertRaisesRegex(ValueError, "initrd SHA256"):
                    SUMMARY.Symbolizer(args, root)


if __name__ == "__main__":
    unittest.main()
