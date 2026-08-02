#!/usr/bin/env python3

"""Enforce the small, explicit production hard-IRQ boundary."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable, Mapping


REGISTRATION_NAMES = (
    "registerHardIsaIrqHandler",
    "registerHardPciIrqHandler",
    "registerIsaSplitIrq",
    "registerPciSplitIrq",
)
RAW_INTERRUPT_REGISTRATION = "registerInterruptHandler"
CALL = re.compile(
    r"\b(" + "|".join((*REGISTRATION_NAMES, RAW_INTERRUPT_REGISTRATION)) +
    r")\s*\("
)
TIME_TRACKER = re.compile(r"\bTimeTracker\b")
DISPATCHER_HEADER = (
    "src/system/include/pedigree/kernel/machine/ThreadedIrqDispatcher.h"
)
INLINE_DISPATCHER_NAME = re.compile(r"\bNormalStaticString\s+m_Name\s*;")
DYNAMIC_DISPATCHER_NAME = re.compile(r"\bString\s+m_Name\s*;")
RTC_SOURCE = "src/system/kernel/machine/mach_pc/Rtc.cc"
RTC_THREADED_REGISTRATION = re.compile(
    r"\bregisterIsaIrqHandler\s*\(\s*8\s*,\s*this\s*,\s*"
    r"IrqPolicy::levelThreaded\s*\(\s*\)\s*\)"
)
SOURCE_SUFFIXES = frozenset((".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"))


@dataclass(frozen=True, order=True)
class CallKey:
    path: str
    name: str
    arguments: str


@dataclass(frozen=True)
class Invocation:
    key: CallKey
    line: int


@dataclass(frozen=True, order=True)
class Diagnostic:
    path: str
    line: int
    message: str

    def render(self) -> str:
        if self.line:
            return f"{self.path}:{self.line}: {self.message}"
        return f"{self.path}: {self.message}"


def allowed(path: str, name: str, arguments: str) -> CallKey:
    return CallKey(path, name, arguments)


ALLOWED_REGISTRATIONS = Counter(
    (
        allowed(
            "src/system/kernel/machine/SplitIrqHandler.cc",
            "registerHardIsaIrqHandler",
            "irq,this,policy",
        ),
        allowed(
            "src/system/kernel/machine/SplitIrqHandler.cc",
            "registerHardPciIrqHandler",
            "this,&device,policy",
        ),
        allowed(
            "src/system/kernel/machine/hosted/SchedulerTimer.cc",
            "registerHardIsaIrqHandler",
            "1,this,IrqPolicy::syntheticHard()",
        ),
        allowed(
            "src/system/kernel/machine/hosted/Timer.cc",
            "registerIsaSplitIrq",
            "irqManager,0,IrqPolicy::syntheticHard()",
        ),
        allowed(
            "src/system/kernel/machine/mach_pc/Pit.cc",
            "registerHardIsaIrqHandler",
            "0,this,IrqPolicy::edgeHard()",
        ),
        allowed(
            "src/system/kernel/machine/mach_pc/Ps2Controller.cc",
            "registerIsaSplitIrq",
            "irqManager,1,IrqPolicy::edgeHard()",
        ),
        allowed(
            "src/system/kernel/machine/mach_pc/Ps2Controller.cc",
            "registerIsaSplitIrq",
            "irqManager,12,IrqPolicy::edgeHard()",
        ),
    )
)


def mask_cpp(source: str, mask_literals: bool = True) -> str:
    """Mask comments and optionally literals while preserving offsets."""

    output = list(source)
    length = len(source)
    i = 0

    def erase(start: int, end: int) -> None:
        for position in range(start, end):
            if output[position] not in ("\n", "\r"):
                output[position] = " "

    while i < length:
        if source.startswith("//", i):
            end = source.find("\n", i + 2)
            if end < 0:
                end = length
            erase(i, end)
            i = end
            continue

        if source.startswith("/*", i):
            close = source.find("*/", i + 2)
            end = length if close < 0 else close + 2
            erase(i, end)
            i = end
            continue

        if source.startswith('R"', i):
            delimiter_end = source.find("(", i + 2, min(length, i + 20))
            if delimiter_end >= 0:
                delimiter = source[i + 2 : delimiter_end]
                terminator = ")" + delimiter + '"'
                close = source.find(terminator, delimiter_end + 1)
                end = length if close < 0 else close + len(terminator)
                if mask_literals:
                    erase(i, end)
                i = end
                continue

        if source[i] in ('"', "'"):
            quote = source[i]
            end = i + 1
            while end < length:
                if source[end] == "\\":
                    end += 2
                    continue
                if source[end] == quote:
                    end += 1
                    break
                end += 1
            if mask_literals:
                erase(i, min(end, length))
            i = end
            continue

        i += 1

    return "".join(output)


def matching_paren(source: str, opening: int) -> int:
    depth = 1
    position = opening + 1
    while position < len(source):
        if source[position] == "(":
            depth += 1
        elif source[position] == ")":
            depth -= 1
            if not depth:
                return position
        position += 1
    return -1


def previous_code_character(source: str, position: int) -> tuple[int, str]:
    position -= 1
    while position >= 0 and source[position].isspace():
        position -= 1
    return position, source[position] if position >= 0 else ""


def looks_like_header_declaration(
    path: str, source: str, start: int, closing: int
) -> bool:
    if Path(path).suffix not in (".h", ".hh", ".hpp"):
        return False

    boundary = max(
        source.rfind(";", 0, start),
        source.rfind("{", 0, start),
        source.rfind("}", 0, start),
    )
    prefix = source[boundary + 1 : start]
    suffix = source[closing + 1 :].lstrip()
    declaration_hint = re.search(
        r"\b(?:virtual|static|inline|constexpr|consteval|friend|"
        r"irq_id_t|bool|void)\b",
        prefix,
    )
    expression_hint = re.search(r"(?:=|\breturn\b|\bif\b|\bwhile\b)", prefix)
    declaration_suffix = (
        not suffix
        or suffix.startswith((";", "=", "{", "const", "noexcept", "override", "final"))
    )
    return bool(declaration_hint and not expression_hint and declaration_suffix)


def find_invocations(path: str, source: str) -> list[Invocation]:
    masked = mask_cpp(source)
    invocations: list[Invocation] = []
    for match in CALL.finditer(masked):
        name = match.group(1)
        before, previous = previous_code_character(masked, match.start(1))

        opening = masked.rfind("(", match.start(1), match.end())
        closing = matching_paren(masked, opening)
        if closing < 0:
            continue

        # A qualified name followed by a body is a definition. A qualified name
        # followed by a statement terminator can still be an explicit base call.
        if previous == ":" and before > 0 and masked[before - 1] == ":":
            suffix = masked[closing + 1 :].lstrip()
            if suffix.startswith("{"):
                continue
        if looks_like_header_declaration(path, masked, match.start(1), closing):
            continue

        arguments = re.sub(r"\s+", "", masked[opening + 1 : closing])
        invocations.append(
            Invocation(
                CallKey(path, name, arguments),
                source.count("\n", 0, match.start(1)) + 1,
            )
        )
    return invocations


def is_hosted_smoke(path: str) -> bool:
    return path.startswith("src/modules/system/hosted-smoke/")


def is_processor_interrupt_manager(path: str) -> bool:
    return path.startswith("src/system/kernel/core/processor/") and path.endswith(
        "/InterruptManager.cc"
    )


def audit_sources(
    sources: Mapping[str, str],
    expected: Counter[CallKey] = ALLOWED_REGISTRATIONS,
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    observed: Counter[CallKey] = Counter()
    locations: dict[CallKey, list[int]] = {}

    for path in sorted(sources):
        if is_hosted_smoke(path):
            continue

        source = sources[path]
        invocations = find_invocations(path, source)
        for invocation in invocations:
            if invocation.key.name in REGISTRATION_NAMES:
                observed[invocation.key] += 1
                locations.setdefault(invocation.key, []).append(invocation.line)
            elif path.startswith("src/modules/"):
                diagnostics.append(
                    Diagnostic(
                        path,
                        invocation.line,
                        "production module uses raw registerInterruptHandler",
                    )
                )

        if is_processor_interrupt_manager(path):
            comment_free = mask_cpp(source, mask_literals=False)
            occurrences = list(TIME_TRACKER.finditer(comment_free))
            if occurrences:
                first = occurrences[0]
                diagnostics.append(
                    Diagnostic(
                        path,
                        source.count("\n", 0, first.start()) + 1,
                        "TimeTracker is forbidden in processor interrupt entry"
                        + (
                            f" ({len(occurrences)} occurrences)"
                            if len(occurrences) != 1
                            else ""
                        ),
                    )
                )

    dispatcher_header = sources.get(DISPATCHER_HEADER)
    if dispatcher_header is not None:
        dynamic_name = DYNAMIC_DISPATCHER_NAME.search(dispatcher_header)
        inline_name = INLINE_DISPATCHER_NAME.search(dispatcher_header)
        if dynamic_name or not inline_name:
            match = dynamic_name or re.search(r"\bm_Name\b", dispatcher_header)
            diagnostics.append(
                Diagnostic(
                    DISPATCHER_HEADER,
                    dispatcher_header.count("\n", 0, match.start()) + 1
                    if match
                    else 0,
                    "threaded IRQ dispatcher name must use inline storage",
                )
            )

    rtc_source = sources.get(RTC_SOURCE)
    if rtc_source is not None:
        masked_rtc = mask_cpp(rtc_source)
        if not RTC_THREADED_REGISTRATION.search(masked_rtc):
            diagnostics.append(
                Diagnostic(
                    RTC_SOURCE,
                    0,
                    "RTC IRQ8 must use ordinary level-threaded delivery",
                )
            )

    for key, count in sorted((observed - expected).items()):
        allowed_count = expected[key]
        extra_lines = locations.get(key, [0])[allowed_count:]
        if not extra_lines:
            extra_lines = locations.get(key, [0])[:1]
        for line in extra_lines:
            diagnostics.append(
                Diagnostic(
                    key.path,
                    line,
                    f"hard/split IRQ registration is not allowlisted: "
                    f"{key.name}({key.arguments})",
                )
            )

    for key, count in sorted((expected - observed).items()):
        diagnostics.append(
            Diagnostic(
                key.path,
                0,
                f"missing allowlisted hard/split IRQ registration: "
                f"{key.name}({key.arguments})"
                + (f" x{count}" if count != 1 else ""),
            )
        )

    return sorted(set(diagnostics))


def load_sources(root: Path) -> dict[str, str]:
    sources: dict[str, str] = {}
    source_root = root / "src"
    for path in sorted(source_root.rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            relative = path.relative_to(root).as_posix()
            sources[relative] = path.read_text(encoding="utf-8", errors="replace")
    return sources


def require_clean(
    label: str, sources: Mapping[str, str], expected: Counter[CallKey]
) -> None:
    diagnostics = audit_sources(sources, expected)
    if diagnostics:
        rendered = "\n".join(diagnostic.render() for diagnostic in diagnostics)
        raise AssertionError(f"{label} unexpectedly failed:\n{rendered}")


def require_failure(
    label: str,
    sources: Mapping[str, str],
    expected: Counter[CallKey],
    message: str,
) -> None:
    diagnostics = audit_sources(sources, expected)
    if not any(message in diagnostic.message for diagnostic in diagnostics):
        rendered = "\n".join(diagnostic.render() for diagnostic in diagnostics)
        raise AssertionError(
            f"{label} did not report {message!r}; diagnostics were:\n{rendered}"
        )


def self_test() -> int:
    allowed_hard = allowed(
        "src/allowed.cc",
        "registerHardIsaIrqHandler",
        "1,this,IrqPolicy::edgeHard()",
    )
    allowed_split = allowed(
        "src/allowed.cc",
        "registerIsaSplitIrq",
        "manager,8,IrqPolicy::levelHard()",
    )
    allowed_qualified = allowed(
        "src/qualified.cc",
        "registerHardPciIrqHandler",
        "this,&device,policy",
    )
    expected = Counter((allowed_hard, allowed_split, allowed_qualified))
    clean_sources = {
        "src/allowed.cc": """
            void Device::start()
            {
                manager.registerHardIsaIrqHandler(
                    1, this, IrqPolicy::edgeHard());
                registerIsaSplitIrq(manager, 8, IrqPolicy::levelHard());
            }
        """,
        "src/declarations.h": """
            class Manager {
              public:
                virtual irq_id_t registerHardIsaIrqHandler(
                    int, Handler *, Policy) = 0;
                bool registerInterruptHandler(int, Handler *);
            };
        """,
        "src/definitions.cc": """
            irq_id_t Manager::registerHardIsaIrqHandler(
                int, Handler *, Policy) { return 0; }
            bool Manager::registerInterruptHandler(int, Handler *) { return true; }
            const char *text = "registerHardPciIrqHandler(ignored)";
            // manager.registerHardPciIrqHandler(ignored);
        """,
        "src/qualified.cc": """
            void Derived::install() {
                Base::registerHardPciIrqHandler(this, &device, policy);
            }
        """,
        "src/modules/system/hosted-smoke/ignored.cc": """
            void test() {
                InterruptManager::instance().registerInterruptHandler(1, this);
                manager.registerHardPciIrqHandler(this, device, policy);
            }
        """,
        "src/system/kernel/core/processor/x64/InterruptManager.cc":
            "void interrupt() {}\n",
        RTC_SOURCE: """
            void Rtc::start() {
                irqManager.registerIsaIrqHandler(
                    8, this, IrqPolicy::levelThreaded());
            }
        """,
    }
    require_clean("allowed calls and non-call syntax", clean_sources, expected)

    extra = dict(clean_sources)
    extra["src/extra.cc"] = """
        void extra() {
            manager->registerHardPciIrqHandler(this, device, policy);
        }
    """
    require_failure(
        "unexpected hard registration",
        extra,
        expected,
        "hard/split IRQ registration is not allowlisted",
    )

    missing = dict(clean_sources)
    missing["src/allowed.cc"] = """
        void Device::start() {
            manager.registerHardIsaIrqHandler(
                1, this, IrqPolicy::edgeHard());
        }
    """
    require_failure(
        "missing allowlisted registration",
        missing,
        expected,
        "missing allowlisted hard/split IRQ registration",
    )

    wrong_rtc_policy = dict(clean_sources)
    wrong_rtc_policy[RTC_SOURCE] = "void Rtc::start() {}"
    require_failure(
        "RTC threaded policy",
        wrong_rtc_policy,
        expected,
        "RTC IRQ8 must use ordinary level-threaded delivery",
    )

    raw = dict(clean_sources)
    raw["src/modules/drivers/raw.cc"] = """
        void install() {
            InterruptManager::instance().registerInterruptHandler(32, this);
        }
    """
    require_failure(
        "raw module registration",
        raw,
        expected,
        "production module uses raw registerInterruptHandler",
    )

    tracker = dict(clean_sources)
    tracker["src/system/kernel/core/processor/x64/InterruptManager.cc"] = """
        #include "pedigree/kernel/process/TimeTracker.h"
        void interrupt() { TimeTracker tracker(0, false); }
    """
    require_failure(
        "interrupt-entry TimeTracker",
        tracker,
        expected,
        "TimeTracker is forbidden in processor interrupt entry",
    )

    constructor_heap = dict(clean_sources)
    constructor_heap[DISPATCHER_HEADER] = """
        class ThreadedIrqDispatcher {
          private:
            String m_Name;
        };
    """
    require_failure(
        "dispatcher constructor heap entry",
        constructor_heap,
        expected,
        "threaded IRQ dispatcher name must use inline storage",
    )

    print("hard IRQ boundary checker self-test passed (7 cases)")
    return 0


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run built-in parser and policy fixtures",
    )
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

    print("hard IRQ boundary check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
