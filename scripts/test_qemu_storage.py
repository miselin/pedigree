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


def verify_iommu(serial, require_high):
    if "Intel VT-d: DMA remapping enabled" not in serial:
        raise RuntimeError("Intel VT-d was not enabled in the guest")
    high_domains = set()
    mapping = (r"Intel VT-d: high physical page 0x([0-9a-fA-F]+) "
               r"mapped to IOVA 0x([0-9a-fA-F]+) in domain (\d+)")
    for physical, iova, domain in re.findall(mapping, serial):
        if int(physical, 16) >= 1 << 32 and int(iova, 16) < 1 << 32:
            high_domains.add(int(domain))
    if require_high and len(high_domains) < 2:
        raise RuntimeError("missing high-page IOVA mappings for both storage controllers")
    return len(high_domains)


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


def combined_uefi_image(esp_image, root_image, destination):
    sector = 512
    alignment = 2048
    root_size = root_image.stat().st_size
    esp_size = esp_image.stat().st_size
    if not root_size or root_size % 4096 or not esp_size or esp_size % sector:
        raise RuntimeError("UEFI root/ESP images need complete filesystem sectors")
    root_first = alignment
    root_blocks = root_size // sector
    esp_first = (root_first + root_blocks + alignment - 1) // alignment * alignment
    esp_blocks = esp_size // sector
    if esp_first + esp_blocks > 0xffffffff:
        raise RuntimeError("combined UEFI fixture exceeds MBR capacity")
    mbr = bytearray(sector)
    mbr[450] = 0x83
    mbr[466] = 0xef
    struct.pack_into("<II", mbr, 454, root_first, root_blocks)
    struct.pack_into("<II", mbr, 470, esp_first, esp_blocks)
    mbr[510:512] = b"\x55\xaa"
    with destination.open("xb") as output:
        output.write(mbr)
        with root_image.open("rb") as root:
            output.seek(root_first * sector)
            copy_range(root, output, 0, root_size)
        with esp_image.open("rb") as esp:
            output.seek(esp_first * sector)
            copy_range(esp, output, 0, esp_size)
        output.truncate((esp_first + esp_blocks) * sector)


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
    result = [args.qemu, "-machine", "q35,i8042=off", "-m", str(args.ram_mib), "-smp", str(args.cpus),
              "-display", "none", "-monitor", "none", "-nic", "none", "-no-reboot",
              "-serial", f"file:{folder / 'serial.log'}", "-drive",
              f"if=pflash,format=raw,readonly=on,file={args.ovmf}",
              "-device", ("nvme,id=nvme,serial=PEDIGREE-STORAGE,mdts=7,"
                          f"max_ioqpairs={args.nvme_queues},msix_qsize={args.nvme_vectors}")]
    if args.intel_iommu:
        result += ["-device", f"intel-iommu,aw-bits={args.iommu_aw_bits},caching-mode=on"]
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
    if args.sriov:
        result += [
            "-device", "pcie-root-port,id=sriov-port,chassis=3,slot=3",
            "-device", "nvme-subsys,id=sriov-subsystem,nqn=pedigree-sriov",
            "-device", ("nvme,id=vf-primary,bus=sriov-port,serial=PEDIGREE-SRIOV,"
                       "model=PEDIGREE-SRIOV-SMOKE,subsys=sriov-subsystem,"
                       "sriov_max_vfs=1,sriov_vq_flexible=2,sriov_vi_flexible=2,"
                       "max_ioqpairs=8,msix_qsize=8"),
            "-drive", drive(folder / "nvme-vf.img", "vf-disk"),
            "-device", ("nvme-ns,drive=vf-disk,bus=vf-primary,nsid=1,shared=off,detached=on,"
                       "logical_block_size=4096,physical_block_size=4096"),
        ]
    if args.hotplug:
        result += [
            "-qmp", "stdio",
            "-global", "ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off",
            "-device", "pcie-root-port,id=hot-port,chassis=4,slot=4",
            "-drive", drive(folder / "nvme-hotplug.img", "hot-disk"),
            "-device", ("nvme,id=hot-nvme,bus=hot-port,drive=hot-disk,"
                       "serial=PEDIGREE-HOTPLUG,model=PEDIGREE-HOTPLUG-SMOKE,mdts=7"),
        ]
    result += ["-trace", f"events={folder / 'trace-events'},file={folder / 'trace.log'}"]
    return result


