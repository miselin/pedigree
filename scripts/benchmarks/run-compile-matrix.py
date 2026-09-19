#!/usr/bin/env python3
"""Run the compiler stage matrix in a disposable one-CPU guest."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import shlex
import shutil
import struct
import subprocess
import time

SPEC = importlib.util.spec_from_file_location(
    "compile_latency", Path(__file__).with_name("run-compile-latency.py"))
COMPILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPILE)
LAUNCH = COMPILE.LAUNCH
CASES = ["tiny", "tiny-pipe", "preprocess", "syntax", "codegen", "assemble",
         "link", "full", "full-pipe"]


def phases(mode):
    if mode == "install":
        return []
    if mode == "trace-link":
        return ["warm-link", "trace-link"]
    if mode == "prepare":
        return ["prepare-preprocess", "prepare-codegen", "prepare-assemble"]
    result = ["cpu-before"]
    for prefix, cases in (("warm", CASES), ("r1", CASES),
                          ("r2", list(reversed(CASES))), ("r3", CASES)):
        result.extend(f"{prefix}-{case}" for case in cases)
    return result + ["cpu-after"]


def input_identity(line):
    match = re.fullmatch(r"COMPILEBENCH input stage=(before|after|prepared|output) "
                         r"path=(\S+) bytes=(\d+) fnv1a64=([0-9a-f]{16})", line)
    if not match:
        raise ValueError(f"malformed input identity: {line}")
    return {"stage": match[1], "path": match[2], "bytes": int(match[3]), "fnv1a64": match[4]}


def validate_identities(identities, mode):
    if mode == "install":
        if identities:
            raise ValueError("unexpected input identities during installation")
        return
    stages = {stage: {} for stage in ("before", "after", "prepared")}
    for item in identities:
        if item["stage"] == "output":
            continue
        stage = stages[item["stage"]]
        if item["path"] in stage:
            raise ValueError("duplicate fixture identity")
        stage[item["path"]] = (item["bytes"], item["fnv1a64"])
    expected = {"which.cc", "tiny.cc"}
    generated = {"which.ii", "which.s", "which.o"}
    if mode in ("run", "trace-link"):
        expected |= generated
        generated = set()
    if (set(stages["before"]) != expected or stages["before"] != stages["after"] or
            set(stages["prepared"]) != generated):
        raise ValueError("incomplete or changed fixture identities")


def linux_bootstrap(args, partition=1):
    setup = ["mkdir -p /mnt/pedigree",
             f"mount -t ext2 /dev/sdb{partition} /mnt/pedigree"]
    if args.setup_iso:
        setup += ["mkdir -p /mnt/setup", "mount -o ro /dev/sr0 /mnt/setup",
                  "cp -a /mnt/setup/root/. /mnt/pedigree/"]
    if args.mode == "install":
        setup += ["echo COMPILEBENCH configuration mode=install profile=none",
                  "echo COMPILEBENCH PASS END"]
        cleanup = ["sync", "umount /mnt/pedigree", "umount /mnt/setup"]
        return ("MATRIX_RC=0; { " + " && ".join(setup) + "; } || MATRIX_RC=$?; " +
                "; ".join(f"{command} || MATRIX_RC=$?" for command in cleanup) +
                "; echo MATRIX-LINUX-CLEAN-END rc=$MATRIX_RC\n")
    setup += ["mkdir -p /mnt/pedigree/dev /mnt/pedigree/proc /mnt/pedigree/tmp",
              "mount --bind /dev /mnt/pedigree/dev",
              "mount -t proc proc /mnt/pedigree/proc",
              "mount -t tmpfs tmpfs /mnt/pedigree/tmp"]
    command = ["/usr/bin/env", "-i", "HOME=/root", "TMPDIR=/tmp",
               "PATH=/usr/bin:/bin", "LC_ALL=C",
               "CPLUS_INCLUDE_PATH=/usr/include/c++/15.3.0:"
               "/usr/include/c++/15.3.0/x86_64-pedigree"]
    if args.profile_phase:
        command.append(f"MATRIX_PROFILE={args.profile_phase}")
    command += ["/usr/sbin/chroot", "/mnt/pedigree"]
    if args.storage == "ramfs":
        command += ["/root/compile-bench/compile-matrix-ramroot", "--stdio"]
    else:
        command += ["/root/compile-bench/compile-matrix", f"--{args.mode}", "--stdio"]
    setup.append(shlex.join(command))
    # The prepared overlay becomes a shared backing image. PASS alone is not
    # sufficient: Linux must finish writeback and detach it before QEMU quits.
    cleanup = ["sync"]
    if args.storage == "ramfs":
        cleanup += ["umount /mnt/pedigree/tmp/compile-matrix-root/proc",
                    "umount /mnt/pedigree/tmp/compile-matrix-root"]
    cleanup += [f"umount /mnt/pedigree/{name}" for name in ("tmp", "proc", "dev")]
    cleanup.append("umount /mnt/pedigree")
    if args.setup_iso:
        cleanup.append("umount /mnt/setup")
    return ("MATRIX_RC=0; { " + " && ".join(setup) + "; } || MATRIX_RC=$?; " +
            "; ".join(f"{command} || MATRIX_RC=$?" for command in cleanup) +
            "; echo MATRIX-LINUX-CLEAN-END rc=$MATRIX_RC\n")


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--os", choices=("linux", "pedigree"), required=True)
    parser.add_argument("--mode", choices=("prepare", "run", "install", "trace-link"), default="run")
    parser.add_argument("--storage", choices=("disk", "ramfs"), default="disk")
    parser.add_argument("--firmware-code", type=Path)
    parser.add_argument("--linux-root", type=Path)
    parser.add_argument("--linux-kernel", type=Path)
    parser.add_argument("--linux-initrd", type=Path)
    parser.add_argument("--setup-iso", type=Path)
    parser.add_argument("--profile-phase", choices=phases("run") + phases("prepare") + ["trace-link"])
    parser.add_argument("--plugin", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--boot-timeout", type=float, default=240)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--qemu-img", default="qemu-img")
    args = parser.parse_args()
    if args.os == "pedigree" and args.firmware_code is None:
        parser.error("--firmware-code is required for Pedigree")
    if args.os == "linux" and not all((args.linux_root, args.linux_kernel, args.linux_initrd)):
        parser.error("Linux requires --linux-root, --linux-kernel and --linux-initrd")
    if args.setup_iso and (args.os != "linux" or args.mode not in ("prepare", "install")):
        parser.error("--setup-iso requires Linux prepare or install mode")
    if args.mode == "install" and not args.setup_iso:
        parser.error("--mode install requires --setup-iso")
    if args.mode == "trace-link" and (args.os != "pedigree" or args.storage != "ramfs"):
        parser.error("--mode trace-link requires Pedigree and --storage ramfs")
    if args.os == "pedigree" and args.mode not in ("run", "trace-link"):
        parser.error("prepare/install runs on Linux so the shared fixture can be flushed")
    if args.storage == "ramfs" and args.mode not in ("run", "trace-link"):
        parser.error("--storage ramfs requires run or trace-link mode")
    if args.profile_phase and args.profile_phase not in phases(args.mode):
        parser.error("--profile-phase does not belong to the selected mode")
    if min(args.timeout, args.boot_timeout) <= 0:
        parser.error("timeouts must be positive")
    return args


def create_overlay(args, backing, destination):
    info = json.loads(subprocess.check_output(
        [args.qemu_img, "info", "--output=json", str(backing)], text=True))
    if info["format"] not in ("raw", "qcow2"):
        raise ValueError(f"unsupported image format: {info['format']}")
    subprocess.run([args.qemu_img, "create", "-f", "qcow2", "-F", info["format"],
                    "-b", str(backing), str(destination)], check=True, capture_output=True)
    return info


def block_requests(snapshot):
    fields = ("rd_operations", "wr_operations", "flush_operations")
    result = {entry["device"]: tuple(entry["stats"][key] for key in fields)
              for entry in snapshot}
    if not result or len(result) != len(snapshot):
        raise ValueError("missing or duplicate QMP block devices")
    return result


def settle_disks(guest):
    before = guest.qmp("query-blockstats")
    quiet = 0
    for _ in range(30):
        time.sleep(1)
        after = guest.qmp("query-blockstats")
        quiet = quiet + 1 if block_requests(before) == block_requests(after) else 0
        if quiet == 2:
            return after
        before = after
    raise RuntimeError("disk activity did not settle before RAM measurement")


def main():
    args = arguments()
    image = args.image.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    expected = phases(args.mode)
    disk = output / "pedigree.qcow2"
    source_files = [Path(__file__), Path(COMPILE.__file__), Path(LAUNCH.__file__),
                    Path(LAUNCH.IO.__file__), Path(__file__).with_name("compile-matrix.c")]
    if args.storage == "ramfs":
        source_files += [Path(__file__).with_name("compile-matrix-ramroot.c"),
                         Path(__file__).with_name("compile-matrix-ramroot-paths.txt")]
    report = {"result": "FAIL", "os": args.os, "mode": args.mode, "cpus": 1,
              "image": str(image), "overlay": str(disk), "expected_phases": expected,
              "storage": args.storage,
              "profile_phase": args.profile_phase,
              "instrumented": args.mode == "trace-link" or bool(args.plugin or args.profile_phase),
              "phases": [], "identities": [], "records": [], "source_sha256": {
                  str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in source_files if path.exists()}}
    process = guest = current = None
    serial_base = serial_input_fd = serial_output_fd = None
    started = time.monotonic()
    try:
        report["qemu_version"] = subprocess.check_output(
            [args.qemu, "--version"], text=True).splitlines()[0]
        report["image_info"] = create_overlay(args, image, disk)
        command = [args.qemu, "-machine", "q35", "-accel", "tcg,thread=multi",
                   "-smp", "1", "-m", "4096", "-cpu", "SandyBridge,-rdrand,-rdseed"]
        if args.os == "pedigree":
            firmware = args.firmware_code.resolve(strict=True)
            shutil.copyfile(firmware, output / "firmware-code.fd")
            report["firmware_sha256"] = hashlib.sha256(firmware.read_bytes()).hexdigest()
            command += ["-drive", f"if=pflash,format=raw,file={output}/firmware-code.fd",
                        "-drive", f"file={disk},if=ide,format=qcow2,cache=writeback,aio=threads"]
        else:
            mbr_path = output / "benchmark-mbr.bin"
            subprocess.run([args.qemu_img, "dd", "-f", report["image_info"]["format"],
                            f"if={image}", f"of={mbr_path}", "bs=512", "count=1"],
                           check=True, capture_output=True)
            mbr = mbr_path.read_bytes()
            if len(mbr) != 512 or mbr[510:] != b"\x55\xaa":
                raise ValueError("benchmark image requires a valid MBR")
            partitions = [i + 1 for i in range(4) if mbr[446 + 16 * i + 4] == 0x83]
            if len(partitions) != 1:
                raise ValueError("benchmark image requires one MBR Linux filesystem partition")
            partition = partitions[0]
            lba, sectors = struct.unpack_from("<II", mbr, 446 + 16 * (partition - 1) + 8)
            report["benchmark_partition"] = {"number": partition, "lba": lba, "sectors": sectors}
            linux_root = args.linux_root.resolve(strict=True)
            linux_disk = output / "linux.qcow2"
            report["linux_root_info"] = create_overlay(args, linux_root, linux_disk)
            kernel = args.linux_kernel.resolve(strict=True)
            initrd = args.linux_initrd.resolve(strict=True)
            report["linux_boot_sha256"] = {
                str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in (kernel, initrd)}
            # The boot disk is separate from the shared compiler filesystem.
            # Both kernels use matching benchmark-disk cache/AIO settings.
            report["linux_control_note"] = (
                "Separate Linux boot disk; "
                "shared benchmark root and normalized compiler argv/environment.")
            command += ["-kernel", str(kernel), "-initrd", str(initrd), "-append",
                        "root=/dev/sda rootfstype=ext2 rw init=/bin/sh "
                        "console=ttyS0,115200n8 panic=-1",
                        "-drive", f"file={linux_disk},if=ide,index=0,format=qcow2,"
                        "cache=writeback,aio=threads",
                        "-drive", f"file={disk},if=ide,index=1,format=qcow2,"
                        "cache=writeback,aio=threads"]
            if args.setup_iso:
                iso = args.setup_iso.resolve(strict=True)
                report["setup_iso_sha256"] = hashlib.sha256(iso.read_bytes()).hexdigest()
                command += ["-drive", f"file={iso},if=ide,index=2,media=cdrom,"
                            "readonly=on,format=raw"]
            (output / "bootstrap.sh").write_text(linux_bootstrap(args, partition))
        for plugin in args.plugin:
            command += ["-plugin", plugin]
        serial_base, serial_input_fd, serial_output_fd = COMPILE.open_serial_fifo(output)
        command += ["-display", "none", "-chardev", f"pipe,id=bench,path={serial_base}",
                    "-serial", "chardev:bench", "-qmp", "stdio", "-nic", "none",
                    "-no-reboot", "-no-shutdown", "-S"]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        (output / "options.json").write_text(json.dumps(vars(args), default=str, indent=2) + "\n")
        COMPILE.write_report(output, report, result="RUNNING")
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
        shell_tail = b""
        bootstrapped = args.os == "pedigree"
        matrix_passed = passed = False
        with (output / "serial.log").open("wb") as log:
            while not passed:
                now = time.monotonic()
                if now >= deadline:
                    raise TimeoutError("guest matrix deadline")
                if not report["phases"] and current is None and now - boot >= args.boot_timeout:
                    raise TimeoutError("guest matrix did not reach its first phase")
                ready, _, _ = select.select([serial_output_fd], [], [], min(1, deadline - now))
                if not ready:
                    if process.poll() is not None:
                        raise RuntimeError("QEMU exited before matrix completion")
                    continue
                data = os.read(serial_output_fd, 65536)
                log.write(data)
                log.flush()
                if not bootstrapped:
                    shell_tail = (shell_tail + data)[-4096:]
                    if re.search(rb"(?:^|[\r\n])[^\r\n]{0,160}# $", shell_tail):
                        os.write(serial_input_fd, linux_bootstrap(args, partition).encode())
                        report["linux_bootstrap_host_s"] = time.monotonic() - boot
                        bootstrapped = True
                if b"Breakpoint exception." in wire + data or b"Page Fault Exception" in wire + data:
                    raise RuntimeError("guest entered the kernel debugger")
                for line in LAUNCH.serial_lines(
                        wire, data, allow_kernel_log=args.mode == "trace-link"):
                    if "PANIC:" in line or line.startswith("Kernel panic"):
                        raise RuntimeError(line)
                    if line.startswith("COMPILEBENCH FAIL"):
                        raise RuntimeError(line)
                    if line.startswith("COMPILEBENCH input "):
                        report["identities"].append(input_identity(line))
                    elif line.startswith("COMPILEBENCH "):
                        report["records"].append(line)
                    match = re.fullmatch(r"COMPILEBENCH RAMROOT READY files=(\d+) bytes=(\d+) fnv1a64=([0-9a-f]{16})", line)
                    if match:
                        if args.storage != "ramfs" or "ramroot" in report:
                            raise RuntimeError("unexpected RAM-root readiness")
                        report["ramroot"] = {"files": int(match[1]), "bytes": int(match[2]),
                                             "fnv1a64": match[3]}
                    match = re.fullmatch(r"COMPILEBENCH configuration mode=(\S+) profile=(\S+)", line)
                    if match:
                        if ("configuration" in report or match[1] != args.mode or
                                match[2] != (args.profile_phase or "none")):
                            raise RuntimeError("unexpected guest mode or profile selection")
                        report["configuration"] = {"mode": match[1], "profile": match[2]}
                    match = re.fullmatch(r"COMPILEBENCH READY phase=(\S+)", line)
                    if match:
                        index = len(report["phases"])
                        if current is not None or index >= len(expected) or match[1] != expected[index]:
                            raise RuntimeError(f"unexpected phase: {line}")
                        current = {"phase": match[1], "gate_acknowledged": False,
                                   "ready_host_s": time.monotonic() - boot}
                        if args.storage == "ramfs" and index == 0:
                            if "ramroot" not in report:
                                raise RuntimeError("RAM-root setup was not verified")
                            report["ramroot_settled_blocks"] = settle_disks(guest)
                        current["blocks_before"] = guest.qmp("query-blockstats")
                        current["irq_before"] = guest.qmp(
                            "human-monitor-command", {"command-line": "info irq"})
                        os.write(serial_input_fd, b"g")
                        current["started_monotonic"] = time.monotonic()
                        COMPILE.write_report(output, report, result="RUNNING", current=current)
                    match = re.fullmatch(r"COMPILEBENCH ACK phase=(\S+)", line)
                    if match:
                        COMPILE.validate_phase_event(current, match[1], "ACK")
                        current["gate_acknowledged"] = True
                        current["gate_ack_wall_s"] = time.monotonic() - current["started_monotonic"]
                    match = re.fullmatch(r"COMPILEBENCH metric phase=(\S+) (.+)", line)
                    if match:
                        COMPILE.validate_phase_event(current, match[1], "metric")
                        fields = [item.split("=", 1) for item in match[2].split()]
                        metrics = {key: int(value) for key, value in fields}
                        if len(metrics) != len(fields) or "rc" not in metrics:
                            raise RuntimeError("duplicate metric key or missing phase status")
                        current["metric"] = metrics
                    match = re.fullmatch(r"COMPILEBENCH DONE phase=(\S+)", line)
                    if match:
                        COMPILE.validate_phase_event(current, match[1], "DONE")
                        current["host_wall_s"] = time.monotonic() - current.pop("started_monotonic")
                        current["blocks_after"] = guest.qmp("query-blockstats")
                        current["irq_after"] = guest.qmp(
                            "human-monitor-command", {"command-line": "info irq"})
                        current["block_delta"] = LAUNCH.block_delta(
                            current["blocks_before"], current["blocks_after"])
                        report["phases"].append(current)
                        COMPILE.write_report(output, report, result="RUNNING")
                        print(json.dumps({"phase": current["phase"], "host_wall_s": current["host_wall_s"],
                                          **current["metric"]}), flush=True)
                        if current["metric"]["rc"]:
                            raise RuntimeError(f"nonzero phase exit status: {current['phase']}")
                        if args.storage == "ramfs" and block_requests(current["blocks_before"]) != block_requests(current["blocks_after"]):
                            raise RuntimeError(f"disk requests during RAM phase: {current['phase']}")
                        current = None
                    if line == "COMPILEBENCH PASS END":
                        if matrix_passed or current is not None or len(report["phases"]) != len(expected):
                            raise RuntimeError("duplicate or premature success marker")
                        if "configuration" not in report:
                            raise RuntimeError("missing guest configuration")
                        validate_identities(report["identities"], args.mode)
                        if args.storage == "ramfs" and block_requests(report["ramroot_settled_blocks"]) != block_requests(report["phases"][-1]["blocks_after"]):
                            raise RuntimeError("disk requests between RAM phases")
                        matrix_passed = True
                        passed = args.os == "pedigree"
                    match = re.fullmatch(r"MATRIX-LINUX-CLEAN-END rc=(\d+)", line)
                    if match:
                        report["linux_cleanup_rc"] = int(match[1])
                        if not matrix_passed or int(match[1]):
                            raise RuntimeError("Linux setup, matrix, or clean unmount failed")
                        passed = True
        report["result"] = "PASS"
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = repr(error)
        if guest and process.poll() is None:
            try:
                guest.qmp("stop")
                report["registers"] = guest.qmp(
                    "human-monitor-command", {"command-line": "info registers -a"})
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
        COMPILE.close_serial_fifo(serial_base, serial_input_fd, serial_output_fd)
        report["total_host_wall_s"] = time.monotonic() - started
        COMPILE.write_report(output, report, current=current)
    print(json.dumps({key: report[key] for key in ("result", "os", "mode", "total_host_wall_s")}), flush=True)
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
