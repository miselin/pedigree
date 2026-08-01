#!/usr/bin/env python3

import re
import sys
from collections import Counter
from pathlib import Path


TOKEN = re.compile(
    r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/|\s+|.',
    re.DOTALL,
)
MARKER = re.compile(
    r"HOSTED-WAIT-TEST: PASS ([A-Za-z0-9][A-Za-z0-9-]*)"
)


def marker_runs(source: str):
    fragments = []
    for match in TOKEN.finditer(source):
        token = match.group(0)
        if token.startswith('"'):
            fragments.append(token[1:-1])
        elif token.isspace() or token.startswith("//") or token.startswith("/*"):
            continue
        else:
            if fragments:
                yield "".join(fragments)
                fragments.clear()
    if fragments:
        yield "".join(fragments)


def collect_markers(source_root: Path):
    occurrences = {}
    for path in sorted(source_root.rglob("*")):
        if path.suffix not in {".cc", ".h"}:
            continue
        source = path.read_text(encoding="latin-1")
        for run in marker_runs(source):
            for match in MARKER.finditer(run):
                marker = f"HOSTED-WAIT-TEST: PASS {match.group(1)}"
                occurrences.setdefault(marker, []).append(path)
    return occurrences


def validate_source_markers(occurrences) -> bool:
    if not occurrences:
        print("No hosted wait markers found.", file=sys.stderr)
        return False

    duplicates = {
        marker: paths
        for marker, paths in occurrences.items()
        if len(paths) != 1
    }
    if duplicates:
        for marker, paths in sorted(duplicates.items()):
            locations = ", ".join(str(path) for path in paths)
            print(f"Duplicate hosted wait marker {marker}: {locations}", file=sys.stderr)
        return False
    return True


def self_test() -> bool:
    sample = r'''
        NOTICE("HOSTED-WAIT-TEST: PASS direct-marker");
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            /* adjacent C strings form one marker */
            "adjacent-marker");
        NOTICE("HOSTED-WAIT-TEST: PASS " << dynamic_name);
    '''
    actual = {
        f"HOSTED-WAIT-TEST: PASS {match.group(1)}"
        for run in marker_runs(sample)
        for match in MARKER.finditer(run)
    }
    expected = {
        "HOSTED-WAIT-TEST: PASS direct-marker",
        "HOSTED-WAIT-TEST: PASS adjacent-marker",
    }
    if actual != expected:
        print(
            f"Marker lexer self-test failed: expected {sorted(expected)}, "
            f"found {sorted(actual)}",
            file=sys.stderr,
        )
        return False
    return True


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return 0 if self_test() else 1

    check_log = len(sys.argv) == 4 and sys.argv[1] == "--check-log"
    if not (len(sys.argv) == 2 or check_log):
        print(
            f"usage: {Path(sys.argv[0]).name} SOURCE_ROOT\n"
            f"       {Path(sys.argv[0]).name} --check-log SOURCE_ROOT LOG\n"
            f"       {Path(sys.argv[0]).name} --self-test",
            file=sys.stderr,
        )
        return 2

    source_root = Path(sys.argv[2] if check_log else sys.argv[1])
    occurrences = collect_markers(source_root)
    if not validate_source_markers(occurrences):
        return 1

    if not check_log:
        for marker in sorted(occurrences):
            print(marker)
        return 0

    log = Path(sys.argv[3]).read_text(encoding="latin-1")
    actual = Counter(
        f"HOSTED-WAIT-TEST: PASS {name}"
        for name in MARKER.findall(log)
    )
    expected = set(occurrences)
    failed = False
    for marker in sorted(expected - set(actual)):
        print(f"Missing hosted wait marker: {marker}", file=sys.stderr)
        failed = True
    for marker in sorted(set(actual) - expected):
        print(f"Unexpected hosted wait marker: {marker}", file=sys.stderr)
        failed = True
    for marker in sorted(expected & set(actual)):
        if actual[marker] != 1:
            print(
                f"Expected one hosted wait marker, found {actual[marker]}: "
                f"{marker}",
                file=sys.stderr,
            )
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
