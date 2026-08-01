#!/usr/bin/env python3

"""Reject aggregate Result returns on explicitly instantiated templates."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


EXTERN_TEMPLATE = re.compile(r"\bextern\s+template\s+class\b")
EXTERN_VOID_POINTER = re.compile(
    r"\bextern\s+template\s+class\b[^;]*<[^;]*\bvoid\s*\*[^;]*>"
)
RESULT_ALIAS = re.compile(
    r"""
    (?:typedef\s+Result\s*<[^;{}]+>\s+(?P<typedef>[A-Za-z_]\w*)\s*;)
    |
    (?:using\s+(?P<using>[A-Za-z_]\w*)\s*=\s*Result\s*<[^;{}]+>\s*;)
    """,
    re.VERBOSE | re.DOTALL,
)
DIRECT_RESULT_METHOD = re.compile(
    r"""
    ^[ \t]*
    (?:MUST_USE_RESULT[ \t\r\n]+)?
    Result\s*<[^;{}]+?>
    [ \t\r\n]+[A-Za-z_]\w*[ \t]*\(
    """,
    re.VERBOSE | re.MULTILINE,
)
VOID_POINTER_EXCLUSION = re.compile(
    r"""
    !\s*
    (?:pedigree_std::)?
    is_same\s*<
    \s*[^,>]+\s*,\s*void\s*\*
    \s*>\s*::\s*value
    """,
    re.VERBOSE,
)


def without_comments(source: str) -> str:
    source = re.sub(r"//[^\n]*", "", source)

    def preserve_lines(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    return re.sub(r"/\*.*?\*/", preserve_lines, source, flags=re.DOTALL)


def line_number(source: str, offset: int) -> int:
    return source.count("\n", 0, offset) + 1


def check_source(source: str) -> list[tuple[int, str]]:
    clean = without_comments(source)
    if not EXTERN_TEMPLATE.search(clean):
        return []

    aliases = {
        match.group("typedef") or match.group("using")
        for match in RESULT_ALIAS.finditer(clean)
    }
    violations: list[tuple[int, str]] = []

    for match in DIRECT_RESULT_METHOD.finditer(clean):
        violations.append(
            (
                line_number(clean, match.start()),
                "direct Result-returning method",
            )
        )

    for alias in aliases:
        direct_alias_method = re.compile(
            rf"""
            ^[ \t]*
            (?:MUST_USE_RESULT[ \t\r\n]+)?
            {re.escape(alias)}
            [ \t\r\n]+[A-Za-z_]\w*[ \t]*\(
            """,
            re.VERBOSE | re.MULTILINE,
        )
        for match in direct_alias_method.finditer(clean):
            violations.append(
                (
                    line_number(clean, match.start()),
                    f"Result alias {alias} returned by a method",
                )
            )

        if EXTERN_VOID_POINTER.search(clean):
            member_adapter = re.compile(
                rf"""
                template\s*<[^\n;{{}}]*>\s*
                (?P<signature>
                    [^;{{}}]*?\b{re.escape(alias)}\b
                    [^;{{}}]*?\b[A-Za-z_]\w*\s*\([^;{{}}]*\)
                )
                """,
                re.VERBOSE | re.DOTALL,
            )
            for match in member_adapter.finditer(clean):
                if not VOID_POINTER_EXCLUSION.search(match.group(0)):
                    violations.append(
                        (
                            line_number(clean, match.start()),
                            f"Result adapter {alias} does not exclude void *",
                        )
                    )

    return sorted(set(violations))


def self_test() -> bool:
    fixtures = {
        "unsafe direct": (
            """
            template <class T> class Direct {
              public:
                Result<T, bool> lookup(int key) const;
            };
            extern template class Direct<void *>;
            """,
            True,
        ),
        "unsafe typedef alias": (
            """
            template <class T> class Alias {
              public:
                typedef Result<T, bool> ReadResult;
                MUST_USE_RESULT ReadResult read(int key) const;
            };
            extern template class Alias<char>;
            """,
            True,
        ),
        "unsafe void adapter": (
            """
            template <class T> class Adapter {
              public:
                typedef Result<T, bool> LookupType;
                bool lookup(int key, T &out) const;
                template <typename U = T>
                typename enable_if<is_same<U, T>::value, LookupType>::type
                lookup(int key) const;
            };
            extern template class Adapter<void *>;
            """,
            True,
        ),
        "safe local adapter": (
            """
            template <class T> class Adapter {
              public:
                typedef Result<T, bool> LookupType;
                bool lookup(int key, T &out) const;
                template <typename U = T>
                typename enable_if<
                    is_same<U, T>::value &&
                        !is_same<U, void *>::value,
                    LookupType>::type
                lookup(int key) const;
            };
            extern template class Adapter<void *>;
            """,
            False,
        ),
    }

    passed = True
    for name, (source, should_fail) in fixtures.items():
        failed = bool(check_source(source))
        if failed != should_fail:
            print(
                f"Result ABI detector self-test failed: {name}",
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
