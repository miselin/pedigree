#!/usr/bin/env python3
"""Report machine-wide instruction dispatch counts for one bracketed guest workload."""

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile


SPEC = importlib.util.spec_from_file_location("qemu_trace_summary", Path(__file__).with_name("summarize.py"))
TRACE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRACE)
HEADER = "# pedigree-qemu-profile-v1"
SCOPE = (
    "All machine activity inside one workload bracket: compiler descendants, the waiting parent, "
    "interrupt handlers, and background kernel work. Counts are instruction dispatches, not retired "
    "instructions, cycles, or elapsed time. Aggregation preserves no chronology or call graph."
)
SYSCALL_NAMES = {**TRACE.COMPILE.LINUX_AMD64_SYSCALL_NAMES, 32: "dup", 33: "dup2", 90: "chmod",
                 95: "umask", 99: "sysinfo", 100: "times", 102: "getuid", 104: "getgid",
                 107: "geteuid", 108: "getegid"}
# The sampled syscall table's overflow bucket is not a raw syscall number.
SYSCALL_NAMES.pop(512, None)


def positive(value):
    result = TRACE.number(value, 10)
    if not result:
        raise ValueError("count must be positive")
    return result


def records(lines):
    header = begun = ended = False
    kernel_total, user_total = 0, None
    instructions, syscalls, disruptions = set(), set(), set()
    for line_number, line in enumerate(lines, 1):
        try:
            if not line.endswith("\n"):
                raise ValueError("unterminated record")
            line = line.rstrip("\r\n")
            if line == HEADER:
                if header or begun:
                    raise ValueError("duplicate or late format header")
                header = True
                continue
            if not line or line.startswith("#"):
                continue
            if not header:
                raise ValueError("missing format header")
            fields = line.split("\t", 4)
            kind = fields[0]
            expected = {"B": 4, "P": 5, "U": 2, "S": 3, "D": 5, "E": 4}.get(kind)
            if expected is None or len(fields) != expected:
                raise ValueError("unknown record or unexpected field count")
            if ended:
                raise ValueError("record after terminal E")
            event = {"kind": kind}
            if kind == "B":
                if begun:
                    raise ValueError("duplicate begin record")
                event.update(zip(("start_pc", "cr3", "rsp"), map(TRACE.number, fields[1:])))
                begun = True
            elif not begun:
                raise ValueError("event outside the workload bracket")
            elif kind == "P":
                event.update(pc=TRACE.number(fields[1]), count=positive(fields[2]),
                             bytes=fields[3].lower(), disassembly=fields[4])
                if event["pc"] < 0xffff800000000000:
                    raise ValueError("P record is not a high-half kernel address")
                if not re.fullmatch(r"(?:[0-9a-f]{2}){1,15}", event["bytes"]):
                    raise ValueError("invalid x86 instruction bytes")
                key = (event["pc"], event["bytes"])
                if key in instructions:
                    raise ValueError("duplicate PC and instruction bytes")
                instructions.add(key)
                kernel_total += event["count"]
            elif kind == "U":
                if user_total is not None:
                    raise ValueError("duplicate user total")
                user_total = TRACE.number(fields[1], 10)
                event["count"] = user_total
            elif kind == "S":
                event.update(number=TRACE.number(fields[1], 10), count=positive(fields[2]))
                if event["number"] in syscalls:
                    raise ValueError("duplicate syscall number")
                syscalls.add(event["number"])
            elif kind == "D":
                event.update(type=TRACE.number(fields[1], 10), from_pc=TRACE.number(fields[2]),
                             to_pc=TRACE.number(fields[3]), count=positive(fields[4]))
                if not 0 < event["type"] < 8:
                    raise ValueError("invalid discontinuity type")
                key = (event["type"], event["from_pc"], event["to_pc"])
                if key in disruptions:
                    raise ValueError("duplicate discontinuity aggregate")
                disruptions.add(key)
            else:
                event.update(status=fields[1], kernel_dispatches=TRACE.number(fields[2], 10),
                             user_dispatches=TRACE.number(fields[3], 10))
                if event["status"] not in ("complete", "exit"):
                    raise ValueError("unknown terminal status")
                if user_total is None:
                    raise ValueError("missing user total")
                if event["kernel_dispatches"] != kernel_total or event["user_dispatches"] != user_total:
                    raise ValueError("terminal totals differ from P/U sums")
                ended = True
            yield event
        except (ValueError, IndexError) as error:
            raise ValueError(f"line {line_number}: {error}") from error
    if not header or not begun or not ended:
        raise ValueError("missing header or complete B/E bracket")


