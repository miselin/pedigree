#!/usr/bin/env python3
"""Decode Pedigree STRACE records without modifying the serial log."""

import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import re
import statistics


ROOT = Path(__file__).resolve().parents[2]
MAPPINGS = ROOT / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
FIELDS = {
    "E": {"id", "pid", "tid", "abi", "nr"},
    "A": {"id", "n", "x", "y", "z"},
    "P": {"id", "arg", "off", "data"},
    "S": {"id", "arg", "status"},
    "X": {"id", "raw", "err", "ns"},
}
PATH_ARGUMENTS = {}
for arguments, names in (
    ((0,), "open stat lstat access execve chdir mkdir rmdir creat unlink readlink chmod chown "
     "lchown utime utimes statfs chroot truncate mknod umount2 swapon swapoff acct setxattr "
     "lsetxattr getxattr lgetxattr listxattr llistxattr removexattr lremovexattr"),
    ((0, 1), "rename link symlink pivot_root mount"),
    ((1,), "openat mkdirat fchownat futimesat newfstatat unlinkat readlinkat fchmodat faccessat "
     "faccessat2 execveat utimensat statx fchmodat2 mknodat name_to_handle_at inotify_add_watch"),
    ((1, 3), "renameat linkat renameat2"),
    ((0, 2), "symlinkat"),
):
    PATH_ARGUMENTS.update(dict.fromkeys(names.split(), arguments))
NOTES = [
    "Elapsed nanoseconds are dispatch wall time, including blocking and execution of other "
    "processes. Overlapping durations are not additive CPU time.",
    "Tracing perturbs execution. The dispatch timer excludes its own entry/path logging and "
    "exit record, but can include other threads' tracing and instrumentation effects.",
    "Raw results are unsigned 64-bit dispatch values, also decoded as signed. Linux-visible "
    "results use -errno when errno is nonzero. Successful exec/exit records describe a "
    "dispatcher action, not a return to the original userspace caller.",
    "The timeline is ordered by first record, with entry/exit serial line numbers showing "
    "interleaving. p95 uses nearest rank; elapsed summaries include records with an exit.",
    "FD/path attribution is best effort from traced operations. Initial/inherited descriptors, "
    "unobserved close-on-exec state, shared descriptor tables, concurrent changes, and resolved "
    "filesystem paths are not fully reconstructible. Relative paths remain relative; attribution "
    "is cleared after successful exec because close-on-exec state is not fully captured.",
    "Sequence gaps and missing/noncontiguous chunks are diagnosed. The format carries no total "
    "path length or final record count, so a lost final path chunk or complete final call can "
    "remain undetectable even when the remaining records are structurally complete.",
    "call_records_complete describes record structure; paths_complete additionally excludes "
    "truncated/invalid paths. workload_complete requires trace-link ACK/DONE and PASS END "
    "markers. Complete is false when workload boundaries cannot be verified.",
]


def syscall_names():
    text = MAPPINGS.read_text()
    return {int(number): name for name, number in re.findall(
        r"^PEDIGREE_LINUX_AMD64_SYSCALL\((\w+),\s*(\d+),", text, re.MULTILINE)}


