/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "msdos.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Partition.h"

class Device;

static Spinlock g_Lock;

static const char* g_pPartitionTypes[256] = {"Empty",
                                             "FAT12",
                                             "XENIX root",
                                             "XENIX usr",
                                             "FAT16 <32M",
                                             "Extended",
                                             "FAT16",
                                             "HPFS/NTFS",
                                             "AIX",
                                             "AIX bootable",
                                             "OS/2 Boot Manag",
                                             "W95 FAT32",
                                             "W95 FAT32 (LBA)",
                                             "",
                                             "W95 FAT16 (LBA)",
                                             "W95 Ext",
                                             "OPUS",
                                             "Hidden FAT12",
                                             "Compaq diagnost",
                                             "",
                                             "Hidden FAT16 <3",
                                             "",
                                             "Hidden FAT16",
                                             "Hidden HPFS/NTF",
                                             "AST SmartSleep",
                                             "",
                                             "",
                                             "Hidden W95 FAT3",
                                             "Hidden W95 FAT3",
                                             "",
                                             "Hidden W95 FAT1",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "NEC DOS",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "Plan 9",
                                             "",
                                             "",
                                             "PartitionMagic",
                                             "",
                                             "",
                                             "",
                                             "Venix 80286",
                                             "PPC PReP Boot",
                                             "SFS",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "QNX4.x",
                                             "QNX4.x 2nd part",
                                             "QNX4.x 3rd part",
                                             "OnTrack DM",
                                             "OnTrack DM6 Aux",
                                             "CP/M",
                                             "OnTrack DM6 Aux",
                                             "OnTrackDM6",
                                             "EZ-Drive",
                                             "Golden Bow",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "Priam Edisk",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "SpeedStor",
                                             "",
                                             "GNU HURD or Sys",
                                             "Novell Netware",
                                             "Novell Netware",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "DiskSecure Mult",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "PC/IX",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "Old Minix",
                                             "Minix / old Lin",
                                             "Linux swap / So",
                                             "Linux",
                                             "OS/2 hidden C:",
                                             "Linux extended",
                                             "NTFS volume set",
                                             "NTFS volume set",
                                             "Linux plaintext",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "Linux LVM",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "Amoeba",
                                             "Amoeba BBT",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "BSD/OS",
                                             "IBM Thinkpad hi",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "FreeBSD",
                                             "OpenBSD",
                                             "NeXTSTEP",
                                             "Darwin UFS",
                                             "NetBSD",
                                             "",
                                             "Darwin boot",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "BSDI fs",
                                             "BSDI swap",
                                             "",
                                             "",
                                             "Boot Wizard hid",
                                             "",
                                             "",
                                             "Solaris boot",
                                             "Solaris",
                                             "",
                                             "DRDOS/sec (FAT-",
                                             "",
                                             "",
                                             "DRDOS/sec (FAT-",
                                             "",
                                             "DRDOS/sec (FAT-",
                                             "Syrinx",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "Non-FS data",
                                             "CP/M / CTOS / .",
                                             "",
                                             "",
                                             "Dell Utility",
                                             "BootIt",
                                             "",
                                             "DOS access",
                                             "",
                                             "DOS R/O",
                                             "SpeedStor",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "BeOS fs",
                                             "",
                                             "",
                                             "EFI GPT",
                                             "EFI (FAT-12/16/",
                                             "Linux/PA-RISC b",
                                             "SpeedStor",
                                             "DOS secondary",
                                             "",
                                             "SpeedStor",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "",
                                             "Linux RAID auto",
                                             "LANstep",
                                             "BBT"};

/// Holds the next partition number to mount
static int gNextPartition = 0;

static bool extended(uint8_t type) {
  return type == 5 || type == 0x0f || type == 0x85;
}

static bool sectorRange(Disk* disk, uint64_t start, uint64_t count) {
  const size_t sectorBytes = disk->getNativeBlockSize();
  const uint64_t sectors = sectorBytes ? disk->getSize() / sectorBytes : 0;
  return count && start < sectors && count <= sectors - start;
}

static void registerPartition(const MsdosPartitionInfo& entry, Disk* disk, uint64_t start) {
  const uint64_t count = LITTLE_TO_HOST32(entry.size);
  if (!sectorRange(disk, start, count) || !start) {
    WARNING("MS-DOS: partition outside disk");
    return;
  }
  int number;
  {
    LockGuard<Spinlock> guard(g_Lock);
    number = gNextPartition++;
  }
  NormalStaticString label("(");
  label += number;
  label += ") ";
  label += g_pPartitionTypes[entry.type];
  const size_t bytes = disk->getNativeBlockSize();
  auto* partition = new Partition(String(label), start * bytes, count * bytes);
  partition->setParent(disk);
  disk->addChild(partition);
}

static bool readEntries(Disk* disk, uint64_t lba, MsdosPartitionInfo* entries) {
  if (!sectorRange(disk, lba, 1))
    return false;
  const uint64_t offset = lba * disk->getNativeBlockSize();
  const BufferView view = disk->read(offset);
  if (!view)
    return false;
  const bool valid = view.size() >= 512 && view[510] == MSDOS_IDENT_1 && view[511] == MSDOS_IDENT_2;
  if (valid)
    MemoryCopy(entries, view.as<uint8_t>(MSDOS_PARTTAB_START), sizeof(MsdosPartitionInfo) * 4);
  disk->unpin(offset);
  return valid;
}

static void readExtended(Disk* disk, uint64_t base, uint64_t count) {
  if (!sectorRange(disk, base, count) || !base)
    return;
  uint64_t visited[128];
  size_t depth = 0;
  uint64_t current = base;
  while (depth < 128) {
    for (size_t i = 0; i < depth; ++i)
      if (visited[i] == current)
        return;
    visited[depth++] = current;
    MsdosPartitionInfo entries[4];
    if (!readEntries(disk, current, entries))
      return;
    const auto& data = entries[0];
    const uint64_t start = current + LITTLE_TO_HOST32(data.start_lba);
    const uint64_t length = LITTLE_TO_HOST32(data.size);
    if ((data.active == 0 || data.active == 0x80) && data.type && !extended(data.type) &&
        data.type != 0xee && start > current && start >= base && start < base + count &&
        length <= base + count - start)
      registerPartition(data, disk, start);
    const auto& link = entries[1];
    if (!extended(link.type) || (link.active != 0 && link.active != 0x80))
      return;
    const uint64_t relative = LITTLE_TO_HOST32(link.start_lba);
    if (!relative || relative >= count)
      return;
    current = base + relative;
  }
  WARNING("MS-DOS: extended partition chain exceeds 128 records");
}

bool msdosReadTable(MsdosPartitionInfo* entries, Disk* disk) {
  // A corrupt GPT must not expose its protective container as a filesystem.
  for (size_t i = 0; i < 4; ++i)
    if (entries[i].type == 0xee)
      return true;
  for (size_t i = 0; i < 4; ++i) {
    const auto& entry = entries[i];
    if (entry.active != 0 && entry.active != 0x80)
      continue;
    const uint64_t start = LITTLE_TO_HOST32(entry.start_lba);
    if (extended(entry.type))
      readExtended(disk, start, LITTLE_TO_HOST32(entry.size));
    else if (entry.type)
      registerPartition(entry, disk, start);
  }
  return true;
}

bool msdosProbeDisk(Disk* disk) {
  const size_t bytes = disk->getNativeBlockSize();
  if (bytes < 512 || bytes > 4096 || (bytes & (bytes - 1)))
    return false;
  MsdosPartitionInfo entries[4];
  return readEntries(disk, 0, entries) && msdosReadTable(entries, disk);
}
