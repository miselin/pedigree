/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "Gpt.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

#include "Partition.h"

namespace {
bool copy(Disk* disk, uint64_t offset, uint8_t* data, size_t bytes) {
  while (bytes) {
    const BufferView view = disk->read(offset);
    if (!view)
      return false;
    const size_t count = bytes < view.size() ? bytes : view.size();
    if (!count) {
      disk->unpin(offset);
      return false;
    }
    MemoryCopy(data, view.data(), count);
    disk->unpin(offset);
    offset += count;
    data += count;
    bytes -= count;
  }
  return true;
}
String partitionName(const uint8_t* entry, size_t stride) {
  String result;
  if (stride <= 56)
    return result;
  const size_t units = min(static_cast<size_t>(36), (stride - 56) / 2);
  for (size_t i = 0; i < units; ++i) {
    uint32_t character = Gpt::little(entry + 56 + i * 2, 2);
    if (!character)
      break;
    if (character >= 0xd800 && character <= 0xdbff) {
      if (i + 1 < units) {
        const uint32_t low = Gpt::little(entry + 56 + (i + 1) * 2, 2);
        if (low >= 0xdc00 && low <= 0xdfff) {
          character = 0x10000 + ((character - 0xd800) << 10) + (low - 0xdc00);
          ++i;
        } else {
          character = '?';
        }
      } else {
        character = '?';
      }
    } else if (character >= 0xdc00 && character <= 0xdfff) {
      character = '?';
    }
    char utf8[5] = {};
    size_t length = String::Utf32ToUtf8(character, utf8);
    if (!length) {
      utf8[0] = '?';
      length = 1;
    }
    result += String(utf8, length, true);
  }
  return result;
}
bool candidate(Disk* disk, size_t sectorBytes, uint64_t sectors, bool backup) {
  uint8_t bytes[4096];
  Gpt::Header header;
  if (!copy(disk, (backup ? sectors - 1 : 1) * sectorBytes, bytes, sectorBytes) ||
      !Gpt::decode(bytes, sectorBytes, sectors, backup, header))
    return false;
  auto* entries = new uint8_t[header.bytes];
  if (!copy(disk, header.table * sectorBytes, entries, header.bytes) ||
      !Gpt::validEntries(entries, header)) {
    delete[] entries;
    return false;
  }
  for (size_t i = 0; i < header.count; ++i) {
    const uint8_t* entry = entries + i * header.stride;
    if (!Gpt::used(entry))
      continue;
    NormalStaticString label("GPT ");
    label += i + 1;
    const uint64_t first = Gpt::little(entry + 32, 8), last = Gpt::little(entry + 40, 8);
    auto* partition =
        new Partition(String(label), first * sectorBytes, (last - first + 1) * sectorBytes);
    char uuid[37];
    Gpt::formatGuid(entry + 16, uuid);
    partition->setPartitionIdentity(String(uuid), partitionName(entry, header.stride));
    partition->setParent(disk);
    disk->addChild(partition);
  }
  delete[] entries;
  NOTICE("GPT: validated " << (backup ? "backup" : "primary") << " table, logical sector " << Dec
                           << sectorBytes << Hex);
  return true;
}
}  // namespace
bool gptProbeDisk(Disk* disk) {
  const size_t bytes = disk->getNativeBlockSize();
  if (bytes < 512 || bytes > 4096 || (bytes & (bytes - 1)) || disk->getSize() % bytes)
    return false;
  const uint64_t sectors = disk->getSize() / bytes;
  if (sectors < 6)
    return false;
  uint8_t mbr[512];
  if (!copy(disk, 0, mbr, sizeof(mbr)) || mbr[510] != 0x55 || mbr[511] != 0xaa)
    return false;
  bool protective = false;
  for (size_t i = 0; i < 4; ++i)
    protective |= mbr[446 + 16 * i + 4] == 0xee && Gpt::little(mbr + 446 + 16 * i + 8, 4) == 1;
  // Old backup headers can survive a reformat to MBR. Only recover a GPT disk.
  if (!protective)
    return false;
  if (!candidate(disk, bytes, sectors, false) && !candidate(disk, bytes, sectors, true))
    WARNING("GPT: no valid primary or backup table");
  return true;
}
