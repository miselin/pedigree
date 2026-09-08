#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Validate USB HID input and disposable mass-storage writes in a UEFI guest."""

import argparse
from collections import deque
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess
import time

SPEC = importlib.util.spec_from_file_location(
    "ahci_smoke", Path(__file__).with_name("test_qemu_ahci.py")
)
ahci = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ahci)

DISK_SIZE = 32 * 1024 * 1024
HEADER_SIZE = 4096
MAGIC = b"PEDIGREE-USB-SMOKE-v1"
WRITE_RANGES = ((8 * 1024 * 1024, 8192), (DISK_SIZE - 4096, 4096))
STEPS = ("bot-hid-contracts", "fixture-identification", "msd-write-flush-reread", "complete")


def expected_chunk(offset, length, sector, written=False):
    value = bytearray(ahci.pattern(offset, length, 0x5A))
    if offset < HEADER_SIZE:
        header = bytearray(HEADER_SIZE)
        header[: len(MAGIC)] = MAGIC
        header[32] = 1
        struct.pack_into("<QI", header, 40, DISK_SIZE, sector)
        end = min(offset + length, HEADER_SIZE)
        value[: end - offset] = header[offset:end]
    if written:
        for start, count in WRITE_RANGES:
            left, right = max(offset, start), min(offset + length, start + count)
            if left < right:
                value[left - offset : right - offset] = ahci.pattern(left, right - left, 0xA5)
    return bytes(value)


def create_fixture(path, sector):
    with path.open("xb") as stream:
        for offset in range(0, DISK_SIZE, 1024 * 1024):
            stream.write(expected_chunk(offset, min(1024 * 1024, DISK_SIZE - offset), sector))
        stream.flush()
        os.fsync(stream.fileno())


def verify_fixture(path, sector):
    if path.stat().st_size != DISK_SIZE:
        raise RuntimeError("USB scratch disk size changed")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for offset in range(0, DISK_SIZE, 1024 * 1024):
            length = min(1024 * 1024, DISK_SIZE - offset)
            actual = stream.read(length)
            expected = expected_chunk(offset, length, sector, True)
            if actual != expected:
                first = next((i for i, pair in enumerate(zip(actual, expected))
                              if pair[0] != pair[1]), len(actual))
                raise RuntimeError(f"USB scratch disk mismatch at byte {offset + first}")
            digest.update(actual)
    return {"verified_bytes": DISK_SIZE, "sha256": digest.hexdigest()}


def serial_progress(text):
    if re.search(r"USB-SMOKE: FAIL|panic:|fatal:|page fault exception|\(FF\)", text, re.I):
        raise RuntimeError("guest failure; inspect serial.log")
    observed = 0
    for value in re.findall(
        r"USB-SMOKE: PASS input-bits=(?:0x)?[0-9a-f]+ observed=((?:0x)?[0-9a-f]+)\b",
        text, re.I,
    ):
        observed |= int(value, 16)
    steps = [step for step in STEPS
             if re.search(rf"USB-SMOKE: PASS {re.escape(step)}(?:\s|$)", text)]
    ready = "USB-SMOKE: READY input" in text
    if "complete" in steps and (len(steps) != len(STEPS) or not ready or observed != 0x3F):
        raise RuntimeError("guest completion lacks required USB storage or input evidence")
    return {"steps": steps, "observed": observed, "ready": ready}


def verify_flushes(text):
    # scsi_req_print logs the command name followed by the remaining CDB bytes.
    writes = set()
    flushes = []
    command = re.compile(
        r"^\[([^\]\r\n]+) id=0\] (WRITE_10|WRITE_16|SYNCHRONIZE_CACHE|"
        r"SPACE_16/SYNCHRONIZE_CACHE_16)((?: 0x[0-9a-fA-F]{2})+) - (.*)$"
    )
    for line in text.splitlines():
        match = command.fullmatch(line)
        if not match:
            continue
        bus, name, raw, mode = match.groups()
        tail = bytes(int(value, 16) for value in raw.split())
        expected_length = 15 if name.endswith("_16") else 9
        if len(tail) != expected_length:
            continue
        if name.startswith("WRITE_"):
            if mode.startswith("to-dev len=") and mode != "to-dev len=0":
                writes.add(bus)
        elif bus in writes and mode == "none" and not (tail[0] & 2):
            flushes.append("0x91" if name.endswith("_16") else "0x35")
    if not flushes:
        raise RuntimeError("no guest SCSI flush after a USB scratch write in qemu.log")
    return {"scratch_flush_commands": len(flushes), "scratch_flush_opcodes": sorted(set(flushes))}


