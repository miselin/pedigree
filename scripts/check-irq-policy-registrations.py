#!/usr/bin/env python3

"""Reject IRQ registrations which bypass an explicit named policy."""

from pathlib import Path
import re
import sys


CALL = re.compile(
    r"(?<!::)\b(register(?:Hard)?(?:Isa|Pci)IrqHandler|"
    r"register(?:Isa|Pci)SplitIrq)\s*\("
)


def call_end(source: str, start: int) -> int:
    depth = 1
    i = start
    quote = ""
    while i < len(source):
        if quote:
            if source[i] == "\\":
                i += 2
                continue
            if source[i] == quote:
                quote = ""
            i += 1
            continue
        if source.startswith("//", i):
            newline = source.find("\n", i + 2)
            i = len(source) if newline < 0 else newline + 1
            continue
        if source.startswith("/*", i):
            close = source.find("*/", i + 2)
            i = len(source) if close < 0 else close + 2
            continue
        if source[i] in {'"', "'"}:
            quote = source[i]
        elif source[i] == "(":
            depth += 1
        elif source[i] == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    failures = []
    for path in sorted((root / "src").rglob("*.cc")):
        source = path.read_text(encoding="utf-8", errors="replace")
        for match in CALL.finditer(source):
            end = call_end(source, match.end())
            if end < 0:
                failures.append((path, match.start(), "unterminated call"))
                continue
            arguments = source[match.end() : end]
            forwarding = (
                path.name == "SplitIrqHandler.cc"
                and re.search(r",\s*policy\s*$", arguments)
            )
            if "IrqPolicy::" not in arguments and not forwarding:
                line = source.count("\n", 0, match.start()) + 1
                failures.append(
                    (path, line, f"{match.group(1)} has no named IrqPolicy")
                )

    for path, line, detail in failures:
        print(f"{path.relative_to(root)}:{line}: {detail}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
