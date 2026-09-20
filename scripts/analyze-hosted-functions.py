#!/usr/bin/env python3
"""Validate hosted compiler-hook captures and export call graphs offline."""

import argparse
from collections import defaultdict
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import struct
import subprocess
import sys


RECORD = struct.Struct("<QQQQ")


@dataclass
class Cost:
    calls: int = 0
    self_ticks: int = 0
    inclusive_ticks: int = 0


@dataclass
class Profile:
    phase: str
    repetitions: list[int] = field(default_factory=list)
    iterations: int = 0
    events: int = 0
    elapsed_ns: int = 0
    root_ticks: int = 0
    functions: dict[int, Cost] = field(default_factory=lambda: defaultdict(Cost))
    edges: dict[tuple[int, int], Cost] = field(
        default_factory=lambda: defaultdict(Cost)
    )


def load_capture(path):
    metadata = json.loads(path.read_text())
    if not isinstance(metadata, dict):
        raise ValueError(f"{path.name}: expected a metadata object")
    for key in (
        "format_version", "repetition", "count", "events", "dropped",
        "invalidation", "elapsed_ns",
    ):
        if type(metadata.get(key)) is not int or metadata[key] < 0:
            raise ValueError(f"{path.name}: invalid {key}")
    if metadata["format_version"] != 1:
        raise ValueError(f"{path.name}: unsupported format version")
    if not metadata["count"] or not metadata["repetition"]:
        raise ValueError(f"{path.name}: count and repetition must be positive")
    phase = metadata.get("phase")
    if not isinstance(phase, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", phase):
        raise ValueError(f"{path.name}: invalid phase")
    if metadata["dropped"] or metadata["invalidation"]:
        raise ValueError(
            f"{path.name}: incomplete capture: dropped={metadata['dropped']} "
            f"invalidation={metadata['invalidation']}"
        )
    raw = path.with_suffix(".bin").read_bytes()
    if len(raw) != metadata["events"] * RECORD.size:
        raise ValueError(f"{path.name}: binary size disagrees with event count")
    if not raw:
        raise ValueError(f"{path.name}: capture contains no events")

    profile = Profile(
        phase, [metadata["repetition"]], metadata["count"], metadata["events"],
        metadata["elapsed_ns"],
    )
    stack = []
    previous_tick = 0
    for index, (tick, function, _call_site, kind) in enumerate(RECORD.iter_unpack(raw)):
        if tick < previous_tick:
            raise ValueError(f"{path.name}: nonmonotonic timestamp at event {index}")
        previous_tick = tick
        if kind == 1:
            stack.append([function, tick, 0])
            continue
        if kind != 2:
            raise ValueError(f"{path.name}: unknown event kind {kind} at {index}")
        if not stack or stack[-1][0] != function:
            raise ValueError(f"{path.name}: unmatched exit at event {index}")
        _, start, children = stack.pop()
        duration = tick - start
        if duration < children:
            raise ValueError(f"{path.name}: negative self duration at event {index}")
        cost = profile.functions[function]
        cost.calls += 1
        cost.inclusive_ticks += duration
        cost.self_ticks += duration - children
        if stack:
            # Inlined hooks share a machine caller; the logical parent is the stack.
            edge = profile.edges[stack[-1][0], function]
            edge.calls += 1
            edge.inclusive_ticks += duration
            stack[-1][2] += duration
        else:
            profile.root_ticks += duration
    if stack:
        raise ValueError(f"{path.name}: {len(stack)} unclosed function entries")
    return profile


def combine(profiles):
    phases = {}
    for profile in profiles:
        total = phases.setdefault(profile.phase, Profile(profile.phase))
        if set(total.repetitions) & set(profile.repetitions):
            raise ValueError(f"{profile.phase}: duplicate repetition")
        total.repetitions.extend(profile.repetitions)
        for name in ("iterations", "events", "elapsed_ns", "root_ticks"):
            setattr(total, name, getattr(total, name) + getattr(profile, name))
        for name in ("functions", "edges"):
            for key, cost in getattr(profile, name).items():
                result = getattr(total, name)[key]
                result.calls += cost.calls
                result.self_ticks += cost.self_ticks
                result.inclusive_ticks += cost.inclusive_ticks
    return phases


def parse_symbols(output):
    symbols = {}
    for line in output.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) != 3 or fields[1] not in ("t", "T", "w", "W", "i", "I"):
            continue
        try:
            address = int(fields[0], 16)
        except ValueError:
            continue
        symbols.setdefault(address, fields[2])
    return symbols


def load_symbols(directory):
    symbols = {}
    for name in ("kernel", "kernel.debug"):
        binary = directory / name
        if not binary.is_file():
            continue
        result = subprocess.run(
            ["nm", "-n", "-C", "--defined-only", str(binary)],
            capture_output=True, text=True, check=False,
        )
        if result.returncode == 0:
            for address, symbol in parse_symbols(result.stdout).items():
                symbols.setdefault(address, symbol)
    if not symbols:
        raise ValueError("nm found no function symbols in kernel or kernel.debug")
    return symbols


