/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/syscallError.h"

#include "FatFilesystem.h"

bool FatFilesystem::invalidateFsInfoHints() {
  if (m_Type != FAT32 || m_FsInfoInvalidated)
    return true;

  const uint32_t sectorBytes = LITTLE_TO_HOST16(m_Superblock.BPB_BytsPerSec);
  const uint32_t reservedSectors = LITTLE_TO_HOST16(m_Superblock.BPB_RsvdSecCnt);
  const uint32_t primary = LITTLE_TO_HOST16(m_Superblock32.BPB_FsInfo);
  if (sectorBytes < sizeof(FSInfo32) || !primary || primary >= reservedSectors)
    return true;

  const uint32_t backupBoot = LITTLE_TO_HOST16(m_Superblock32.BPB_BkBootSec);
  const uint32_t sectors[] = {primary, primary + backupBoot};
  const size_t copies =
      backupBoot && backupBoot < reservedSectors && sectors[1] < reservedSectors ? 2 : 1;
  uint8_t* bytes = new uint8_t[sectorBytes];
  if (!bytes) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  for (size_t i = 0; i < copies; ++i) {
    if (!readSectorBlock(sectors[i], sectorBytes, reinterpret_cast<uintptr_t>(bytes))) {
      delete[] bytes;
      SYSCALL_ERROR(IoError);
      return false;
    }

    FSInfo32* info = reinterpret_cast<FSInfo32*>(bytes);
    if (LITTLE_TO_HOST32(info->FSI_LeadSig) != 0x41615252 ||
        LITTLE_TO_HOST32(info->FSI_StrucSig) != 0x61417272 ||
        LITTLE_TO_HOST32(info->FSI_TrailSig) != 0xAA550000)
      continue;

    // Rewrite even unknown hints: a failed flush may have changed only the disk cache.
    info->FSI_Free_Count = HOST_TO_LITTLE32(0xFFFFFFFFU);
    info->FSI_NxtFree = HOST_TO_LITTLE32(0xFFFFFFFFU);
    if (!writeSectorBlock(sectors[i], sectorBytes, reinterpret_cast<uintptr_t>(bytes))) {
      delete[] bytes;
      SYSCALL_ERROR(IoError);
      return false;
    }
  }
  delete[] bytes;
  m_FsInfoInvalidated = true;
  return true;
}
