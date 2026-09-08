#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Boot the optional AHCI smoke module against a disposable patterned disk."""

import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import time

DISK_SIZE = 32 * 1024 * 1024
HEADER_SIZE = 4096
MAGIC = b"PEDIGREE-AHCI-SMOKE-v1"
BASE_SEED = 0x5A
WRITE_SEED = 0xA5
ERROR_OFFSET = 24 * 1024 * 1024
READ_RANGES = ((4096, 4096), (65536 - 512, 8192),
               (1024 * 1024 - 512, 128 * 1024), (DISK_SIZE - 4096, 4096))
WRITE_RANGES = ((8 * 1024 * 1024 + 512, 8192),
                (16 * 1024 * 1024 - 512, 128 * 1024), (DISK_SIZE - 512, 512))
STEPS = ("fixture-identification", "range-rejection", "patterned-reads", "writes-and-sync",
         "concurrent-reads", "uncached-rereads", "ahci-root-mount", "interrupt-completions", "complete")
ERROR_STEPS = ("transport-error", "offline-rejection", "root-after-error")
TRACE_EVENTS = ("handle_cmd_fis_dump", "ide_bus_exec_cmd", "ahci_cmd_done", "process_ncq_command", "ncq_finish")


def pattern(offset, length, seed):
    return bytes(((p * 37) ^ (p >> 8) ^ (p >> 16) ^ seed) & 255
                 for p in range(offset, offset + length))


def header(inject_read_error=False):
    value = bytearray(HEADER_SIZE)
    value[:len(MAGIC)] = MAGIC
    struct.pack_into("<IIQ", value, 32, 1, BASE_SEED, DISK_SIZE)
    value[48] = int(inject_read_error)
    return bytes(value)


def expected_chunk(offset, length, written=False, inject_read_error=False):
    value = bytearray(pattern(offset, length, BASE_SEED))
    if offset < HEADER_SIZE:
        end = min(offset + length, HEADER_SIZE)
        value[:end - offset] = header(inject_read_error)[offset:end]
    if written:
        for start, size in WRITE_RANGES:
            left, right = max(offset, start), min(offset + length, start + size)
            if right > left:
                value[left - offset:right - offset] = pattern(left, right - left, WRITE_SEED)
    return bytes(value)


def create_fixture(path, inject_read_error=False):
    with path.open("xb") as stream:
        for offset in range(0, DISK_SIZE, 1024 * 1024):
            stream.write(expected_chunk(offset, min(1024 * 1024, DISK_SIZE - offset),
                                        inject_read_error=inject_read_error))
        stream.flush()
        os.fsync(stream.fileno())


def verify_fixture(path, inject_read_error=False):
    if path.stat().st_size != DISK_SIZE:
        raise RuntimeError("scratch disk size changed")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for offset in range(0, DISK_SIZE, 1024 * 1024):
            actual = stream.read(min(1024 * 1024, DISK_SIZE - offset))
            expected = expected_chunk(offset, len(actual), written=True,
                                      inject_read_error=inject_read_error)
            if actual != expected:
                first = next(i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1])
                raise RuntimeError(f"scratch disk mismatch at byte {offset + first}")
            digest.update(actual)
    return {"verified_bytes": DISK_SIZE, "sha256": digest.hexdigest()}


def serial_progress(text, inject_read_error=False):
    if "AHCI-SMOKE: FAIL" in text or "PANIC" in text:
        raise RuntimeError("guest failure; inspect serial.log")
    required = STEPS[:-1] + ERROR_STEPS + STEPS[-1:] if inject_read_error else STEPS
    completed = [step for step in required if f"AHCI-SMOKE: PASS {step}" in text]
    match = re.search(r"AHCI-SMOKE: PASS interrupt-completions count=(\d+)", text)
    count = int(match.group(1)) if match else 0
    if "complete" in completed and (len(completed) != len(required) or count == 0):
        raise RuntimeError("guest completion lacks required test or interrupt evidence")
    return completed, count


