#!/usr/bin/env python3
"""Symbolize a bounded QEMU instruction-dispatch trace without inventing call stacks."""

import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile


SPEC = importlib.util.spec_from_file_location(
    "summarize_compile", Path(__file__).resolve().parents[1] / "summarize-compile.py"
)
COMPILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPILE)
HEADER = "# pedigree-qemu-trace-v1"
SPECIAL = {
    "cli", "sti", "pushf", "pushfd", "pushfq", "popf", "popfd", "popfq",
    "rdmsr", "wrmsr", "swapgs", "iret", "iretd", "iretq", "sysret", "sysretq",
    "syscall", "sysenter", "sysexit", "int", "int3", "cpuid", "invlpg", "invpcid",
    "rdtsc", "rdtscp", "hlt", "call", "callq", "callw", "ret", "retq", "retw", "retf", "iretw",
}


def number(value, base=16):
    if not re.fullmatch(r"(?:0x)?[0-9a-fA-F]+" if base == 16 else r"[0-9]+", value):
        raise ValueError(f"invalid base-{base} integer: {value!r}")
    result = int(value, base)
    if result >= 1 << 64:
        raise ValueError("integer exceeds 64 bits")
    return result


def records(lines):
    """Validate event order while streaming; instruction rows need not fit in RAM."""
    traces, header, footer = {}, False, None
    for line_number, line in enumerate(lines, 1):
        try:
            if not line.endswith("\n"):
                raise ValueError("unterminated record")
            line = line.rstrip("\r\n")
            if line == HEADER:
                if header or traces:
                    raise ValueError("duplicate or late format header")
                header = True
                continue
            if line.startswith("# finished "):
                match = re.fullmatch(r"# finished traces=(\d+) requested=(\d+) matching_starts=(\d+)", line)
                if not match or footer:
                    raise ValueError("invalid or duplicate capture-count footer")
                footer = dict(zip(("traces", "requested", "matching_starts"), map(int, match.groups())))
                if footer["traces"] != len(traces) or footer["requested"] != len(traces):
                    raise ValueError("footer capture count differs from observed or requested captures")
                yield {"kind": "F", **footer}
                continue
            if not line or line.startswith("#"):
                continue
            if footer:
                raise ValueError("record after capture-count footer")
            if not header:
                raise ValueError("missing format header")
            fields = line.split("\t", 8)
            kind, trace_id = fields[:2]
            if not re.fullmatch(r"[A-Za-z0-9_.:-]+", trace_id):
                raise ValueError("invalid trace ID")
            event = {"kind": kind, "trace_id": trace_id}
            expected = {"B": 6, "I": 9, "D": 6, "E": 6}.get(kind)
            if expected is None or len(fields) != expected:
                raise ValueError(f"unexpected {kind!r} field count")
            if kind == "B":
                if trace_id in traces:
                    raise ValueError("duplicate trace ID")
                event.update(zip(("start_pc", "return_pc", "start_rsp", "start_cr3"),
                                 map(number, fields[2:])))
                traces[trace_id] = {"count": 0, "ended": False, **event}
            else:
                state = traces.get(trace_id)
                if state is None or state["ended"]:
                    raise ValueError("event outside an active trace")
                if kind == "I":
                    seq = number(fields[2], 10)
                    if seq != state["count"] + 1:
                        raise ValueError("instruction sequence is not contiguous")
                    event.update(seq=seq)
                    event.update(zip(("pc", "rsp", "cr3", "rflags"),
                                     map(number, fields[3:7])))
                    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}){1,15}", fields[7]):
                        raise ValueError("invalid x86 instruction bytes")
                    event.update(bytes=fields[7].lower(), disassembly=fields[8])
                    if seq == 1 and event["pc"] != state["start_pc"]:
                        raise ValueError("first instruction differs from start PC")
                    state["count"] = seq
                elif kind == "D":
                    event.update(seq=number(fields[2], 10), type=number(fields[3], 10),
                                 from_pc=number(fields[4]), to_pc=number(fields[5]))
                    if event["seq"] != state["count"] or not 0 < event["type"] < 8:
                        raise ValueError("invalid discontinuity sequence or type")
                else:
                    event.update(status=fields[2], count=number(fields[3], 10),
                                 arrival_pc=number(fields[4]), retval=number(fields[5]))
                    if event["status"] not in ("complete", "limit", "exit"):
                        raise ValueError("unknown terminal status")
                    if event["count"] != state["count"]:
                        raise ValueError("terminal count differs from instruction rows")
                    if event["status"] == "complete" and event["arrival_pc"] != state["return_pc"]:
                        raise ValueError("complete trace did not arrive at the return PC")
                    state["ended"] = True
            yield event
        except (ValueError, IndexError) as error:
            raise ValueError(f"line {line_number}: {error}") from error
    if not header or not traces:
        raise ValueError("trace contains no header or no captures")
    if any(not trace["ended"] for trace in traces.values()):
        raise ValueError("trace ended without a terminal E record")
    if footer is None:
        raise ValueError("missing capture-count footer")


