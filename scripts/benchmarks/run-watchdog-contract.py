#!/usr/bin/env python3
"""Run the IB700 contract on an immutable image with a disposable QEMU overlay."""

import argparse
import hashlib
import json
import os
import select
import shutil
import signal
import subprocess
import time
from datetime import UTC, datetime
from pathlib import Path

MAX_LOG_BYTES = 64 * 1024 * 1024
MAX_QMP_MESSAGE_BYTES = 1024 * 1024


class Guest:
    def __init__(self, process, output, report, started):
        self.process, self.output, self.report, self.started = (
            process,
            output,
            report,
            started,
        )
        self.wire = bytearray()
        self.serial_wire = bytearray()
        self.serial_offset = 0
        self.ident = 0
        self.phase = "boot"
        self.ended = False

    def serial(self):
        for name in ("serial.log", "qemu.log", "qmp.jsonl"):
            log = self.output / name
            if log.exists() and log.stat().st_size > MAX_LOG_BYTES:
                raise RuntimeError(f"{name} exceeded the 64 MiB log limit")
        path = self.output / "serial.log"
        if not path.exists():
            return
        with path.open("rb") as stream:
            stream.seek(self.serial_offset)
            data = stream.read(MAX_LOG_BYTES + 1)
        if len(data) > MAX_LOG_BYTES:
            raise RuntimeError("serial input exceeded the 64 MiB log limit")
        self.serial_offset += len(data)
        self.serial_wire.extend(data)
        while b"\n" in self.serial_wire:
            line, _, rest = self.serial_wire.partition(b"\n")
            self.serial_wire[:] = rest
            line = line.decode("utf-8", "replace").strip()
            if "WATCHDOG-CONTRACT:" not in line:
                continue
            marker = {"host_wall_s": time.monotonic() - self.started, "line": line}
            self.report["serial_markers"].append(marker)
            if "BEGIN armed-refresh" in line:
                self.phase = "armed-refresh"
            elif "BEGIN disabled-after-unload" in line:
                self.phase = "disabled-after-unload"
            elif "PASS unload" in line:
                self.report["unload_wall_s"] = marker["host_wall_s"]
                self.phase = "unloaded"
            elif "PASS armed-refresh" in line:
                self.report["armed_refresh_passed"] = True
            elif "PASS disabled-after-unload" in line:
                self.report["disabled_after_unload_passed"] = True
            elif "END PASS" in line:
                self.ended = True
            elif "FAIL" in line:
                self.report["guest_failure"] = line
            print(line, flush=True)

    def receive(self, timeout):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.wire:
            ready, _, _ = select.select(
                [self.process.stdout], [], [], max(0, deadline - time.monotonic())
            )
            if not ready:
                return None
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                if self.wire:
                    raise RuntimeError("QMP closed with an incomplete message")
                raise EOFError("QMP closed")
            self.wire.extend(data)
            if len(self.wire) > MAX_QMP_MESSAGE_BYTES:
                raise RuntimeError("QMP input exceeded the 1 MiB message limit")
        line, _, rest = self.wire.partition(b"\n")
        self.wire[:] = rest
        message = json.loads(line)
        self.serial()
        record = {
            "host_wall_s": time.monotonic() - self.started,
            "serial_phase": self.phase,
            "message": message,
        }
        with (self.output / "qmp.jsonl").open("a") as log:
            log.write(json.dumps(record) + "\n")
        if "event" in message:
            self.report["qmp_events"].append(record)
            if message["event"] == "WATCHDOG":
                if "unload_wall_s" in self.report:
                    record["after_unload_wall_s"] = (
                        record["host_wall_s"] - self.report["unload_wall_s"]
                    )
                self.report["watchdogs"].append(record)
                print("WATCHDOG event " + json.dumps(record), flush=True)
        return message

    def qmp(self, command, arguments=None, timeout=10):
        self.ident += 1
        request = {"execute": command, "id": self.ident}
        if arguments is not None:
            request["arguments"] = arguments
        self.process.stdin.write((json.dumps(request) + "\n").encode())
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            response = self.receive(max(0, deadline - time.monotonic()))
            if response is None:
                break
            if response.get("id") != self.ident:
                continue
            if "error" in response:
                raise RuntimeError(response["error"])
            return response["return"]
        raise TimeoutError("QMP " + command)


