#!/usr/bin/env python3

"""Run one command with a deadline and retire its complete process group."""

import argparse
import os
import signal
import subprocess
import sys
import tempfile
import time


def process_group_exists(process_group: int) -> bool:
    try:
        os.killpg(process_group, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # A process group created by this helper should remain signalable, but
        # conservatively treat an unexpected permission failure as still live.
        return True
    return True


def wait_for_process_group_exit(process_group: int, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while process_group_exists(process_group):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(0.01, remaining))
    return True


def reap_leader(process: subprocess.Popen, seconds: float) -> None:
    if process.poll() is not None:
        return
    try:
        process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        pass


def stop_process_group(
    process: subprocess.Popen, process_group: int, grace_seconds: float
) -> bool:
    """Retire every member, even if the process-group leader has already exited."""
    if not process_group_exists(process_group):
        return True

    try:
        os.killpg(process_group, signal.SIGTERM)
    except ProcessLookupError:
        return True
    except PermissionError:
        # Darwin can transiently report EPERM while it reaps an exited
        # session leader. Verify the group before declaring cleanup failed.
        return wait_for_process_group_exit(process_group, grace_seconds)
    reap_leader(process, min(grace_seconds, 0.1))
    if wait_for_process_group_exit(process_group, grace_seconds):
        return True

    try:
        os.killpg(process_group, signal.SIGKILL)
    except ProcessLookupError:
        return True
    except PermissionError:
        return wait_for_process_group_exit(process_group, grace_seconds)
    reap_leader(process, min(grace_seconds, 0.1))
    return wait_for_process_group_exit(process_group, grace_seconds)


def run(
    command: list[str],
    seconds: float,
    grace_seconds: float,
    label: str,
    report_timeout: bool = True,
    report_group_leak: bool = True,
) -> int:
    try:
        process = subprocess.Popen(command, start_new_session=True)
    except FileNotFoundError:
        print(f"Command not found for {label}: {command[0]}", file=sys.stderr)
        return 127

    process_group = process.pid
    try:
        returncode = process.wait(timeout=seconds)
        if not process_group_exists(process_group):
            return returncode
        retired = stop_process_group(process, process_group, grace_seconds)
        if report_group_leak:
            print(
                f"{label} exited but left processes in its process group; "
                + (
                    "they were forcibly retired."
                    if retired
                    else "forced termination could not retire the group."
                ),
                file=sys.stderr,
            )
        return 125
    except subprocess.TimeoutExpired:
        if report_timeout:
            print(
                f"{label} exceeded its {seconds:g}-second deadline; "
                "terminating its process group.",
                file=sys.stderr,
            )
        if not stop_process_group(process, process_group, grace_seconds):
            print(
                f"{label} process group remained live after forced termination.",
                file=sys.stderr,
            )
        return 124
    except KeyboardInterrupt:
        try:
            os.killpg(process_group, signal.SIGINT)
        except ProcessLookupError:
            pass
        if not wait_for_process_group_exit(process_group, grace_seconds):
            stop_process_group(process, process_group, grace_seconds)
        reap_leader(process, min(grace_seconds, 0.1))
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
    with tempfile.TemporaryDirectory(prefix="pedigree-deadline-") as temp:
        child_pid_file = os.path.join(temp, "child.pid")
        orphaned_group = run(
            [
                sys.executable,
                "-c",
                (
                    "import pathlib, signal, subprocess, sys; "
                    "child = subprocess.Popen([sys.executable, '-c', "
                    "'import signal, time; "
                    "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                    "time.sleep(30)']); "
                    "pathlib.Path(sys.argv[1]).write_text(str(child.pid))"
                ),
                child_pid_file,
            ],
            2,
            0.1,
            "deadline helper orphaned-child self-test",
            False,
            False,
        )
        try:
            child_pid = int(open(child_pid_file, encoding="utf-8").read())
        except (FileNotFoundError, ValueError):
            child_pid = 0

        child_still_live = child_pid != 0
        deadline = time.monotonic() + 1
        while child_still_live and time.monotonic() < deadline:
            child_still_live = pid_is_live(child_pid)
            time.sleep(0.01)

    if (
        quick != 7
        or timed_out != 124
        or elapsed >= 2
        or orphaned_group != 125
        or child_still_live
    ):
        print(
            "Deadline helper self-test failed: "
            "quick="
            f"{quick}, timeout={timed_out}, elapsed={elapsed:.3f}, "
            f"orphaned_group={orphaned_group}, child_still_live={child_still_live}",
            file=sys.stderr,
        )
        return False
    return True


def pid_is_live(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False

    # A SIGKILLed orphan may briefly remain a zombie until PID 1 reaps it.
    # That process cannot execute and is not a descendant leak.
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8") as status:
            state = status.read().rsplit(")", 1)[1].strip().split(maxsplit=1)[0]
            return state != "Z"
    except (FileNotFoundError, IndexError):
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