def run(args):
    folder = args.run_dir.resolve()
    folder.mkdir(parents=True, exist_ok=False)
    report = {"success": False, "cpus": args.cpus, "root": args.root,
              "ahci_sector_size": args.ahci_sector_size, "ram_mib": args.ram_mib,
              "intel_iommu": args.intel_iommu, "trace_iommu": args.trace_iommu}
    child = None
    try:
        if args.root_image:
            combined = folder / "combined-uefi.img"
            combined_uefi_image(args.image, args.root_image, combined)
            args.image = combined
        if args.root == "ahci":
            ahci.create_fixture(folder / "ahci.img")
        else:
            gpt_root(args.image, folder / "root-gpt.img", folder / "boot.img")
        for sector in (512, 4096):
            nvme_fixture(folder / f"nvme-{sector}.img", sector)
        if args.sriov:
            nvme_fixture(folder / "nvme-vf.img", 4096)
        if args.hotplug:
            nvme_fixture(folder / "nvme-hotplug.img", 512)
        trace_events = (*ahci.TRACE_EVENTS, "pci_nvme_flush_ns")
        if args.trace_iommu:
            trace_events += ("vtd_dmar_translate", "vtd_dmar_fault")
        if args.sriov:
            trace_events += ("sriov_register_vfs", "sriov_unregister_vfs", "sriov_config_write")
        (folder / "trace-events").write_text("\n".join(trace_events) + "\n")
        argv = command(args, folder)
        (folder / "command.json").write_text(json.dumps(argv, indent=2) + "\n")
        with (folder / "qemu.log").open("wb") as output, (folder / "qmp.log").open("wb") as transcript:
            child = subprocess.Popen(argv, stdout=subprocess.PIPE if args.hotplug else output,
                                     stderr=output, stdin=subprocess.PIPE if args.hotplug else subprocess.DEVNULL,
                                     start_new_session=True)
            deadline = time.monotonic() + args.timeout
            qmp = None
            sent = set()
            if args.hotplug:
                usb_spec = importlib.util.spec_from_file_location(
                    "usb_smoke", Path(__file__).with_name("test_qemu_usb.py"))
                usb = importlib.util.module_from_spec(usb_spec)
                usb_spec.loader.exec_module(usb)
                def check_qemu():
                    if child.poll() is not None:
                        raise RuntimeError("QEMU exited during hotplug")
                qmp = usb.Qmp(child, transcript, check_qemu)
                greeting = qmp.receive(deadline)
                if "QMP" not in greeting:
                    raise RuntimeError("missing QMP greeting")
                qmp.execute("qmp_capabilities", None, deadline)
            while True:
                serial_path = folder / "serial.log"
                serial = serial_path.read_text(errors="replace") if serial_path.exists() else ""
                if re.search(r"(?:AHCI|NVME)-SMOKE: FAIL|panic:|fatal:|page fault exception|\(FF\)", serial, re.I):
                    raise RuntimeError("guest failure; inspect serial.log")
                if qmp:
                    for cycle in (1, 2):
                        for stage in ("busy", "remove"):
                            marker = f"NVME-SMOKE: READY hotplug-{stage} cycle={cycle}"
                            if marker in serial and marker not in sent:
                                if stage == "remove" and serial.count("eject denied: device busy") < cycle:
                                    raise RuntimeError("missing busy-eject refusal before releasing disk use")
                                qmp.execute("device_del", {"id": "hot-nvme"}, deadline)
                                sent.add(marker)
                    marker = "NVME-SMOKE: READY hotplug-insert"
                    if marker in serial and marker not in sent and "orderly eject complete" in serial:
                        qmp.execute("device_add", {
                            "driver": "nvme", "id": "hot-nvme", "bus": "hot-port",
                            "drive": "hot-disk", "serial": "PEDIGREE-HOTPLUG",
                            "model": "PEDIGREE-HOTPLUG-SMOKE", "mdts": 7,
                        }, deadline)
                        sent.add(marker)
                nvme_done = "NVME-SMOKE: PASS complete namespaces=2" in serial
                ahci_done = args.root != "ahci" or "AHCI-SMOKE: PASS complete" in serial
                hotplug_done = not args.hotplug or serial.count("orderly eject complete") == 2
                if nvme_done and ahci_done and hotplug_done:
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
        if args.intel_iommu:
            report["remapped_high_domains"] = verify_iommu(serial, args.ram_mib > 4096)
            if "NVMe: using isolated DMA domain" not in serial:
                raise RuntimeError("NVMe did not attach an isolated DMA domain")
        for sector in (512, 4096):
            nvme_fixture(folder / f"nvme-{sector}.img", sector, True)
        if args.sriov:
            if not all(f"NVME-SMOKE: PASS sriov-cycle={cycle}" in serial for cycle in (1, 2)):
                raise RuntimeError("missing repeated VF I/O and retirement evidence")
            nvme_fixture(folder / "nvme-vf.img", 4096, True)
            report["sriov_cycles"] = 2
        if args.hotplug:
            if not all(f"NVME-SMOKE: PASS hotplug-removed cycle={cycle}" in serial for cycle in (1, 2)):
                raise RuntimeError("missing repeated hotplug I/O and removal evidence")
            if "driver bound=true" not in serial:
                raise RuntimeError("reinserted controller was not dynamically bound")
            nvme_fixture(folder / "nvme-hotplug.img", 512, True)
            report["hotplug_cycles"] = 2
        trace = (folder / "trace.log").read_text(errors="replace")
        if args.trace_iommu and "vtd_dmar_fault" in trace:
            raise RuntimeError("IOMMU fault during storage I/O")
        verify_nvme_flushes(trace)
        if args.root == "ahci":
            report.update(ahci.verify_fixture(folder / "ahci.img"))
            report.update(ahci.trace_summary(trace))
            if ("AHCI-SMOKE: PASS clean-page-sync" not in serial or
                    not report["scratch_clean_page_read_commands"]):
                raise RuntimeError("missing clean-page sync and read evidence")
            if report["scratch_clean_page_write_commands"]:
                raise RuntimeError("read-only scratch page was written back")
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
    parser.add_argument("--root-image", type=Path)
    parser.add_argument("--root", choices=("ahci", "nvme"), default="ahci")
    parser.add_argument("--ahci-sector-size", choices=(512,), type=int, default=512)
    parser.add_argument("--cpus", choices=(1, 4), type=int, default=4)
    parser.add_argument("--ram-mib", type=int, default=768)
    parser.add_argument("--intel-iommu", action="store_true")
    parser.add_argument("--nvme-queues", type=int, default=64,
                        help="I/O queue pairs offered by the primary test controller")
    parser.add_argument("--nvme-vectors", type=int, default=65,
                        help="MSI-X entries offered by the primary test controller")
    parser.add_argument("--sriov", action="store_true", help="exercise isolated NVMe VF creation, I/O, and retirement twice")
    parser.add_argument("--hotplug", action="store_true", help="exercise busy-eject refusal, removal and reinsertion with NVMe I/O")
    parser.add_argument("--trace-iommu", action="store_true")
    parser.add_argument("--iommu-aw-bits", choices=(39, 48), type=int, default=39)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ovmf", type=Path, default=Path("/opt/homebrew/share/qemu/edk2-x86_64-code.fd"))
    parser.add_argument("--timeout", type=float, default=240)
    args = parser.parse_args()
    if (not args.image.is_file() or (args.root_image and not args.root_image.is_file()) or
            not args.ovmf.is_file() or not 0 < args.timeout <= 3600 or
            not 128 <= args.ram_mib <= 65536):
        parser.error("image/root/OVMF must exist, RAM must be 128-65536 MiB, and timeout must be between 0 and 3600")
    if not 1 <= args.nvme_queues <= 64 or not 1 <= args.nvme_vectors <= 2048:
        parser.error("NVMe queues must be 1-64 and vectors must be 1-2048")
    if args.trace_iommu and not args.intel_iommu:
        parser.error("trace-iommu requires intel-iommu")
    if args.sriov and not args.intel_iommu:
        parser.error("sriov requires intel-iommu")
    args.image = args.image.resolve()
    if args.root_image:
        args.root_image = args.root_image.resolve()
    args.ovmf = args.ovmf.resolve()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    result = run(args)
    print(json.dumps(result))
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
