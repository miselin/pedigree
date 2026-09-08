#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Validate modern storage using a fresh UEFI image and disposable QEMU disks."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import signal
import struct
import subprocess
import time
import uuid
import zlib

SPEC = importlib.util.spec_from_file_location("ahci_smoke", Path(__file__).with_name("test_qemu_ahci.py"))
ahci = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ahci)
DISK_SIZE = 32 * 1024 * 1024


def nvme_chunk(offset, length, sector, written=False):
    value = bytearray(ahci.pattern(offset, length, 0x5a))
    if offset < 4096:
        header = bytearray(4096)
        header[:len(b"PEDIGREE-NVME-SMOKE-v1")] = b"PEDIGREE-NVME-SMOKE-v1"
        header[32] = 1
        struct.pack_into("<QI", header, 40, DISK_SIZE, sector)
        end = min(4096, offset + length)
        value[:end - offset] = header[offset:end]
    if written:
        for start, count in ((8 * 1024 * 1024, 65536), (16 * 1024 * 1024, 4096),
                             (DISK_SIZE - sector, sector)):
            left, right = max(start, offset), min(start + count, offset + length)
            if left < right:
                value[left - offset:right - offset] = ahci.pattern(left, right - left, 0xa5)
    return bytes(value)


def nvme_fixture(path, sector, verify=False):
    with path.open("rb" if verify else "xb") as stream:
        for offset in range(0, DISK_SIZE, 1024 * 1024):
            expected = nvme_chunk(offset, 1024 * 1024, sector, verify)
            if verify:
                if stream.read(len(expected)) != expected:
                    raise RuntimeError(f"NVMe backing image differs at chunk {offset}: {path.name}")
            else:
                stream.write(expected)
        if verify and stream.read(1):
            raise RuntimeError("NVMe image size changed")


def verify_nvme_flushes(trace):
    namespaces = {int(value, 0) for value in
                  re.findall(r"pci_nvme_flush_ns nsid (0x[0-9a-fA-F]+|[0-9]+)\b", trace)}
    if not {7, 23}.issubset(namespaces):
        raise RuntimeError("missing guest NVMe flush for a scratch namespace")


def ncq_maximum(trace):
    active = set()
    maximum = 0
    for line in trace.splitlines():
        match = re.search(r"(process_ncq_command|ncq_finish) ahci\([^)]*\)\[1\]\[tag:(\d+)\]", line)
        if match:
            tag = int(match.group(2))
            if match.group(1) == "process_ncq_command":
                if tag in active:
                    raise RuntimeError("NCQ tag reused before completion")
                active.add(tag)
                maximum = max(maximum, len(active))
            else:
                active.discard(tag)
    return maximum


def copy_range(source, target, offset, length):
    source.seek(offset)
    while length:
        chunk = source.read(min(length, 1024 * 1024))
        if not chunk:
            raise RuntimeError("source image truncated")
        if chunk.strip(b"\0"):
            target.write(chunk)
        else:
            target.seek(len(chunk), 1)
        length -= len(chunk)


def gpt_root(source, destination, boot_destination):
    """Keep the ESP on a 512-byte disk and put the root on a 4Kn GPT disk."""
    with source.open("rb") as stream:
        mbr = bytearray(stream.read(512))
        root_start, root_blocks = struct.unpack_from("<II", mbr, 446 + 8)
        esp_start, esp_blocks = struct.unpack_from("<II", mbr, 462 + 8)
        if mbr[450] != 0x83 or mbr[466] != 0xef or mbr[510:] != b"\x55\xaa":
            raise RuntimeError("expected the Pedigree UEFI root+ESP MBR layout")
        root_size = root_blocks * 512
        if root_size % 4096:
            raise RuntimeError("root filesystem size is not 4Kn aligned")
        with boot_destination.open("xb") as boot:
            copy_range(stream, boot, 0, (esp_start + esp_blocks) * 512)
            boot.truncate()
            mbr[446:462] = bytes(16)
            boot.seek(0)
            boot.write(mbr)
        sector = 4096
        first = 256
        end = first + root_size // sector - 1
        sectors = end + 257
        entries = bytearray(16384)
        entries[:16] = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
        entries[16:32] = uuid.UUID("28ab32fd-96a1-41a8-b105-ed354d915bfd").bytes_le
        struct.pack_into("<QQ", entries, 32, first, end)
        disk_guid = uuid.UUID("68b9bfe9-79df-4606-af10-e2fbb3cd713e").bytes_le
        def header(backup):
            data = bytearray(sector)
            struct.pack_into("<8sIIIIQQQQ16sQIII", data, 0, b"EFI PART", 0x10000, 92, 0, 0,
                             sectors - 1 if backup else 1, 1 if backup else sectors - 1,
                             6, sectors - 6, disk_guid, sectors - 5 if backup else 2,
                             128, 128, zlib.crc32(entries))
            struct.pack_into("<I", data, 16, zlib.crc32(data[:92]))
            return data
        with destination.open("xb") as disk:
            disk.truncate(sectors * sector)
            protective = bytearray(sector)
            protective[450] = 0xee
            struct.pack_into("<II", protective, 454, 1, sectors - 1)
            protective[510:512] = b"\x55\xaa"
            disk.write(protective)
            disk.write(header(False))
            disk.write(entries)
            disk.seek(first * sector)
            copy_range(stream, disk, root_start * 512, root_size)
            disk.seek((sectors - 5) * sector)
            disk.write(entries)
            disk.write(header(True))