def signed(value, bits=64):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def decode_record(line):
    match = re.search(r"\bSTRACE ([EAPSX])\s+(.*)", line)
    if not match:
        raise ValueError("unrecognized or truncated STRACE record")
    kind, payload = match.groups()
    pairs = re.findall(r"(\w+)=(\S*)", payload)
    fields = dict(pairs)
    if len(pairs) != len(fields) or set(fields) != FIELDS[kind]:
        raise ValueError(f"{kind}: missing, duplicate, or unexpected fields")
    for key, value in fields.items():
        if key in {"abi", "status", "data"}:
            continue
        if not re.fullmatch(r"(?:0x)?[0-9a-fA-F]{1,16}", value):
            raise ValueError(f"{kind}: invalid hexadecimal {key}")
        fields[key] = int(value, 16)
    if kind == "E" and fields["abi"] not in {"L", "P"}:
        raise ValueError("E: unknown ABI")
    if kind == "A" and fields["n"] not in {0, 3}:
        raise ValueError("A: argument offset must be 0 or 3")
    if kind in {"P", "S"} and fields["arg"] > 5:
        raise ValueError(f"{kind}: invalid argument index")
    if kind == "P" and (not re.fullmatch(r"(?:[0-9a-fA-F]{2}){1,32}", fields["data"])
                         or fields["off"] + len(fields["data"]) // 2 > 256):
        raise ValueError("P: invalid path chunk or offset")
    if kind == "S" and fields["status"] not in {"ok", "truncated", "invalid"}:
        raise ValueError("S: invalid path status")
    return kind, fields


def parse_trace(text, names=None):
    names = syscall_names() if names is None else names
    calls, issues, trace_lines = {}, [], []
    markers = {"ack": [], "done": [], "pass": []}
    for line_number, line in enumerate(text.splitlines(), 1):
        for key, pattern in (("ack", r"COMPILEBENCH ACK phase=trace-link\b"),
                             ("done", r"COMPILEBENCH DONE phase=trace-link\b"),
                             ("pass", r"COMPILEBENCH PASS END\b")):
            if re.search(pattern, line):
                markers[key].append(line_number)
        if "STRACE" not in line:
            continue
        trace_lines.append(line_number)
        try:
            kind, fields = decode_record(line)
        except ValueError as error:
            issues.append(f"line {line_number}: {error}")
            continue
        ident = fields.pop("id")
        call = calls.setdefault(ident, {
            "id": ident, "first_line": line_number, "entry_line": None, "exit_line": None,
            "pid": None, "tid": None, "abi": None, "nr": None, "name": "unknown",
            "args": [None] * 6, "paths": {}, "issues": [], "exit": None,
        })
        problems = call["issues"]
        if call["exit_line"] is not None:
            problems.append(f"line {line_number}: {kind} after exit (duplicate or out of order)")
        if kind != "E" and call["entry_line"] is None:
            problems.append(f"line {line_number}: {kind} before entry")
        if kind == "E":
            if call["entry_line"] is not None:
                problems.append(f"line {line_number}: duplicate entry")
            else:
                call.update(fields, entry_line=line_number)
                call["name"] = names.get(fields["nr"], f"syscall_{fields['nr']}") if fields["abi"] == "L" else f"posix_{fields['nr']}"
        elif kind == "A":
            offset = fields["n"]
            if call["args"][offset] is not None:
                problems.append(f"line {line_number}: duplicate argument group {offset}")
            elif (offset == 0 and call["args"][3] is not None) or call["paths"]:
                problems.append(f"line {line_number}: argument group out of order")
            call["args"][offset:offset + 3] = [fields[key] for key in ("x", "y", "z")]
        elif kind in {"P", "S"}:
            if None in call["args"]:
                problems.append(f"line {line_number}: path before complete arguments")
            path = call["paths"].setdefault(fields["arg"], {"chunks": [], "status": None})
            if path["status"] is not None:
                problems.append(f"line {line_number}: path record after status")
            if kind == "P":
                length = sum(len(chunk["data"]) // 2 for chunk in path["chunks"])
                if fields["off"] != length:
                    problems.append(f"line {line_number}: missing, duplicate, or out-of-order path chunk")
                path["chunks"].append({"off": fields["off"], "data": fields["data"]})
            else:
                path["status"] = fields["status"]
        elif call["exit"] is None:
            call["exit_line"] = line_number
            call["exit"] = fields

    for call in calls.values():
        if call["entry_line"] is None:
            call["issues"].append("missing entry")
        if None in call["args"]:
            call["issues"].append("missing argument group")
        if call["exit"] is None:
            call["issues"].append("missing exit")
        expected_paths = set(PATH_ARGUMENTS.get(call["name"], ())) if call["abi"] == "L" else set()
        if set(call["paths"]) != expected_paths:
            call["issues"].append(f"path arguments mismatch: expected {sorted(expected_paths)}, "
                                  f"found {sorted(call['paths'])}")
        for arg, path in call["paths"].items():
            if path["status"] is None:
                call["issues"].append(f"argument {arg}: missing path status")
            path["hex"] = "".join(chunk["data"] for chunk in path.pop("chunks"))
            path["text"] = bytes.fromhex(path["hex"]).decode("utf-8", errors="backslashreplace")
            path["complete"] = path["status"] == "ok"
        call["complete"] = not call["issues"]
        call["args_hex"] = [None if arg is None else f"0x{arg:016x}" for arg in call["args"]]
        result = call["exit"]
        if result is not None:
            result["raw_hex"] = f"0x{result['raw']:016x}"
            result["raw_signed"] = signed(result["raw"])
            result["linux_visible"] = (-result["err"] if result["err"] else result["raw_signed"]) if call["abi"] == "L" else None
            result["kind"] = ("dispatch_action" if result["err"] == 0 and result["raw"] == 0 and call["name"] in
                              {"execve", "execveat", "exit", "exit_group"} else "return")
            result["error"] = bool(result["err"] or (call["abi"] == "L" and -4095 <= result["raw_signed"] < 0))
            if result["kind"] == "dispatch_action":
                result["linux_visible"] = None
        issues.extend(f"call 0x{call['id']:x}: {problem}" for problem in call["issues"])
    ordered = sorted(calls.values(), key=lambda call: call["first_line"])
    if not ordered:
        issues.append("no syscall records")
    ids = sorted(calls)
    for before, after in zip(ids, ids[1:]):
        if after != before + 1:
            issues.append(f"missing call IDs between 0x{before:x} and 0x{after:x}")
    records_complete = not issues
    workload_complete = (all(len(markers[key]) == 1 for key in markers)
                         and markers["ack"][0] < markers["done"][0] < markers["pass"][0])
    if any(markers.values()):
        if not workload_complete:
            issues.append("missing, duplicate, or out-of-order workload boundary markers")
        elif any(not markers["ack"][0] < line < markers["done"][0] for line in trace_lines):
            issues.append("trace records outside trace-link ACK/DONE boundaries")
            workload_complete = False
    else:
        issues.append("workload boundaries absent; capture completion cannot be verified")
    attribute_fds(ordered)
    return {"complete": records_complete and workload_complete,
            "call_records_complete": records_complete, "workload_complete": workload_complete,
            "paths_complete": records_complete and all(path["complete"] for call in ordered for path in call["paths"].values()),
            "markers": markers, "issues": issues, "calls": ordered,
            "syscalls": group_stats(ordered, lambda call: (call["abi"], call["nr"], call["name"])),
            "processes": group_stats(ordered, lambda call: call["pid"]),
            "fd_path_syscalls": group_stats([call for call in ordered if call["fd_paths"]],
                lambda call: (call["pid"], call["name"], call["fd_paths"][0]["fd"], call["fd_paths"][0]["path"])),
            "notes": NOTES}


def attribute_fds(calls):
    tables = defaultdict(dict)
    events = []
    fd_first = {"read", "write", "pread64", "pwrite64", "readv", "writev", "preadv", "pwritev",
                "close", "dup", "dup2", "dup3", "fcntl", "fstat", "lseek", "ftruncate", "fsync", "fdatasync", "ioctl", "getdents64"}
    for call in calls:
        call["fd_paths"] = []
        if call["entry_line"] is not None:
            events.append((call["entry_line"], False, call))
        if call["exit_line"] is not None:
            events.append((call["exit_line"], True, call))
    for _, exiting, call in sorted(events, key=lambda event: event[0]):
        if call["abi"] != "L" or None in call["args"]:
            continue
        name, args, table = call["name"], call["args"], tables[call["pid"]]
        if not exiting:
            index = 0 if name in fd_first or name == "openat" else 4 if name == "mmap" and not args[3] & 0x20 else None
            if index is not None:
                fd = signed(args[index], 32)
                call["fd_paths"].append({"arg": index, "fd": fd, "path": table.get(fd)})
            continue
        result = call["exit"]
        if not call["complete"] or result["err"] or result["raw_signed"] < 0:
            continue
        fd = result["raw_signed"]
        if name in {"open", "openat", "creat"}:
            path = call["paths"].get(1 if name == "openat" else 0)
            table[fd] = path["text"] if path and path["complete"] else None
            call["opened_fd"] = fd
        elif name == "close":
            table.pop(signed(args[0], 32), None)
        elif name in {"dup", "dup2", "dup3"} or (name == "fcntl" and args[1] in {0, 1030}):
            table[fd] = call["fd_paths"][0]["path"]
            call["opened_fd"] = fd
        elif name in {"fork", "vfork", "clone"} and fd > 0:
            call["child_pid"] = fd
            if fd not in tables:
                tables[fd] = table.copy()
        elif name in {"execve", "execveat"}:
            table.clear()


def group_stats(calls, key):
    groups = defaultdict(list)
    for call in calls:
        groups[key(call)].append(call)
    result = []
    for identity, members in groups.items():
        elapsed = sorted(call["exit"]["ns"] for call in members if call["exit"] is not None)
        result.append({"key": identity, "count": len(members),
                       "completed": len(elapsed), "errors": sum(bool(call["exit"] and call["exit"]["error"]) for call in members),
                       "total_ns": sum(elapsed), "median_ns": statistics.median(elapsed) if elapsed else None,
                       "p95_ns": elapsed[math.ceil(len(elapsed) * .95) - 1] if elapsed else None,
                       "max_ns": max(elapsed) if elapsed else None})
    return sorted(result, key=lambda row: row["total_ns"], reverse=True)


def timeline(summary):
    lines = ["Syscall dispatch timeline (raw register arguments, hexadecimal)", ""]
    for call in summary["calls"]:
        result = call["exit"]
        ending = "UNPAIRED" if result is None else (
            f"raw={result['raw_hex']} signed={result['raw_signed']} errno={result['err']} "
            f"linux_visible={result['linux_visible']} kind={result['kind']} elapsed_ns={result['ns']}")
        lines.append(f"lines={call['entry_line']}..{call['exit_line']} id=0x{call['id']:x} "
                     f"pid={call['pid']} tid={call['tid']} abi={call['abi']} "
                     f"{call['name']} nr={call['nr']}({', '.join(arg or '?' for arg in call['args_hex'])}) -> {ending}")
        for arg, path in call["paths"].items():
            lines.append(f"  path arg={arg} status={path['status']} text={json.dumps(path['text'])} hex={path['hex']}")
        if call["fd_paths"]:
            lines.append(f"  fd attribution: {json.dumps(call['fd_paths'])}")
        if "opened_fd" in call:
            lines.append(f"  opened fd={call['opened_fd']}")
        lines.extend(f"  INCOMPLETE: {problem}" for problem in call["issues"])
    return "\n".join(lines) + "\n"


def markdown(summary):
    lines = ["# Syscall trace", "", f"Complete: {summary['complete']}. "
             f"Call records complete: {summary['call_records_complete']}. "
             f"Workload complete: {summary['workload_complete']}. Paths complete: {summary['paths_complete']}.", ""]
    for title, rows in (("Syscalls", summary["syscalls"]), ("Processes", summary["processes"])):
        lines += [f"## {title}", "", "Elapsed durations are nanoseconds.", "",
                  "| Key | Calls | Exits | Errors | Total | Median | p95 | Maximum |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|"]
        for row in rows:
            label = ":".join(map(str, row["key"])) if isinstance(row["key"], tuple) else str(row["key"])
            lines.append(f"| {label} | " + " | ".join(str(row[field]) for field in
                         ("count", "completed", "errors", "total_ns", "median_ns", "p95_ns", "max_ns")) + " |")
        lines.append("")
    lines += ["## Diagnostics", ""] + ([f"- {issue}" for issue in summary["issues"]] or ["No record errors."])
    lines += ["", "## Interpretation", ""] + [f"- {note}" for note in NOTES]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        raw = args.input.read_bytes()
        summary = parse_trace(raw.decode("utf-8", errors="replace"))
        summary["input"] = {"path": str(args.input.resolve()), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        summary["syscall_mapping"] = str(MAPPINGS)
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        (args.output / "summary.md").write_text(markdown(summary))
        (args.output / "timeline.txt").write_text(timeline(summary))
    except (OSError, ValueError) as error:
        parser.exit(2, f"error: {error}\n")
    print(args.output / "summary.md")
    return 0 if summary["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