def special_instructions(disassembly):
    words = disassembly.lower().strip().split()
    if not words:
        return []
    result = []
    while words and words[0] in ("lock", "rep", "repe", "repz", "repne", "repnz", "bnd"):
        result.append(words.pop(0) + "_prefix")
    if words and words[0] in SPECIAL:
        result.append(words[0])
    if words and words[0].startswith("mov") and re.search(r"\bcr3\b", disassembly.lower()):
        result.append("mov_cr3")
    return result


class Symbolizer:
    def __init__(self, args, temporary):
        self.tables, self.byte_ranges, self.byte_provenance = {}, {}, {}
        self.source_overrides = {}
        self.unsized_ends = {}
        self.add_table("kernel", args.kernel, args.nm, getattr(args, "kernel_code", None))
        self.ranges = {}
        self.mapping = {"status": "unavailable", "reason": "no module map or two runtime anchors"}
        observed = COMPILE.module_ranges(args.serial.read_text(errors="replace")) if args.serial else {}
        if args.module_map:
            manifest = json.loads(args.module_map.read_text())
        elif len(observed) >= 2:
            manifest = COMPILE.initrd_module_map(args.initrd, observed)
        else:
            manifest = None
        initrd_hash = hashlib.sha256(args.initrd.read_bytes()).hexdigest()
        self.mapping["initrd_sha256"] = initrd_hash
        if manifest:
            if manifest.get("initrd_sha256") != initrd_hash:
                raise ValueError("module map does not identify the supplied initrd SHA256")
            mapped = manifest["modules"]
            for name, region in observed.items():
                if mapped.get(name) != region:
                    raise ValueError(f"module map conflicts with runtime anchor: {name}")
            regions = sorted((v["base"], v["end"]) for v in mapped.values())
            for index, (base, end) in enumerate(regions):
                if base < 0xffff800000000000 or end <= base or (index and base < regions[index - 1][1]):
                    raise ValueError("invalid or overlapping module ranges")
            self.ranges = mapped
            self.mapping = {"status": "anchored" if len(observed) >= 2 else "unverified",
                            "fresh_anchors": observed, "initrd_sha256": initrd_hash,
                            "source": manifest.get("source"),
                            "reason": "runtime layout checked against supplied serial anchors"
                            if len(observed) >= 2 else "retained layout lacks two current runtime anchors"}
            with tarfile.open(args.initrd) as archive:
                for index, entry in enumerate(manifest.get("archive_entries", [])):
                    name = entry["module_name_from_elf"]
                    if name not in mapped:
                        raise ValueError(f"archive entry absent from module map: {name}")
                    member = archive.getmember(entry["archive_path"])
                    if not member.isfile():
                        raise ValueError("module archive entry is not a regular file")
                    data = archive.extractfile(member).read()
                    if hashlib.sha256(data).hexdigest() != entry["sha256"]:
                        raise ValueError(f"module hash mismatch: {name}")
                    path = temporary / f"module-{index}.elf"
                    path.write_bytes(data)
                    self.add_table(name, path, args.nm)
        if args.user:
            self.add_table("user", args.user, args.nm)
            if self.tables["user"].elf_type != 2:
                raise ValueError("--user requires ET_EXEC; PIE runtime load bias is unavailable")
        self.provenance = {name: table.provenance() for name, table in self.tables.items()}
        for name in self.provenance:
            self.provenance[name]["instruction_bytes"] = self.byte_provenance[name]
        for name in self.ranges:
            if name in self.provenance:
                self.provenance[name]["path"] = f"{args.initrd.resolve()}::{name}"
                self.provenance[name]["instruction_bytes"]["path"] = f"{args.initrd.resolve()}::{name}"
        self.provenance["module_mapping"] = self.mapping
        self.provenance["runtime_identity"] = "supplied ELF identities are not verified against runtime bytes"

    def add_table(self, name, path, nm, code=None):
        self.tables[name] = COMPILE.Symbols(path, nm)
        table = self.tables[name]
        output = subprocess.check_output([nm, "--numeric-sort", "--demangle", "--print-size", str(path)], text=True)
        for line in output.splitlines():
            match = re.fullmatch(r"([0-9a-fA-F]+)\s+([TtWw])\s+(.+)", line)
            if match:
                table.entries.setdefault(int(match[1], 16), (0, match[3]))
        table.addresses = sorted(table.entries)
        byte_path = code or path
        data = byte_path.read_bytes()
        self.byte_provenance[name] = {"path": str(byte_path.resolve()), "sha256": hashlib.sha256(data).hexdigest()}
        sections = []
        executable_ranges = []
        if code and data[:6] != b"\x7fELF\x02\x01":
            raise ValueError("--kernel-code must be x86-64 little-endian ELF")
        if data[:6] == b"\x7fELF\x02\x01":
            offset = struct.unpack_from("<Q", data, 40)[0]
            stride, count = struct.unpack_from("<HH", data, 58)
            for index in range(count):
                section = struct.unpack_from("<IIQQQQIIQQ", data, offset + index * stride)
                if section[2] & 4 and section[5]:
                    executable_ranges.append((section[3], section[3] + section[5]))
                if section[2] & 4 and section[1] != 8 and section[5]:
                    start, size = section[4], section[5]
                    if start + size > len(data):
                        raise ValueError(f"executable section outside supplied ELF: {name}")
                    sections.append((section[3], section[3] + size, data[start:start + size]))
        if code and sorted(executable_ranges) != sorted(table.executable_ranges):
            raise ValueError("kernel code and symbol ELF executable address ranges differ")
        self.byte_ranges[name] = sections

    def location(self, pc):
        for name, region in self.ranges.items():
            if region["base"] <= pc < region["end"]:
                return name, pc - region["base"]
        for name in ("kernel", "user"):
            table = self.tables.get(name)
            if table and any(low <= pc < high for low, high in table.executable_ranges):
                return name, pc
        return "unknown", pc

    def lookup(self, pc):
        name, relative = self.location(pc)
        override = getattr(self, "source_overrides", {}).get((name, relative))
        if override:
            return name, override[0]
        table = self.tables.get(name)
        symbol = table.lookup(relative) if table else None
        executable_end = next((high for low, high in table.executable_ranges if low <= relative < high), None) if table else None
        if table and executable_end is not None:
            index = bisect_right(table.addresses, relative) - 1
            if index >= 0:
                address = table.addresses[index]
                size, name_at_address = table.entries[address]
                following = table.addresses[index + 1] if index + 1 < len(table.addresses) else executable_end
                if not size:
                    boundary = getattr(self, "unsized_ends", {}).get((name, address), executable_end)
                    symbol = (name_at_address + " (zero-size symbol; next-symbol/section bound)"
                              if relative < min(following, executable_end, boundary) else None)
        elif table:
            symbol = None
        suffix = " [layout unverified]" if name in self.ranges and self.mapping["status"] != "anchored" else ""
        return name, (symbol or f"0x{relative:x} (unsymbolized)") + suffix

    def resolve_unsized_sources(self, addresses, nm, addr2line=None):
        """Do not let a size-less assembly label absorb following anonymous C++ code."""
        tool = addr2line or shutil.which(nm[:-2] + "addr2line" if nm.endswith("nm") else "addr2line")
        if not tool:
            self.provenance["unsized_source_check"] = "unavailable: no addr2line"
            return
        candidates = {}
        for pc in addresses:
            image, relative = self.location(pc)
            table = self.tables.get(image)
            if not table:
                continue
            index = bisect_right(table.addresses, relative) - 1
            if index < 0 or not table.entries[table.addresses[index]][0] or not table.lookup(relative):
                candidates.setdefault(image, set()).add(relative)
        for image, pending in candidates.items():
            ordered = sorted(pending)
            result = subprocess.run([tool, "-f", "-C", "-e", str(self.tables[image].path)],
                                    input="".join(f"0x{pc:x}\n" for pc in ordered),
                                    capture_output=True, text=True, check=True).stdout.splitlines()
            if len(result) != 2 * len(ordered):
                raise ValueError("unexpected addr2line response length")
            for index, pc in enumerate(ordered):
                location = result[2 * index + 1]
                match = re.match(r"(.+\.(?:c|cc|cpp|cxx|h|hpp)):(\d+)", location)
                if not match or int(match[2]) == 0:
                    continue
                # addr2line can itself reuse the preceding assembly function name.
                # A verified source location is safer than that inferred name.
                label = Path(match[1]).name + " [DWARF source; unresolved function]"
                self.source_overrides[(image, pc)] = (label, location)
                table = self.tables[image]
                preceding = bisect_right(table.addresses, pc) - 1
                if preceding >= 0 and not table.entries[table.addresses[preceding]][0]:
                    key = image, table.addresses[preceding]
                    self.unsized_ends[key] = min(pc, self.unsized_ends.get(key, pc))
        self.provenance["unsized_source_check"] = {
            "tool": tool, "queried": sum(map(len, candidates.values())),
            "source_overrides": {f"{image}:0x{pc:x}": location
                                 for (image, pc), (_, location) in self.source_overrides.items()},
        }

    def verify(self, event):
        name, relative = self.location(event["pc"])
        observed = bytes.fromhex(event["bytes"])
        for low, high, data in self.byte_ranges.get(name, []):
            if low <= relative and relative + len(observed) <= high:
                expected = data[relative - low:relative - low + len(observed)]
                return "matched" if observed == expected else "mismatched"
        return "unmapped"


