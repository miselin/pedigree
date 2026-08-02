#!/usr/bin/env python3

"""Reject nested aggregate returns on exported kernel interfaces."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


EXPORTED_CLASS = re.compile(
    r"\b(?P<kind>class|struct)\s+EXPORTED_PUBLIC\s+"
    r"(?P<name>[A-Za-z_]\w*)[^;{]*\{"
)
AGGREGATE = re.compile(r"\bstruct\s+([A-Za-z_]\w*)[^;{]*\{")
ACCESS = re.compile(r"\b(?P<access>public|protected|private)\s*:\s*")
METHOD = re.compile(r"^(?P<result>.*?)\b(?P<name>[A-Za-z_]\w*)\s*\(", re.DOTALL)


RAW_LITERAL = re.compile(
    r'(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\('
)


def without_comments_and_literals(source: str) -> str:
    masked = list(source)

    def blank(start: int, end: int):
        for index in range(start, end):
            if masked[index] != "\n":
                masked[index] = " "

    offset = 0
    while offset < len(source):
        if source.startswith("//", offset):
            end = source.find("\n", offset + 2)
            end = len(source) if end < 0 else end
            blank(offset, end)
            offset = end
            continue
        if source.startswith("/*", offset):
            end = source.find("*/", offset + 2)
            end = len(source) if end < 0 else end + 2
            blank(offset, end)
            offset = end
            continue

        raw = RAW_LITERAL.match(source, offset)
        if raw:
            terminator = ")" + raw.group("delimiter") + '"'
            end = source.find(terminator, raw.end())
            end = len(source) if end < 0 else end + len(terminator)
            blank(offset, end)
            offset = end
            continue

        if source[offset] in ('"', "'"):
            quote = source[offset]
            end = offset + 1
            while end < len(source):
                if source[end] == "\\":
                    end += 2
                    continue
                if source[end] == quote:
                    end += 1
                    break
                end += 1
            blank(offset, min(end, len(source)))
            offset = end
            continue

        offset += 1

    return "".join(masked)


def matching_brace(source: str, opening: int) -> int | None:
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return offset
    return None


def direct_declarations(body: str):
    start = 0
    depth = 0
    offset = 0
    while offset < len(body):
        character = body[offset]
        if character == "{" and depth == 0:
            signature = body[start:offset]
            if signature.strip():
                yield start, signature, True
            closing = matching_brace(body, offset)
            if closing is None:
                return
            offset = closing
            start = offset + 1
        elif character == ";" and depth == 0:
            yield start, body[start:offset], False
            start = offset + 1
        offset += 1


def line_number(source: str, offset: int) -> int:
    return source.count("\n", 0, offset) + 1


def check_source(source: str) -> list[tuple[int, str]]:
    clean = without_comments_and_literals(source)
    violations: list[tuple[int, str]] = []

    for exported in EXPORTED_CLASS.finditer(clean):
        opening = clean.find("{", exported.start(), exported.end())
        closing = matching_brace(clean, opening)
        if closing is None:
            continue

        body = clean[opening + 1 : closing]
        aggregates = {match.group(1) for match in AGGREGATE.finditer(body)}
        if not aggregates:
            continue

        access_level = "private" if exported.group("kind") == "class" else "public"
        for relative_offset, declaration, has_body in direct_declarations(body):
            access = list(ACCESS.finditer(declaration))
            if access:
                access_level = access[-1].group("access")
                declaration = declaration[access[-1].end() :]
            if access_level != "public":
                continue
            if has_body:
                continue
            declaration = " ".join(declaration.split())
            if not declaration or declaration.startswith(
                ("class ", "struct ", "enum ", "typedef ", "using ")
            ):
                continue

            method = METHOD.match(declaration)
            if not method:
                continue
            result = method.group("result").strip()
            for aggregate in aggregates:
                if re.search(rf"(?:\b[A-Za-z_]\w*::)*\b{aggregate}\s*$", result):
                    absolute = opening + 1 + relative_offset
                    violations.append(
                        (
                            line_number(clean, absolute),
                            f"exported method returns nested aggregate {aggregate}",
                        )
                    )

    return sorted(set(violations))


def self_test() -> bool:
    fixtures = {
        "unsafe result": (
            """
            class EXPORTED_PUBLIC Registry {
              public:
                struct Result { bool handled; bool rearm; };
                Result dispatch(int irq);
            };
            """,
            True,
        ),
        "unsafe multiline": (
            """
            class EXPORTED_PUBLIC Registry {
              public:
                struct Snapshot { int value; };
                MUST_USE_RESULT
                Registry::Snapshot
                snapshot() const;
            };
            """,
            True,
        ),
        "safe scalar out": (
            """
            class EXPORTED_PUBLIC Registry {
              public:
                struct Result { bool handled; };
                bool dispatch(int irq, Result &result);
            };
            """,
            False,
        ),
        "safe pointer": (
            """
            class EXPORTED_PUBLIC Registry {
              public:
                struct Entry { int value; };
                Entry *lookup(int key);
            };
            """,
            False,
        ),
        "safe reference": (
            """
            class EXPORTED_PUBLIC Registry {
              public:
                struct Entry { int value; };
                const Entry &lookup(int key);
            };
            """,
            False,
        ),
        "safe inline definition": (
            """
            class EXPORTED_PUBLIC Registry {
              public:
                struct Result { bool handled; };
                Result localResult() { return {true}; }
                bool dispatch(int irq, Result &result);
            };
            """,
            False,
        ),
        "literal false positive": (
            r'''
            constexpr auto diagnostic = R"abi(
                class EXPORTED_PUBLIC Fake {
                    struct Result { bool value; };
                    Result broken();
                };
            )abi";
            class EXPORTED_PUBLIC Registry {
              public:
                struct Result { bool handled; };
                bool dispatch(int irq, Result &result);
            };
            ''',
            False,
        ),
        "local class": (
            """
            class Registry {
              public:
                struct Result { bool handled; };
                Result dispatch(int irq);
            };
            """,
            False,
        ),
    }

    passed = True
    for name, (source, should_fail) in fixtures.items():
        failed = bool(check_source(source))
        if failed != should_fail:
            print(
                f"exported aggregate ABI detector self-test failed: {name}",
                file=sys.stderr,
            )
            passed = False
    return passed


def headers(paths: list[Path]):
    for path in paths:
        if path.is_dir():
            yield from sorted(path.rglob("*.h"))
        elif path.suffix == ".h":
            yield path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("paths", nargs="*", type=Path)
    arguments = parser.parse_args()

    if arguments.self_test and not self_test():
        return 2

    found = False
    for path in headers(arguments.paths):
        source = path.read_text(encoding="utf-8", errors="replace")
        for line, detail in check_source(source):
            print(f"{path}:{line}: {detail}")
            found = True
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main())
