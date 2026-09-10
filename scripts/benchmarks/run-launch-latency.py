#!/usr/bin/env python3
"""Measure isolated executable launches and reads in a disposable QEMU guest."""

import argparse
from collections import Counter
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import socket
import subprocess
import time

SPEC = importlib.util.spec_from_file_location(
    "io_latency", Path(__file__).with_name("run-io-latency.py"))
IO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IO)
EVENTS = ("process_ncq_command", "execute_ncq_command_read",
          "execute_ncq_command_write", "ncq_finish", "qmp_enter_query_blockstats")
KERNEL_LOG = re.compile(rb"\([A-Z]{2}\) \[\d+\.\d+\]")


def serial_lines(wire, data):
    wire.extend(data)
    if KERNEL_LOG.search(wire):
        raise ValueError("kernel serial logging is enabled; use --disable-log-to-serial "
                         "as a separate kernel command-line token without a trailing newline")
    if len(wire) > 1024 * 1024:
        raise ValueError("unterminated serial record exceeds limit")
    lines = []
    while b"\n" in wire:
        raw, _, rest = wire.partition(b"\n")
        wire[:] = rest
        lines.append(raw.decode(errors="replace").strip())
    return lines


def trace_phases(trace, phases):
    """Use QMP trace markers, rather than buffered file offsets, as boundaries."""
    query = -1
    active = set()
    results = []
    current = None
    for line in trace.splitlines():
        if "qmp_enter_query_blockstats " in line:
            query += 1
            if query % 2 == 0:
                if len(results) >= len(phases):
                    raise ValueError("extra block-stat trace boundary")
                current = {"phase": phases[len(results)], "maximum_ncq": len(active),
                           "ncq_submissions": 0, "ncq_completions": 0,
                           "active_at_start": len(active), "read_sizes": Counter(),
                           "read_commands": 0, "write_commands": 0}
            else:
                current["active_at_end"] = len(active)
                current["clean_boundaries"] = not (
                    current["active_at_start"] or current["active_at_end"])
                current["read_sizes"] = dict(current["read_sizes"])
                results.append(current)
                current = None
            continue
        match = re.search(
            r"(process_ncq_command|ncq_finish) ahci\(([^)]*)\)\[(\d+)\]\[tag:(\d+)\]", line)
        if match:
            key = tuple(match.groups()[1:])
            if match[1] == "process_ncq_command":
                if key in active:
                    raise ValueError("NCQ tag reused before completion")
                active.add(key)
                if current is not None:
                    current["ncq_submissions"] += 1
                    current["maximum_ncq"] = max(current["maximum_ncq"], len(active))
            else:
                if key not in active:
                    raise ValueError("NCQ completion without submission")
                active.remove(key)
                if current is not None:
                    current["ncq_completions"] += 1
        if current is not None:
            if "execute_ncq_command_read " in line:
                current["read_commands"] += 1
                count = re.search(r"NCQ reading (\d+) sectors", line)
                if not count:
                    raise ValueError("unrecognized NCQ read size")
                current["read_sizes"][str(int(count[1]) * 512)] += 1
            elif "execute_ncq_command_write " in line:
                current["write_commands"] += 1
    if query + 1 != len(phases) * 2 or current is not None:
        raise ValueError("missing block-stat trace boundaries")
    return results


