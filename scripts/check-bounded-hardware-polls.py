#!/usr/bin/env python3

"""Reject hardware-status while loops with no visible deadline.

This is intentionally a narrow source check rather than a C++ parser. It finds
direct register reads in while conditions, plus the common form which refreshes
a condition variable from a register in the loop body. A loop is accepted only
when a counter/deadline is syntactically part of the loop or guards a break.
"""

from pathlib import Path
import re
import sys


HARDWARE_READ = re.compile(
    r"(?:->|\.)read(?:8|16|32|64)\s*\(|"
    r"(?:->|\.)(?:readRegister|readStatus|getPortStatus|hasInterrupt|hasCompleted)\s*\(|"
    r"PciBus\s*::\s*instance\s*\(\s*\)\s*\.\s*readConfigSpace\s*\("
)
WHILE = re.compile(r"\bwhile\s*\(")
IDENTIFIER = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


def mask_comments_and_literals(source: str) -> str:
    """Preserve offsets/newlines while hiding comments and string contents."""

    result = list(source)
    i = 0
    state = "code"
    quote = ""
    while i < len(source):
        if state == "line_comment":
            if source[i] == "\n":
                state = "code"
            else:
                result[i] = " "
            i += 1
            continue
        if state == "block_comment":
            if source.startswith("*/", i):
                result[i] = result[i + 1] = " "
                i += 2
                state = "code"
            else:
                if source[i] != "\n":
                    result[i] = " "
                i += 1
            continue
        if state == "literal":
            if source[i] == "\\":
                result[i] = " "
                if i + 1 < len(source) and source[i + 1] != "\n":
                    result[i + 1] = " "
                i += 2
                continue
            if source[i] == quote:
                result[i] = " "
                state = "code"
            elif source[i] != "\n":
                result[i] = " "
            i += 1
            continue

        if source.startswith("//", i):
            result[i] = result[i + 1] = " "
            i += 2
            state = "line_comment"
        elif source.startswith("/*", i):
            result[i] = result[i + 1] = " "
            i += 2
            state = "block_comment"
        elif source[i] in {'"', "'"}:
            quote = source[i]
            result[i] = " "
            i += 1
            state = "literal"
        else:
            i += 1
    return "".join(result)


def matching(text: str, opening: int, open_char: str, close_char: str) -> int:
    depth = 0
    for i in range(opening, len(text)):
        if text[i] == open_char:
            depth += 1
        elif text[i] == close_char:
            depth -= 1
            if depth == 0:
                return i
    return -1


def loop_body(masked: str, after_condition: int) -> tuple[str, int]:
    start = after_condition
    while start < len(masked) and masked[start].isspace():
        start += 1
    if start >= len(masked):
        return "", start
    if masked[start] == "{":
        end = matching(masked, start, "{", "}")
        if end < 0:
            return masked[start + 1 :], len(masked)
        return masked[start + 1 : end], end + 1
    end = masked.find(";", start)
    if end < 0:
        return masked[start:], len(masked)
    return masked[start : end + 1], end + 1


def mutates(name: str, text: str) -> bool:
    escaped = re.escape(name)
    return bool(
        re.search(rf"(?:\+\+|--)\s*{escaped}\b", text)
        or re.search(rf"\b{escaped}\s*(?:\+\+|--|[-+]=)", text)
    )


def has_deadline(condition: str, body: str) -> bool:
    if re.search(r"(?:\+\+|--)\s*[A-Za-z_]|[A-Za-z_]\w*\s*(?:\+\+|--)", condition):
        return True
    if "Time::getTicks" in condition:
        return True

    for name in set(IDENTIFIER.findall(condition)):
        if mutates(name, body):
            return True

    cursor = 0
    while True:
        branch = re.search(r"\bif\s*\(", body[cursor:])
        if not branch:
            return False
        opening = body.find("(", cursor + branch.start())
        closing = matching(body, opening, "(", ")")
        if closing < 0:
            return False

        guard = body[opening + 1 : closing]
        statement, end = loop_body(body, closing + 1)
        exits = re.search(r"\b(?:break|return)\b|\bpanic\s*\(", statement)
        if exits and "Time::getTicks" in guard:
            return True

        for name in set(IDENTIFIER.findall(guard)):
            if not re.search(r"(?:poll|attempt|retry)", name, re.IGNORECASE):
                continue
            escaped = re.escape(name)
            changed_in_guard = re.search(
                rf"(?:\+\+|--)\s*{escaped}\b|"
                rf"\b{escaped}\s*(?:\+\+|--|[-+]=)",
                guard,
            )
            compared_in_guard = re.search(
                rf"\b{escaped}\b[^;&|]{{0,80}}(?:==|!=|<=|>=|<|>)|"
                rf"(?:==|!=|<=|>=|<|>)[^;&|]{{0,80}}\b{escaped}\b",
                guard,
            )
            changed_before_guard = mutates(name, body[: opening])
            if exits and (
                (changed_in_guard and (compared_in_guard or "!" in guard))
                or (compared_in_guard and changed_before_guard)
            ):
                return True
        cursor = max(end, closing + 1)


