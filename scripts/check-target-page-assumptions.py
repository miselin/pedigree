#!/usr/bin/env python3

"""Reject unclassified 4 KiB VM-page assumptions in portable kernel code."""

from __future__ import annotations

import re
import sys
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parent.parent
TARGETS = (
    "src/system/include/pedigree/kernel/machine/Disk.h",
    "src/system/include/pedigree/kernel/process/Event.h",
    "src/system/include/pedigree/kernel/process/Ipc.h",
    "src/system/include/pedigree/kernel/utilities/BufferView.h",
    "src/system/include/pedigree/kernel/utilities/Cache.h",
    "src/system/kernel/core/process/Event.cc",
    "src/system/kernel/core/process/Ipc.cc",
    "src/system/kernel/core/process/MemoryPressureKiller.cc",
    "src/system/kernel/core/process/PerProcessorScheduler.cc",
    "src/system/kernel/core/processor/hosted",
    "src/system/kernel/linker",
    "src/system/kernel/machine/hosted/Timer.cc",
    "src/system/kernel/machine/mach_pc/Rtc.cc",
    "src/system/kernel/machine/Disk.cc",
    "src/system/kernel/utilities/Cache.cc",
    "src/system/kernel/utilities/MemoryCount.cc",
    "src/system/kernel/utilities/MemoryPool.cc",
    "src/modules/drivers/common/partition/Partition.h",
    "src/modules/drivers/common/scsi/ScsiDisk.cc",
    "src/modules/drivers/hosted/diskimage/DiskImage.cc",
    "src/modules/subsys/posix/file-syscalls.cc",
    "src/modules/subsys/posix/ProcFs.cc",
    "src/modules/system/ext2/Ext2Filesystem.cc",
    "src/modules/system/hosted-smoke/requestqueue-regressions.cc",
    "src/modules/system/iso9660",
    "src/modules/system/linker",
    "src/modules/system/lodisk/LoDisk.cc",
    "src/modules/system/rawfs/RawFsFile.cc",
    "src/modules/system/status_server/main.cc",
    "src/modules/system/vfs/MemoryMappedFile.cc",
    "src/user/applications/testsuite/mprotect.c",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".s", ".S", ".ld"}
PAGE_LITERAL = re.compile(
    r"(?i)(?<![0-9a-f])(?:4096|0x0*1000|0x0*fff)(?![0-9a-f])"
    r"|(?:<<|>>)\s*12\b"
)

# These are byte-sized API policies, not VM-page geometry. Keeping the
# exceptions textual makes additions review-visible instead of allowing a
# whole file to escape the audit.
ALLOWED = {
    ("src/system/include/pedigree/kernel/process/Event.h", "#define EVENT_LIMIT 4096"),
    (
        "src/system/include/pedigree/kernel/process/Event.h",
        "amount of information up to a hard maximum size of EVENT_LIMIT (usually 4096",
    ),
    (
        "src/system/include/pedigree/kernel/process/Ipc.h",
        "static constexpr size_t InlineCapacity = 4096;",
    ),
    (
        "src/system/include/pedigree/kernel/process/Ipc.h",
        "/// the @IpcMessage constructor with regionHandle == 0 and nBytes >= 4096",
    ),
    (
        "src/system/kernel/machine/mach_pc/Rtc.cc",
        "{4096, 0x04, {244140ULL, 244141ULL}},    {8192, 0x03, {122070ULL, 122070ULL}},",
    ),
    (
        "src/modules/subsys/posix/file-syscalls.cc",
        "buf->f_bsize = 4096;",
    ),
    (
        "src/modules/subsys/posix/file-syscalls.cc",
        "buf->f_frsize = 4096;",
    ),
    (
        "src/modules/system/ext2/Ext2Filesystem.cc",
        'static uint8_t g_pSparseBlock[4096] ALIGN(4096) SECTION(".bss");',
    ),
    (
        "src/modules/system/ext2/Ext2Filesystem.cc",
        "if (m_BlockSize > 4096) {",
    ),
    (
        "src/modules/system/ext2/Ext2Filesystem.cc",
        'ERROR("Ext2: filesystem\'s block size is too large (must be 4096 or less, but is " << m_BlockSize',
    ),
    (
        "src/modules/system/ext2/Ext2Filesystem.cc",
        "uint32_t mask = LITTLE_TO_HOST16(inode->i_mode) & 0x0FFF;",
    ),
}


def source_files() -> list[Path]:
    files: list[Path] = []
    for relative in TARGETS:
        path = SOURCE_ROOT / relative
        if path.is_dir():
            files.extend(
                candidate
                for candidate in path.rglob("*")
                if candidate.is_file() and candidate.suffix in SOURCE_SUFFIXES
            )
        elif path.is_file():
            files.append(path)
        else:
            raise FileNotFoundError(relative)
    return sorted(set(files))


def main() -> int:
    failures: list[str] = []
    for path in source_files():
        relative = path.relative_to(SOURCE_ROOT).as_posix()
        for line_number, line in enumerate(path.read_text().splitlines(), 1):
            if not PAGE_LITERAL.search(line):
                continue
            if (relative, line.strip()) in ALLOWED:
                continue
            failures.append(f"{relative}:{line_number}: {line.strip()}")

    if failures:
        print("Unclassified target-page assumptions found:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1

    print("Target-page source audit passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
