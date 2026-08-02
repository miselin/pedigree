#!/usr/bin/env python3

"""Enforce the reviewed CPU-time accounting entry and deferral boundary."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable, Mapping


SOURCE_SUFFIXES = frozenset((".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"))
MEMBER_NAMES = (
    "recordTime",
    "trackTime",
    "transitionTime",
    "transitionTimeAtInterruptReturn",
    "publishDeferredTimeAccounting",
)
UNQUALIFIED_NAMES = ("reportTimesUpdated", "enableTimeAccountingReports")
CONTROL_CALLS = frozenset(
    ("if", "for", "while", "switch", "sizeof", "alignof", "decltype")
)


@dataclass(frozen=True, order=True)
class CallKey:
    path: str
    receiver: str
    name: str
    arguments: str


@dataclass(frozen=True, order=True)
class ScopeKey:
    path: str
    type_name: str
    arguments: str


@dataclass(frozen=True)
class Invocation:
    key: CallKey
    line: int


@dataclass(frozen=True)
class ScopeInvocation:
    key: ScopeKey
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


@dataclass(frozen=True)
class RawBodySpec:
    path: str
    label: str
    signature: str
    allowed_calls: frozenset[str]


def call(path: str, receiver: str, name: str, arguments: str) -> CallKey:
    return CallKey(path, receiver, name, arguments)


def scope(path: str, type_name: str, arguments: str) -> ScopeKey:
    return ScopeKey(path, type_name, arguments)


EXPECTED_MEMBER_CALLS = Counter(
    (
        call(
            "src/modules/subsys/posix/system-syscalls.cc",
            "currentThread",
            "trackTime",
            "CpuTimeMode::Kernel",
        ),
        call(
            "src/modules/subsys/posix/system-syscalls.cc",
            "currentThread",
            "trackTime",
            "CpuTimeMode::Kernel",
        ),
        call(
            "src/modules/subsys/posix/PosixSubsystem.cc",
            "currentThread",
            "recordTime",
            "CpuTimeMode::User",
        ),
        call(
            "src/system/kernel/core/process/InterruptTimeAccounting.cc",
            "m_pThread",
            "transitionTime",
            "KernelTimeTransition::interrupted(m_bFromUserspace),"
            "KernelTimeTransition::handler()",
        ),
        call(
            "src/system/kernel/core/process/InterruptTimeAccounting.cc",
            "m_pThread",
            "transitionTime",
            "KernelTimeTransition::handler(),"
            "KernelTimeTransition::resumed(false)",
        ),
        call(
            "src/system/kernel/core/process/InterruptTimeAccounting.cc",
            "thread",
            "transitionTimeAtInterruptReturn",
            "CpuTimeMode::Kernel,CpuTimeMode::User",
        ),
        call(
            "src/system/kernel/core/process/TimeTracker.cc",
            "m_pThread",
            "transitionTime",
            "KernelTimeTransition::interrupted(m_bFromUserspace),"
            "KernelTimeTransition::handler()",
        ),
        call(
            "src/system/kernel/core/process/TimeTracker.cc",
            "thread",
            "transitionTime",
            "KernelTimeTransition::handler(),"
            "KernelTimeTransition::resumed(m_bFromUserspace)",
        ),
        call(
            "src/system/kernel/core/process/Process.cc",
            "getScheduler()",
            "publishDeferredTimeAccounting",
            "",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pThread",
            "recordTime",
            "CpuTimeMode::Kernel",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pCurrentThread",
            "trackTime",
            "CpuTimeMode::Kernel",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pNextThread",
            "recordTime",
            "CpuTimeMode::Kernel",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pThread",
            "transitionTime",
            "CpuTimeMode::Kernel,CpuTimeMode::User",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pThread",
            "transitionTime",
            "CpuTimeMode::Kernel,CpuTimeMode::User",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pCurrentThread",
            "trackTime",
            "CpuTimeMode::Kernel",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pThread",
            "recordTime",
            "bUsermode?CpuTimeMode::User:CpuTimeMode::Kernel",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pCurrentThread",
            "trackTime",
            "CpuTimeMode::Kernel",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pThread",
            "recordTime",
            "CpuTimeMode::User",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pThread",
            "trackTime",
            "CpuTimeMode::Kernel",
        ),
        call(
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "pNextThread",
            "recordTime",
            "CpuTimeMode::Kernel",
        ),
        call(
            "src/system/kernel/core/processor/x64/SyscallManager.cc",
            "getCurrentThread()",
            "transitionTime",
            "CpuTimeMode::Kernel,CpuTimeMode::User",
        ),
        call(
            "src/system/kernel/core/processor/x64/SyscallManager.cc",
            "current",
            "transitionTime",
            "CpuTimeMode::Kernel,CpuTimeMode::User",
        ),
    )
)

EXPECTED_UNQUALIFIED_CALLS = Counter(
    (
        call(
            "src/system/kernel/core/process/Process.cc",
            "",
            "reportTimesUpdated",
            "user,user+getKernelTime()",
        ),
        call(
            "src/modules/subsys/posix/PosixProcess.cc",
            "",
            "enableTimeAccountingReports",
            "",
        ),
        call(
            "src/modules/subsys/posix/PosixProcess.cc",
            "",
            "enableTimeAccountingReports",
            "",
        ),
    )
)

EXPECTED_SCOPES = Counter(
    (
        scope(
            "src/system/kernel/core/processor/x64/InterruptManager.cc",
            "InterruptTimeAccounting",
            "!interruptState.kernelMode()",
        ),
        scope(
            "src/system/kernel/core/processor/hosted/InterruptManager.cc",
            "InterruptTimeAccounting",
            "fromUserspace",
        ),
        scope(
            "src/system/kernel/core/processor/x64/SyscallManager.cc",
            "TimeTracker",
            "0,true",
        ),
    )
)


RAW_BODY_SPECS = (
    RawBodySpec(
        "src/system/include/pedigree/kernel/process/DeferredTimeAccounting.h",
        "ThreadTimeAccounting::record",
        r"\bvoid\s+record\s*\(\s*CpuTimeMode\s+mode\s*,",
        frozenset(
            (
                "entry",
                "installProcessorBaseline",
                "__atomic_load_n",
                "__atomic_compare_exchange_n",
            )
        ),
    ),
    RawBodySpec(
        "src/system/include/pedigree/kernel/process/DeferredTimeAccounting.h",
        "ThreadTimeAccounting::elapsed",
        r"\bTime::Timestamp\s+elapsed\s*\(\s*CpuTimeMode\s+mode\s*,",
        frozenset(
            (
                "entry",
                "installProcessorBaseline",
                "__atomic_load_n",
                "__atomic_compare_exchange_n",
            )
        ),
    ),
    RawBodySpec(
        "src/system/include/pedigree/kernel/process/DeferredTimeAccounting.h",
        "ThreadTimeAccounting::installProcessorBaseline",
        r"\bstatic\s+bool\s+installProcessorBaseline\s*\(",
        frozenset(("__atomic_load_n", "__atomic_store_n")),
    ),
    RawBodySpec(
        "src/system/include/pedigree/kernel/process/DeferredTimeAccounting.h",
        "DeferredTimeAccounting::publish",
        r"\bbool\s+publish\s*\(\s*Time::Timestamp\s+elapsed\s*\)",
        frozenset(("__atomic_exchange_n",)),
    ),
    RawBodySpec(
        "src/system/include/pedigree/kernel/process/DeferredTimeAccounting.h",
        "DeferredTimeAccountingWorkerState::publish",
        r"\bvoid\s+publish\s*\(\s*\)",
        frozenset(("__atomic_add_fetch",)),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/Thread.cc",
        "CpuTimeSample::CpuTimeSample",
        r"(?<!~)\bCpuTimeSample\s*\(\s*\)",
        frozenset(
            (
                "timestamp",
                "processor",
                "m_InterruptsWereEnabled",
                "getInterrupts",
                "setInterrupts",
                "id",
                "getTicks",
            )
        ),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/Thread.cc",
        "CpuTimeSample::~CpuTimeSample",
        r"~CpuTimeSample\s*\(\s*\)",
        frozenset(("setInterrupts",)),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/Thread.cc",
        "Thread::recordTime",
        r"\bvoid\s+Thread::recordTime\s*\(",
        frozenset(("record", "__atomic_store_n")),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/Thread.cc",
        "Thread::trackTime",
        r"\bvoid\s+Thread::trackTime\s*\(",
        frozenset(("elapsed", "publishTimeAccounting")),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/Thread.cc",
        "Thread::transitionTime",
        r"\bvoid\s+Thread::transitionTime\s*\(",
        frozenset(
            ("elapsed", "record", "__atomic_store_n", "publishTimeAccounting")
        ),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/Thread.cc",
        "Thread::transitionTimeAtInterruptReturn",
        r"\bvoid\s+Thread::transitionTimeAtInterruptReturn\s*\(",
        frozenset(
            (
                "id",
                "getTicks",
                "elapsed",
                "record",
                "__atomic_store_n",
                "publishTimeAccounting",
            )
        ),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/Thread.cc",
        "Thread::currentTimeAccountingMode",
        r"\bCpuTimeMode\s+Thread::currentTimeAccountingMode\s*\(",
        frozenset(("__atomic_load_n",)),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/InterruptTimeAccounting.cc",
        "InterruptTimeAccounting constructor",
        r"\bInterruptTimeAccounting::InterruptTimeAccounting\s*\(",
        frozenset(
            (
                "information",
                "getCurrentThread",
                "getParent",
                "m_pThread",
                "m_bFromUserspace",
                "transitionTime",
                "interrupted",
                "handler",
            )
        ),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/InterruptTimeAccounting.cc",
        "InterruptTimeAccounting destructor",
        r"\bInterruptTimeAccounting::~InterruptTimeAccounting\s*\(",
        frozenset(("transitionTime", "handler", "resumed")),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/InterruptTimeAccounting.cc",
        "InterruptTimeAccounting::finishUserReturn",
        r"\bvoid\s+InterruptTimeAccounting::finishUserReturn\s*\(",
        frozenset(
            ("currentTimeAccountingMode", "transitionTimeAtInterruptReturn")
        ),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/TimeTracker.cc",
        "TimeTracker constructor",
        r"\bTimeTracker::TimeTracker\s*\(",
        frozenset(
            (
                "information",
                "getCurrentThread",
                "getParent",
                "m_pProcess",
                "m_pThread",
                "m_bFromUserspace",
                "transitionTime",
                "interrupted",
                "handler",
            )
        ),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/TimeTracker.cc",
        "TimeTracker destructor",
        r"\bTimeTracker::~TimeTracker\s*\(",
        frozenset(("finish",)),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/TimeTracker.cc",
        "TimeTracker::finish",
        r"\bvoid\s+TimeTracker::finish\s*\(",
        frozenset(("transitionTime", "handler", "resumed")),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/Process.cc",
        "Process::publishTimeAccounting",
        r"\bvoid\s+Process::publishTimeAccounting\s*\(",
        frozenset(("__atomic_fetch_add", "publishTimeAccountingBatch")),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/Process.cc",
        "Process::publishTimeAccountingBatch",
        r"\bvoid\s+Process::publishTimeAccountingBatch\s*\(",
        frozenset(
            (
                "__atomic_load_n",
                "publish",
                "information",
                "getScheduler",
                "publishDeferredTimeAccounting",
            )
        ),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/PerProcessorScheduler.cc",
        "PerProcessorScheduler::ringIrqWorkDoorbell",
        r"\bvoid\s+PerProcessorScheduler::ringIrqWorkDoorbell\s*\(",
        frozenset(),
    ),
    RawBodySpec(
        "src/system/kernel/core/process/PerProcessorScheduler.cc",
        "PerProcessorScheduler::publishDeferredTimeAccounting",
        r"\bvoid\s+PerProcessorScheduler::publishDeferredTimeAccounting\s*\(",
        frozenset(("publish", "ringIrqWorkDoorbell")),
    ),
    RawBodySpec(
        "src/system/kernel/time/Time.cc",
        "Time::getTicks",
        r"\bTimestamp\s+getTicks\s*\(\s*\)",
        frozenset(("instance", "getTimer", "getTickCountNano")),
    ),
    RawBodySpec(
        "src/system/kernel/machine/mach_pc/Pc.cc",
        "Pc::getTimer",
        r"\bTimer\s*\*\s*Pc::getTimer\s*\(",
        frozenset(("instance",)),
    ),
    RawBodySpec(
        "src/system/kernel/machine/mach_pc/Rtc.cc",
        "Rtc::getTickCountNano",
        r"\buint64_t\s+Rtc::getTickCountNano\s*\(",
        frozenset(
            (
                "getInterrupts",
                "setInterrupts",
                "information",
                "getTscClockAnchor",
                "readOrderedTsc",
                "fromAnchor",
                "value",
                "publish",
            )
        ),
    ),
)


FORBIDDEN_RAW_TOKENS = re.compile(
    r"\b(?:LockGuard|RecursingLockGuard|Spinlock|Mutex|WaitQueue|Semaphore|"
    r"ConditionVariable|new|delete|malloc|calloc|realloc|free)\b"
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


def normalize(source: str) -> str:
    return re.sub(r"\s+", "", source)


def matching_forward(source: str, opening: int, left: str, right: str) -> int:
    depth = 1
    for position in range(opening + 1, len(source)):
        if source[position] == left:
            depth += 1
        elif source[position] == right:
            depth -= 1
            if not depth:
                return position
    return -1


def matching_open_paren(source: str, closing: int) -> int:
    depth = 1
    for position in range(closing - 1, -1, -1):
        if source[position] == ")":
            depth += 1
        elif source[position] == "(":
            depth -= 1
            if not depth:
                return position
    return -1


def receiver_before(source: str, name_start: int) -> str:
    position = name_start - 1
    while position >= 0 and source[position].isspace():
        position -= 1
    if position >= 1 and source[position - 1 : position + 1] == "->":
        position -= 2
    elif position >= 0 and source[position] == ".":
        position -= 1
    else:
        return ""

    while position >= 0 and source[position].isspace():
        position -= 1
    end = position + 1
    if position >= 0 and source[position] == ")":
        opening = matching_open_paren(source, position)
        if opening < 0:
            return ""
        position = opening - 1
        while position >= 0 and source[position].isspace():
            position -= 1
        while position >= 0 and (source[position].isalnum() or source[position] == "_"):
            position -= 1
        return normalize(source[position + 1 : end])

    while position >= 0 and (source[position].isalnum() or source[position] == "_"):
        position -= 1
    return normalize(source[position + 1 : end])


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
        r"\b(?:virtual|static|inline|constexpr|consteval|friend|bool|void)\b",
        prefix,
    )
    expression_hint = re.search(r"(?:=|\breturn\b|\bif\b|\bwhile\b)", prefix)
    declaration_suffix = not suffix or suffix.startswith(
        (";", "=", "{", "const", "noexcept", "override", "final")
    )
    return bool(declaration_hint and not expression_hint and declaration_suffix)


def find_named_calls(
    path: str, source: str, names: Iterable[str], members: bool
) -> list[Invocation]:
    masked = mask_cpp(source)
    pattern = re.compile(r"\b(" + "|".join(map(re.escape, names)) + r")\s*\(")
    invocations: list[Invocation] = []
    for match in pattern.finditer(masked):
        name = match.group(1)
        opening = masked.rfind("(", match.start(1), match.end())
        closing = matching_forward(masked, opening, "(", ")")
        if closing < 0:
            continue
        receiver = receiver_before(masked, match.start(1))
        if members != bool(receiver):
            continue
        before = masked[: match.start(1)].rstrip()
        if not receiver and before.endswith("::"):
            continue
        if looks_like_header_declaration(path, masked, match.start(1), closing):
            continue
        invocations.append(
            Invocation(
                CallKey(
                    path,
                    receiver,
                    name,
                    normalize(masked[opening + 1 : closing]),
                ),
                source.count("\n", 0, match.start(1)) + 1,
            )
        )
    return invocations


def find_scopes(path: str, source: str) -> list[ScopeInvocation]:
    if Path(path).suffix not in (".cc", ".cpp", ".cxx"):
        return []
    masked = mask_cpp(source)
    pattern = re.compile(
        r"\b(InterruptTimeAccounting|TimeTracker)\b"
        r"(?:\s+[A-Za-z_]\w*)?\s*\("
    )
    scopes: list[ScopeInvocation] = []
    for match in pattern.finditer(masked):
        before = masked[: match.start(1)].rstrip()
        if before.endswith(("::", "::~")):
            continue
        opening = masked.rfind("(", match.start(1), match.end())
        closing = matching_forward(masked, opening, "(", ")")
        if closing < 0:
            continue
        scopes.append(
            ScopeInvocation(
                ScopeKey(
                    path,
                    match.group(1),
                    normalize(masked[opening + 1 : closing]),
                ),
                source.count("\n", 0, match.start(1)) + 1,
            )
        )
    return scopes


def compare_calls(
    observed: Iterable[Invocation], expected: Counter[CallKey], label: str
) -> list[Diagnostic]:
    counts: Counter[CallKey] = Counter()
    lines: dict[CallKey, list[int]] = {}
    for invocation in observed:
        counts[invocation.key] += 1
        lines.setdefault(invocation.key, []).append(invocation.line)
    diagnostics: list[Diagnostic] = []
    for key, count in sorted((counts - expected).items()):
        for line in lines.get(key, [0])[expected[key] : expected[key] + count]:
            diagnostics.append(
                Diagnostic(
                    key.path,
                    line,
                    f"{label} is not allowlisted: {key.receiver + ('->' if key.receiver else '')}"
                    f"{key.name}({key.arguments})",
                )
            )
    for key, count in sorted((expected - counts).items()):
        diagnostics.append(
            Diagnostic(
                key.path,
                0,
                f"missing allowlisted {label}: "
                f"{key.receiver + ('->' if key.receiver else '')}"
                f"{key.name}({key.arguments})"
                + (f" x{count}" if count != 1 else ""),
            )
        )
    return diagnostics


def compare_scopes(
    observed: Iterable[ScopeInvocation], expected: Counter[ScopeKey]
) -> list[Diagnostic]:
    counts: Counter[ScopeKey] = Counter()
    lines: dict[ScopeKey, list[int]] = {}
    for invocation in observed:
        counts[invocation.key] += 1
        lines.setdefault(invocation.key, []).append(invocation.line)
    diagnostics: list[Diagnostic] = []
    for key, count in sorted((counts - expected).items()):
        for line in lines.get(key, [0])[expected[key] : expected[key] + count]:
            diagnostics.append(
                Diagnostic(
                    key.path,
                    line,
                    f"accounting scope is not allowlisted: "
                    f"{key.type_name}({key.arguments})",
                )
            )
    for key, count in sorted((expected - counts).items()):
        diagnostics.append(
            Diagnostic(
                key.path,
                0,
                f"missing allowlisted accounting scope: "
                f"{key.type_name}({key.arguments})"
                + (f" x{count}" if count != 1 else ""),
            )
        )
    return diagnostics


def extract_body(source: str, signature: str) -> tuple[int, str] | None:
    masked = mask_cpp(source)
    matches = list(re.finditer(signature, masked, re.MULTILINE))
    if len(matches) != 1:
        return None
    match = matches[0]
    opening = masked.find("{", match.end())
    if opening < 0:
        return None
    closing = matching_forward(masked, opening, "{", "}")
    if closing < 0:
        return None
    return match.start(), masked[match.end() : closing]


def audit_raw_bodies(
    sources: Mapping[str, str], specs: Iterable[RawBodySpec] = RAW_BODY_SPECS
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    call_pattern = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
    for spec in specs:
        source = sources.get(spec.path)
        if source is None:
            diagnostics.append(Diagnostic(spec.path, 0, f"missing {spec.label}"))
            continue
        extracted = extract_body(source, spec.signature)
        if extracted is None:
            diagnostics.append(
                Diagnostic(spec.path, 0, f"could not uniquely locate {spec.label}")
            )
            continue
        start, body = extracted
        line = source.count("\n", 0, start) + 1
        forbidden = FORBIDDEN_RAW_TOKENS.search(body)
        if forbidden:
            diagnostics.append(
                Diagnostic(
                    spec.path,
                    line + body.count("\n", 0, forbidden.start()),
                    f"{spec.label} contains forbidden raw-path token "
                    f"{forbidden.group(0)}",
                )
            )
        for match in call_pattern.finditer(body):
            callee = match.group(1)
            if callee in CONTROL_CALLS or callee in spec.allowed_calls:
                continue
            diagnostics.append(
                Diagnostic(
                    spec.path,
                    line + body.count("\n", 0, match.start()),
                    f"{spec.label} calls non-allowlisted raw-path helper {callee}",
                )
            )
    return diagnostics


def require_body_shape(
    sources: Mapping[str, str], path: str, label: str, signature: str,
    ordered_tokens: Iterable[str]
) -> list[Diagnostic]:
    source = sources.get(path)
    if source is None:
        return [Diagnostic(path, 0, f"missing {label}")]
    extracted = extract_body(source, signature)
    if extracted is None:
        return [Diagnostic(path, 0, f"could not uniquely locate {label}")]
    start, body = extracted
    compact = normalize(body)
    cursor = 0
    for token in ordered_tokens:
        position = compact.find(token, cursor)
        if position < 0:
            return [
                Diagnostic(
                    path,
                    source.count("\n", 0, start) + 1,
                    f"{label} is missing ordered lifecycle boundary {token}",
                )
            ]
        cursor = position + len(token)
    return []


def audit_lifecycle_and_wiring(
    root: Path, sources: Mapping[str, str]
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    shapes = (
        (
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "PerProcessorScheduler initialization",
            r"\bvoid\s+PerProcessorScheduler::initialise\s*\(",
            ("startNewThreadWorker(", "startTimeAccountingWorker(pThread->getParent())"),
        ),
        (
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "PerProcessorScheduler destruction",
            r"\bPerProcessorScheduler::~PerProcessorScheduler\s*\(",
            ("stopTimeAccountingWorker()", "removeHandler(this)"),
        ),
        (
            "src/system/kernel/core/process/PerProcessorScheduler.cc",
            "accounting-worker stop",
            r"\bvoid\s+PerProcessorScheduler::stopTimeAccountingWorker\s*\(",
            (
                "m_StopTimeAccountingWorker=1",
                "ringIrqWorkDoorbell()",
                "serviceIrqWorkDoorbell()",
                "m_TimeAccountingWorker.join()",
            ),
        ),
        (
            "src/system/kernel/core/process/Scheduler.cc",
            "scheduler accounting drain",
            r"\bvoid\s+Scheduler::drainDeferredTimeAccounting\s*\(",
            (
                "m_SchedulerLock.release()",
                "process->drainDeferredTimeAccounting()",
                "process->endExternalLease()",
            ),
        ),
        (
            "src/system/kernel/core/process/Process.cc",
            "process accounting drain",
            r"\bvoid\s+Process::drainDeferredTimeAccounting\s*\(",
            ("tryAcquire(report)", "reportTimesUpdated(user,user+getKernelTime())"),
        ),
        (
            "src/system/kernel/core/process/Process.cc",
            "process accounting close",
            r"\bvoid\s+Process::closeDeferredTimeAccounting\s*\(",
            (
                "m_bTimeAccountingReportsEnabled,false",
                "m_TimeAccountingReports.closeAndWait()",
                "m_DeferredTimeAccounting.take()",
            ),
        ),
        (
            "src/system/kernel/core/process/Process.cc",
            "process termination admission close",
            r"\bbool\s+Process::beginTermination\s*\(",
            ("closeDeferredTimeAccounting()", "m_Lock.acquire()"),
        ),
        (
            "src/modules/subsys/posix/PosixProcess.cc",
            "POSIX absolute timer reporting",
            r"\bvoid\s+PosixProcess::reportTimesUpdated\s*\(",
            (
                "m_VirtualIntervalTimer.consumeCpuTime(userTotal)",
                "m_ProfileIntervalTimer.consumeCpuTime(total)",
            ),
        ),
        (
            "src/system/kernel/core/process/Thread.cc",
            "Thread accounting-mode entry publication",
            r"\bvoid\s+Thread::recordTime\s*\(",
            (
                "m_TimeAccounting.record(mode,sample.timestamp,sample.processor)",
                "__atomic_store_n(&m_CurrentTimeAccountingMode,"
                "static_cast<size_t>(mode),__ATOMIC_RELEASE)",
            ),
        ),
        (
            "src/system/kernel/core/process/Thread.cc",
            "Thread accounting-mode transition publication",
            r"\bvoid\s+Thread::transitionTime\s*\(",
            (
                "m_TimeAccounting.elapsed(from,sample.timestamp,sample.processor)",
                "m_TimeAccounting.record(to,sample.timestamp,sample.processor)",
                "__atomic_store_n(&m_CurrentTimeAccountingMode,"
                "static_cast<size_t>(to),__ATOMIC_RELEASE)",
            ),
        ),
        (
            "src/system/kernel/core/process/Thread.cc",
            "interrupt-return accounting transition",
            r"\bvoid\s+Thread::transitionTimeAtInterruptReturn\s*\(",
            (
                "Processor::id()",
                "Time::getTicks()",
                "m_TimeAccounting.elapsed(from,timestamp,processor)",
                "m_TimeAccounting.record(to,timestamp,processor)",
                "__atomic_store_n(&m_CurrentTimeAccountingMode,"
                "static_cast<size_t>(to),__ATOMIC_RELEASE)",
            ),
        ),
        (
            "src/system/kernel/core/process/Thread.cc",
            "current accounting-mode acquisition",
            r"\bCpuTimeMode\s+Thread::currentTimeAccountingMode\s*\(",
            (
                "__atomic_load_n(&m_CurrentTimeAccountingMode,"
                "__ATOMIC_ACQUIRE)",
            ),
        ),
        (
            "src/system/kernel/core/process/InterruptTimeAccounting.cc",
            "interrupt user-return completion",
            r"\bvoid\s+InterruptTimeAccounting::finishUserReturn\s*\(",
            (
                "thread->currentTimeAccountingMode()==CpuTimeMode::Kernel",
                "thread->transitionTimeAtInterruptReturn("
                "CpuTimeMode::Kernel,CpuTimeMode::User)",
            ),
        ),
    )
    for path, label, signature, tokens in shapes:
        diagnostics.extend(
            require_body_shape(sources, path, label, signature, tokens)
        )

    header_path = "src/system/include/pedigree/kernel/process/DeferredTimeAccounting.h"
    header = normalize(mask_cpp(sources.get(header_path, "")))
    for token in (
        "__atomic_always_lock_free(sizeof(Time::Timestamp),nullptr)",
        "__atomic_always_lock_free(sizeof(size_t),nullptr)",
        "__atomic_always_lock_free(sizeof(bool),nullptr)",
    ):
        if token not in header:
            diagnostics.append(
                Diagnostic(header_path, 0, f"missing accounting lock-free gate {token}")
            )

    wiring = (
        (
            "src/system/kernel/CMakeLists.txt",
            "core/process/InterruptTimeAccounting.cc",
        ),
        (
            "src/buildutil/CMakeLists.txt",
            "testsuite/test-DeferredTimeAccounting.cc",
        ),
        (
            "src/buildutil/CMakeLists.txt",
            "testsuite/test-IntervalTimerState.cc",
        ),
    )
    for path, token in wiring:
        try:
            text = (root / path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        if text.count(token) != 1:
            diagnostics.append(
                Diagnostic(path, 0, f"expected exactly one build wiring entry {token}")
            )

    marker_path = "src/modules/system/hosted-smoke/scheduler-regressions.cc"
    marker = "HOSTED-WAIT-TEST: PASS deferred-time-accounting-worker"
    if sources.get(marker_path, "").count(marker) != 1:
        diagnostics.append(
            Diagnostic(marker_path, 0, "missing unique deferred-accounting PASS marker")
        )
    return diagnostics


def audit_sources(
    sources: Mapping[str, str],
    expected_members: Counter[CallKey] = EXPECTED_MEMBER_CALLS,
    expected_unqualified: Counter[CallKey] = EXPECTED_UNQUALIFIED_CALLS,
    expected_scopes: Counter[ScopeKey] = EXPECTED_SCOPES,
) -> list[Diagnostic]:
    members: list[Invocation] = []
    unqualified: list[Invocation] = []
    scopes: list[ScopeInvocation] = []
    for path in sorted(sources):
        if path.startswith("src/modules/system/hosted-smoke/"):
            continue
        source = sources[path]
        members.extend(find_named_calls(path, source, MEMBER_NAMES, True))
        unqualified.extend(
            find_named_calls(path, source, UNQUALIFIED_NAMES, False)
        )
        unqualified.extend(
            find_named_calls(path, source, UNQUALIFIED_NAMES, True)
        )
        scopes.extend(find_scopes(path, source))
    diagnostics = compare_calls(members, expected_members, "accounting member call")
    diagnostics.extend(
        compare_calls(unqualified, expected_unqualified, "accounting callback call")
    )
    diagnostics.extend(compare_scopes(scopes, expected_scopes))
    diagnostics.extend(audit_raw_bodies(sources))
    return sorted(set(diagnostics))


def load_sources(root: Path) -> dict[str, str]:
    sources: dict[str, str] = {}
    for path in sorted((root / "src").rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            sources[path.relative_to(root).as_posix()] = path.read_text(
                encoding="utf-8", errors="replace"
            )
    return sources


def require(condition: bool, label: str) -> None:
    if not condition:
        raise AssertionError(label)


def self_test() -> int:
    path = "src/example.cc"
    clean = r'''
        void Thing::recordTime(CpuTimeMode) {}
        void run() {
            target->recordTime(CpuTimeMode::Kernel);
            const char *s = "other->recordTime(CpuTimeMode::User)";
            // other->recordTime(CpuTimeMode::User);
        }
    '''
    found = find_named_calls(path, clean, ("recordTime",), True)
    expected = Counter((call(path, "target", "recordTime", "CpuTimeMode::Kernel"),))
    require(not compare_calls(found, expected, "call"), "clean call parsing")

    wrong_receiver = clean.replace("target->recordTime", "other->recordTime", 1)
    require(
        bool(compare_calls(
            find_named_calls(path, wrong_receiver, ("recordTime",), True),
            expected,
            "call",
        )),
        "wrong receiver was accepted",
    )

    wrong_mode = clean.replace("CpuTimeMode::Kernel", "CpuTimeMode::User", 1)
    require(
        bool(compare_calls(
            find_named_calls(path, wrong_mode, ("recordTime",), True),
            expected,
            "call",
        )),
        "wrong mode was accepted",
    )

    scope_source = "void irq() { InterruptTimeAccounting a(!state.kernelMode()); }"
    scope_expected = Counter(
        (scope(path, "InterruptTimeAccounting", "!state.kernelMode()"),)
    )
    require(
        not compare_scopes(find_scopes(path, scope_source), scope_expected),
        "clean scope parsing",
    )
    require(
        bool(compare_scopes([], scope_expected)),
        "missing accounting scope was accepted",
    )

    callback_source = (
        "void run() { reportTimesUpdated(user, total); "
        "other->reportTimesUpdated(user, total); }"
    )
    callback_expected: Counter[CallKey] = Counter()
    require(
        bool(compare_calls(
            find_named_calls(path, callback_source, ("reportTimesUpdated",), False)
            + find_named_calls(path, callback_source, ("reportTimesUpdated",), True),
            callback_expected,
            "callback",
        )),
        "extra callback was accepted",
    )

    raw_path = "src/raw.cc"
    raw_spec = RawBodySpec(
        raw_path,
        "raw",
        r"\bvoid\s+raw\s*\(",
        frozenset(("safe",)),
    )
    require(
        not audit_raw_bodies({raw_path: "void raw() { safe(); }"}, (raw_spec,)),
        "clean raw body",
    )
    require(
        bool(audit_raw_bodies(
            {raw_path: "void raw() { safe(); helperThatLocks(); }"},
            (raw_spec,),
        )),
        "helper-smuggled operation was accepted",
    )

    shape_source = {path: "void close() { disable(); wait(); discard(); }"}
    require(
        not require_body_shape(
            shape_source,
            path,
            "close",
            r"\bvoid\s+close\s*\(",
            ("disable()", "wait()", "discard()"),
        ),
        "clean lifecycle shape",
    )
    require(
        bool(require_body_shape(
            shape_source,
            path,
            "close",
            r"\bvoid\s+close\s*\(",
            ("disable()", "missing()", "discard()"),
        )),
        "missing lifecycle wiring was accepted",
    )

    print("time accounting boundary checker self-test passed (7 cases)")
    return 0


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test", action="store_true", help="run parser and policy fixtures"
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
    sources = load_sources(root)
    diagnostics = audit_sources(sources)
    diagnostics.extend(audit_lifecycle_and_wiring(root, sources))
    for diagnostic in sorted(set(diagnostics)):
        print(diagnostic.render(), file=sys.stderr)
    if diagnostics:
        return 1
    print("time accounting boundary check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