def trace_summary(text):
    # QEMU logs the FIS port synchronously before dispatching its ATA command.
    port = None
    fis_bytes = None
    target_read_pending = False
    flushes = []
    taskfile_errors = 0
    root_reads_after_error = 0
    for line in text.splitlines():
        irq = re.search(r"ahci_trigger_irq.*ahci\([^)]*\)\[(\d+)\]: trigger irq \+[^()]*"
                        r"\(0x([0-9a-fA-F]+)\)", line)
        if (irq and target_read_pending and int(irq.group(1)) == 1 and
                int(irq.group(2), 16) & (1 << 30)):
            taskfile_errors += 1
        fis = re.search(r"handle_cmd_fis_dump.*ahci\([^)]*\)\[(\d+)\]:", line)
        if fis:
            port = int(fis.group(1))
            fis_bytes = None
        row = re.fullmatch(r"\s*0x00:\s+((?:[0-9a-fA-F]{2}\s+){15}[0-9a-fA-F]{2})\s*", line)
        if row and port is not None:
            fis_bytes = bytes.fromhex(row.group(1))
        command = re.search(r"ide_bus_exec_cmd.*cmd 0x([0-9a-fA-F]+)", line)
        if command:
            opcode = int(command.group(1), 16)
            if port == 1:
                # BIOS IDENTIFY PACKET failures also raise TFES. Only the
                # deliberately faulted data read establishes error evidence.
                target_read_pending = False
                if (fis_bytes and opcode == fis_bytes[2] == 0x25 and
                        fis_bytes[0] == 0x27 and fis_bytes[1] & 0x80 and fis_bytes[7] & 0x40):
                    lba = int.from_bytes(fis_bytes[4:7] + fis_bytes[8:11], "little")
                    target_read_pending = lba == ERROR_OFFSET // 512
            if port == 1 and opcode in (0xE7, 0xEA):
                flushes.append(f"0x{opcode:02x}")
            if port == 0 and opcode == 0x25 and taskfile_errors:
                root_reads_after_error += 1
            port = None
            fis_bytes = None
    return {"scratch_flush_commands": len(flushes), "scratch_flush_opcodes": sorted(set(flushes)),
            "scratch_taskfile_errors": taskfile_errors,
            "root_reads_after_scratch_error": root_reads_after_error}


def error_config():
    return ('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
            f'errno = "{errno.EIO}"\nsector = "{ERROR_OFFSET // 512}"\n'
            'once = "on"\nimmediately = "off"\n')


def qemu_command(args, run_dir):
    def disk_path(path):
        return str(path).replace(",", ",,")

    scratch = f"file={disk_path(run_dir / 'scratch.img')}"
    if args.inject_read_error:
        scratch = (f"file.driver=blkdebug,file.config={disk_path(run_dir / 'blkdebug.conf')},"
                   f"file.image.driver=file,file.image.filename={disk_path(run_dir / 'scratch.img')}")
    scratch += ",format=raw,if=none,id=scratch,cache=writeback,iops_rd=100"
    if args.inject_read_error:
        scratch += ",rerror=report"
    return [args.qemu, "-machine", "pc", "-m", "512", "-smp", str(args.cpus),
            "-display", "none", "-monitor", "none", "-no-reboot", "-nic", "none",
            "-serial", f"file:{run_dir / 'serial.log'}", "-boot", "d",
            "-drive", f"file={disk_path(args.iso)},format=raw,media=cdrom,if=ide,index=2,readonly=on",
            "-device", "ich9-ahci,id=ahci",
            "-drive", f"file={disk_path(args.root_image)},format=raw,if=none,id=rootdisk,snapshot=on",
            "-device", "ide-hd,drive=rootdisk,bus=ahci.0",
            "-drive", scratch,
            "-device", "ide-hd,drive=scratch,bus=ahci.1",
            "-trace", f"events={disk_path(run_dir / 'trace-events')},file={disk_path(run_dir / 'trace.log')}"]


def stop_child(child):
    if child is None or child.poll() is not None:
        return
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait(timeout=5)