def verify_outcome(report, ended, expect_expiry):
    if report.get("guest_failure"):
        raise RuntimeError(report["guest_failure"])
    watchdogs = report["watchdogs"]
    if watchdogs:
        if not expect_expiry:
            raise RuntimeError("unexpected WATCHDOG event")
        event = watchdogs[0]
        if (
            not report.get("armed_refresh_passed")
            or "unload_wall_s" not in report
            or event["serial_phase"] != "disabled-after-unload"
            or event["message"].get("data", {}).get("action") != "pause"
        ):
            raise RuntimeError(
                "watchdog expired before successful unload or did not pause"
            )
        return "expected-watchdog-after-unload"
    if ended:
        if expect_expiry:
            raise RuntimeError("guest passed without the expected WATCHDOG event")
        if (
            not report.get("armed_refresh_passed")
            or "unload_wall_s" not in report
            or not report.get("disabled_after_unload_passed")
        ):
            raise RuntimeError("guest END lacks required armed/unload/disabled phases")
        return "end-pass-without-watchdog"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument(
        "--output", type=Path, required=True, help="New output directory"
    )
    parser.add_argument("--firmware-code", type=Path, required=True)
    parser.add_argument("--firmware-vars", type=Path)
    parser.add_argument("--cpus", type=int, choices=(1, 4), default=1)
    parser.add_argument(
        "--expect-expiry",
        action="store_true",
        help="Require an actual WATCHDOG pause after module unload",
    )
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--qemu-img", default="qemu-img")
    args = parser.parse_args()
    if not 0 < args.timeout <= 120:
        parser.error("timeout must be positive and at most 120 seconds")
    image = args.image.resolve(strict=True)
    firmware = args.firmware_code.resolve(strict=True)
    variables = args.firmware_vars.resolve(strict=True) if args.firmware_vars else None
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output / "disk.qcow2"
    started = time.monotonic()
    report = {
        "result": "RUNNING",
        "image": str(image),
        "overlay": str(disk),
        "cpus": args.cpus,
        "expect_expiry": args.expect_expiry,
        "timeout_s": args.timeout,
        "started_utc": datetime.now(UTC).isoformat(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "serial_markers": [],
        "qmp_events": [],
        "watchdogs": [],
    }
    process = guest = None

    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"signal {signum}")

    previous_sigterm = signal.signal(signal.SIGTERM, interrupted)

    def save():
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")

    try:
        info = json.loads(
            subprocess.check_output(
                [args.qemu_img, "info", "--output=json", str(image)],
                text=True,
                timeout=15,
            )
        )
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
            timeout=15,
        )
        report["qemu_version"] = subprocess.check_output(
            [args.qemu, "--version"], text=True, timeout=10
        ).splitlines()[0]
        shutil.copyfile(firmware, output / "firmware-code.fd")
        flash = f"if=pflash,format=raw,file={output}/firmware-code.fd"
        if variables:
            flash += ",readonly=on"
        command = [
            args.qemu,
            "-machine",
            "q35",
            "-accel",
            "tcg,thread=multi",
            "-smp",
            str(args.cpus),
            "-m",
            "4096",
            "-cpu",
            "SandyBridge,-rdrand,-rdseed",
            "-drive",
            flash,
        ]
        if variables:
            shutil.copyfile(variables, output / "firmware-vars.fd")
            command += [
                "-drive",
                f"if=pflash,format=raw,file={output}/firmware-vars.fd",
            ]
        command += [
            "-drive",
            f"file={disk},if=ide,format=qcow2",
            "-device",
            "ib700",
            "-watchdog-action",
            "pause",
            "-display",
            "none",
            "-serial",
            f"file:{output}/serial.log",
            "-qmp",
            "stdio",
            "-nic",
            "none",
            "-no-reboot",
            "-no-shutdown",
            "-S",
        ]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        save()
        with (output / "qemu.log").open("w") as log:
            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=log,
                bufsize=0,
            )
        guest = Guest(process, output, report, started)
        greeting = guest.receive(10)
        if not greeting or "QMP" not in greeting:
            raise RuntimeError("missing QMP greeting")
        guest.qmp("qmp_capabilities")
        report["boot_wall_s"] = time.monotonic() - started
        guest.qmp("cont")
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            guest.serial()
            guest.receive(min(0.1, max(0, deadline - time.monotonic())))
            outcome = verify_outcome(report, guest.ended, args.expect_expiry)
            if outcome:
                report["final_qemu_status"] = guest.qmp("query-status")
                report["outcome"] = verify_outcome(
                    report, guest.ended, args.expect_expiry
                )
                report["result"] = "PASS"
                break
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before the expected outcome")
            save()
        else:
            raise TimeoutError("watchdog contract deadline")
    except (
        OSError,
        EOFError,
        RuntimeError,
        ValueError,
        KeyError,
        subprocess.SubprocessError,
        KeyboardInterrupt,
    ) as error:
        report["result"] = "FAIL"
        report["error"] = repr(error)
    finally:
        try:
            if process is not None:
                if process.poll() is None and guest:
                    try:
                        guest.qmp("quit", timeout=3)
                    except (
                        OSError,
                        EOFError,
                        RuntimeError,
                        ValueError,
                        KeyError,
                    ) as error:
                        report["quit_error"] = repr(error)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
                report["qemu_exit_code"] = process.returncode
                if guest:
                    guest.serial()
                    while True:
                        try:
                            if guest.receive(0) is None:
                                break
                        except EOFError:
                            break
                if process.returncode:
                    raise RuntimeError(f"QEMU exited with status {process.returncode}")
            if report["result"] == "PASS":
                report["outcome"] = verify_outcome(
                    report, guest.ended, args.expect_expiry
                )
        except (
            OSError,
            EOFError,
            RuntimeError,
            ValueError,
            KeyError,
            subprocess.SubprocessError,
        ) as error:
            report["result"] = "FAIL"
            report["cleanup_error"] = repr(error)
        finally:
            signal.signal(signal.SIGTERM, previous_sigterm)
        if report["result"] != "PASS":
            report.pop("outcome", None)
        report["total_wall_s"] = time.monotonic() - started
        save()
    print(
        json.dumps(
            {
                key: report[key]
                for key in ("result", "outcome", "error")
                if key in report
            }
        )
    )
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