def block_delta(before, after):
    fields = ("rd_bytes", "rd_operations", "rd_total_time_ns", "wr_bytes",
              "wr_operations", "wr_total_time_ns", "flush_operations",
              "flush_total_time_ns")
    old = {item["device"]: item["stats"] for item in before}
    return {item["device"]: {field: item["stats"].get(field, 0) -
                            old[item["device"]].get(field, 0) for field in fields}
            for item in after if item["device"] in old}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--firmware-code", type=Path, required=True)
    parser.add_argument("--firmware-vars", type=Path)
    parser.add_argument("--cpus", type=int, choices=(1, 4), default=4)
    parser.add_argument("--mode", choices=("launch", "read-sequential", "read-permuted",
                                          "mmap-sequential", "mmap-permuted", "sync"),
                        default="launch")
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--prewarm", action="store_true")
    parser.add_argument("--no-trace", action="store_true",
                        help="Timing control without QEMU NCQ trace logging")
    parser.add_argument("--write-iops", type=int, default=0,
                        help="Limit backing writes for queue-depth qualification; 0 disables throttling")
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--qemu-img", default="qemu-img")
    args = parser.parse_args()
    if args.write_iops < 0:
        parser.error("write-iops must be nonnegative")
    if not 1 <= args.iterations <= 100 or args.timeout <= 0:
        parser.error("iterations must be 1..100 and timeout positive")
    if args.mode == "sync" and (args.iterations != 1 or args.prewarm):
        parser.error("the sync fixture requires --iterations 1 and no prewarm")
    expected = ["prewarm"] if args.prewarm else []
    groups = (("launch", "fork", "exec") if args.mode == "launch" else
              ("sync-dirty", "sync-clean", "sync-redirty") if args.mode == "sync" else (args.mode,))
    expected += [f"{group}-{i}" for group in groups for i in range(args.iterations)]
    image = args.image.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"result": "FAIL", "image": str(image), "cpus": args.cpus,
              "trace_enabled": not args.no_trace, "write_iops": args.write_iops,
              "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "expected_phases": expected, "phases": []}
    process = guest = serial = None
    try:
        report["qemu_version"] = subprocess.check_output(
            [args.qemu, "--version"], text=True).splitlines()[0]
        info = json.loads(subprocess.check_output(
            [args.qemu_img, "info", "--output=json", str(image)], text=True))
        subprocess.run([args.qemu_img, "create", "-f", "qcow2", "-F", info["format"],
                        "-b", str(image), str(output / "disk.qcow2")],
                       check=True, capture_output=True)
        shutil.copyfile(args.firmware_code, output / "firmware-code.fd")
        firmware = f"if=pflash,format=raw,file={output}/firmware-code.fd"
        if args.firmware_vars:
            firmware += ",readonly=on"
        command = [args.qemu, "-machine", "q35", "-accel", "tcg,thread=multi",
                   "-smp", str(args.cpus), "-m", "4096", "-cpu",
                   "SandyBridge,-rdrand,-rdseed", "-drive", firmware]
        if args.firmware_vars:
            shutil.copyfile(args.firmware_vars, output / "firmware-vars.fd")
            command += ["-drive", f"if=pflash,format=raw,file={output}/firmware-vars.fd"]
        (output / "events.txt").write_text("\n".join(EVENTS) + "\n")
        serial_path = output / "serial.sock"
        if len(os.fsencode(serial_path)) >= 100:
            raise ValueError("output path is too long for a portable Unix socket")
        throttle = f",iops_wr={args.write_iops}" if args.write_iops else ""
        command += ["-drive", f"file={output}/disk.qcow2,if=ide,format=qcow2{throttle}",
                    "-display", "none", "-chardev",
                    f"socket,id=bench,path={serial_path},server=on,wait=off",
                    "-serial", "chardev:bench", "-qmp", "stdio", "-nic", "none",
                    "-no-reboot", "-no-shutdown", "-S"]
        if not args.no_trace:
            command += ["-trace", f"events={output}/events.txt,file={output}/ahci.trace"]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (output / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=log, bufsize=0)
        guest = IO.Guest(process, output)
        guest.receive()
        guest.qmp("qmp_capabilities")
        serial = socket.socket(socket.AF_UNIX)
        serial.connect(str(serial_path))
        serial.setblocking(False)
        guest.qmp("cont")
        deadline = time.monotonic() + args.timeout
        wire = bytearray()
        current = None
        passed = False
        with (output / "serial.log").open("wb") as log:
            while not passed:
                if time.monotonic() >= deadline:
                    raise TimeoutError("guest benchmark deadline")
                ready, _, _ = select.select([serial], [], [], min(1, deadline - time.monotonic()))
                if not ready:
                    if process.poll() is not None:
                        raise RuntimeError("QEMU exited before benchmark completion")
                    continue
                data = serial.recv(65536)
                if not data:
                    raise RuntimeError("serial closed")
                log.write(data)
                log.flush()
                for line in serial_lines(wire, data):
                    if "LAUNCHBENCH FAIL" in line:
                        raise RuntimeError(line)
                    match = re.search(r"LAUNCHBENCH READY phase=(\S+)", line)
                    if match:
                        index = len(report["phases"])
                        if current is not None or index >= len(expected) or match[1] != expected[index]:
                            raise RuntimeError(f"unexpected phase: {line}")
                        current = {"phase": match[1], "blocks_before": guest.qmp("query-blockstats")}
                        serial.sendall(b"g")
                    match = re.search(
                        r"LAUNCHBENCH metric phase=(\S+) first_us=(\d+) total_us=(\d+) "
                        r"bytes=(\d+) checksum=(\d+)", line)
                    if match:
                        if current is None or match[1] != current["phase"] or "metric" in current:
                            raise RuntimeError(f"unexpected metric: {line}")
                        current["metric"] = dict(zip(
                            ("first_us", "total_us", "bytes", "checksum"),
                            (int(value) for value in match.groups()[1:])))
                        current["bytes_kind"] = ("synced_file_span" if args.mode == "sync" else
                                                 "mapped_file_span" if match[1].startswith("mmap-")
                                                 else "transferred_bytes")
                        if not (0 <= current["metric"]["first_us"] <= current["metric"]["total_us"]):
                            raise RuntimeError("invalid guest timing interval")
                    match = re.search(r"LAUNCHBENCH DONE phase=(\S+)", line)
                    if match:
                        if current is None or match[1] != current["phase"] or "metric" not in current:
                            raise RuntimeError(f"incomplete phase: {line}")
                        current["blocks_after"] = guest.qmp("query-blockstats")
                        current["block_delta"] = block_delta(current["blocks_before"], current["blocks_after"])
                        report["phases"].append(current)
                        print(json.dumps({"phase": current["phase"], **current["metric"]}), flush=True)
                        current = None
                    if "LAUNCHBENCH PASS END" in line:
                        if current is not None or len(report["phases"]) != len(expected):
                            raise RuntimeError("premature success marker")
                        passed = True
        report["result"] = "PASS"
    except Exception as error:
        report["error"] = repr(error)
        if guest:
            try:
                guest.qmp("stop")
                report["registers"] = guest.qmp(
                    "human-monitor-command", {"command-line": "info registers"})
            except Exception as capture_error:
                report["capture_error"] = repr(capture_error)
    finally:
        if guest and process.poll() is None:
            try:
                guest.qmp("quit")
            except Exception:
                process.terminate()
        if process:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if serial:
            serial.close()
        if report["result"] == "PASS" and not args.no_trace:
            try:
                traces = trace_phases((output / "ahci.trace").read_text(), expected)
                for phase, trace in zip(report["phases"], traces):
                    phase["trace"] = trace
            except Exception as error:
                report["result"] = "FAIL"
                report["trace_error"] = repr(error)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "phases"}))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
