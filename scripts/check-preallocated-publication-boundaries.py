#!/usr/bin/env python3

"""Keep RequestQueue preallocated publication context-neutral and out of IRQs."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable, Mapping


SOURCE_SUFFIXES = frozenset((".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"))
IMPLEMENTATION_PATHS = frozenset(
    (
        "src/system/include/pedigree/kernel/utilities/RequestQueue.h",
        "src/system/kernel/utilities/RequestQueue.cc",
    )
)
PUBLICATION_NAMES = (
    "publishPreallocated",
    "republishPreallocatedWhileReleasing",
)
PUBLICATION = re.compile(
    r"\b(" + "|".join(PUBLICATION_NAMES) + r")\s*\("
)
LEGACY_IDENTIFIERS = (
    "InterruptRequest",
    "InterruptEnqueueResult",
    "enqueueFromInterrupt",
    "republishWhileReleasing",
    "publishInterruptRequest",
    "releaseInterruptRequest",
    "setAfterInterruptAdmissionHookForTest",
    "closeInterruptAdmission",
    "waitForInterruptPublishers",
    "m_AfterInterruptAdmissionHook",
    "m_AfterInterruptAdmissionContext",
    "m_pInterruptOwner",
    "canPushFromInterrupt",
    "pushFromInterrupt",
)
LEGACY = re.compile(r"\b(" + "|".join(LEGACY_IDENTIFIERS) + r")\b")
HARD_HANDLER_SURFACE = re.compile(
    r"\b(?:HardIrqHandler|SplitIrqHandler)\b|\bhardIrq\s*\("
)


@dataclass(frozen=True, order=True)
class CallSite:
    path: str
    name: str


@dataclass(frozen=True, order=True)
class Diagnostic:
    path: str
    line: int
    message: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


ALLOWED_PUBLICATIONS = Counter(
    (
        CallSite(
            "src/modules/system/network-stack/NetworkStack.cc",
            "publishPreallocated",
        ),
        CallSite(
            "src/modules/drivers/common/usb-hcd/PortChangeRequest.h",
            "publishPreallocated",
        ),
        CallSite(
            "src/modules/drivers/common/usb-hcd/PortChangeRequest.h",
            "republishPreallocatedWhileReleasing",
        ),
    )
)


def mask_cpp(source: str) -> str:
    """Mask comments and literals while preserving offsets and line numbers."""

    output = list(source)
    length = len(source)
    position = 0

    def erase(start: int, end: int) -> None:
        for index in range(start, end):
            if output[index] not in ("\n", "\r"):
                output[index] = " "

    while position < length:
        if source.startswith("//", position):
            end = source.find("\n", position + 2)
            end = length if end < 0 else end
            erase(position, end)
            position = end
            continue

        if source.startswith("/*", position):
            close = source.find("*/", position + 2)
            end = length if close < 0 else close + 2
            erase(position, end)
            position = end
            continue

        if source.startswith('R"', position):
            delimiter_end = source.find("(", position + 2, min(length, position + 20))
            if delimiter_end >= 0:
                terminator = ")" + source[position + 2 : delimiter_end] + '"'
                close = source.find(terminator, delimiter_end + 1)
                end = length if close < 0 else close + len(terminator)
                erase(position, end)
                position = end
                continue

        if source[position] in ('"', "'"):
            quote = source[position]
            end = position + 1
            while end < length:
                if source[end] == "\\":
                    end += 2
                    continue
                if source[end] == quote:
                    end += 1
                    break
                end += 1
            erase(position, min(end, length))
            position = end
            continue

        position += 1

    return "".join(output)


def audit_sources(
    sources: Mapping[str, str],
    expected: Counter[CallSite] = ALLOWED_PUBLICATIONS,
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    observed: Counter[CallSite] = Counter()
    locations: dict[CallSite, list[int]] = {}

    for path in sorted(sources):
        source = sources[path]
        masked = mask_cpp(source)

        for match in LEGACY.finditer(masked):
            diagnostics.append(
                Diagnostic(
                    path,
                    source.count("\n", 0, match.start()) + 1,
                    f"legacy IRQ-named publication identifier remains: "
                    f"{match.group(1)}",
                )
            )

        if path in IMPLEMENTATION_PATHS or path.startswith(
            "src/modules/system/hosted-smoke/"
        ):
            continue

        calls = list(PUBLICATION.finditer(masked))
        for match in calls:
            call = CallSite(path, match.group(1))
            observed[call] += 1
            locations.setdefault(call, []).append(
                source.count("\n", 0, match.start()) + 1
            )

        if calls and HARD_HANDLER_SURFACE.search(masked):
            for match in calls:
                diagnostics.append(
                    Diagnostic(
                        path,
                        source.count("\n", 0, match.start()) + 1,
                        "preallocated RequestQueue publication shares a raw "
                        "hard-handler source",
                    )
                )

    for call, count in sorted((observed - expected).items()):
        allowed_count = expected[call]
        for line in locations.get(call, [0])[allowed_count : allowed_count + count]:
            diagnostics.append(
                Diagnostic(
                    call.path,
                    line,
                    "preallocated RequestQueue publication escaped its "
                    f"audited non-hard call sites: {call.name}",
                )
            )

    for call, count in sorted((expected - observed).items()):
        diagnostics.append(
            Diagnostic(
                call.path,
                0,
                "missing audited preallocated RequestQueue publication: "
                f"{call.name}" + (f" x{count}" if count != 1 else ""),
            )
        )

    return sorted(set(diagnostics))


def load_sources(root: Path) -> dict[str, str]:
    sources: dict[str, str] = {}
    for path in sorted((root / "src").rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            sources[path.relative_to(root).as_posix()] = path.read_text(
                encoding="utf-8", errors="replace"
            )
    return sources


def require_failure(
    label: str,
    sources: Mapping[str, str],
    expected: Counter[CallSite],
    message: str,
) -> None:
    diagnostics = audit_sources(sources, expected)
    if not any(message in diagnostic.message for diagnostic in diagnostics):
        rendered = "\n".join(diagnostic.render() for diagnostic in diagnostics)
        raise AssertionError(
            f"{label} did not report {message!r}; diagnostics were:\n{rendered}"
        )


def self_test() -> int:
    allowed_path = "src/modules/ordinary.cc"
    allowed_call = CallSite(allowed_path, "publishPreallocated")
    expected = Counter((allowed_call,))
    clean = {
        allowed_path: """
            void publish(RequestQueue &queue, PreallocatedRequest &request) {
                queue.publishPreallocated(request, 0);
                // queue.enqueueFromInterrupt(request, 0);
                const char *ignored = "InterruptRequest";
            }
        """,
        "src/system/include/pedigree/kernel/utilities/RequestQueue.h": """
            class RequestQueue {
                void publishPreallocated(PreallocatedRequest &, size_t);
            };
        """,
    }
    diagnostics = audit_sources(clean, expected)
    if diagnostics:
        raise AssertionError(
            "allowed publication unexpectedly failed:\n"
            + "\n".join(diagnostic.render() for diagnostic in diagnostics)
        )

    legacy = dict(clean)
    legacy["src/modules/system/hosted-smoke/legacy.cc"] = (
        "RequestQueue::InterruptRequest request;\n"
    )
    require_failure(
        "hosted legacy identifier",
        legacy,
        expected,
        "legacy IRQ-named publication identifier remains",
    )

    unexpected = dict(clean)
    unexpected["src/modules/unexpected.cc"] = (
        "void f() { queue.publishPreallocated(request, 0); }\n"
    )
    require_failure(
        "unallowlisted publication",
        unexpected,
        expected,
        "publication escaped its audited non-hard call sites",
    )

    hard_path = "src/modules/hard.cc"
    hard_call = CallSite(hard_path, "publishPreallocated")
    hard = dict(clean)
    hard[hard_path] = """
        class Device : private HardIrqHandler {
            void hardIrq() { queue.publishPreallocated(request, 0); }
        };
    """
    require_failure(
        "hard-handler publication",
        hard,
        expected + Counter((hard_call,)),
        "publication shares a raw hard-handler source",
    )

    missing = dict(clean)
    missing[allowed_path] = "void noPublication() {}\n"
    require_failure(
        "missing audited publication",
        missing,
        expected,
        "missing audited preallocated RequestQueue publication",
    )

    print("preallocated publication boundary checker self-test passed (5 cases)")
    return 0


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help=argparse.SUPPRESS,
    )
    return parser.parse_args(argv)


def main(argv: Iterable[str] = ()) -> int:
    args = parse_args(argv)
    if args.self_test:
        return self_test()

    root = args.root.resolve()
    if not (root / "src").is_dir():
        print(f"source tree not found below {root}", file=sys.stderr)
        return 2

    diagnostics = audit_sources(load_sources(root))
    for diagnostic in diagnostics:
        print(diagnostic.render(), file=sys.stderr)
    if diagnostics:
        return 1

    print("preallocated publication boundary check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
