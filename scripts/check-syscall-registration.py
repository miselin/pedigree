#!/usr/bin/env python3

"""Reject syscall registrations that do not transfer token ownership."""

from __future__ import annotations

import re
import sys
from pathlib import Path


IDENTIFIER = re.compile(r"\bregisterSyscallHandler\b")
UNREGISTER = re.compile(r"\bunregisterSyscallHandler\b")
TOKEN = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
    re.DOTALL,
)


def without_comments_and_literals(source: str) -> str:
    def replace(match: re.Match[str]) -> str:
        return "".join("\n" if char == "\n" else " " for char in match.group(0))

    return TOKEN.sub(replace, source)


def line_number(source: str, offset: int) -> int:
    return source.count("\n", 0, offset) + 1


def registration_arguments(
    source: str, identifier_end: int
) -> tuple[list[str] | None, int]:
    cursor = identifier_end
    while cursor < len(source) and source[cursor].isspace():
        cursor += 1
    if cursor == len(source) or source[cursor] != "(":
        return None, cursor

    arguments: list[str] = []
    argument_start = cursor + 1
    parentheses = 1
    brackets = 0
    braces = 0
    cursor += 1
    while cursor < len(source):
        char = source[cursor]
        if char == "(":
            parentheses += 1
        elif char == ")":
            parentheses -= 1
            if parentheses == 0:
                arguments.append(source[argument_start:cursor].strip())
                return arguments, cursor + 1
        elif char == "[":
            brackets += 1
        elif char == "]":
            brackets -= 1
        elif char == "{":
            braces += 1
        elif char == "}":
            braces -= 1
        elif (
            char == ","
            and parentheses == 1
            and brackets == 0
            and braces == 0
        ):
            arguments.append(source[argument_start:cursor].strip())
            argument_start = cursor + 1
        cursor += 1

    return [], cursor


def check_source(source: str) -> list[tuple[int, str]]:
    clean = without_comments_and_literals(source)
    violations: list[tuple[int, str]] = []

    for match in UNREGISTER.finditer(clean):
        violations.append(
            (
                line_number(clean, match.start()),
                "raw syscall unregistration API",
            )
        )

    for match in IDENTIFIER.finditer(clean):
        arguments, _ = registration_arguments(clean, match.end())
        if arguments is None:
            continue
        line = line_number(clean, match.start())
        if not arguments:
            violations.append((line, "unterminated registration argument list"))
        elif len(arguments) != 3:
            violations.append(
                (
                    line,
                    f"registration has {len(arguments)} arguments, expected 3",
                )
            )
        elif re.fullmatch(r"(?:0|NULL|nullptr|\{\})", arguments[2]):
            violations.append((line, "registration has no ownership token"))

    return sorted(set(violations))


def self_test() -> bool:
    fixtures = {
        "safe call": (
            "manager.registerSyscallHandler(service, handler, registration);",
            False,
        ),
        "safe nested call": (
            "manager.registerSyscallHandler(serviceFor(1, 2), handler, token);",
            False,
        ),
        "unsafe legacy call": (
            "manager.registerSyscallHandler(service, handler);",
            True,
        ),
        "unsafe null token": (
            "manager.registerSyscallHandler(service, handler, nullptr);",
            True,
        ),
        "unsafe raw unregister": (
            "manager.unregisterSyscallHandler(service, handler);",
            True,
        ),
        "ignored comment and literal": (
            '// registerSyscallHandler(service, handler)\n'
            '"unregisterSyscallHandler";',
            False,
        ),
    }

    passed = True
    for name, (source, should_fail) in fixtures.items():
        failed = bool(check_source(source))
        if failed != should_fail:
            print(
                f"Syscall registration detector self-test failed: {name}",
                file=sys.stderr,
            )
            passed = False
    return passed


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return 0 if self_test() else 1
    if len(sys.argv) != 2:
        print(
            f"usage: {Path(sys.argv[0]).name} SOURCE_ROOT\n"
            f"       {Path(sys.argv[0]).name} --self-test",
            file=sys.stderr,
        )
        return 2

    source_root = Path(sys.argv[1])
    api_header = (
        source_root
        / "system/include/pedigree/kernel/processor/SyscallManager.h"
    )
    api_source = without_comments_and_literals(
        api_header.read_text(encoding="latin-1")
    )
    api_registration = next(
        (
            arguments
            for match in IDENTIFIER.finditer(api_source)
            for arguments, _ in [
                registration_arguments(api_source, match.end())
            ]
            if arguments is not None
        ),
        None,
    )
    failed = False
    if (
        api_registration is None
        or len(api_registration) != 3
        or not re.search(r"\bRegistration\s*&", api_registration[2])
    ):
        print(
            f"{api_header}: the public API must require a Registration & token",
            file=sys.stderr,
        )
        failed = True

    for path in sorted(source_root.rglob("*")):
        if path.suffix not in {".cc", ".h"}:
            continue
        source = path.read_text(encoding="latin-1")
        for line, reason in check_source(source):
            print(f"{path}:{line}: {reason}", file=sys.stderr)
            failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
