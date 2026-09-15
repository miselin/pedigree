#!/usr/bin/env python3
"""Summarize compile-latency timing, QMP samples, IRQ counters, and disk traffic."""

import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile


def registers(text):
    """CPL identifies execution mode; a kernel address alone does not imply work."""
    for match in re.finditer(r"CPU#(\d+)\s+(.*?)(?=CPU#\d+|\Z)", text, re.S):
        body = match[2]
        fields = {key: int(value, 16) for key, value in
                  re.findall(r"\b(RIP|CR3)=([0-9a-fA-F]+)", body)}
        fields.update({key: int(value) for key, value in
                       re.findall(r"\b(CPL|HLT)=(\d+)", body)})
        if "RIP" in fields:
            yield {"cpu": int(match[1]), **fields}


def irq_counts(text):
    counts = {}
    controller = None
    for line in text.splitlines():
        match = re.fullmatch(r"IRQ statistics for (.+):", line.strip())
        if match:
            controller = match[1]
            counts[controller] = {}
        match = re.fullmatch(r"\s*(\d+):\s*(\d+)\s*", line)
        if match and controller is not None:
            counts[controller][match[1]] = int(match[2])
    return counts


def irq_delta(before, after, seconds):
    old, new = irq_counts(before), irq_counts(after)
    result = {}
    for controller, counters in new.items():
        result[controller] = {}
        for irq in old.get(controller, {}).keys() | counters.keys():
            count = counters.get(irq, 0) - old.get(controller, {}).get(irq, 0)
            if count:
                result[controller][irq] = {"count": count,
                                           "per_host_second": count / seconds if seconds else None}
    return result


def syscall_latency_buckets(metric):
    if "syscall_h0" not in metric:
        return None
    return [metric.get(f"syscall_h{index}", 0) for index in range(16)]


USER_RETURN_STAGES = (
    "interrupt_tail", "syscall_tail", "interrupt_work", "syscall_work",
    "checkpoint", "process_stop", "deferred_fault", "event",
    "interrupt_affinity", "syscall_affinity", "interrupt_accounting",
    "syscall_accounting",
)