def run(args):
    run_dir = args.run_dir.resolve()
    run_dir.mkdir(parents=True, exist_ok=False)
    report = {"success": False, "cpus": args.cpus, "steps": [],
              "timeout_seconds": args.timeout, "scratch_size": DISK_SIZE,
              "read_ranges": READ_RANGES, "write_ranges": WRITE_RANGES,
              "inject_read_error": args.inject_read_error}
    child = None
    started = time.monotonic()
    try:
        create_fixture(run_dir / "scratch.img", args.inject_read_error)
        events = TRACE_EVENTS
        if args.inject_read_error:
            (run_dir / "blkdebug.conf").write_text(error_config())
            events += ("ahci_trigger_irq",)
            report["error_offset"] = ERROR_OFFSET
            report["error_return_bound_seconds"] = 5
        (run_dir / "trace-events").write_text("\n".join(events) + "\n")
        command = qemu_command(args, run_dir)
        report["command"] = command
        (run_dir / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (run_dir / "qemu-stderr.log").open("wb") as stderr:
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=stderr,
                                     start_new_session=True)
            deadline = time.monotonic() + args.timeout
            while True:
                serial_path = run_dir / "serial.log"
                serial = serial_path.read_text(errors="replace") if serial_path.exists() else ""
                report["steps"], report["interrupt_completions"] = serial_progress(serial, args.inject_read_error)
                if "complete" in report["steps"]:
                    break
                if child.poll() is not None:
                    raise RuntimeError(f"QEMU exited before completion: {child.returncode}; inspect qemu-stderr.log")
                if time.monotonic() >= deadline:
                    raise RuntimeError("guest smoke timed out; inspect serial.log and trace.log")
                time.sleep(0.1)
            stop_child(child)
        report["qemu_returncode"] = child.returncode
        if child.returncode not in (0, -signal.SIGTERM):
            raise RuntimeError(f"QEMU exited abnormally: {child.returncode}; inspect qemu-stderr.log")
        serial_progress((run_dir / "serial.log").read_text(errors="replace"), args.inject_read_error)
        report.update(trace_summary((run_dir / "trace.log").read_text(errors="replace")))
        if report["scratch_flush_commands"] == 0:
            raise RuntimeError("no guest ATA FLUSH CACHE command traced on scratch AHCI port 1")
        if args.inject_read_error and report["scratch_taskfile_errors"] == 0:
            raise RuntimeError("no AHCI task-file error traced on scratch port 1")
        if args.inject_read_error and report["root_reads_after_scratch_error"] == 0:
            raise RuntimeError("no root-port DMA read traced after the scratch error")
        report.update(verify_fixture(run_dir / "scratch.img", args.inject_read_error))
        report["success"] = True
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = str(error) or type(error).__name__
    finally:
        try:
            stop_child(child)
        finally:
            report["elapsed_seconds"] = round(time.monotonic() - started, 3)
            (run_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--root-image", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--cpus", type=int, choices=(1, 4), default=1)
    parser.add_argument("--run-dir", type=Path, required=True, help="new directory for disk and evidence")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--inject-read-error", action="store_true",
                        help="inject one scratch read EIO after the persistence tests")
    args = parser.parse_args()
    if not 0 < args.timeout <= 3600:
        parser.error("--timeout must be between 0 and 3600 seconds")
    for name in ("iso", "root_image"):
        path = getattr(args, name).expanduser().resolve()
        if not path.is_file():
            parser.error(f"{name.replace('_', '-')} is not a regular file: {path}")
        setattr(args, name, path)
    if args.run_dir.exists():
        parser.error("--run-dir must not already exist")

    def interrupt(signum, frame):
        raise KeyboardInterrupt()

    signal.signal(signal.SIGTERM, interrupt)
    result = run(args)
    print(json.dumps({"success": result["success"], "report": str(args.run_dir.resolve() / "report.json"),
                      **({"error": result["error"]} if "error" in result else {})}))
    return 0 if result["success"] else 1


if __name__ == "__main__":
    sys.exit(main())
