#!/usr/bin/env python3

"""Run one command with a deadline and retire its complete process group."""

import argparse
import os
import signal
import subprocess
import sys
import time


def stop_process_group(process: subprocess.Popen, grace_seconds: float) -> None:
    if process.poll() is not None:
        return

    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=grace_seconds)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
        process.wait()


def run(
    command: list[str],
    seconds: float,
    grace_seconds: float,
    label: str,
    report_timeout: bool = True,
) -> int:
    try:
        process = subprocess.Popen(command, start_new_session=True)
    except FileNotFoundError:
        print(f"Command not found for {label}: {command[0]}", file=sys.stderr)
        return 127

    try:
        return process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        if report_timeout:
            print(
                f"{label} exceeded its {seconds:g}-second deadline; "
                "terminating its process group.",
                file=sys.stderr,
            )
        stop_process_group(process, grace_seconds)
        return 124
    except KeyboardInterrupt:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGINT)
            except ProcessLookupError:
                return process.returncode or 130
            try:
                process.wait(timeout=grace_seconds)
            except subprocess.TimeoutExpired:
                stop_process_group(process, grace_seconds)
        return 130


def self_test() -> bool:
    quick = run(
        [sys.executable, "-c", "raise SystemExit(7)"],
        2,
        0.1,
        "deadline helper quick-exit self-test",
    )
    started = time.monotonic()
    timed_out = run(
        [sys.executable, "-c", "import time; time.sleep(5)"],
        0.05,
        0.05,
        "deadline helper timeout self-test",
        False,
    )
    elapsed = time.monotonic() - started
    if quick != 7 or timed_out != 124 or elapsed >= 2:
        print(
            "Deadline helper self-test failed: "
            f"quick={quick}, timeout={timed_out}, elapsed={elapsed:.3f}",
            file=sys.stderr,
        )
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float)
    parser.add_argument("--grace-seconds", type=float, default=5)
    parser.add_argument("--label", default="command")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    if args.self_test:
        if args.command or args.seconds is not None:
            parser.error("--self-test does not accept a command or --seconds")
        return 0 if self_test() else 1

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if args.seconds is None or args.seconds <= 0:
        parser.error("--seconds must be positive")
    if args.grace_seconds <= 0:
        parser.error("--grace-seconds must be positive")
    if not command:
        parser.error("a command is required after --")

    return run(command, args.seconds, args.grace_seconds, args.label)


if __name__ == "__main__":
    raise SystemExit(main())