def function_name(address, symbols):
    symbol = symbols.get(address)
    return f"{symbol} [0x{address:x}]" if symbol else f"0x{address:x}"


def text_report(phases, symbols, limit):
    lines = [
        "Hosted function instrumentation (validated, no dropped events)",
        "TSC ticks are elapsed instrumented durations, not CPU cycles or native timings.",
        "Counts cover instrumented functions, including logical inline invocations.",
        "Uninstrumented host library internals are opaque; their time stays in the caller.",
        "Hook overhead and host preemption remain in the durations; no overhead subtraction.",
        "Only complete root spans are timed; gaps between roots are excluded from TSC totals.",
        "Inclusive times overlap across nested/recursive calls and must not be summed.",
        "Symbols require exact function-address matches; unknown addresses stay hexadecimal.",
    ]
    for phase, profile in sorted(phases.items()):
        unknown = sum(cost.calls for address, cost in profile.functions.items() if address not in symbols)
        lines.extend([
            "", f"Phase: {phase}",
            f"Repetitions: {sorted(profile.repetitions)}; iterations: {profile.iterations}; "
            f"events: {profile.events}; elapsed_ns: {profile.elapsed_ns}",
            f"Covered root TSC ticks: {profile.root_ticks}; "
            f"function invocations: {sum(c.calls for c in profile.functions.values())}; "
            f"unsymbolized invocations: {unknown}",
            "", "Top invocation counts:",
            "        calls   calls/iteration         self ticks    inclusive ticks  function",
        ])
        for address, cost in sorted(profile.functions.items(), key=lambda x: (-x[1].calls, x[0]))[:limit]:
            lines.append(
                f"{cost.calls:13d} {cost.calls / profile.iterations:17.3f} "
                f"{cost.self_ticks:18d} {cost.inclusive_ticks:18d}  {function_name(address, symbols)}"
            )
        lines.extend(["", "Top self time:",
                      " self %         self ticks        child ticks    inclusive ticks        calls  function"])
        for address, cost in sorted(profile.functions.items(), key=lambda x: (-x[1].self_ticks, x[0]))[:limit]:
            share = 100 * cost.self_ticks / profile.root_ticks if profile.root_ticks else 0
            lines.append(
                f"{share:7.2f} {cost.self_ticks:18d} "
                f"{cost.inclusive_ticks - cost.self_ticks:18d} {cost.inclusive_ticks:18d} "
                f"{cost.calls:12d}  {function_name(address, symbols)}"
            )
        lines.extend(["", "Top caller/callee inclusive time:",
                      "        calls   calls/iteration    inclusive ticks  caller -> callee"])
        for (parent, child), cost in sorted(profile.edges.items(), key=lambda x: (-x[1].inclusive_ticks, x[0]))[:limit]:
            lines.append(
                f"{cost.calls:13d} {cost.calls / profile.iterations:17.3f} "
                f"{cost.inclusive_ticks:18d}  {function_name(parent, symbols)} -> "
                f"{function_name(child, symbols)}"
            )
    return "\n".join(lines) + "\n"


def callgrind_report(profile, symbols):
    lines = [
        "# callgrind format", "version: 1", "creator: analyze-hosted-functions.py",
        f"desc: {profile.phase}; {profile.iterations} iterations; logical inline calls",
        "desc: TSC ticks include instrumentation overhead; not CPU cycles or native timings",
        "positions: instr", "events: TscTicks", f"summary: {profile.root_ticks}",
        "ob=kernel", "fl=hosted compiler instrumentation",
    ]
    children = defaultdict(list)
    for (parent, child), cost in profile.edges.items():
        children[parent].append((child, cost))
    for address, cost in sorted(profile.functions.items()):
        lines.extend(["", f"fn={function_name(address, symbols)}",
                      f"0x{address:x} {cost.self_ticks}"])
        for child, edge in sorted(children[address]):
            lines.extend([
                f"cfn={function_name(child, symbols)}",
                f"calls={edge.calls} 0x{child:x}",
                f"0x{address:x} {edge.inclusive_ticks}",
            ])
    lines.append(f"totals: {profile.root_ticks}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--limit", type=int, default=30)
    args = parser.parse_args(argv)
    try:
        if args.limit < 1:
            raise ValueError("--limit must be positive")
        paths = sorted(args.directory.glob("*-*.json"))
        if not paths:
            raise ValueError("no phase-repetition.json captures found")
        # Validate every capture before creating any report from a partial run.
        phases = combine(load_capture(path) for path in paths)
        symbols = load_symbols(args.directory)
        report = text_report(phases, symbols, args.limit)
        (args.directory / "report.txt").write_text(report)
        for phase, profile in phases.items():
            (args.directory / f"callgrind.{phase}").write_text(callgrind_report(profile, symbols))
        print(report, end="")
    except (OSError, ValueError) as error:
        print(f"Invalid hosted function profile: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