def command(args, folder):
    def drive(path, name, snapshot=False):
        value = f"file={str(path).replace(',', ',,')},format=raw,if=none,id={name},cache=writeback"
        return value + (",snapshot=on" if snapshot else "")
    result = [args.qemu, "-machine", "q35,i8042=off", "-m", "768", "-smp", str(args.cpus),
              "-display", "none", "-monitor", "none", "-nic", "none", "-no-reboot",
              "-serial", f"file:{folder / 'serial.log'}", "-drive",
              f"if=pflash,format=raw,readonly=on,file={args.ovmf}",
              "-device", "nvme,id=nvme,serial=PEDIGREE-STORAGE,mdts=7"]
    if args.root == "ahci":
        result += ["-drive", drive(args.image, "root", True),
                   "-device", "ide-hd,drive=root,bus=ide.0",
                   "-drive", drive(folder / "ahci.img", "scratch") + ",iops_rd=100",
                   "-device", f"ide-hd,drive=scratch,bus=ide.1,logical_block_size={args.ahci_sector_size},physical_block_size={args.ahci_sector_size},discard_granularity={args.ahci_sector_size}"]
    else:
        result += ["-drive", drive(folder / "boot.img", "boot", True),
                   "-device", "ide-hd,drive=boot,bus=ide.0",
                   "-drive", drive(folder / "root-gpt.img", "root", True),
                   "-device", "nvme-ns,drive=root,bus=nvme,nsid=1,shared=off,logical_block_size=4096,physical_block_size=4096"]
    for sector, nsid in ((512, 7), (4096, 23)):
        result += ["-drive", drive(folder / f"nvme-{sector}.img", f"nvme{sector}"),
                   "-device", f"nvme-ns,drive=nvme{sector},bus=nvme,nsid={nsid},shared=off,logical_block_size={sector},physical_block_size={sector}"]
    result += ["-trace", f"events={folder / 'trace-events'},file={folder / 'trace.log'}"]
    return result


def run(args):
    folder = args.run_dir.resolve()
    folder.mkdir(parents=True, exist_ok=False)
    report = {"success": False, "cpus": args.cpus, "root": args.root,
              "ahci_sector_size": args.ahci_sector_size}
    child = None
    try:
        if args.root == "ahci":
            ahci.create_fixture(folder / "ahci.img")
        else:
            gpt_root(args.image, folder / "root-gpt.img", folder / "boot.img")
        for sector in (512, 4096):
            nvme_fixture(folder / f"nvme-{sector}.img", sector)
        (folder / "trace-events").write_text("\n".join((*ahci.TRACE_EVENTS, "pci_nvme_flush_ns")) + "\n")
        argv = command(args, folder)
        (folder / "command.json").write_text(json.dumps(argv, indent=2) + "\n")
        with (folder / "qemu.log").open("wb") as output:
            child = subprocess.Popen(argv, stdout=output, stderr=output, stdin=subprocess.DEVNULL,
                                     start_new_session=True)
            deadline = time.monotonic() + args.timeout
            while True:
                serial_path = folder / "serial.log"
                serial = serial_path.read_text(errors="replace") if serial_path.exists() else ""
                if re.search(r"(?:AHCI|NVME)-SMOKE: FAIL|panic:|fatal:|page fault exception|\(FF\)", serial, re.I):
                    raise RuntimeError("guest failure; inspect serial.log")
                nvme_done = "NVME-SMOKE: PASS complete namespaces=2" in serial
                ahci_done = args.root != "ahci" or "AHCI-SMOKE: PASS complete" in serial
                if nvme_done and ahci_done:
                    break
                if child.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("guest exited or timed out before required smoke completion")
                time.sleep(0.1)
            ahci.stop_child(child)
        if args.root == "nvme":
            if "GPT: validated primary table, logical sector 4096" not in serial:
                raise RuntimeError("missing native 4Kn GPT evidence")
            if "NVME-SMOKE: root namespace=1" not in serial:
                raise RuntimeError("root filesystem was not proved on the NVMe namespace")
        for sector in (512, 4096):
            nvme_fixture(folder / f"nvme-{sector}.img", sector, True)
        trace = (folder / "trace.log").read_text(errors="replace")
        verify_nvme_flushes(trace)
        if args.root == "ahci":
            report.update(ahci.verify_fixture(folder / "ahci.img"))
            report.update(ahci.trace_summary(trace))
            report["maximum_ncq_outstanding"] = ncq_maximum(trace)
            if report["maximum_ncq_outstanding"] < 2:
                raise RuntimeError("no overlapping scratch NCQ commands traced")
            if not report["scratch_flush_commands"]:
                raise RuntimeError("no guest ATA flush traced")
        report["success"] = True
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = str(error) or type(error).__name__
    finally:
        ahci.stop_child(child)
        (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--root", choices=("ahci", "nvme"), default="ahci")
    parser.add_argument("--ahci-sector-size", choices=(512,), type=int, default=512)
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
