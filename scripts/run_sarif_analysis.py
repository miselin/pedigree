#!/usr/bin/env python3

"""Run GCC's static analyzer over a CMake compilation database."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
EXCLUDED_SOURCE_DIRECTORIES = (
    Path("src/modules/system/config/sqlite3"),
    Path("src/system/kernel/machine/mach_pc/x86emu"),
)
SARIF_SCHEMA = (
    "https://docs.oasis-open.org/sarif/sarif/v2.1.0/errata01/"
    "os/schemas/sarif-schema-2.1.0.json"
)


class AnalysisError(RuntimeError):
    pass


@dataclass(frozen=True)
class CompileEntry:
    index: int
    source: Path
    directory: Path
    arguments: tuple[str, ...]

    def output_name(self) -> str:
        digest = hashlib.sha256(
            "\0".join(self.arguments).encode("utf-8")
        ).hexdigest()[:12]
        basename = re.sub(r"[^A-Za-z0-9_.-]", "_", self.source.name)
        return f"{self.index:05d}-{basename}-{digest}.sarif"


@dataclass(frozen=True)
class AnalysisResult:
    entry: CompileEntry
    document: dict[str, Any]
    returncode: int
    stdout: str
    stderr: str


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def load_compile_entries(
    compile_commands: Path, source_root: Path
) -> list[CompileEntry]:
    try:
        raw_entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AnalysisError(
            f"Could not read compilation database {compile_commands}: {error}"
        ) from error

    if not isinstance(raw_entries, list):
        raise AnalysisError("The compilation database must contain a JSON array.")

    source_root = source_root.resolve()
    entries: list[CompileEntry] = []
    for index, raw in enumerate(raw_entries):
        if not isinstance(raw, dict) or "directory" not in raw or "file" not in raw:
            raise AnalysisError(f"Compilation database entry {index} is malformed.")

        directory = Path(raw["directory"]).resolve()
        source = Path(raw["file"])
        if not source.is_absolute():
            source = directory / source
        source = source.resolve()

        if source.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if not _is_within(source, source_root):
            continue
        relative_source = source.relative_to(source_root)
        if any(
            _is_within(relative_source, excluded)
            for excluded in EXCLUDED_SOURCE_DIRECTORIES
        ):
            continue

        if "arguments" in raw:
            arguments = tuple(str(argument) for argument in raw["arguments"])
        elif "command" in raw:
            arguments = tuple(shlex.split(str(raw["command"])))
        else:
            raise AnalysisError(
                f"Compilation database entry {index} has no command or arguments."
            )
        if not arguments:
            raise AnalysisError(f"Compilation database entry {index} has no compiler.")

        entries.append(CompileEntry(index, source, directory, arguments))

    if not entries:
        raise AnalysisError(
            f"No in-tree C or C++ translation units were found in {compile_commands}."
        )
    return entries


def _compiler_output(compiler: str, directory: Path, argument: str) -> str:
    try:
        result = subprocess.run(
            [compiler, argument],
            cwd=directory,
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise AnalysisError(
            f"Could not inspect analysis compiler {compiler}: {error}"
        ) from error
    return result.stdout.strip()


def validate_compilers(entries: Iterable[CompileEntry]) -> None:
    compilers: dict[tuple[str, Path], CompileEntry] = {}
    for entry in entries:
        compilers.setdefault((entry.arguments[0], entry.directory), entry)

    for (compiler, directory), _entry in compilers.items():
        target = _compiler_output(compiler, directory, "-dumpmachine")
        version = _compiler_output(compiler, directory, "-dumpfullversion")
        try:
            major = int(version.split(".", 1)[0])
        except ValueError as error:
            raise AnalysisError(
                f"Could not parse compiler version {version!r} from {compiler}."
            ) from error

        if target != "x86_64-pedigree" or major < 15:
            raise AnalysisError(
                "SARIF analysis requires GCC 15 or newer targeting "
                f"x86_64-pedigree; {compiler} reports {target} GCC {version}."
            )


def analysis_arguments(entry: CompileEntry, sarif_path: Path) -> list[str]:
    arguments: list[str] = []
    skip_next = False
    options_with_values = {"-o", "-MF", "-MT", "-MQ"}
    options_without_values = {
        "-c",
        "-MD",
        "-MMD",
        "-MP",
        "-MG",
        "-fanalyzer",
        "-w",
    }

    for argument in entry.arguments:
        if skip_next:
            skip_next = False
            continue
        if argument in options_with_values:
            skip_next = True
            continue
        if argument in options_without_values:
            continue
        if argument == "-Werror" or argument.startswith("-Werror="):
            continue
        if argument.startswith(("-o", "-MF", "-MT", "-MQ")):
            continue
        if argument.startswith("-fdiagnostics-format="):
            continue
        if argument.startswith("-fdiagnostics-add-output="):
            continue
        arguments.append(argument)

    arguments.extend(
        [
            "-c",
            "-o",
            os.devnull,
            "-fanalyzer",
            "-fdiagnostics-add-output="
            f"sarif:version=2.1,file={sarif_path.resolve()}",
        ]
    )
    return arguments


def _run_entry(
    entry: CompileEntry, per_tu_dir: Path, diagnostics_dir: Path
) -> AnalysisResult:
    output_path = (per_tu_dir / entry.output_name()).resolve()
    try:
        result = subprocess.run(
            analysis_arguments(entry, output_path),
            cwd=entry.directory,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        raise AnalysisError(
            f"Could not analyze {entry.source} with {entry.arguments[0]}: {error}"
        ) from error

    text_output = result.stderr
    if result.stdout:
        text_output += ("\n" if text_output else "") + result.stdout
    if text_output:
        (diagnostics_dir / f"{output_path.stem}.log").write_text(
            text_output, encoding="utf-8"
        )

    try:
        document = json.loads(output_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        detail = text_output.strip() or "no compiler output"
        raise AnalysisError(
            f"Compiler did not emit valid SARIF for {entry.source}: {detail}"
        ) from error
    if not isinstance(document, dict) or not isinstance(document.get("runs"), list):
        raise AnalysisError(f"Compiler emitted malformed SARIF for {entry.source}.")

    return AnalysisResult(
        entry, document, result.returncode, result.stdout, result.stderr
    )


def merge_results(results: Sequence[AnalysisResult]) -> dict[str, Any]:
    runs: list[dict[str, Any]] = []
    for result in sorted(results, key=lambda item: item.entry.index):
        for run in result.document["runs"]:
            if not isinstance(run, dict):
                raise AnalysisError(
                    f"Compiler emitted malformed SARIF for {result.entry.source}."
                )
            properties = run.setdefault("properties", {})
            if isinstance(properties, dict):
                properties["pedigreeCompileCommandIndex"] = result.entry.index
                properties["pedigreeSource"] = str(result.entry.source)
            runs.append(run)
    return {"$schema": SARIF_SCHEMA, "version": "2.1.0", "runs": runs}


def _first_diagnostic(result: AnalysisResult) -> str:
    for run in result.document["runs"]:
        if not isinstance(run, dict):
            continue
        for diagnostic in run.get("results", []):
            if not isinstance(diagnostic, dict):
                continue
            message = diagnostic.get("message", {})
            if isinstance(message, dict) and isinstance(message.get("text"), str):
                return message["text"]
    return (
        result.stderr.strip()
        or result.stdout.strip()
        or "compiler exited unsuccessfully"
    )


def run_analysis(
    compile_commands: Path, source_root: Path, output_dir: Path, jobs: int
) -> tuple[Path, int, int]:
    if jobs < 1:
        raise AnalysisError("Analysis jobs must be a positive integer.")
    if output_dir.exists():
        raise AnalysisError(f"SARIF output directory already exists: {output_dir}")

    entries = load_compile_entries(compile_commands, source_root)
    validate_compilers(entries)

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(prefix=f".{output_dir.name}.", dir=output_dir.parent)
    )
    try:
        per_tu_dir = staging / "translation-units"
        per_tu_dir.mkdir()
        diagnostics_dir = staging / "text-diagnostics"
        diagnostics_dir.mkdir()
        results: list[AnalysisResult] = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            futures = [
                executor.submit(_run_entry, entry, per_tu_dir, diagnostics_dir)
                for entry in entries
            ]
            for future in concurrent.futures.as_completed(futures):
                results.append(future.result())

        failures = [result for result in results if result.returncode != 0]
        if failures:
            details = "; ".join(
                f"{result.entry.source}: {_first_diagnostic(result)}"
                for result in failures[:5]
            )
            if len(failures) > 5:
                details += f"; and {len(failures) - 5} more"
            raise AnalysisError(
                f"GCC analysis failed for {len(failures)} translation unit(s): "
                f"{details}"
            )

        merged = merge_results(results)
        report = staging / "pedigree.sarif"
        report.write_text(
            json.dumps(merged, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        finding_count = sum(
            len(run.get("results", []))
            for run in merged["runs"]
            if isinstance(run.get("results", []), list)
        )
        os.replace(staging, output_dir)
        return output_dir / report.name, len(entries), finding_count
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def positive_integer(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=repository)
    parser.add_argument(
        "--jobs", type=positive_integer, default=min(4, os.cpu_count() or 1)
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        report, unit_count, finding_count = run_analysis(
            args.compile_commands.resolve(),
            args.source_root.resolve(),
            args.output_dir.resolve(),
            args.jobs,
        )
    except AnalysisError as error:
        print(f"SARIF analysis failed: {error}", file=sys.stderr)
        return 1

    print(f"Analyzed {unit_count} translation units; found {finding_count} diagnostics.")
    print(f"SARIF report: {report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