def input_sequence():
    key = {"type": "qcode", "data": "a"}
    return (
        ("key-down", 1, {"type": "key", "data": {"down": True, "key": key}}),
        ("key-up", 2, {"type": "key", "data": {"down": False, "key": key}}),
        ("mouse-x", 4, {"type": "rel", "data": {"axis": "x", "value": 17}}),
        ("mouse-y", 8, {"type": "rel", "data": {"axis": "y", "value": -9}}),
        ("button-down", 16, {"type": "btn", "data": {"down": True, "button": "left"}}),
        ("button-up", 32, {"type": "btn", "data": {"down": False, "button": "left"}}),
    )


class Qmp:
    def __init__(self, child, transcript, check):
        self.child = child
        self.transcript = transcript
        self.check = check
        self.buffer = b""
        self.messages = deque()
        self.next_id = 0
        os.set_blocking(child.stdin.fileno(), False)
        os.set_blocking(child.stdout.fileno(), False)

    def receive(self, deadline):
        while True:
            self.check()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("QMP response timed out")
            if self.messages:
                return self.messages.popleft()
            ready, _, _ = select.select([self.child.stdout], [], [], min(remaining, 0.1))
            if not ready:
                continue
            data = os.read(self.child.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError("QMP pipe closed before the expected response")
            self.transcript.write(b"< " + data)
            self.transcript.flush()
            self.buffer += data
            if len(self.buffer) > 1024 * 1024:
                raise RuntimeError("QMP response exceeded 1 MiB")
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if line.strip():
                    message = json.loads(line)
                    if not isinstance(message, dict):
                        raise RuntimeError("QMP response is not an object")
                    self.messages.append(message)

    def execute(self, name, arguments, deadline):
        self.next_id += 1
        request = {"execute": name, "id": self.next_id}
        if arguments is not None:
            request["arguments"] = arguments
        payload = json.dumps(request).encode() + b"\n"
        self.transcript.write(b"> " + payload)
        self.transcript.flush()
        while payload:
            self.check()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("QMP request timed out")
            _, writable, _ = select.select([], [self.child.stdin], [], min(remaining, 0.1))
            if writable:
                payload = payload[os.write(self.child.stdin.fileno(), payload):]
        while True:
            message = self.receive(deadline)
            if message.get("id") != self.next_id:
                if "event" in message:
                    continue
                raise RuntimeError("QMP response has an unexpected request id")
            if "error" in message:
                raise RuntimeError(f"QMP {name} failed: {message['error']}")
            if "return" not in message:
                raise RuntimeError("QMP response lacks a result")
            return message["return"]


def command(args, folder):
    def drive(path, name, snapshot=False):
        value = f"file={str(path).replace(',', ',,')},format=raw,if=none,id={name},cache=writeback"
        return value + (",snapshot=on" if snapshot else "")

    controller = "usb-ehci" if args.controller == "ehci" else "piix3-usb-uhci"
    version = 2 if args.controller == "ehci" else 1
    firmware = str(args.ovmf).replace(",", ",,")
    result = [
        args.qemu, "-machine", "q35,usb=off,i8042=off", "-m", "768", "-smp", str(args.cpus),
        "-display", "none", "-monitor", "none", "-qmp", "stdio", "-nic", "none", "-no-reboot",
        "-serial", f"file:{folder / 'serial.log'}",
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-drive", drive(args.image, "root", True), "-device", "ide-hd,drive=root,bus=ide.0",
        "-device", f"{controller},id=usb",
        "-device", f"usb-kbd,id=kbd,bus=usb.0,port=1,usb_version={version}",
        "-device", f"usb-mouse,id=mouse,bus=usb.0,port=2,usb_version={version}",
    ]
    storage_bus, storage_port = "usb.0", 3
    if args.controller == "uhci":
        # UHCI has two root ports; a second controller keeps this fixture independent of hubs.
        result += ["-device", "piix3-usb-uhci,id=storageusb"]
        storage_bus, storage_port = "storageusb.0", 1
    result += [
        "-drive", drive(folder / "usb.img", "scratch"),
        "-device", f"usb-storage,id=msd,bus={storage_bus},port={storage_port},drive=scratch,"
        "serial=PEDIGREE-USB-SMOKE,commandlog=on,"
        f"logical_block_size={args.sector_size},physical_block_size={args.sector_size}",
    ]
    return result


def run(args):
    folder = args.run_dir.resolve()
    folder.mkdir(parents=True, exist_ok=False)
    report = {"success": False, "controller": args.controller,
              "cpus": args.cpus, "sector_size": args.sector_size}
    child = None
    try:
        create_fixture(folder / "usb.img", args.sector_size)
        argv = command(args, folder)
        (folder / "command.json").write_text(json.dumps(argv, indent=2) + "\n")
        with (folder / "qemu.log").open("wb") as output, (folder / "qmp.log").open("wb") as transcript:
            child = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=output, bufsize=0, start_new_session=True)
            deadline = time.monotonic() + args.timeout

            def check():
                serial_path = folder / "serial.log"
                serial = serial_path.read_text(errors="replace") if serial_path.exists() else ""
                progress = serial_progress(serial)
                if child.poll() is not None:
                    raise RuntimeError("QEMU exited before USB validation finished")
                if time.monotonic() >= deadline:
                    raise RuntimeError("USB guest timed out")
                return progress

            def wait_for(predicate, label, stage_deadline):
                while True:
                    progress = check()
                    if predicate(progress):
                        return progress
                    if time.monotonic() >= stage_deadline:
                        raise RuntimeError(f"USB guest timed out waiting for {label}")
                    time.sleep(0.05)

            qmp = Qmp(child, transcript, check)
            greeting = qmp.receive(min(deadline, time.monotonic() + 10))
            if "QMP" not in greeting:
                raise RuntimeError("missing QMP greeting")
            qmp.execute("qmp_capabilities", None, min(deadline, time.monotonic() + 10))
            wait_for(lambda progress: progress["ready"], "input readiness", deadline)
            expected = 0
            for label, bit, event in input_sequence():
                if check()["observed"] & bit:
                    raise RuntimeError(f"guest observed {label} before its QMP injection")
                qmp.execute("input-send-event", {"events": [event]},
                            min(deadline, time.monotonic() + 10))
                expected |= bit
                wait_for(lambda progress: progress["observed"] & expected == expected,
                         label, min(deadline, time.monotonic() + 20))
            wait_for(lambda progress: "complete" in progress["steps"], "completion", deadline)
            ahci.stop_child(child)
        report.update(serial_progress((folder / "serial.log").read_text(errors="replace")))
        report.update(verify_flushes((folder / "qemu.log").read_text(errors="replace")))
        report.update(verify_fixture(folder / "usb.img", args.sector_size))
        report["success"] = True
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = str(error) or type(error).__name__
    finally:
        ahci.stop_child(child)
        if child is not None:
            child.stdin.close()
            child.stdout.close()
        (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--controller", choices=("ehci", "uhci"), default="ehci")
    parser.add_argument("--sector-size", choices=(512, 4096), type=int, default=512)
    parser.add_argument("--cpus", choices=(1, 4), type=int, default=4)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ovmf", type=Path, default=Path("/opt/homebrew/share/qemu/edk2-x86_64-code.fd"))
    parser.add_argument("--timeout", type=float, default=240)
    args = parser.parse_args()
    if not args.image.is_file() or not args.ovmf.is_file() or not 0 < args.timeout <= 3600:
        parser.error("image/OVMF must exist and timeout must be between 0 and 3600")
    args.image = args.image.resolve()
    args.ovmf = args.ovmf.resolve()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    result = run(args)
    print(json.dumps(result))
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
