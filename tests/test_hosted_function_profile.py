import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "hosted_function_profile", ROOT / "scripts/analyze-hosted-functions.py"
)
PROFILE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PROFILE
SPEC.loader.exec_module(PROFILE)


class HostedFunctionProfileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def capture(self, records, **changes):
        metadata = dict(
            phase="getuid", repetition=1, count=2, events=len(records),
            dropped=0, invalidation=0, elapsed_ns=100, format_version=1,
        )
        metadata.update(changes)
        path = self.directory / f"getuid-{metadata['repetition']}.json"
        path.write_text(json.dumps(metadata))
        path.with_suffix(".bin").write_bytes(
            b"".join(PROFILE.RECORD.pack(*record) for record in records)
        )
        return path

    def test_nested_inline_calls_use_hook_stack_not_machine_call_site(self):
        path = self.capture([
            (10, 0x100, 0x900, 1),
            (12, 0x200, 0x900, 1),
            (13, 0x300, 0x900, 1),
            (15, 0x300, 0x900, 2),
            (17, 0x200, 0x900, 2),
            (20, 0x100, 0x900, 2),
        ])
        result = PROFILE.load_capture(path)
        self.assertEqual(result.functions[0x100], PROFILE.Cost(1, 5, 10))
        self.assertEqual(result.functions[0x200], PROFILE.Cost(1, 3, 5))
        self.assertEqual(result.functions[0x300], PROFILE.Cost(1, 2, 2))
        self.assertEqual(set(result.edges), {(0x100, 0x200), (0x200, 0x300)})
        self.assertEqual(result.edges[0x200, 0x300].inclusive_ticks, 2)
        self.assertEqual(sum(cost.self_ticks for cost in result.functions.values()), 10)

    def test_repeated_roots_exclude_gaps_and_repetitions_aggregate(self):
        records = [
            (10, 0x100, 0, 1), (20, 0x100, 0, 2),
            (100, 0x100, 0, 1), (130, 0x100, 0, 2),
        ]
        first = PROFILE.load_capture(self.capture(records))
        second = PROFILE.load_capture(self.capture(records, repetition=2))
        result = PROFILE.combine([first, second])["getuid"]
        self.assertEqual(result.root_ticks, 80)
        self.assertEqual(result.functions[0x100], PROFILE.Cost(4, 80, 80))
        self.assertEqual(result.iterations, 4)
        self.assertEqual(result.elapsed_ns, 200)
        report = PROFILE.text_report({"getuid": result}, {0x100: "getuid()"}, 10)
        self.assertIn("1.000", report)
        self.assertIn("getuid() [0x100]", report)
        with self.assertRaisesRegex(ValueError, "duplicate repetition"):
            PROFILE.combine([first, first])

    def test_recursion_preserves_exclusive_time_and_recursive_edge(self):
        result = PROFILE.load_capture(self.capture([
            (10, 0x100, 0, 1), (12, 0x100, 0, 1),
            (16, 0x100, 0, 2), (20, 0x100, 0, 2),
        ]))
        self.assertEqual(result.functions[0x100], PROFILE.Cost(2, 10, 14))
        self.assertEqual(result.edges[0x100, 0x100], PROFILE.Cost(1, 0, 4))
        self.assertEqual(result.root_ticks, 10)

    def test_invalid_event_streams_are_rejected(self):
        streams = [
            ([(10, 1, 0, 2)], "unmatched exit"),
            ([(10, 1, 0, 1), (11, 2, 0, 2)], "unmatched exit"),
            ([(10, 1, 0, 1)], "unclosed"),
            ([(10, 1, 0, 1), (9, 1, 0, 2)], "nonmonotonic"),
            ([(10, 1, 0, 3)], "unknown event kind"),
            ([], "no events"),
        ]
        for records, expected in streams:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(ValueError, expected):
                    PROFILE.load_capture(self.capture(records))

    def test_lost_events_and_unsupported_transitions_are_rejected(self):
        records = [(10, 1, 0, 1), (20, 1, 0, 2)]
        for changes in ({"dropped": 1}, {"invalidation": 1}, {"invalidation": 2}):
            with self.subTest(changes=changes):
                with self.assertRaisesRegex(ValueError, "incomplete capture"):
                    PROFILE.load_capture(self.capture(records, **changes))

    def test_binary_size_and_metadata_are_validated(self):
        records = [(10, 1, 0, 1), (20, 1, 0, 2)]
        for changes in (
            {"events": 1}, {"events": 3}, {"count": 0}, {"count": True},
            {"repetition": 0}, {"elapsed_ns": -1}, {"format_version": 2},
            {"phase": "../../bad"},
        ):
            with self.subTest(changes=changes):
                with self.assertRaises(ValueError):
                    PROFILE.load_capture(self.capture(records, **changes))
        path = self.capture(records)
        path.with_suffix(".bin").write_bytes(path.with_suffix(".bin").read_bytes() + b"x")
        with self.assertRaisesRegex(ValueError, "binary size"):
            PROFILE.load_capture(path)

    def test_symbols_are_exact_and_data_symbols_are_not_functions(self):
        symbols = PROFILE.parse_symbols(
            "00000100 T Actual::function()\n"
            "00000200 b some_data\n"
            "00000300 W inline_function()\n"
            "         U undefined_function\n"
        )
        self.assertEqual(symbols, {0x100: "Actual::function()", 0x300: "inline_function()"})
        self.assertEqual(PROFILE.function_name(0x101, symbols), "0x101")

    def test_callgrind_exports_exclusive_cost_and_inclusive_edges(self):
        result = PROFILE.load_capture(self.capture([
            (10, 0x100, 0, 1), (12, 0x200, 0, 1),
            (16, 0x200, 0, 2), (20, 0x100, 0, 2),
        ]))
        output = PROFILE.callgrind_report(result, {0x100: "parent", 0x200: "child"})
        self.assertIn("events: TscTicks\nsummary: 10", output)
        self.assertIn("fn=parent [0x100]\n0x100 6", output)
        self.assertIn("cfn=child [0x200]\ncalls=1 0x200\n0x100 4", output)
        self.assertIn("fn=child [0x200]\n0x200 4", output)

    def test_cli_validates_all_captures_before_writing_reports(self):
        records = [(10, 0x100, 0, 1), (20, 0x100, 0, 2)]
        self.capture(records)
        self.capture(records, repetition=2, invalidation=1)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(PROFILE.main([str(self.directory)]), 1)
        self.assertFalse((self.directory / "report.txt").exists())
        self.assertEqual(list(self.directory.glob("callgrind.*")), [])

    def test_cli_writes_validated_report_and_callgrind(self):
        self.capture([(10, 0x100, 0, 1), (20, 0x100, 0, 2)])
        with mock.patch.object(PROFILE, "load_symbols", return_value={0x100: "example"}):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(PROFILE.main([str(self.directory)]), 0)
        self.assertIn("no dropped events", (self.directory / "report.txt").read_text())
        self.assertIn("fn=example [0x100]", (self.directory / "callgrind.getuid").read_text())


if __name__ == "__main__":
    unittest.main()
