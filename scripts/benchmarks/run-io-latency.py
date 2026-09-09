#!/usr/bin/env python3
"""Run the I/O latency benchmark through a disposable QEMU disk overlay."""

import argparse
import json
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import time


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New directory for the retained overlay and logs")
    parser.add_argument("--cpus", type=int, choices=(1, 4), required=True)
    parser.add_argument("--firmware-code", type=Path, required=True)
    parser.add_argument("--firmware-vars", type=Path,
                        help="Variable-store template for split OVMF firmware")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--qemu-img", default="qemu-img")
    parser.add_argument("--guest-binary", default="/io-latency")
    parser.add_argument("--mode", choices=("full", "read-only", "verify-existing"),
                        default="full")
    parser.add_argument("--size-mib", type=int, choices=range(1, 9), default=1)
    parser.add_argument("--keep-scratch", action="store_true")
    parser.add_argument("--boot-timeout", type=float, default=240)
    parser.add_argument("--benchmark-timeout", type=float, default=240)
    args = parser.parse_args()
    if not re.fullmatch(r"/[a-z0-9/._-]+", args.guest_binary):
        parser.error("--guest-binary must be an absolute path using lowercase ASCII, digits, /._-")
    if min(args.boot_timeout, args.benchmark_timeout) <= 0:
        parser.error("timeouts must be positive")
    return args


class Guest:
    def __init__(self, process, output):
        self.process = process
        self.output = output
        self.wire = bytearray()
        self.ident = 0

    def receive(self):
        end = time.monotonic() + 15
        while b"\n" not in self.wire:
            ready, _, _ = select.select(
                [self.process.stdout], [], [], max(0, end - time.monotonic()))
            if not ready:
                raise TimeoutError("QMP response")
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError("QMP closed")
            self.wire.extend(data)
        line, _, rest = self.wire.partition(b"\n")
        self.wire[:] = rest
        return json.loads(line)

    def qmp(self, command, args=None):
        self.ident += 1
        request = {"execute": command, "id": self.ident}
        if args is not None:
            request["arguments"] = args
        self.process.stdin.write((json.dumps(request) + "\n").encode())
        while True:
            response = self.receive()
            if response.get("id") != self.ident:
                continue
            if "error" in response:
                raise RuntimeError(response["error"])
            return response["return"]

    def serial(self):
        path = self.output / "serial.log"
        return path.read_bytes() if path.exists() else b""

    def wait(self, marker, seconds, start=0):
        end = time.monotonic() + seconds
        while True:
            content = self.serial()[start:]
            if b"IOBENCH FAIL" in content:
                raise RuntimeError("Benchmark reported failure")
            if marker in content:
                return
            if time.monotonic() > end:
                raise TimeoutError(f"Waiting for {marker!r}")
            if self.process.poll() is not None:
                raise RuntimeError("QEMU exited")
            time.sleep(0.1)

    def type(self, text):
        codes = {" ": "spc", "-": "minus", "/": "slash", "\n": "ret", ".": "dot"}
        for char in text:
            shifted = char == "_"
            code = "minus" if shifted else codes.get(char, char)
            for down in (True, False):
                events = [{"type": "key", "data": {
                    "down": down, "key": {"type": "qcode", "data": code}}}]
                if shifted:
                    shift = {"type": "key", "data": {
                        "down": down, "key": {"type": "qcode", "data": "shift"}}}
                    events = [shift] + events if down else events + [shift]
                self.qmp("input-send-event", {"events": events})
                time.sleep(0.06)


def main():
    args = arguments()
    image = args.image.resolve(strict=True)
    firmware = args.firmware_code.resolve(strict=True)
    variables = args.firmware_vars.resolve(strict=True) if args.firmware_vars else None
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"result": "FAIL", "cpus": args.cpus, "mode": args.mode,
              "size_mib": args.size_mib, "image": str(image),
              "overlay": str(output / "disk.qcow2")}
    process = guest = None
    try:
        info = json.loads(subprocess.check_output(
            [args.qemu_img, "info", "--output=json", str(image)], text=True))
        create = [args.qemu_img, "create", "-f", "qcow2", "-F", info["format"],
                  "-b", str(image), str(output / "disk.qcow2")]
        report["overlay_command"] = create
        subprocess.run(create, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        shutil.copyfile(firmware, output / "firmware-code.fd")
        firmware_arg = f"if=pflash,format=raw,file={output}/firmware-code.fd"
        if variables:
            firmware_arg += ",readonly=on"
        command = [args.qemu, "-machine", "q35", "-accel", "tcg,thread=multi",
                   "-smp", str(args.cpus), "-m", "4096", "-cpu",
                   "SandyBridge,-rdrand,-rdseed", "-drive", firmware_arg]
        if variables:
            shutil.copyfile(variables, output / "firmware-vars.fd")
            command += ["-drive", f"if=pflash,format=raw,file={output}/firmware-vars.fd"]
        command += ["-drive", f"file={output}/disk.qcow2,if=ide,format=qcow2",
                    "-display", "none", "-serial", f"file:{output}/serial.log",
                    "-qmp", "stdio", "-nic", "none", "-no-reboot", "-no-shutdown"]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (output / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=log, bufsize=0)
        guest = Guest(process, output)
        boot = time.monotonic()
        guest.receive()
        guest.qmp("qmp_capabilities")
        guest.wait(b"Username:", args.boot_timeout)
        report["username_wall_s"] = time.monotonic() - boot
        guest.type("root\n")
        guest.wait(b"Password:", 30)
        guest.type("root\n")
        guest.wait(b"#\x1b[0m ", 90)
        report["shell_wall_s"] = time.monotonic() - boot
        print(f"LOGIN wall_s={report['shell_wall_s']:.3f}", flush=True)
        invocation = f"{args.guest_binary} {args.size_mib}"
        if args.mode != "full":
            invocation += " " + args.mode
        if args.keep_scratch:
            invocation += " --keep-scratch"
        report["guest_command"] = invocation
        report["blocks_before"] = guest.qmp("query-blockstats")
        mark = len(guest.serial())
        guest.type(invocation + "\n")
        guest.wait(b"IOBENCH BEGIN", 60, mark)
        begin = time.monotonic()
        guest.wait(b"IOBENCH PASS END", args.benchmark_timeout, mark)
        report["benchmark_wall_s"] = time.monotonic() - begin
        report["blocks_after"] = guest.qmp("query-blockstats")
        guest.qmp("screendump", {"filename": str(output / "success.ppm")})
        report["result"] = "PASS"
    except Exception as error:
        report["error"] = repr(error)
        if guest:
            try:
                guest.qmp("stop")
                guest.qmp("screendump", {"filename": str(output / "failure.ppm")})
                for name, monitor in (("registers", "info registers"), ("pic", "info pic")):
                    report[name] = guest.qmp("human-monitor-command", {"command-line": monitor})
            except Exception as capture_error:
                report["capture_error"] = repr(capture_error)
    finally:
        if guest:
            report["metrics"] = [line for line in guest.serial().decode(errors="replace").splitlines()
                                 if "IOBENCH " in line]
        if process:
            try:
                guest.qmp("quit")
            except Exception:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return report["result"] != "PASS"


if __name__ == "__main__":
    sys.exit(main())
