#!/usr/bin/env python3

"""Run bounded, ISO-only Pedigree UP and SMP QEMU checkpoints."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time


COMMON_MARKERS = (
    "Local APIC initialised",
    "Falling back to dual 8259 PIC Mode",
    "trace: initial init done, enabling interrupts",
    "initrd @",
)
RTC_PROGRESS_MARKERS = (
    "Rtc::initialise2",
    "TSC calibration:",
)
RTC_TIME_PROGRESS = "RTC-backed timestamps advanced by one second"
SCENARIOS = {
    "up": {
        "cpus": 1,
        "machine": "pc",
        "markers": COMMON_MARKERS
        + (
            "Multiprocessor: Found 1 processors",
            "Currently running on CPU #0, skipping boot (not necessary)",
        ),
    },
    "smp": {
        "cpus": 4,
        "machine": "pc",
        "markers": COMMON_MARKERS
        + (
            "Multiprocessor: Found 4 processors",
            "Processor #1 started.",
            "Processor #2 started.",
            "Processor #3 started.",
        ),
    },
}
FAILURE_MARKERS = (
    "panic:",
    "Page Fault Exception",
    "Double Fault Exception",
    "Triple fault",
    "application processor startup timed out",
    "did not acknowledge startup",
)
MANAGED_QEMU_OPTIONS = {
    "-blockdev",
    "-boot",
    "-cdrom",
    "-daemonize",
    "-display",
    "-drive",
    "-fsdev",
    "-hda",
    "-hdb",
    "-hdc",
    "-hdd",
    "-m",
    "-monitor",
    "-netdev",
    "-nographic",
    "-nic",
    "-qmp",
    "-serial",
    "-smp",
}


def validate_extra_args(extra_args: list[str]) -> None:
    for argument in extra_args:
        option = argument.split("=", 1)[0]
        if option in MANAGED_QEMU_OPTIONS:
            raise ValueError(
                f"extra QEMU option is managed by the ISO-only runner: {option}"
            )


def build_command(
    qemu: str,
    iso: Path,
    serial_log: Path,
    scenario: str,
    extra_args: list[str],
) -> list[str]:
    validate_extra_args(extra_args)
    cpus = SCENARIOS[scenario]["cpus"]
    machine = SCENARIOS[scenario]["machine"]
    return [
        qemu,
        "-machine",
        str(machine),
        "-smp",
        str(cpus),
        "-m",
        "512",
        "-boot",
        "order=d,strict=on",
        "-drive",
        f"file={iso},if=ide,media=cdrom,format=raw,readonly=on,index=2",
        "-display",
        "none",
        "-monitor",
        "stdio",
        "-serial",
        f"file:{serial_log}",
        "-nic",
        "none",
        "-no-reboot",
        "-no-shutdown",
        *extra_args,
    ]


def evaluate_output(
    output: str,
    scenario: str,
    require_rtc_progress: bool = False,
    required_markers: tuple[str, ...] = (),
) -> tuple[str | None, list[str]]:
    for marker in FAILURE_MARKERS:
        if marker in output:
            return marker, []
    markers = SCENARIOS[scenario]["markers"]
    if require_rtc_progress:
        markers += RTC_PROGRESS_MARKERS
    missing = [marker for marker in markers if marker not in output]
    missing.extend(marker for marker in required_markers if marker not in output)
    if require_rtc_progress:
        timestamp_seconds = [
            int(match) for match in re.findall(r"\[(\d+)\.\d+\]", output)
        ]
        if (
            not timestamp_seconds
            or max(timestamp_seconds) - min(timestamp_seconds) < 1
        ):
            missing.append(RTC_TIME_PROGRESS)
    return None, missing


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=2)


def append_result(result_log: Path, marker: str) -> None:
    with result_log.open("a", encoding="utf-8") as output:
        output.write(marker + "\n")


def fail(
    process: subprocess.Popen[bytes], result_log: Path, scenario: str, reason: str
) -> int:
    stop_process(process)
    marker = f"QEMU-ISO-CHECKPOINT: FAIL scenario={scenario} reason={reason}"
    append_result(result_log, marker)
    print(marker, file=sys.stderr)
    return 1


def run_checkpoint(args: argparse.Namespace) -> int:
    scenario = args.scenario
    iso = args.iso.resolve()
    if not iso.is_file():
        print(f"ISO artifact is unavailable: {iso}", file=sys.stderr)
        return 2

    extra_args = args.qemu_args
    if extra_args and extra_args[0] == "--":
        extra_args = extra_args[1:]
    try:
        validate_extra_args(extra_args)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2

    log_dir = args.log_dir.resolve()
    log_dir.mkdir(parents=True, exist_ok=True)
    serial_log = log_dir / f"{scenario}.serial.log"
    result_log = log_dir / f"{scenario}.result.log"
    serial_log.write_bytes(b"")
    result_log.write_text(
        f"QEMU-ISO-CHECKPOINT: START scenario={scenario} "
        f"cpus={SCENARIOS[scenario]['cpus']}\n",
        encoding="utf-8",
    )

    command = build_command(args.qemu, iso, serial_log, scenario, extra_args)
    print(f"QEMU command: {shlex.join(command)}")
    print(f"Serial log: {serial_log}")
    print(f"Result log: {result_log}")
    append_result(result_log, f"QEMU command: {shlex.join(command)}")

    with result_log.open("a", encoding="utf-8") as qemu_output:
        try:
            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=qemu_output,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        except FileNotFoundError:
            marker = (
                f"QEMU-ISO-CHECKPOINT: FAIL scenario={scenario} "
                f"reason=qemu-not-found"
            )
            append_result(result_log, marker)
            print(marker, file=sys.stderr)
            return 127

        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            output = serial_log.read_text(encoding="utf-8", errors="replace")
            failure_marker, missing = evaluate_output(
                output,
                scenario,
                args.require_rtc_progress,
                tuple(args.require_marker),
            )
            if failure_marker is not None:
                return fail(
                    process,
                    result_log,
                    scenario,
                    f"guest-marker:{failure_marker}",
                )
            if not missing:
                try:
                    if process.stdin is None:
                        raise RuntimeError("QEMU monitor input is unavailable")
                    process.stdin.write(b"quit\n")
                    process.stdin.flush()
                    process.wait(timeout=5)
                except (BrokenPipeError, RuntimeError, subprocess.TimeoutExpired) as error:
                    return fail(
                        process,
                        result_log,
                        scenario,
                        f"monitor-shutdown:{error}",
                    )
                if process.returncode != 0:
                    return fail(
                        process,
                        result_log,
                        scenario,
                        f"qemu-exit:{process.returncode}",
                    )
                marker = (
                    f"QEMU-ISO-CHECKPOINT: PASS scenario={scenario} "
                    "shutdown=monitor-quit"
                )
                append_result(result_log, marker)
                print(marker)
                return 0
            if process.poll() is not None:
                return fail(
                    process,
                    result_log,
                    scenario,
                    f"qemu-early-exit:{process.returncode};missing={','.join(missing)}",
                )
            time.sleep(0.05)

        output = serial_log.read_text(encoding="utf-8", errors="replace")
        _, missing = evaluate_output(
            output,
            scenario,
            args.require_rtc_progress,
            tuple(args.require_marker),
        )
        return fail(
            process,
            result_log,
            scenario,
            f"deadline;missing={','.join(missing)}",
        )


def self_test() -> bool:
    fake_iso = Path("/tmp/pedigree-test.iso")
    command = build_command(
        "qemu-test",
        fake_iso,
        Path("/tmp/up.serial.log"),
        "up",
        ["-S", "-gdb", "tcp::1234"],
    )
    assert command.count("-drive") == 1
    assert any("media=cdrom" in argument for argument in command)
    assert not any("hdd.img" in argument for argument in command)
    assert command[command.index("-smp") + 1] == "1"
    assert command[command.index("-machine") + 1] == "pc"
    assert command[-3:] == ["-S", "-gdb", "tcp::1234"]

    try:
        validate_extra_args(["-drive", "file=unexpected.img"])
    except ValueError:
        pass
    else:
        raise AssertionError("storage override was accepted")

    up_output = "\n".join(SCENARIOS["up"]["markers"])
    failure, missing = evaluate_output(up_output, "up")
    assert failure is None and not missing
    failure, missing = evaluate_output(
        up_output, "up", required_markers=("focused regression passed",)
    )
    assert failure is None and missing == ["focused regression passed"]
    failure, missing = evaluate_output("panic: model failure", "up")
    assert failure == "panic:" and not missing
    rtc_output = (
        "[100.0]\n"
        + up_output
        + "\n"
        + "\n".join(RTC_PROGRESS_MARKERS)
        + "\n[101.0]"
    )
    failure, missing = evaluate_output(rtc_output, "up", True)
    assert failure is None and not missing
    failure, missing = evaluate_output(up_output, "up", True)
    assert failure is None and missing == [
        *RTC_PROGRESS_MARKERS,
        RTC_TIME_PROGRESS,
    ]
    assert SCENARIOS["smp"]["cpus"] == 4
    assert SCENARIOS["smp"]["machine"] == "pc"
    assert "Processor #3 started." in SCENARIOS["smp"]["markers"]
    return True


def parse_args() -> argparse.Namespace:
    repository = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", nargs="?", choices=sorted(SCENARIOS))
    parser.add_argument("--iso", type=Path, default=repository / "build/pedigree.iso")
    parser.add_argument(
        "--log-dir", type=Path, default=repository / "build/qemu-iso-checkpoints"
    )
    parser.add_argument("--seconds", type=float, default=90)
    parser.add_argument(
        "--qemu", default=os.environ.get("QEMU", "qemu-system-x86_64")
    )
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--require-rtc-progress",
        action="store_true",
        help="wait for RTC calibration and one second of IRQ8-driven progress",
    )
    parser.add_argument(
        "--require-marker",
        action="append",
        default=[],
        help="wait for an additional serial-log marker (repeatable)",
    )
    arguments = sys.argv[1:]
    if "--" in arguments:
        separator = arguments.index("--")
        runner_args = arguments[:separator]
        qemu_args = arguments[separator + 1 :]
    else:
        runner_args = arguments
        qemu_args = []
    args = parser.parse_args(runner_args)
    args.qemu_args = qemu_args
    if args.self_test:
        if args.scenario or args.qemu_args:
            parser.error("--self-test does not accept a scenario or QEMU arguments")
        return args
    if args.scenario is None:
        parser.error("a scenario is required")
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    return args


def main() -> int:
    args = parse_args()
    if args.self_test:
        return 0 if self_test() else 1
    return run_checkpoint(args)


if __name__ == "__main__":
    raise SystemExit(main())