def summarize(trace, output, symbolize, verify=None):
    functions, instructions, specials, disruptions = Counter(), Counter(), Counter(), Counter()
    traces, error, byte_checks, footer = {}, None, {}, None
    with (output / "symbolized-trace.txt").open("w") as rendered:
        rendered.write("Instruction dispatches, not cycles or retired instructions. No inferred call graph.\n")
        try:
            with trace.open() as source:
                for event in records(source):
                    if event["kind"] == "F":
                        footer = event
                        rendered.write(json.dumps(event) + "\n")
                        continue
                    kind, trace_id = event["kind"], event["trace_id"]
                    if kind == "B":
                        traces[trace_id] = {**event, "instructions": 0, "discontinuities": 0,
                                            "cr3_changes": 0, "last_cr3": event["start_cr3"]}
                    elif kind == "I":
                        capture = traces[trace_id]
                        capture["instructions"] += 1
                        changed = event["cr3"] != capture["last_cr3"]
                        capture["cr3_changes"] += changed
                        capture["last_cr3"] = event["cr3"]
                        image, symbol = symbolize(event["pc"])
                        byte_key = (event["pc"], event["bytes"])
                        if byte_key not in byte_checks:
                            byte_checks[byte_key] = verify(event) if verify else "unmapped"
                        functions[(image, symbol)] += 1
                        instructions[(event["pc"], image, symbol, event["bytes"], event["disassembly"])] += 1
                        specials.update(special_instructions(event["disassembly"]))
                        flags = " CR3-CHANGED" if changed else ""
                        rendered.write(f"{trace_id}:{event['seq']} 0x{event['pc']:016x} "
                                       f"rsp=0x{event['rsp']:x} cr3=0x{event['cr3']:x} "
                                       f"flags=0x{event['rflags']:x} {event['bytes']} "
                                       f"{event['disassembly']}  {image}:{symbol}{flags}\n")
                        continue
                    elif kind == "D":
                        traces[trace_id]["discontinuities"] += 1
                        for bit, label in ((1, "interrupt_notification"), (2, "exception_notification"),
                                           (4, "hostcall_notification")):
                            if event["type"] & bit:
                                disruptions[label] += 1
                    else:
                        traces[trace_id].update(event)
                    rendered.write(json.dumps(event) + "\n")
        except ValueError as failure:
            error = str(failure)
    captures = list(traces.values())
    for capture in captures:
        capture.pop("last_cr3", None)
        capture["uninterrupted"] = not (capture["discontinuities"] or capture["cr3_changes"])
        for key in ("start_pc", "return_pc", "start_rsp", "start_cr3", "arrival_pc", "retval"):
            if key in capture:
                capture[key] = f"0x{capture[key]:x}"
    byte_counts = Counter(byte_checks.values())
    mismatch = byte_counts["mismatched"] > 0
    if mismatch:
        error = (error + "; " if error else "") + "observed instruction bytes differ from supplied ELF"
    valid = error is None
    complete = valid and all(c.get("status") == "complete" for c in captures)
    return {"schema_version": 1, "event": "InsnDispatch", "validation": {
        "valid": valid, "complete": complete, "error": error}, "captures": captures,
        "instruction_dispatches": sum(functions.values()), "footer": footer,
        "byte_validation": {
            "unit": "distinct PC and observed instruction bytes",
            "verdict": "fail" if mismatch else "pass" if byte_counts["matched"] and not byte_counts["unmapped"]
            else "partial" if byte_counts["matched"] else "unavailable",
            "matched": byte_counts["matched"], "mismatched": byte_counts["mismatched"],
            "unmapped": byte_counts["unmapped"],
            "mismatches": [{"pc": f"0x{pc:x}", "bytes": code}
                           for (pc, code), status in byte_checks.items() if status == "mismatched"][:30],
            "scope": "exact bytes at observed executable-section addresses; not whole-image identity or retirement",
        },
        "special_instructions": dict(specials), "discontinuities": dict(disruptions),
        "functions": [{"image": image, "symbol": symbol, "dispatches": count}
                      for (image, symbol), count in functions.most_common()],
        "instructions": [{"pc": f"0x{pc:x}", "image": image, "symbol": symbol,
                          "bytes": code, "disassembly": asm, "dispatches": count}
                         for (pc, image, symbol, code, asm), count in instructions.most_common()]}