def activity_diagnostics(metric):
    if "activity_interrupts" not in metric:
        return None
    vectors = {key[10:]: value for key, value in metric.items()
               if key.startswith("activity_v")}
    return {
        "interrupts": metric["activity_interrupts"],
        "exceptions": metric.get("activity_exceptions", 0),
        "hardware_interrupts": metric.get("activity_hardware_interrupts", 0),
        "other_interrupts": metric.get("activity_other_interrupts", 0),
        "vectors": vectors,
        "interrupt_duration_buckets": [metric.get(f"activity_irq_h{index}", 0)
                                        for index in range(16)],
        "page_fault_duration_buckets": [metric.get(f"activity_pf_h{index}", 0)
                                         for index in range(16)],
        "scheduler_timer_duration_buckets": [metric.get(f"activity_timer_h{index}", 0)
                                              for index in range(16)],
        "hard_dispatches": metric.get("activity_hard_dispatches", 0),
        "hard_duration_buckets": [metric.get(f"activity_hard_h{index}", 0)
                                   for index in range(16)],
        "threaded_dispatches": metric.get("activity_threaded_dispatches", 0),
        "threaded_duration_buckets": [metric.get(f"activity_threaded_h{index}", 0)
                                       for index in range(16)],
        "scheduler_timer_ticks": metric.get("activity_scheduler_timer_ticks", 0),
        "schedule_calls": metric.get("activity_schedule_calls", 0),
        "same_thread": metric.get("activity_same_thread", 0),
        "context_switches": metric.get("activity_context_switches", 0),
        "idle_selections": metric.get("activity_idle_selections", 0),
        "idle_fallbacks": metric.get("activity_idle_fallbacks", 0),
        "idle_fallback_current_ready": metric.get("activity_idle_fallback_ready", 0),
        "idle_fallback_current_pending": metric.get("activity_idle_fallback_pending", 0),
        "no_eligible_selections": metric.get("activity_no_eligible", 0),
        "ready_queue_scans": metric.get("activity_ready_scans", 0),
        "ready_queue_visits": metric.get("activity_ready_visits", 0),
        "ready_queue_predicate_rejects": metric.get("activity_ready_predicate_rejects", 0),
        "ready_queue_selection_samples": metric.get("activity_ready_selection_samples", 0),
        "ready_queue_selection_duration_buckets": [
            metric.get(f"activity_ready_selection_h{index}", 0) for index in range(16)],
        "time_accounting_samples": metric.get("activity_time_accounting_samples", 0),
        "time_accounting_duration_buckets": [
            metric.get(f"activity_time_accounting_h{index}", 0) for index in range(16)],
        "idle_halts": metric.get("activity_idle_halts", 0),
        "framebuffer_flips": metric.get("activity_framebuffer_flips", 0),
        "framebuffer_cells": metric.get("activity_framebuffer_cells", 0),
        "framebuffer_duration_buckets": [metric.get(f"activity_framebuffer_h{index}", 0)
                                          for index in range(16)],
        "user_return": {
            "sample_period": metric.get("activity_ur_sample_period"),
            "stages": {
                stage: {
                    "samples": metric.get(f"activity_ur_{stage}_samples", 0),
                    "total_ns": metric.get(f"activity_ur_{stage}_total_ns", 0),
                    "duration_buckets": [
                        metric.get(f"activity_ur_{stage}_h{index}", 0)
                        for index in range(16)
                    ],
                }
                for stage in USER_RETURN_STAGES
            },
            "fault_handled_samples": metric.get("activity_ur_fault_handled_samples", 0),
            "fault_fallback_samples": metric.get("activity_ur_fault_fallback_samples", 0),
            "interrupt_affinity_waited_samples": metric.get(
                "activity_ur_interrupt_affinity_waited_samples", 0),
            "syscall_affinity_waited_samples": metric.get(
                "activity_ur_syscall_affinity_waited_samples", 0),
            "ablation": {
                "mask": metric.get("benchmark_user_return_ablation", 0),
                "interrupt": {
                    "eligible": metric.get(
                        "activity_ur_interrupt_ablation_eligible", 0),
                    "fast": metric.get("activity_ur_interrupt_ablation_fast", 0),
                    "fallback": metric.get(
                        "activity_ur_interrupt_ablation_fallback", 0),
                },
                "syscall": {
                    "eligible": metric.get(
                        "activity_ur_syscall_ablation_eligible", 0),
                    "fast": metric.get("activity_ur_syscall_ablation_fast", 0),
                    "fallback": metric.get(
                        "activity_ur_syscall_ablation_fallback", 0),
                },
            },
        },
        "user_entry": {
            "sample_period": metric.get("activity_ue_sample_period"),
            "capture_calls": metric.get("activity_ue_capture_calls", 0),
            "restore_calls": metric.get("activity_ue_restore_calls", 0),
            "capture_samples": metric.get("activity_ue_capture_samples", 0),
            "restore_samples": metric.get("activity_ue_restore_samples", 0),
            "capture_tsc_total": metric.get("activity_ue_capture_tsc_total", 0),
            "restore_tsc_total": metric.get("activity_ue_restore_tsc_total", 0),
            "empty_tsc_samples": metric.get("activity_ue_empty_tsc_samples", 0),
            "empty_tsc_total": metric.get("activity_ue_empty_tsc_total", 0),
            "capture_tsc_buckets": [metric.get(f"activity_ue_capture_tsc_h{index}", 0)
                                    for index in range(16)],
            "restore_tsc_buckets": [metric.get(f"activity_ue_restore_tsc_h{index}", 0)
                                    for index in range(16)],
            "empty_tsc_buckets": [metric.get(f"activity_ue_empty_tsc_h{index}", 0)
                                  for index in range(16)],
        },
    }


def module_ranges(serial):
    result = {}
    for match in re.finditer(r"COMPILEBENCH KERNELELF: Preloaded module (.+?) "
                            r"at (?:0x)?([0-9a-fA-F]+) to (?:0x)?([0-9a-fA-F]+)", serial):
        result[match[1]] = {"base": int(match[2], 16), "end": int(match[3], 16)}
    return result


