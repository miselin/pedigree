import json
import os
import tempfile
import unittest
from pathlib import Path

from scripts.run_sarif_analysis import (
    AnalysisError,
    CompileEntry,
    analysis_arguments,
    load_compile_entries,
    run_analysis,
)


class SarifAnalysisTests(unittest.TestCase):
    def make_compiler(self, root: Path, version: str = "15.3.0", exitcode: int = 0):
        compiler = root / "x86_64-pedigree-gcc"
        compiler.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import pathlib\n"
            "import sys\n"
            f"version = {version!r}\n"
            f"exitcode = {exitcode}\n"
            "if '-dumpmachine' in sys.argv:\n"
            "    print('x86_64-pedigree')\n"
            "elif '-dumpfullversion' in sys.argv:\n"
            "    print(version)\n"
            "else:\n"
            "    source = next(arg for arg in sys.argv if arg.endswith('.c'))\n"
            "    sink = next(arg for arg in sys.argv if arg.startswith('-fdiagnostics-add-output='))\n"
            "    sink = pathlib.Path(sink.split('file=', 1)[1])\n"
            "    document = {\n"
            "        '$schema': 'https://example.invalid/sarif.json',\n"
            "        'version': '2.1.0',\n"
            "        'runs': [{'results': [{'message': {'text': pathlib.Path(source).name}}]}],\n"
            "    }\n"
            "    sink.write_text(json.dumps(document), encoding='utf-8')\n"
            "    print('text diagnostic for ' + source, file=sys.stderr)\n"
            "    sys.exit(exitcode)\n",
            encoding="utf-8",
        )
        compiler.chmod(0o755)
        return compiler

    def write_database(self, root: Path, compiler: Path):
        source_root = root / "source"
        first = source_root / "one" / "duplicate.c"
        second = source_root / "two" / "duplicate.c"
        first.parent.mkdir(parents=True)
        second.parent.mkdir(parents=True)
        first.write_text("int one(void) { return 1; }\n", encoding="utf-8")
        second.write_text("int two(void) { return 2; }\n", encoding="utf-8")
        database = root / "compile_commands.json"
        database.write_text(
            json.dumps(
                [
                    {
                        "directory": str(root),
                        "file": str(source),
                        "arguments": [
                            str(compiler),
                            "-Werror",
                            "-c",
                            str(source),
                            "-o",
                            str(root / f"{index}.o"),
                        ],
                    }
                    for index, source in enumerate((first, second))
                ]
            ),
            encoding="utf-8",
        )
        return source_root, database

    def test_unique_per_tu_outputs_are_merged(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            compiler = self.make_compiler(root)
            source_root, database = self.write_database(root, compiler)
            output = root / "sarif"

            report, unit_count, finding_count = run_analysis(
                database, source_root, output, jobs=2
            )

            per_tu = sorted((output / "translation-units").glob("*.sarif"))
            self.assertEqual(len(per_tu), 2)
            self.assertNotEqual(per_tu[0].name, per_tu[1].name)
            text_logs = sorted((output / "text-diagnostics").glob("*.log"))
            self.assertEqual(len(text_logs), 2)
            self.assertIn("text diagnostic", text_logs[0].read_text(encoding="utf-8"))
            merged = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(len(merged["runs"]), 2)
            self.assertEqual(unit_count, 2)
            self.assertEqual(finding_count, 2)

    def test_legacy_compiler_is_rejected_before_output_creation(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            compiler = self.make_compiler(root, version="8.3.0")
            source_root, database = self.write_database(root, compiler)
            output = root / "sarif"

            with self.assertRaisesRegex(AnalysisError, "requires GCC 15"):
                run_analysis(database, source_root, output, jobs=1)

            self.assertFalse(output.exists())

    def test_analysis_command_cannot_overwrite_build_outputs(self):
        entry = CompileEntry(
            0,
            Path("/source/test.c"),
            Path("/build"),
            (
                "/compiler/gcc",
                "-Werror=return-type",
                "-w",
                "-MD",
                "-MF",
                "test.d",
                "-c",
                "/source/test.c",
                "-o",
                "test.o",
            ),
        )

        sarif_path = Path("/analysis/test.sarif")
        arguments = analysis_arguments(entry, sarif_path)

        self.assertNotIn("test.o", arguments)
        self.assertNotIn("test.d", arguments)
        self.assertNotIn("-w", arguments)
        self.assertFalse(any(arg.startswith("-Werror") for arg in arguments))
        self.assertIn("-c", arguments)
        self.assertNotIn("-fsyntax-only", arguments)
        output_index = arguments.index("-o")
        self.assertEqual(arguments[output_index + 1], os.devnull)
        self.assertIn("-fanalyzer", arguments)
        self.assertEqual(
            arguments[-1],
            "-fdiagnostics-add-output="
            "sarif:version=2.1,file=/analysis/test.sarif",
        )

    def test_bundled_sources_are_excluded(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            source_root = root / "source"
            project_source = source_root / "src/system/kernel/owned.c"
            bundled_source = (
                source_root / "src/modules/system/config/sqlite3/sqlite3.c"
            )
            project_source.parent.mkdir(parents=True)
            bundled_source.parent.mkdir(parents=True)
            project_source.write_text("int owned;\n", encoding="utf-8")
            bundled_source.write_text("int bundled;\n", encoding="utf-8")
            database = root / "compile_commands.json"
            database.write_text(
                json.dumps(
                    [
                        {
                            "directory": str(root),
                            "file": str(source),
                            "arguments": ["gcc", "-c", str(source)],
                        }
                        for source in (project_source, bundled_source)
                    ]
                ),
                encoding="utf-8",
            )

            entries = load_compile_entries(database, source_root)

            self.assertEqual(
                [entry.source for entry in entries], [project_source.resolve()]
            )

    def test_failed_analysis_does_not_publish_partial_output(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            compiler = self.make_compiler(root, exitcode=1)
            source_root, database = self.write_database(root, compiler)
            output = root / "sarif"

            with self.assertRaisesRegex(AnalysisError, "failed for 2"):
                run_analysis(database, source_root, output, jobs=2)

            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