def indirect_hardware_condition(condition: str, body: str) -> bool:
    names = set(IDENTIFIER.findall(condition))
    for name in names:
        assignment = re.compile(
            rf"\b{re.escape(name)}\s*=\s*[^;]{{0,400}}(?:"
            + HARDWARE_READ.pattern
            + r")",
            re.DOTALL,
        )
        if assignment.search(body):
            return True
    return False


def failures_for_source(source: str) -> list[tuple[int, str]]:
    masked = mask_comments_and_literals(source)
    failures: list[tuple[int, str]] = []
    cursor = 0
    while True:
        match = WHILE.search(masked, cursor)
        if not match:
            break
        opening = masked.find("(", match.start())
        closing = matching(masked, opening, "(", ")")
        if closing < 0:
            break
        condition = masked[opening + 1 : closing]
        body, end = loop_body(masked, closing + 1)
        direct = HARDWARE_READ.search(condition) is not None
        body_poll = condition.strip() in {"true", "1"} and (
            HARDWARE_READ.search(body) is not None
        )
        indirect = indirect_hardware_condition(condition, body)
        if (direct or body_poll or indirect) and not has_deadline(condition, body):
            line = source.count("\n", 0, match.start()) + 1
            kind = "direct" if direct else "body" if body_poll else "refreshed"
            failures.append((line, f"unbounded {kind} hardware-status poll"))
        # Continue just after the condition so nested hardware polls are also
        # audited instead of being hidden by an unrelated outer loop.
        cursor = closing + 1
    return failures


def self_test() -> int:
    bad = [
        "while (io->read32(Status) & Busy) { Time::delay(1); }",
        "while (status & Busy) { status = io->read8(Status); Processor::pause(); }",
        "while (true) { if (io->read32(Status)) break; }",
        "while (true) { if (io->read32(Status)) break; ++polls; }",
    ]
    good = [
        "while (polls-- && (io->read32(Status) & Busy)) { Time::delay(1); }",
        "while (status & Busy) { if (++polls == limit) break; status = io->read8(Status); }",
        "while (io->read32(Status) & Busy) { if (!polls--) panic(\"timeout\"); }",
        "while (io->read32(Status) & Busy) { if (++attempts == limit) return false; }",
        "while (true) { if (io->read32(Status) || ++polls == limit) break; }",
        "// while (io->read32(Status) & Busy);",
    ]
    if any(len(failures_for_source(sample)) != 1 for sample in bad):
        print("bounded hardware-poll detector missed its bad sample", file=sys.stderr)
        return 1
    if any(failures_for_source(sample) for sample in good):
        print("bounded hardware-poll detector rejected its good sample", file=sys.stderr)
        return 1
    return 0


def source_paths(arguments: list[str]) -> list[Path]:
    root = Path(__file__).resolve().parents[1]
    if arguments:
        paths = [Path(argument) for argument in arguments]
    else:
        paths = [root / "src"]

    sources: list[Path] = []
    for path in paths:
        if path.is_dir():
            sources.extend(path.rglob("*.cc"))
            sources.extend(path.rglob("*.h"))
        elif path.suffix in {".cc", ".h"}:
            sources.append(path)
    return sorted(set(sources))


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return self_test()

    root = Path(__file__).resolve().parents[1]
    failed = False
    for path in source_paths(sys.argv[1:]):
        source = path.read_text(encoding="utf-8", errors="replace")
        for line, detail in failures_for_source(source):
            try:
                display = path.relative_to(root)
            except ValueError:
                display = path
            print(f"{display}:{line}: {detail}", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