def initrd_module_map(path, observed):
    """Reproduce the x64 boot loader's allocation, then check every live anchor."""
    if len(observed) < 2:
        raise ValueError("initrd reconstruction requires at least two distinct runtime module anchors")
    modules, entries = {}, []
    # VirtualAddressSpace.h fixes this x64 base; never derive it from one sample.
    base, cursor = 0xffffffff90000000, 0
    with tarfile.open(path) as archive:
        for index, member in enumerate(archive):
            if (not member.isfile() or member.offset != cursor or
                    member.offset_data != member.offset + 512 or "/" in member.name):
                raise ValueError("initrd must contain plain, ordered module entries without extended headers")
            cursor = member.offset_data + ((member.size + 511) & ~511)
            if member.name.startswith("._"):
                continue
            data = archive.extractfile(member).read()
            if (data[:6] != b"\x7fELF\x02\x01" or
                    struct.unpack_from("<HH", data, 16) != (3, 62)):
                raise ValueError(f"module must be x86_64 little-endian ELF DYN: {member.name}")
            phoff, shoff = struct.unpack_from("<QQ", data, 32)
            phsize, phnum, shsize, shnum = struct.unpack_from("<HHHH", data, 54)
            if phsize != 56 or shsize != 64:
                raise ValueError("unsupported ELF header entry size")
            segments = [struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize)
                        for i in range(phnum)]
            # Elf::loadModule sums these ends, rather than taking their maximum.
            size = (sum(p[3] + p[6] for p in segments if p[0] == 1) + 4095) & ~4095
            if not size:
                raise ValueError("module has no PT_LOAD allocation")

            def file_offset(address):
                for p in segments:
                    if p[0] == 1 and p[3] <= address < p[3] + p[5]:
                        return p[2] + address - p[3]
                raise ValueError("module name is outside file-backed PT_LOAD segments")

            sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shsize)
                        for i in range(shnum)]
            names = set()
            for section in sections:
                if section[1] not in (2, 11):
                    continue
                strings = sections[section[6]]
                table = data[strings[4]:strings[4] + strings[5]]
                if section[9] != 24:
                    raise ValueError("unsupported ELF symbol entry size")
                for i in range(section[5] // section[9]):
                    symbol = struct.unpack_from("<IBBHQQ", data, section[4] + i * section[9])
                    if table[symbol[0]:].split(b"\0", 1)[0] == b"g_pModuleName" and symbol[3]:
                        pointer = struct.unpack_from("<Q", data, file_offset(symbol[4]))[0]
                        name = data[file_offset(pointer):].split(b"\0", 1)[0].decode()
                        names.add(name)
            if len(names) != 1:
                raise ValueError(f"module name cannot be resolved unambiguously: {member.name}")
            name = names.pop()
            if not name or "\n" in name or name in modules:
                raise ValueError("invalid or duplicate module name")
            modules[name] = {"base": base, "end": base + size}
            entries.append({"index": index, "archive_path": member.name,
                            "module_name_from_elf": name, "allocation_size": size,
                            "sha256": hashlib.sha256(data).hexdigest()})
            base += size
    for name, region in observed.items():
        if modules.get(name) != region:
            raise ValueError(f"initrd allocation conflicts with runtime anchor: {name}")
    return {"source": "inferred x64 initrd layout; every retained runtime anchor verified",
            "initrd": str(path.resolve()), "initrd_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "allocation_base": "0xffffffff90000000",
            "allocation_rule": "archive order; round4096(sum(PT_LOAD.vaddr+memsz)); ELF g_pModuleName",
            "verified_anchors": observed, "archive_entries": entries, "modules": modules}


class Symbols:
    def __init__(self, path, nm):
        self.path = path.resolve(strict=True)
        data = self.path.read_bytes()
        if data[:4] != b"\x7fELF" or data[5] not in (1, 2):
            raise ValueError(f"not an ELF file: {path}")
        self.elf_type = struct.unpack(("<" if data[5] == 1 else ">") + "H", data[16:18])[0]
        self.sha256 = hashlib.sha256(data).hexdigest()
        self.addresses = []
        self.entries = {}
        self.dwarf = {}
        self.executable_ranges = []
        if data[:6] == b"\x7fELF\x02\x01":
            shoff = struct.unpack_from("<Q", data, 40)[0]
            shsize, shnum = struct.unpack_from("<HH", data, 58)
            for index in range(shnum):
                section = struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * shsize)
                if section[2] & 4 and section[5]:
                    self.executable_ranges.append((section[3], section[3] + section[5]))
        if self.elf_type not in (2, 3):
            return
        output = subprocess.check_output(
            [nm, "--numeric-sort", "--demangle", "--print-size", str(self.path)], text=True)
        for line in output.splitlines():
            match = re.fullmatch(r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([TtWw])\s+(.+)", line)
            if not match:
                continue
            address, size = int(match[1], 16), int(match[2], 16)
            if address not in self.entries or size > self.entries[address][0]:
                self.entries[address] = (size, match[4])
        self.addresses = sorted(self.entries)

    def lookup(self, address):
        index = bisect_right(self.addresses, address) - 1
        if index < 0:
            return self.dwarf.get(address)
        start = self.addresses[index]
        size, name = self.entries[start]
        if address < start + size or address == start:
            return name
        if not size and index + 1 < len(self.addresses) and address < self.addresses[index + 1]:
            return name + " (zero-size symbol; next-symbol bound)"
        return self.dwarf.get(address)

    def resolve_dwarf(self, addresses, addr2line):
        if not addresses or not addr2line or self.elf_type not in (2, 3):
            return
        ordered = sorted(addresses)
        result = subprocess.run([addr2line, "-f", "-C", "-e", str(self.path)], check=True,
                                input="".join(f"0x{value:x}\n" for value in ordered),
                                text=True, capture_output=True).stdout.splitlines()
        if len(result) != 2 * len(ordered):
            raise ValueError("unexpected addr2line response length")
        for index, address in enumerate(ordered):
            name, location = result[2 * index:2 * index + 2]
            if name != "??" and not location.startswith("??"):
                self.dwarf[address] = name + " [DWARF]"

    def provenance(self):
        return {"path": str(self.path), "sha256": self.sha256, "elf_type": self.elf_type,
                "symbol_count": len(self.addresses),
                "dwarf_addresses": len(self.dwarf),
                "usable": self.elf_type in (2, 3)}


def classify_stack(ips, symbol_tables, ranges):
    """Separate a seeded thread root and stop before unverifiable caller addresses."""
    result = {"symbols": [], "omitted_callers": 0, "invalid_callers": 0,
              "unverified_callers": 0, "synthetic_roots": 0, "stop_reason": "complete"}
    for index, raw in enumerate(ips):
        name, relative = "kernel", raw
        for module, region in ranges.items():
            if region["base"] <= raw < region["end"]:
                name, relative = module, raw - region["base"]
                break
        table = symbol_tables.get(name)
        entry = table.entries.get(relative) if table else None
        if index and entry and entry[1].startswith("Thread::threadExited("):
            result["symbols"].append("[synthetic thread root: Thread::threadExited]")
            result.update(synthetic_roots=1, omitted_callers=len(ips) - index - 1, stop_reason="thread-root")
            break
        relative -= index > 0
        if index and (table is None or not any(low <= relative < high for low, high in table.executable_ranges)):
            result["unverified_callers" if table is None else "invalid_callers"] = 1
            result.update(omitted_callers=len(ips) - index, stop_reason="unverified-or-nonexecutable-caller")
            break
        symbol = table.lookup(relative) if table else None
        result["symbols"].append(f"{name}:{symbol}" if symbol else f"{name}+0x{relative:x} (unsymbolized)")
    return result


def sample_summary(path, symbolize, top, stack_classifier=None):
    phases = defaultdict(lambda: {"snapshots": 0, "cpu_samples": 0,
                                  "states": Counter(), "per_cpu": defaultdict(Counter),
                                  "kernel_running_symbols": Counter(),
                                  "kernel_halted_symbols": Counter(), "user_pcs": Counter(),
                                  "stack_samples": 0, "stacks_with_callers": 0,
                                  "stack_stop_reasons": Counter(), "stack_inclusive_symbols": Counter(),
                                  "stack_chains": Counter(),
                                  "stack_omitted_callers": 0, "stack_invalid_callers": 0,
                                  "stack_unverified_callers": 0, "stack_synthetic_roots": 0,
                                  "stack_classification_stops": Counter(),
                                  "paused_snapshots": 0, "paused_wall_s": 0.0,
                                  "query_wall_s": 0.0, "first_wall_s": None, "last_wall_s": None})
    if not path.exists():
        return {}
    with path.open() as stream:
        for line in stream:
            if not line.endswith("\n"):
                break  # The active runner may not have finished this record yet.
            sample = json.loads(line)
            phase = phases[sample["phase"]]
            phase["snapshots"] += 1
            phase["query_wall_s"] += sample.get("query_wall_s", 0)
            phase["paused_snapshots"] += bool(sample.get("paused"))
            phase["paused_wall_s"] += sample.get("paused_wall_s", 0)
            stamp = sample.get("phase_wall_s")
            if phase["first_wall_s"] is None:
                phase["first_wall_s"] = stamp
            phase["last_wall_s"] = stamp
            for cpu in registers(sample["registers"]):
                phase["cpu_samples"] += 1
                mode = "kernel" if cpu.get("CPL") == 0 else "user" if cpu.get("CPL") == 3 else "unknown"
                state = "halted" if cpu.get("HLT") else mode + "_running"
                phase["states"][state] += 1
                phase["per_cpu"][str(cpu["cpu"])][state] += 1
                if mode == "kernel":
                    counter = "kernel_halted_symbols" if cpu.get("HLT") else "kernel_running_symbols"
                    phase[counter][symbolize(cpu["RIP"])] += 1
                elif mode == "user":
                    # Different processes may place unrelated functions at the same address.
                    phase["user_pcs"][f"cr3={cpu.get('CR3', 0):x} rip={cpu['RIP']:x}"] += 1
            for stack in sample.get("stacks", []):
                phase["stack_samples"] += 1
                phase["stack_stop_reasons"][stack["stop_reason"]] += 1
                if stack_classifier:
                    classified = stack_classifier(stack["ips"])
                    symbols = classified["symbols"]
                    for key in ("omitted_callers", "invalid_callers", "unverified_callers", "synthetic_roots"):
                        phase["stack_" + key] += classified[key]
                    phase["stack_classification_stops"][classified["stop_reason"]] += 1
                else:
                    symbols = [symbolize(ip - (index > 0)) for index, ip in enumerate(stack["ips"])]
                real_symbols = [name for name in symbols if not name.startswith("[synthetic thread root:")]
                phase["stacks_with_callers"] += len(real_symbols) > 1
                phase["stack_inclusive_symbols"].update(set(real_symbols))
                phase["stack_chains"][" <- ".join(symbols)] += 1
    for phase in phases.values():
        total = phase["cpu_samples"]
        phase["state_percent"] = {key: 100 * count / total for key, count in phase["states"].items()}
        for key in ("kernel_running_symbols", "kernel_halted_symbols", "user_pcs"):
            phase[key] = [{"symbol" if key != "user_pcs" else "address": name, "count": count,
                           "percent_all_cpu_samples": 100 * count / total}
                          for name, count in phase[key].most_common(top)]
        for key in ("stack_inclusive_symbols", "stack_chains"):
            phase[key] = [{"symbol" if key == "stack_inclusive_symbols" else "chain": name,
                           "count": count, "percent_stack_samples": 100 * count / phase["stack_samples"]}
                          for name, count in phase[key].most_common(top)]
    return dict(phases)


def summarize(args):
    directory = args.directory.resolve(strict=True)
    report_path = directory / "report.json"
    report = json.loads(report_path.read_text()) if report_path.exists() else {"result": "RUNNING", "phases": []}
    serial_path = directory / "serial.log"
    serial = serial_path.read_text(errors="replace") if serial_path.exists() else ""
    ranges = module_ranges(serial)
    map_provenance = None
    if args.module_map or args.initrd:
        manifest = (initrd_module_map(args.initrd, ranges) if args.initrd else
                    json.loads(args.module_map.read_text()))
        mapped = manifest["modules"]
        for name, observed in ranges.items():
            if name not in mapped or mapped[name] != observed:
                raise ValueError(f"module map conflicts with observed module: {name}")
        regions = sorted((value["base"], value["end"]) for value in mapped.values())
        for index, (base, end) in enumerate(regions):
            if base < 0xffff800000000000 or end <= base or (index and base < regions[index - 1][1]):
                raise ValueError("module map contains invalid or overlapping ranges")
        map_provenance = {key: value for key, value in manifest.items() if key != "modules"}
        map_provenance["validated_observed_modules"] = len(ranges)
        if args.module_map:
            map_provenance["path"] = str(args.module_map.resolve())
        if args.write_module_map:
            args.write_module_map.write_text(json.dumps(manifest, indent=2) + "\n")
        ranges = mapped
    symbol_tables = {}
    if args.kernel:
        symbol_tables["kernel"] = Symbols(args.kernel, args.nm)
    for item in args.module:
        name, path = item.split("=", 1)
        symbol_tables[name] = Symbols(Path(path), args.nm)
    if args.module_dir:
        available = list(args.module_dir.rglob("*.debug")) + list(args.module_dir.rglob("*.o"))
        archive_names = {entry["module_name_from_elf"]: Path(entry["archive_path"]).stem
                         for entry in (map_provenance or {}).get("archive_entries", [])}
        for name in ranges:
            filename = archive_names.get(name, name)
            matches = [path for path in available if path.name in (filename + ".debug", filename + ".o.debug")]
            if not matches:
                matches = [path for path in available if path.name == filename + ".o"]
            if name not in symbol_tables and len(matches) == 1:
                symbol_tables[name] = Symbols(matches[0], args.nm)

    def target(address):
        for name, region in ranges.items():
            if region["base"] <= address < region["end"]:
                return name, address - region["base"]
        return "kernel", address

    samples_path = directory / "samples.jsonl"
    if args.addr2line and samples_path.exists():
        unresolved = defaultdict(set)
        with samples_path.open() as samples:
            for line in samples:
                if not line.endswith("\n"):
                    break
                sample = json.loads(line)
                addresses = [cpu["RIP"] for cpu in registers(sample["registers"]) if cpu.get("CPL") == 0]
                addresses += [ip - (index > 0) for stack in sample.get("stacks", [])
                              for index, ip in enumerate(stack["ips"])]
                for address in addresses:
                    name, relative = target(address)
                    if name in symbol_tables and not symbol_tables[name].lookup(relative):
                        unresolved[name].add(relative)
        for name, addresses in unresolved.items():
            symbol_tables[name].resolve_dwarf(addresses, args.addr2line)

    def symbolize(address):
        name, relative = target(address)
        table = symbol_tables.get(name)
        symbol = table.lookup(relative) if table else None
        return f"{name}:{symbol}" if symbol else f"{name}+0x{relative:x} (unsymbolized)"

    profiles = sample_summary(directory / "samples.jsonl", symbolize, args.top,
                              lambda ips: classify_stack(ips, symbol_tables, ranges))
    phases = {}
    for phase in report["phases"]:
        name = phase["phase"]
        metric = phase.get("metric", {})
        seconds = phase.get("host_wall_s", 0)
        disk = {}
        for device, counters in phase.get("block_delta", {}).items():
            if not any(counters.values()):
                continue
            disk[device] = {**counters, "read_mib": counters.get("rd_bytes", 0) / 1048576,
                            "write_mib": counters.get("wr_bytes", 0) / 1048576}
        phases[name] = {"host_wall_s": seconds, "guest_wall_s": metric.get("total_us", 0) / 1e6,
                        "user_s": metric.get("user_us", 0) / 1e6,
                        "system_s": metric.get("system_us", 0) / 1e6,
                        "syscalls": metric.get("syscalls"),
                        "syscall_latency_buckets": syscall_latency_buckets(metric),
                        "activity": activity_diagnostics(metric),
                        "rc": metric.get("rc"), "disk": disk,
                        "irq": irq_delta(phase.get("irq_before", ""), phase.get("irq_after", ""), seconds)}
    if not report_path.exists():
        for match in re.finditer(r"COMPILEBENCH metric phase=(\S+) ([^\n]+)", serial):
            metric = {key: int(value) for key, value in
                      (item.split("=", 1) for item in match[2].split())}
            phases[match[1]] = {"guest_wall_s": metric["total_us"] / 1e6,
                                "user_s": metric["user_us"] / 1e6,
                                "system_s": metric["system_us"] / 1e6,
                                "syscalls": metric.get("syscalls"),
                                "syscall_latency_buckets": syscall_latency_buckets(metric),
                                "activity": activity_diagnostics(metric),
                                "rc": metric["rc"]}
    for name, profile in profiles.items():
        phases.setdefault(name, {})["profile"] = profile
    return {"directory": str(directory), "result": report["result"], "error": report.get("error"),
            "cpus": report.get("cpus"), "phases": phases, "module_ranges": ranges,
            "module_map_provenance": map_provenance,
            "symbol_files": {name: table.provenance() for name, table in symbol_tables.items()},
            "notes": ["Samples are QMP observations, not an exact accounting of CPU time; sampling can perturb timing.",
                      "HLT is reported separately from running kernel samples; per-CPU samples are the denominator.",
                      "IRQ controller counts are reported separately and must not be summed across controllers.",
                      "Activity interrupt durations cover the C++ interrupt dispatch boundary and can include time suspended by a context switch; hard and threaded buckets cover their respective callback scopes.",
                      "Block service times can overlap and do not directly measure guest blocked time.",
                      "Zero-size assembly symbols use the next symbol as a labeled inferred bound; sized symbols use their exact ranges.",
                      "ET_REL files require unavailable section relocations and remain unsymbolized.",
                      "Unpaused RBP reads can race with scheduling; paused snapshots retain pause duration. Interrupt frames may truncate either walk.",
                      "The seeded Thread::threadExited return is a synthetic root; nonexecutable or unverified callers are omitted and counted.",
                      "Inclusive stack counts include each symbol once per attempted stack and are separate from leaf sample percentages.",
                      "Debug files must match the booted image; their hashes identify the files supplied here."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--module", action="append", default=[], metavar="NAME=SYMBOL_FILE")
    parser.add_argument("--module-dir", type=Path, help="Find unambiguous module .debug or .o files beneath this directory")
    parser.add_argument("--module-map", type=Path,
                        help="JSON manifest with modules mapping names to base/end; checked against serial anchors")
    parser.add_argument("--initrd", type=Path,
                        help="Reconstruct x64 module allocation from the matching frozen raw or gzipped initrd")
    parser.add_argument("--write-module-map", type=Path, help="Save the validated reconstructed module manifest")
    parser.add_argument("--nm", default=shutil.which("llvm-nm") or shutil.which("nm") or "nm")
    bundled_addr2line = Path(__file__).resolve().parents[2] / "compilers/dir/bin/x86_64-pedigree-addr2line"
    parser.add_argument("--addr2line", default=shutil.which("llvm-addr2line") or shutil.which("addr2line") or
                        (str(bundled_addr2line) if bundled_addr2line.exists() else None),
                        help="DWARF fallback for functions omitted from nm; empty string disables it")
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--output", type=Path, help="Write full JSON summary")
    args = parser.parse_args()
    if args.top < 1 or any("=" not in item for item in args.module):
        parser.error("top must be positive and modules must use NAME=SYMBOL_FILE")
    if args.initrd and args.module_map:
        parser.error("choose initrd reconstruction or an existing module map")
    if args.write_module_map and not args.initrd:
        parser.error("write-module-map requires initrd")
    result = summarize(args)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"{result['result']} {result['directory']}")
    if result["error"]:
        print(result["error"])
    for name, phase in result["phases"].items():
        profile = phase.get("profile", {})
        timing = " ".join(f"{key}={phase[key]:.3f}" for key in
                          ("host_wall_s", "guest_wall_s", "user_s", "system_s") if key in phase)
        print(f"{name}: {timing} rc={phase.get('rc', '?')} samples={profile.get('cpu_samples', 0)}")
        if profile:
            print("  " + " ".join(f"{state}={percent:.1f}%" for state, percent in profile["state_percent"].items()))
            for item in profile["kernel_running_symbols"][:5]:
                print(f"  {item['count']:5d} {item['percent_all_cpu_samples']:5.1f}% {item['symbol']}")
            if profile["stack_samples"]:
                print(f"  stacks: {profile['stacks_with_callers']}/{profile['stack_samples']} with callers; "
                      f"paused_snapshots={profile['paused_snapshots']} paused_s={profile['paused_wall_s']:.3f}")
                for item in profile["stack_inclusive_symbols"][:5]:
                    print(f"  {item['count']:5d} {item['percent_stack_samples']:5.1f}% inclusive {item['symbol']}")
        for device, counters in phase.get("disk", {}).items():
            print(f"  {device}: read={counters['read_mib']:.3f}MiB/{counters.get('rd_operations', 0)}ops "
                  f"write={counters['write_mib']:.3f}MiB/{counters.get('wr_operations', 0)}ops")
        for controller, counters in phase.get("irq", {}).items():
            rates = " ".join(f"irq{irq}={count['per_host_second']:.1f}/s"
                             for irq, count in sorted(counters.items(), key=lambda x: int(x[0]))
                             if count["per_host_second"] is not None)
            if rates:
                print(f"  {controller}: {rates}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