def write_reports(report, output):
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    validation = report["validation"]
    lines = ["# QEMU instruction trace", "", f"Validation: {'valid' if validation['valid'] else 'INVALID'}; "
             f"capture: {'complete' if validation['complete'] else 'INCOMPLETE'}.", "",
             f"Instruction dispatches: {report['instruction_dispatches']:,}.", "",
             "Counts describe observed dispatches, not cycles, elapsed time, or instruction retirement. "
             "Interrupt handlers and other address spaces remain included and are flagged in raw flow. "
             "Interrupt notifications can include software entry; they are not a hardware IRQ count. "
             "RFLAGS may omit lazy condition-code bits; IF is retained. "
             "No call graph is inferred across interrupts, tail calls, or assembly returns.", "",
             "Module mapping: " + report["symbols"]["module_mapping"]["status"] + ". "
             + report["symbols"]["module_mapping"]["reason"] + ".", "",
             report["symbols"]["runtime_identity"] + ".", ""]
    checked = report["byte_validation"]
    lines += [f"Observed bytes: **{checked['verdict']}**; {checked['matched']} matched, "
              f"{checked['mismatched']} mismatched, {checked['unmapped']} unmapped distinct PC/byte pairs.",
              checked["scope"] + ".", ""]
    if validation["error"]:
        lines += ["Validation error: " + validation["error"], ""]
    lines += ["| Capture | Status | Instructions | Discontinuities | CR3 changes |",
              "| --- | --- | ---: | ---: | ---: |"]
    for capture in report["captures"]:
        lines.append(f"| {capture['trace_id']} | {capture.get('status', 'missing end')} | "
                     f"{capture['instructions']} | {capture['discontinuities']} | {capture['cr3_changes']} |")
    lines += ["", "| Image / symbol | Dispatches |", "| --- | ---: |"]
    for function in report["functions"][:30]:
        label = (function["image"] + ":" + function["symbol"]).replace("|", "\\|")
        lines.append(f"| {label} | {function['dispatches']} |")
    lines += ["", "Special instruction counts: `" + json.dumps(report["special_instructions"]) + "`.", ""]
    (output / "summary.md").write_text("\n".join(lines))
    with (output / "callgrind.out").open("w") as stream:
        stream.write("# callgrind format\nversion: 1\ncreator: pedigree-qemu-trace\n"
                     f"desc: valid={validation['valid']} complete={validation['complete']}; flat dispatch counts only\n"
                     "positions: instr\nevents: InsnDispatch\n"
                     f"summary: {report['instruction_dispatches']}\n")
        for row in report["instructions"]:
            stream.write(f"ob={row['image']}\nfl=unknown\nfn={row['symbol']}\n"
                         f"{row['pc']} {row['dispatches']}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--kernel-code", type=Path, help="frozen executable ELF for bytes when --kernel is a separate debug ELF")
    parser.add_argument("--initrd", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new report directory")
    parser.add_argument("--nm", default=shutil.which("llvm-nm") or "nm")
    parser.add_argument("--addr2line", help="source lookup for unsized symbols; defaults to nm's toolchain")
    parser.add_argument("--user", type=Path, help="optional non-PIE user ELF at linked addresses")
    parser.add_argument("--serial", type=Path, help="runtime module anchors from this capture's boot")
    parser.add_argument("--module-map", type=Path, help="existing summarize-compile module manifest")
    args = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix="pedigree-trace-symbols-") as temporary:
            symbols = Symbolizer(args, Path(temporary))
            with args.trace.open() as source:
                symbols.resolve_unsized_sources((e["pc"] for e in records(source) if e["kind"] == "I"),
                                                args.nm, args.addr2line)
            args.output.mkdir(parents=True, exist_ok=False)
            report = summarize(args.trace, args.output, symbols.lookup, symbols.verify)
            symbols.provenance["runtime_identity"] = (
                "Observed instruction bytes are checked separately below; whole-image runtime identity remains unverified"
            )
            report.update(trace=str(args.trace.resolve()),
                          trace_sha256=hashlib.sha256(args.trace.read_bytes()).hexdigest(),
                          symbols=symbols.provenance)
            write_reports(report, args.output)
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"trace summary failed: {error}\n")
    print(args.output / "summary.md")
    return 0 if report["validation"]["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