def summarize(profile, symbolize, verify=None):
    functions, specials, notifications = Counter(), Counter(), Counter()
    byte_counts, byte_dispatches = Counter(), Counter()
    rows, syscalls, disruptions = [], [], []
    begin = end = error = None
    user_total = None
    try:
        with profile.open() as source:
            for event in records(source):
                kind = event["kind"]
                if kind == "B":
                    begin = {key: f"0x{value:x}" for key, value in event.items() if key != "kind"}
                elif kind == "P":
                    image, symbol = symbolize(event["pc"])
                    result = verify(event) if verify else "unmapped"
                    if result not in ("matched", "mismatched", "unmapped"):
                        raise ValueError("unknown byte-validation result")
                    byte_counts[result] += 1
                    byte_dispatches[result] += event["count"]
                    functions[(image, symbol)] += event["count"]
                    for instruction in TRACE.special_instructions(event["disassembly"]):
                        specials[instruction] += event["count"]
                    rows.append({"pc": f"0x{event['pc']:x}", "image": image, "symbol": symbol,
                                 "bytes": event["bytes"], "disassembly": event["disassembly"],
                                 "dispatches": event["count"], "byte_validation": result})
                elif kind == "U":
                    user_total = event["count"]
                elif kind == "S":
                    syscalls.append({"number": event["number"], "name": SYSCALL_NAMES.get(event["number"]),
                                     "calls": event["count"]})
                elif kind == "D":
                    for bit, label in ((1, "interrupt_notification"), (2, "exception_notification"),
                                       (4, "hostcall_notification")):
                        if event["type"] & bit:
                            notifications[label] += event["count"]
                    from_image, from_symbol = symbolize(event["from_pc"])
                    to_image, to_symbol = symbolize(event["to_pc"])
                    disruptions.append({"type": event["type"], "from_pc": f"0x{event['from_pc']:x}",
                                        "to_pc": f"0x{event['to_pc']:x}", "count": event["count"],
                                        "from_symbol": f"{from_image}:{from_symbol}",
                                        "to_symbol": f"{to_image}:{to_symbol}"})
                else:
                    end = event
    except ValueError as failure:
        error = str(failure)
    format_valid = error is None
    if byte_counts["mismatched"]:
        error = (error + "; " if error else "") + "observed instruction bytes differ from supplied ELF"
    verdict = "fail" if byte_counts["mismatched"] else (
        "pass" if byte_counts["matched"] and not byte_counts["unmapped"] else
        "partial" if byte_counts["matched"] else "unavailable")
    return {"schema_version": 1, "event": "InsnDispatch", "scope": SCOPE,
            "validation": {"valid": error is None, "format_valid": format_valid,
                           "complete": error is None and end is not None and end["status"] == "complete",
                           "error": error}, "begin": begin, "end": end,
            "kernel_dispatches": sum(functions.values()), "user_dispatches": user_total,
            "byte_validation": {"verdict": verdict,
                                "unit": "distinct kernel PC and observed instruction bytes",
                                **{key: byte_counts[key] for key in ("matched", "mismatched", "unmapped")},
                                "dispatches_by_result": dict(byte_dispatches),
                                "scope": "observed kernel instructions only; user code is not recorded or byte-checked",
                                "mismatches": [row for row in rows if row["byte_validation"] == "mismatched"][:30]},
            "functions": [{"image": image, "symbol": symbol, "dispatches": count}
                          for (image, symbol), count in functions.most_common()],
            "instructions": sorted(rows, key=lambda row: (-row["dispatches"], row["pc"])),
            "syscalls": sorted(syscalls, key=lambda row: (-row["calls"], row["number"])),
            "syscall_calls": sum(row["calls"] for row in syscalls),
            "special_instructions": dict(specials), "notification_counts": dict(notifications),
            "discontinuities": sorted(disruptions, key=lambda row: -row["count"])}


