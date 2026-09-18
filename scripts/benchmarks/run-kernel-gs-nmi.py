#!/usr/bin/env python3
# Copyright (c) 2026, Pedigree Developers.
#
# Please see the CONTRIB file in the root of the source tree for a full
# list of contributors.
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

"""Inject one NMI at each GS window in a disposable one-CPU guest.

Requires the synthetic-vm fixture to run kernel-gs-contract (without arguments).
Uses QEMU's GDB remote protocol directly; no host gdb installation is required.
The diagnostic build must enable USER_ENTRY_DIAGNOSTICS and ACTIVITY_DIAGNOSTICS.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

SELECTOR_LOADED = "pedigree_user_gs_selector_loaded"
WINDOWS = [
    ("pedigree_syscall_entry_swapgs", "user", "user"),
    ("pedigree_syscall_entry_kernel_gs", "kernel", "user"),
    ("pedigree_syscall_kernel_stack", "kernel", "kernel"),
    ("pedigree_user_gs_restore_swapgs", "kernel", "kernel"),
    ("pedigree_user_gs_restore_user", "user", "kernel"),
    (SELECTOR_LOADED, "zero", "kernel"),
    ("pedigree_user_gs_restore_kernel", "kernel", "kernel"),
    ("pedigree_syscall_exit_swapgs", "kernel", "user"),
    ("pedigree_syscall_exit_user_gs", "user", "user"),
]
RETURN = "_ZN19X64InterruptManager19returnFromInterruptER17X64InterruptState"
DIAGNOSTICS = "pedigree_nmi_entry_diagnostics"
FIELDS = ("count", "rip", "cs", "rsp", "information", "index", "activeGs", "frame")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def high(value):
    return 0xFFFF800000000000 <= value <= 0xFFFFFFFFFFFFFFFF


def low(value):
    return 0 < value < 0x0000800000000000


def sha(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def symbols(nm, path):
    output = subprocess.check_output([nm, "-n", str(path)], text=True, timeout=30)
    return {
        name: int(address, 16)
        for address, name in re.findall(
            r"^([0-9a-fA-F]+)\s+\S\s+(\S+)\s*$", output, re.MULTILINE
        )
    }


def elf_bytes(path, address, size):
    with path.open("rb") as stream:
        header = stream.read(64)
        require(header[:6] == b"\x7fELF\x02\x01", f"not ELF64 little endian: {path}")
        require(
            struct.unpack_from("<H", header, 16)[0] == 2,
            f"requires fixed ET_EXEC: {path}",
        )
        offset = struct.unpack_from("<Q", header, 32)[0]
        entry_size, count = struct.unpack_from("<HH", header, 54)
        for index in range(count):
            stream.seek(offset + index * entry_size)
            (
                kind,
                _flags,
                file_offset,
                virtual,
                _physical,
                file_size,
                _mem_size,
                _align,
            ) = struct.unpack("<IIQQQQQQ", stream.read(56))
            if (
                kind == 1
                and virtual <= address
                and address + size <= virtual + file_size
            ):
                stream.seek(file_offset + address - virtual)
                return stream.read(size)
    raise RuntimeError(f"ELF address not file-backed: {path}:0x{address:x}")


class Remote:
    """Small acknowledged GDB-remote client: breakpoints, memory, continue, stop."""

    def __init__(self, connection, pump, output):
        self.connection = connection
        self.pump = pump
        self.buffer = bytearray()
        self.trace = (output / "gdb-remote.log").open("w")

    def send(self, payload):
        raw = payload.encode()
        self.trace.write(f"> {payload}\n")
        self.trace.flush()
        self.connection.sendall(b"$" + raw + b"#" + f"{sum(raw) & 255:02x}".encode())

    def receive(self, deadline):
        while True:
            self.pump()
            while self.buffer and self.buffer[0] in b"+-":
                require(self.buffer[0] != ord("-"), "GDB packet rejected")
                del self.buffer[0]
            if self.buffer:
                require(self.buffer[0] == ord("$"), "unexpected GDB packet prefix")
                end = self.buffer.find(b"#", 1)
                if end >= 0 and len(self.buffer) >= end + 3:
                    raw = bytes(self.buffer[1:end])
                    checksum = int(self.buffer[end + 1 : end + 3], 16)
                    del self.buffer[: end + 3]
                    require((sum(raw) & 255) == checksum, "GDB reply checksum")
                    self.connection.sendall(b"+")
                    decoded = bytearray()
                    index = 0
                    while index < len(raw):
                        byte = raw[index]
                        if byte == ord("}"):
                            index += 1
                            decoded.append(raw[index] ^ 0x20)
                        elif byte == ord("*"):
                            index += 1
                            require(bool(decoded), "invalid GDB run-length encoding")
                            decoded.extend(bytes([decoded[-1]]) * (raw[index] - 29))
                        else:
                            decoded.append(byte)
                        index += 1
                    reply = decoded.decode()
                    self.trace.write(f"< {reply}\n")
                    self.trace.flush()
                    return reply
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("GDB reply/stop deadline")
            ready, _, _ = select.select([self.connection], [], [], min(0.05, remaining))
            if ready:
                data = self.connection.recv(65536)
                require(bool(data), "GDB connection closed")
                self.buffer.extend(data)

    def command(self, payload):
        self.send(payload)
        return self.receive(time.monotonic() + 15)

    def breakpoint(self, address, enable):
        packet = f"{'Z' if enable else 'z'}1,{address:x},1"
        require(self.command(packet) == "OK", f"GDB hardware breakpoint: {packet}")

    def memory(self, address, size):
        reply = self.command(f"m{address:x},{size:x}")
        require(not reply.startswith("E"), f"GDB memory failed: {reply}")
        data = bytes.fromhex(reply)
        require(len(data) == size, "short GDB memory reply")
        return data

    def stop_reply(self, seconds):
        reply = self.receive(time.monotonic() + seconds)
        require(
            reply.startswith(("T05", "S05", "T02", "S02")), f"unexpected stop: {reply}"
        )
        return reply

    def close(self):
        self.connection.close()
        self.trace.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo", type=Path, default=Path(__file__).resolve().parents[2]
    )
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--kernel-debug", type=Path, required=True)
    parser.add_argument("--kernel-code", type=Path, required=True)
    parser.add_argument("--guest-elf", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--firmware-code", type=Path, required=True)
    parser.add_argument("--firmware-vars", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--window-timeout", type=float, default=30)
    parser.add_argument("--gdb-port", type=int, default=0)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--qemu-img", default="qemu-img")
    parser.add_argument("--nm", type=Path)
    args = parser.parse_args()
    require(args.timeout > 0 and args.window_timeout > 0, "timeouts must be positive")
    require(0 <= args.gdb_port < 65536, "invalid GDB port")
    repo = args.repo.resolve(strict=True)
    image = args.image.resolve(strict=True)
    kernel = args.kernel_debug.resolve(strict=True)
    kernel_code = args.kernel_code.resolve(strict=True)
    binary = args.guest_elf.resolve(strict=True)
    config = args.config.resolve(strict=True)
    config_text = config.read_text()
    for option in (
        "PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS",
        "PEDIGREE_ACTIVITY_DIAGNOSTICS",
    ):
        require(
            re.search(rf"^#define\s+{option}\s+1\b", config_text, re.MULTILINE),
            f"required build option: {option}",
        )
    nm = str(args.nm or repo / "pedigree-compiler-15.3.0-r2/bin/x86_64-pedigree-nm")
    kernel_symbols = symbols(nm, kernel)
    guest_symbols = symbols(nm, binary)
    required = [name for name, _, _ in WINDOWS if name != SELECTOR_LOADED] + [
        RETURN,
        DIAGNOSTICS,
        "pedigree_interrupt_iret",
    ]
    require(
        all(name in kernel_symbols for name in required),
        "missing diagnostic kernel symbols",
    )
    # Derive the boundary after MOV GS,AX only from its verified encoding.
    selector_load = kernel_symbols["pedigree_user_gs_restore_user"]
    selector_bytes = b"\x8e\xe8"
    require(
        elf_bytes(kernel_code, selector_load, len(selector_bytes)) == selector_bytes,
        "unexpected user GS selector-load bytes",
    )
    kernel_symbols[SELECTOR_LOADED] = selector_load + len(selector_bytes)
    interrupt_iret = kernel_symbols["pedigree_interrupt_iret"]
    require(
        elf_bytes(kernel_code, interrupt_iret, 2) == b"\x48\xcf",
        "unexpected interrupt IRETQ bytes",
    )
    gate = guest_symbols["kernel_gs_user_loop"]
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(
        "compile_runner", repo / "scripts/benchmarks/run-compile-latency.py"
    )
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "result": "FAIL",
        "cpus": 1,
        "image": str(image),
        "kernel_debug": str(kernel),
        "kernel_sha256": sha(kernel),
        "kernel_code": str(kernel_code),
        "kernel_code_sha256": sha(kernel_code),
        "guest_elf": str(binary),
        "guest_sha256": sha(binary),
        "config": str(config),
        "config_sha256": sha(config),
        "runner_sha256": sha(Path(__file__)),
        "windows": [],
        "limitations": [
            "One CPU only",
            "Diagnostic build, not a timing run",
            "One NMI per boundary; no nested NMI, #MC, #DB, #GP, or #SS injection",
            "Requires fixed-address ET_EXEC guest and synthetic-vm serial gate",
        ],
    }
    process = guest = remote = serial_log = None
    serial_base = serial_input = serial_output = None
    started = time.monotonic()
    deadline = started + args.timeout
    wire = bytearray()
    state = {"ready": False, "canary_pass": False, "enclosing_pass": False}

    def save():
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")

    def pump():
        require(time.monotonic() < deadline, "overall deadline")
        if process and process.poll() is not None:
            raise RuntimeError("QEMU exited unexpectedly")
        if serial_output is None:
            return
        while select.select([serial_output], [], [], 0)[0]:
            data = os.read(serial_output, 65536)
            if not data:
                break
            serial_log.write(data)
            serial_log.flush()
            wire.extend(data)
        require(len(wire) < 1048576, "serial record too long")
        while b"\n" in wire:
            line, _, remaining = wire.partition(b"\n")
            wire[:] = remaining
            text = line.decode(errors="replace").strip()
            require(
                not any(
                    marker in text
                    for marker in (
                        "PANIC:",
                        "CONTRACT FAIL",
                        "COMPILEBENCH FAIL",
                        "Page Fault Exception",
                        "Breakpoint exception.",
                    )
                ),
                text,
            )
            if text == "COMPILEBENCH READY phase=vm-synthetic":
                require(not state["ready"], "duplicate synthetic-vm gate")
                state["ready"] = True
            if "COMPILEBENCH metric" in text:
                match = re.search(r"\brc=(\d+)", text)
                if match:
                    require(int(match[1]) == 0, text)
            state["canary_pass"] |= text == "KERNEL-GS-CONTRACT PASS END workers=4"
            state["enclosing_pass"] |= text == "COMPILEBENCH PASS END"

    def registers():
        text = guest.qmp("human-monitor-command", {"command-line": "info registers"})
        result = {
            name: int(value, 16)
            for name, value in re.findall(r"\b(RIP|RSP)=([0-9a-fA-F]+)", text)
        }
        for name in ("CS", "GS", "TR"):
            match = re.search(
                rf"^{name}\s*=([0-9a-fA-F]+)\s+([0-9a-fA-F]+)", text, re.MULTILINE
            )
            require(match is not None, f"missing {name} register")
            result[name] = int(match[1 if name == "CS" else 2], 16)
        require("RIP" in result and "RSP" in result, "missing RIP/RSP")
        return result

    try:
        info = json.loads(
            subprocess.check_output(
                [args.qemu_img, "info", "--output=json", str(image)],
                text=True,
                timeout=30,
            )
        )
        disk = output / "disk.qcow2"
        subprocess.run(
            [
                args.qemu_img,
                "create",
                "-f",
                "qcow2",
                "-F",
                info["format"],
                "-b",
                str(image),
                str(disk),
            ],
            check=True,
            capture_output=True,
            timeout=30,
        )
        shutil.copyfile(args.firmware_code, output / "firmware-code.fd")
        firmware = f"if=pflash,format=raw,file={output}/firmware-code.fd"
        if args.firmware_vars:
            firmware += ",readonly=on"
        port = args.gdb_port
        if not port:
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
        serial_base, serial_input, serial_output = runner.open_serial_fifo(output)
        serial_log = (output / "serial.log").open("wb")
        command = [
            args.qemu,
            "-machine",
            "q35",
            "-accel",
            "tcg,thread=multi",
            "-smp",
            "1",
            "-m",
            "4096",
            "-cpu",
            "SandyBridge,-rdrand,-rdseed",
            "-drive",
            firmware,
        ]
        if args.firmware_vars:
            shutil.copyfile(args.firmware_vars, output / "firmware-vars.fd")
            command += [
                "-drive",
                f"if=pflash,format=raw,file={output}/firmware-vars.fd",
            ]
        command += [
            "-drive",
            f"file={disk},if=ide,format=qcow2",
            "-display",
            "none",
            "-chardev",
            f"pipe,id=bench,path={serial_base}",
            "-serial",
            "chardev:bench",
            "-qmp",
            "stdio",
            "-nic",
            "none",
            "-no-reboot",
            "-no-shutdown",
            "-gdb",
            f"tcp:127.0.0.1:{port}",
            "-S",
        ]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (output / "qemu.log").open("w") as qemu_log:
            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=qemu_log,
                bufsize=0,
            )
        guest = runner.LAUNCH.IO.Guest(process, output)
        guest.receive()
        guest.qmp("qmp_capabilities")
        connection = socket.create_connection(("127.0.0.1", port), timeout=15)
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        remote = Remote(connection, pump, output)
        remote.command("qSupported:hwbreak+;swbreak+")
        require(remote.command("?").startswith(("T", "S")), "initial stopped state")
        remote.send("c")
        while not state["ready"]:
            pump()
            select.select([serial_output], [], [], 0.05)
        connection.sendall(b"\x03")
        remote.stop_reply(args.window_timeout)
        remote.breakpoint(gate, True)
        remote.send("c")
        os.write(serial_input, b"g")
        guest_gate_code = elf_bytes(binary, gate, 8)
        # Other ET_EXEC programs can temporarily use the same virtual address.
        for attempt in range(64):
            remote.stop_reply(args.window_timeout)
            initial = registers()
            require(initial["RIP"] == gate, "unexpected guest gate stop")
            if initial["CS"] & 3 == 3 and remote.memory(gate, 8) == guest_gate_code:
                break
            remote.breakpoint(gate, False)
            remote.send("s")
            remote.stop_reply(args.window_timeout)
            remote.breakpoint(gate, True)
            remote.send("c")
        else:
            raise RuntimeError("guest loop gate never matched its ELF")
        require(low(initial["GS"]), "guest gate requires nonzero user GS")
        require(
            struct.unpack("<Q", remote.memory(initial["GS"], 8))[0]
            in range(0x35BE6809AC142700, 0x35BE6809AC142706),
            "guest GS canary mismatch",
        )
        report["gate_registers"] = initial
        remote.breakpoint(gate, False)
        record_address = kernel_symbols[DIAGNOSTICS]
        prior = struct.unpack("<8Q", remote.memory(record_address, 64))[0]
        report["initial_nmi_count"] = prior
        stable_anchor = None
        rendezvous = kernel_symbols[RETURN]
        for name, gs_kind, stack_kind in WINDOWS:
            address = kernel_symbols[name]
            require(
                remote.memory(address, 8) == elf_bytes(kernel_code, address, 8),
                f"kernel ELF bytes mismatch: {name}",
            )
            remote.breakpoint(address, True)
            remote.send("c")
            remote.stop_reply(args.window_timeout)
            before = registers()
            require(
                before["RIP"] == address and before["CS"] == 8,
                f"window stop mismatch: {name}",
            )
            if gs_kind == "zero":
                require(before["GS"] == 0, f"selector load did not clear GS: {name}")
            else:
                require(
                    (high if gs_kind == "kernel" else low)(before["GS"]),
                    f"wrong pre-NMI GS: {name}",
                )
            require(
                (high if stack_kind == "kernel" else low)(before["RSP"]),
                f"wrong pre-NMI stack: {name}",
            )
            if gs_kind == "user":
                magic = struct.unpack("<Q", remote.memory(before["GS"], 8))[0]
                require(
                    magic in range(0x35BE6809AC142700, 0x35BE6809AC142706),
                    f"non-contract GS at {name}",
                )
            remote.breakpoint(address, False)
            remote.breakpoint(rendezvous, True)
            guest.qmp("inject-nmi")
            remote.send("c")
            remote.stop_reply(args.window_timeout)
            after = registers()
            require(
                after["RIP"] == rendezvous, "not stopped after NMI record publication"
            )
            record = dict(
                zip(FIELDS, struct.unpack("<8Q", remote.memory(record_address, 64)))
            )
            require(
                record["count"] == prior + 1,
                f"NMI count did not advance exactly once: {name}",
            )
            require(
                record["rip"] == address
                and record["cs"] == before["CS"]
                and record["rsp"] == before["RSP"],
                f"NMI did not interrupt exact boundary: {name}",
            )
            require(
                record["index"] == 0
                and high(record["information"])
                and high(record["activeGs"]),
                f"invalid NMI processor identity: {name}",
            )
            anchor = struct.unpack("<4Q", remote.memory(record["activeGs"], 32))
            require(
                anchor[2] == record["information"]
                and anchor[3] == 0
                and high(anchor[0]),
                "kernel GS anchor contents mismatch",
            )
            if stable_anchor is None:
                stable_anchor = record["activeGs"]
            require(
                record["activeGs"] == stable_anchor, "one-CPU kernel anchor changed"
            )
            ist_top = struct.unpack("<Q", remote.memory(after["TR"] + 0x2C, 8))[0]
            require(
                high(ist_top) and ist_top - 8192 <= record["frame"] <= ist_top - 208,
                "NMI frame is outside IST2",
            )
            prior = record["count"]
            remote.breakpoint(rendezvous, False)
            # The next window may be in this NMI's own restore helper. Wait for
            # IRET before arming it, so the next injection is not NMI-blocked.
            require(
                remote.memory(interrupt_iret, 2) == b"\x48\xcf",
                "live interrupt IRET mismatch",
            )
            remote.breakpoint(interrupt_iret, True)
            remote.send("c")
            remote.stop_reply(args.window_timeout)
            require(registers()["RIP"] == interrupt_iret, "NMI did not reach IRET")
            remote.breakpoint(interrupt_iret, False)
            remote.send("s")
            remote.stop_reply(args.window_timeout)
            restored = registers()
            for field in ("RIP", "RSP", "CS", "GS"):
                require(
                    restored[field] == before[field],
                    f"NMI return changed {field}: {name}",
                )
            report["windows"].append(
                {
                    "symbol": name,
                    "address": address,
                    "before": before,
                    "record": record,
                    "ist2_top": ist_top,
                    "restored": restored,
                    "interrupt_iret": interrupt_iret,
                    "result": "PASS",
                }
            )
            save()
            print(f"NMI-WINDOW PASS {name} count={prior}", flush=True)
        remote.send("c")
        while not (state["canary_pass"] and state["enclosing_pass"]):
            pump()
            select.select([serial_output], [], [], 0.05)
        report["result"] = "PASS"
        report["guest_canaries"] = "PASS"
        report["enclosing_workload"] = "PASS"
    except Exception as error:  # noqa: BLE001 - Preserve every failure in the report.
        report["error"] = repr(error)
        if guest and process and process.poll() is None:
            try:
                guest.qmp("stop")
                report["failure_registers"] = guest.qmp(
                    "human-monitor-command", {"command-line": "info registers -a"}
                )
            except Exception as capture_error:  # noqa: BLE001 - Keep the original failure.
                report["capture_error"] = repr(capture_error)
    finally:
        if guest and process and process.poll() is None:
            try:
                guest.qmp("quit")
            except Exception:  # noqa: BLE001 - Always stop our disposable guest.
                process.terminate()
        if process:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if remote:
            remote.close()
        if serial_log:
            serial_log.close()
        runner.close_serial_fifo(serial_base, serial_input, serial_output)
        report["host_wall_s"] = time.monotonic() - started
        report["serial_state"] = state
        save()
    print(
        json.dumps(
            {
                "result": report["result"],
                "windows": len(report["windows"]),
                "error": report.get("error"),
                "output": str(output),
            }
        )
    )
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
