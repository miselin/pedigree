#!/usr/bin/env python3
"""Time the which.cc compilation and controls in a disposable QEMU guest."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import re
import select
import shutil
import subprocess
import time

SPEC = importlib.util.spec_from_file_location(
    "launch_latency", Path(__file__).with_name("run-launch-latency.py"))
LAUNCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCH)
PHASES = ["idle", "cpu", "compile-exact", "compile-warm-1", "compile-warm-2",
          "preprocess", "codegen", "assemble", "link", "run", "sync",
          "anon-1mib", "anon-4mib", "anon-16mib", "anon-64mib", "anon-contract"]


def public_phase_state(current):
    return {key: value for key, value in current.items()
            if not key.endswith("_monotonic")}


def write_report(output, report, *, result=None, current=None):
    snapshot = {**report}
    if result is not None:
        snapshot["result"] = result
    if current is not None:
        snapshot["incomplete_phase"] = public_phase_state(current)
    temporary = output / "report.json.tmp"
    temporary.write_text(json.dumps(snapshot, indent=2) + "\n")
    temporary.replace(output / "report.json")


def validate_phase_event(current, phase, event):
    if current is None:
        raise RuntimeError(f"{event} without active phase: {phase}")
    if phase != current["phase"]:
        raise RuntimeError(
            f"{event} phase {phase} does not match active phase {current['phase']}")
    if event == "ACK":
        if current["gate_acknowledged"]:
            raise RuntimeError(f"duplicate ACK for phase: {phase}")
        return
    if not current["gate_acknowledged"]:
        raise RuntimeError(f"{event} before ACK for phase: {phase}")
    if event == "metric" and "metric" in current:
        raise RuntimeError(f"duplicate metric for phase: {phase}")
    if event == "DONE" and "metric" not in current:
        raise RuntimeError(f"DONE before metric for phase: {phase}")


def open_serial_fifo(output):
    """Create a QEMU pipe chardev and return host input/output descriptors."""
    base = output / "serial"
    input_path = Path(f"{base}.in")
    output_path = Path(f"{base}.out")
    input_fd = output_fd = None
    try:
        os.mkfifo(input_path)
        os.mkfifo(output_path)
        # O_RDWR keeps both sides of each FIFO open while QEMU starts. QEMU's
        # pipe backend opens serial.in for reading and serial.out for writing;
        # opening both ends here prevents either open from blocking or failing
        # with ENXIO.
        input_fd = os.open(input_path, os.O_RDWR | os.O_NONBLOCK)
        output_fd = os.open(output_path, os.O_RDWR | os.O_NONBLOCK)
        return base, input_fd, output_fd
    except Exception:
        close_serial_fifo(base, input_fd, output_fd)
        raise


def close_serial_fifo(base, input_fd, output_fd):
    """Close FIFO descriptors and remove the transient endpoints."""
    for descriptor in (input_fd, output_fd):
        if descriptor is not None:
            os.close(descriptor)
    if base is not None:
        for suffix in (".in", ".out"):
            Path(f"{base}{suffix}").unlink(missing_ok=True)


def sample_stacks(guest, register_text, paused=False, max_frames=8):
    """Inspect bounded frame chains; the caller controls whether CPUs are paused."""
    stacks = []
    for match in re.finditer(r"CPU#(\d+)\s+(.*?)(?=CPU#\d+|\Z)", register_text, re.S):
        fields = {key: int(value, 16) for key, value in
                  re.findall(r"\b(RIP|RBP|RSP|CR3)=([0-9a-fA-F]+)", match[2])}
        state = dict(re.findall(r"\b(CPL|HLT)=(\d+)", match[2]))
        if state.get("CPL") != "0" or state.get("HLT") != "0" or "RIP" not in fields:
            continue
        cpu = int(match[1])
        stack = {"cpu": cpu, "registers": fields, "ips": [fields["RIP"]],
                 "frame_pointers": [], "stop_reason": "depth-limit", "asynchronous": not paused}
        stacks.append(stack)
        frame, floor = fields.get("RBP", 0), fields.get("RSP", 0)
        if not 0xffff800000000000 <= floor <= 0xfffffffffffffff0:
            stack["stop_reason"] = "non-kernel-stack-pointer"
            continue
        for _ in range(max_frames - 1):
            if (not 0xffff800000000000 <= frame <= 0xfffffffffffffff0 or frame % 8 or
                    not floor <= frame <= floor + 65536):
                stack["stop_reason"] = "frame-outside-bounds"
                break
            try:
                response = guest.qmp("human-monitor-command", {
                    "command-line": f"x/2gx 0x{frame:x}", "cpu-index": cpu})
                parsed = re.search(r"(?:0x)?([0-9a-fA-F]+):\s+0x([0-9a-fA-F]+)\s+"
                                   r"0x([0-9a-fA-F]+)", response)
                if not parsed or int(parsed[1], 16) != frame:
                    stack["stop_reason"] = "memory-read-parse-error"
                    stack["error_response"] = response
                    break
            except Exception as error:
                stack["stop_reason"] = "memory-read-error"
                stack["error"] = repr(error)
                break
            next_frame, return_ip = int(parsed[2], 16), int(parsed[3], 16)
            if not 0xffff800000000000 <= return_ip <= 0xffffffffffffffff:
                stack["stop_reason"] = "non-kernel-return-address"
                break
            stack["frame_pointers"].append(frame)
            stack["ips"].append(return_ip)
            if next_frame <= frame:
                stack["stop_reason"] = "non-increasing-frame"
                break
            frame = next_frame
    return stacks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--firmware-code", type=Path, required=True)
    parser.add_argument("--firmware-vars", type=Path)
    parser.add_argument("--cpus", type=int, choices=(1, 4), default=1)
    parser.add_argument("--linked", action="store_true",
                        help="Image contains /root/compile-bench/link-cxx; start with -lstdc++")
    parser.add_argument("--quick", action="store_true",
                        help="Image contains quick-run and link-cxx; two compiles plus controls")
    parser.add_argument("--skip-sync", action="store_true",
                        help="Image contains no-sync; performance-only run with writes disabled")
    parser.add_argument("--synthetic-vm", action="store_true",
                        help="Run only the image's synthetic VM syscall workload")
    parser.add_argument("--verify-persisted", action="store_true",
                        help="Require persisted-output verification and execution, without compilation")
    parser.add_argument("--reuse-overlay", type=Path,
                        help="Reboot an existing disposable qcow2; requires --verify-persisted and new output directory")
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--sample-interval", type=float, default=0,
                        help="Seconds between register samples; 0 disables sampling")
    parser.add_argument("--sample-jitter", action="store_true",
                        help="Vary sampling intervals from 0.5x to 1.5x with fixed seed 0")
    parser.add_argument("--sample-stacks", action="store_true",
                        help="Read best-effort kernel RBP chains while CPUs run; perturbs timing")
    parser.add_argument("--paused-samples", action="store_true",
                        help="Pause all CPUs for coherent 24-frame samples; requires sample-stacks")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--plugin", action="append", default=[],
                        help="QEMU TCG plugin specification (repeatable; instrumentation changes timing)")
    parser.add_argument("--qemu-img", default="qemu-img")
    args = parser.parse_args()
    if args.timeout <= 0 or args.sample_interval < 0:
        parser.error("timeout must be positive and sample-interval nonnegative")
    if args.sample_stacks and not args.sample_interval:
        parser.error("sample-stacks requires a positive sample-interval")
    if args.sample_jitter and not args.sample_interval:
        parser.error("sample-jitter requires a positive sample-interval")
    if args.paused_samples and (not args.sample_stacks or not args.sample_interval):
        parser.error("paused-samples requires sample-stacks and a positive sample-interval")
    if args.quick and args.verify_persisted:
        parser.error("quick and verify-persisted are separate runs")
    if args.skip_sync and args.verify_persisted:
        parser.error("skip-sync cannot verify persistence")
    if args.synthetic_vm and (args.quick or args.linked or args.skip_sync or args.verify_persisted):
        parser.error("synthetic-vm cannot be combined with compile workload options")
    if args.reuse_overlay and not args.verify_persisted:
        parser.error("reuse-overlay requires verify-persisted")
    if args.quick:
        args.linked = True
    sample_random = random.Random(0)

    def sample_delay():
        if not args.sample_interval:
            return float("inf")
        return args.sample_interval * (sample_random.uniform(0.5, 1.5) if args.sample_jitter else 1)

    image = args.image.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    expected = PHASES.copy()
    if args.synthetic_vm:
        expected = ["vm-synthetic"]
    if args.linked:
        expected[expected.index("compile-exact")] = "compile-cold"
    if args.quick:
        expected = ["idle", "cpu", "compile-cold", "compile-warm-1", "run", "sync", "anon-contract"]
    if args.verify_persisted:
        expected = ["verify-persisted", "run-persisted"]
    if args.skip_sync:
        expected.remove("sync")
    disk = args.reuse_overlay.resolve(strict=True) if args.reuse_overlay else output / "disk.qcow2"
    report = {"result": "FAIL", "image": str(image), "cpus": args.cpus,
              "quick": args.quick, "verify_persisted": args.verify_persisted,
              "sync_skipped": args.skip_sync,
              "overlay": str(disk), "overlay_reused": bool(args.reuse_overlay),
              "sample_interval_s": args.sample_interval,
              "sample_jitter": args.sample_jitter,
              "sample_jitter_seed": 0 if args.sample_jitter else None,
              "sample_stacks": args.sample_stacks,
              "paused_samples": args.paused_samples,
              "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "expected_phases": expected, "phases": []}
    process = guest = current = None
    serial_base = serial_input_fd = serial_output_fd = None
    started = time.monotonic()
    try:
        report["qemu_version"] = subprocess.check_output(
            [args.qemu, "--version"], text=True).splitlines()[0]
        info = json.loads(subprocess.check_output(
            [args.qemu_img, "info", "--output=json", str(image)], text=True))
        if args.reuse_overlay:
            prior = json.loads((disk.parent / "report.json").read_text())
            overlay_info = json.loads(subprocess.check_output(
                [args.qemu_img, "info", "--output=json", str(disk)], text=True))
            backing = overlay_info.get("full-backing-filename", "")
            if (disk == image or overlay_info.get("format") != "qcow2" or not backing or
                    Path(backing).resolve() != image or prior.get("result") != "PASS" or
                    prior.get("overlay") != str(disk) or prior.get("image") != str(image) or
                    prior.get("persisted", {}).get("action") != "stored"):
                raise ValueError("reuse requires a matching successful disposable overlay with stored output identity")
            report["prior_report"] = str(disk.parent / "report.json")
            report["expected_persisted"] = prior["persisted"]
        else:
            subprocess.run([args.qemu_img, "create", "-f", "qcow2", "-F", info["format"],
                            "-b", str(image), str(disk)], check=True, capture_output=True)
        shutil.copyfile(args.firmware_code, output / "firmware-code.fd")
        firmware = f"if=pflash,format=raw,file={output}/firmware-code.fd"
        if args.firmware_vars:
            firmware += ",readonly=on"
        command = [args.qemu, "-machine", "q35", "-accel", "tcg,thread=multi",
                   "-smp", str(args.cpus), "-m", "4096", "-cpu",
                   "SandyBridge,-rdrand,-rdseed", "-drive", firmware]
        for plugin in args.plugin:
            command += ["-plugin", plugin]
        if args.firmware_vars:
            shutil.copyfile(args.firmware_vars, output / "firmware-vars.fd")
            command += ["-drive", f"if=pflash,format=raw,file={output}/firmware-vars.fd"]
        serial_base, serial_input_fd, serial_output_fd = open_serial_fifo(output)
        command += ["-drive", f"file={disk},if=ide,format=qcow2",
                    "-display", "none", "-chardev",
                    f"pipe,id=bench,path={serial_base}",
                    "-serial", "chardev:bench", "-qmp", "stdio", "-nic", "none",
                    "-no-reboot", "-no-shutdown", "-S"]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (output / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=log, bufsize=0)
        guest = LAUNCH.IO.Guest(process, output)
        guest.receive()
        guest.qmp("qmp_capabilities")
        boot = time.monotonic()
        guest.qmp("cont")
        deadline = boot + args.timeout
        wire = bytearray()
        passed = False
        next_sample = float("inf")
        with (output / "serial.log").open("wb") as log, \
                (output / "samples.jsonl").open("w") as samples:
            while not passed:
                now = time.monotonic()
                if now >= deadline:
                    raise TimeoutError("guest benchmark deadline")
                if current is not None and now >= next_sample:
                    sample = {"phase": current["phase"], "host_wall_s": now - boot,
                              "phase_wall_s": now - current["started_monotonic"],
                              "paused": args.paused_samples}
                    paused_at = None
                    try:
                        if args.paused_samples:
                            guest.qmp("stop")
                            paused_at = time.monotonic()
                            sample["stop_query_wall_s"] = paused_at - now
                        sample["registers"] = guest.qmp(
                            "human-monitor-command", {"command-line": "info registers -a"})
                        if args.sample_stacks:
                            sample["stacks"] = sample_stacks(
                                guest, sample["registers"], args.paused_samples,
                                24 if args.paused_samples else 8)
                    finally:
                        if paused_at is not None:
                            resume_at = time.monotonic()
                            sample["paused_wall_s"] = resume_at - paused_at
                            guest.qmp("cont")
                            sample["resume_query_wall_s"] = time.monotonic() - resume_at
                    sample["query_wall_s"] = time.monotonic() - now
                    samples.write(json.dumps(sample) + "\n")
                    samples.flush()
                    current["samples"] += 1
                    next_sample = time.monotonic() + sample_delay()
                wait = max(0, min(1, deadline - time.monotonic(),
                                  next_sample - time.monotonic()))
                ready, _, _ = select.select([serial_output_fd], [], [], wait)
                if not ready:
                    if process.poll() is not None:
                        raise RuntimeError("QEMU exited before benchmark completion")
                    continue
                data = os.read(serial_output_fd, 65536)
                if not data:
                    raise RuntimeError("serial closed")
                log.write(data)
                log.flush()
                if b"Breakpoint exception." in wire + data or b"Page Fault Exception" in wire + data:
                    raise RuntimeError("guest entered the kernel debugger")
                for line in LAUNCH.serial_lines(wire, data):
                    if "PANIC:" in line:
                        raise RuntimeError(line)
                    if "COMPILEBENCH FAIL" in line:
                        raise RuntimeError(line)
                    if line == "COMPILEBENCH skipped phase=sync reason=writes-disabled-performance-only":
                        if not args.skip_sync:
                            raise RuntimeError("unrequested sync omission")
                        report["sync_skip_reason"] = "writes-disabled-performance-only"
                    match = re.fullmatch(r"COMPILEBENCH persisted action=(stored|verified) "
                                         r"bytes=(\d+) fnv1a64=([0-9a-f]{16})", line)
                    if match:
                        if "persisted" in report:
                            raise RuntimeError("duplicate persisted output identity")
                        report["persisted"] = {"action": match[1], "bytes": int(match[2]),
                                               "fnv1a64": match[3]}
                    match = re.fullmatch(r"COMPILEBENCH READY phase=(\S+)", line)
                    if match:
                        index = len(report["phases"])
                        if current is not None or index >= len(expected) or match[1] != expected[index]:
                            raise RuntimeError(f"unexpected phase: {line}")
                        now = time.monotonic()
                        current = {"phase": match[1], "samples": 0,
                                   "gate_acknowledged": False,
                                   "ready_host_s": now - boot}
                        write_report(output, report, result="RUNNING", current=current)
                        current["blocks_before"] = guest.qmp("query-blockstats")
                        current["irq_before"] = guest.qmp(
                            "human-monitor-command", {"command-line": "info irq"})
                        os.write(serial_input_fd, b"g")
                        sent = time.monotonic()
                        current["started_monotonic"] = sent
                        current["go_monotonic"] = sent
                        current["host_started_s"] = sent - boot
                        current["go_host_s"] = sent - boot
                        current["ready_to_go_wall_s"] = sent - now
                        next_sample = sent + sample_delay()
                        write_report(output, report, result="RUNNING", current=current)
                    match = re.fullmatch(r"COMPILEBENCH ACK phase=(\S+)", line)
                    if match:
                        validate_phase_event(current, match[1], "ACK")
                        now = time.monotonic()
                        current["gate_acknowledged"] = True
                        current["ack_host_s"] = now - boot
                        current["gate_ack_wall_s"] = now - current["go_monotonic"]
                        write_report(output, report, result="RUNNING", current=current)
                    match = re.fullmatch(r"COMPILEBENCH metric phase=(\S+) (.+)", line)
                    if match:
                        validate_phase_event(current, match[1], "metric")
                        current["metric"] = {key: int(value) for key, value in
                                             (item.split("=", 1) for item in match[2].split())}
                        if match[1] == "compile-exact" and current["metric"]["rc"]:
                            expected.insert(3, "compile-practical")
                    match = re.fullmatch(r"COMPILEBENCH DONE phase=(\S+)", line)
                    if match:
                        validate_phase_event(current, match[1], "DONE")
                        now = time.monotonic()
                        current["host_wall_s"] = now - current.pop("started_monotonic")
                        current.pop("go_monotonic")
                        current["host_done_s"] = now - boot
                        current["blocks_after"] = guest.qmp("query-blockstats")
                        current["irq_after"] = guest.qmp(
                            "human-monitor-command", {"command-line": "info irq"})
                        current["block_delta"] = LAUNCH.block_delta(
                            current["blocks_before"], current["blocks_after"])
                        report["phases"].append(current)
                        write_report(output, report, result="RUNNING")
                        print(json.dumps({"phase": current["phase"],
                                          "host_wall_s": current["host_wall_s"],
                                          **current["metric"]}), flush=True)
                        current = None
                        next_sample = float("inf")
                    if line == "COMPILEBENCH PASS END":
                        if current is not None or len(report["phases"]) != len(expected):
                            raise RuntimeError("premature success marker")
                        if any(p["metric"]["rc"] for p in report["phases"]
                               if p["phase"] != "compile-exact"):
                            raise RuntimeError("nonzero phase exit status")
                        if args.skip_sync and "sync_skip_reason" not in report:
                            raise RuntimeError("missing explicit sync skip marker")
                        if args.verify_persisted:
                            persisted = report.get("persisted", {})
                            if persisted.get("action") != "verified":
                                raise RuntimeError("missing persisted output verification")
                            if "expected_persisted" in report and any(
                                    persisted.get(key) != report["expected_persisted"].get(key)
                                    for key in ("bytes", "fnv1a64")):
                                raise RuntimeError("persisted output differs from the previous run")
                        passed = True
        report["result"] = "PASS"
    except Exception as error:
        report["error"] = repr(error)
        if current is not None:
            report["incomplete_phase"] = public_phase_state(current)
        if guest:
            try:
                guest.qmp("stop")
                for key, command in (("registers", "info registers -a"), ("irq", "info irq")):
                    report[key] = guest.qmp("human-monitor-command", {"command-line": command})
                report["failure_blocks"] = guest.qmp("query-blockstats")
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
        close_serial_fifo(serial_base, serial_input_fd, serial_output_fd)
        report["total_host_wall_s"] = time.monotonic() - started
        write_report(output, report, current=current)
    print(json.dumps({key: value for key, value in report.items() if key != "phases"}))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