def write_reports(report, output):
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    validation, checked = report["validation"], report["byte_validation"]
    lines = ["# Whole-workload QEMU profile", "",
             f"Validation: {'valid' if validation['valid'] else 'INVALID'}; "
             f"capture: {'complete' if validation['complete'] else 'INCOMPLETE'}.", "", report["scope"], "",
             f"Kernel dispatches: **{report['kernel_dispatches']:,}**. "
             f"User dispatches: **{report['user_dispatches'] if report['user_dispatches'] is not None else 'missing'}**.", "",
             f"Observed kernel bytes: **{checked['verdict']}**; {checked['matched']} matched, "
             f"{checked['mismatched']} mismatched, {checked['unmapped']} unmapped distinct PC/byte pairs.",
             checked["scope"] + ".", "",
             "Module mapping: " + report["symbols"]["module_mapping"]["status"] + ". "
             + report["symbols"]["module_mapping"]["reason"] + ".", "",
             "Interrupt notifications may include software interrupts. Their totals are not hardware IRQ counts. "
             "The aggregate format records only the starting CR3; it cannot count address-space changes.", ""]
    if validation["error"]:
        lines += ["Validation error: " + validation["error"], ""]
    lines += ["| Kernel image / symbol | Dispatches | Kernel share |", "| --- | ---: | ---: |"]
    for function in report["functions"][:40]:
        name = (function["image"] + ":" + function["symbol"]).replace("|", "\\|")
        share = 100 * function["dispatches"] / max(report["kernel_dispatches"], 1)
        lines.append(f"| {name} | {function['dispatches']:,} | {share:.2f}% |")
    lines += ["", "| Linux amd64 syscall number / name | Calls |", "| --- | ---: |"]
    for syscall in report["syscalls"]:
        lines.append(f"| {syscall['number']} / {syscall['name'] or 'unknown'} | {syscall['calls']:,} |")
    lines += ["", "| Special kernel instruction | Dispatches |", "| --- | ---: |"]
    for instruction, count in sorted(report["special_instructions"].items(), key=lambda item: -item[1]):
        lines.append(f"| {instruction} | {count:,} |")
    lines += ["", "Notification counts: `" + json.dumps(report["notification_counts"]) + "`.", "",
              "`symbolized-profile.tsv` and `callgrind.out` contain flat kernel instruction counts. "
              "User dispatches are retained only as a total and are excluded from that symbolized view.", ""]
    (output / "summary.md").write_text("\n".join(lines))
    with (output / "symbolized-profile.tsv").open("w") as stream:
        stream.write("pc\tdispatches\tbytes\tdisassembly\timage\tsymbol\tbyte_validation\n")
        for row in report["instructions"]:
            stream.write("\t".join(str(row[key]).replace("\t", " ") for key in
                                   ("pc", "dispatches", "bytes", "disassembly", "image", "symbol", "byte_validation")) + "\n")
    with (output / "callgrind.out").open("w") as stream:
        stream.write("# callgrind format\nversion: 1\ncreator: pedigree-qemu-profile\n"
                     f"desc: valid={validation['valid']} complete={validation['complete']}; flat machine-wide kernel dispatches\n"
                     "positions: instr\nevents: InsnDispatch\n"
                     f"summary: {report['kernel_dispatches']}\n")
        for row in report["instructions"]:
            stream.write(f"ob={row['image']}\nfl=unknown\nfn={row['symbol']}\n{row['pc']} {row['dispatches']}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", "--trace", dest="profile", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--kernel-code", type=Path, help="frozen executable when --kernel is separate debug ELF")
    parser.add_argument("--initrd", type=Path, required=True)
    parser.add_argument("--serial", type=Path, help="runtime module anchors from the captured boot")
    parser.add_argument("--module-map", type=Path, help="existing summarize-compile module manifest")
    parser.add_argument("--nm", default=shutil.which("llvm-nm") or "nm")
    parser.add_argument("--addr2line", help="source lookup for unsized symbols; defaults to nm's toolchain")
    parser.add_argument("--output", type=Path, required=True, help="new report directory")
    args = parser.parse_args()
    args.user = None
    try:
        with tempfile.TemporaryDirectory(prefix="pedigree-profile-symbols-") as temporary:
            symbols = TRACE.Symbolizer(args, Path(temporary))
            with args.profile.open() as source:
                symbols.resolve_unsized_sources((e["pc"] for e in records(source) if e["kind"] == "P"),
                                                args.nm, args.addr2line)
            args.output.mkdir(parents=True, exist_ok=False)
            report = summarize(args.profile, symbols.lookup, symbols.verify)
            symbols.provenance["runtime_identity"] = (
                "Observed kernel instruction bytes are checked; whole-image runtime identity remains unverified"
            )
            report.update(profile=str(args.profile.resolve()),
                          profile_sha256=hashlib.sha256(args.profile.read_bytes()).hexdigest(),
                          symbols=symbols.provenance)
            write_reports(report, args.output)
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"profile summary failed: {error}\n")
    print(args.output / "summary.md")
    return 0 if report["validation"]["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
