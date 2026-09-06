/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2Filesystem.h"
#include "ext2.h"

uint32_t Ext2Filesystem::findFreeInode() {
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_WriteLock);
#endif
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  const uint32_t total = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_count);
  for (size_t group = 0; group < m_nGroupDescriptors; ++group) {
    GroupDesc* descriptor = m_pGroupDescriptors[group];
    if (!LITTLE_TO_HOST16(descriptor->bg_free_inodes_count)) {
      continue;
    }
    if (!ensureFreeInodeBitmapLoaded(group)) {
      return 0;
    }
    Vector<size_t>& bitmap = m_pInodeBitmaps[group];
    for (uint32_t index = 0; index < perGroup; ++index) {
      const uint64_t number = static_cast<uint64_t>(group) * perGroup + index + 1;
      if (number > 0xffffffffULL || (total && number > total)) {
        break;
      }
      const size_t field = (index / 8) / m_BlockSize;
      auto* byte = reinterpret_cast<uint8_t*>(bitmap[field] + (index / 8) % m_BlockSize);
      const uint8_t bit = 1U << (index % 8);
      if (*byte & bit) {
        continue;
      }
      Inode* inode = getInode(static_cast<uint32_t>(number));
      if (!inode) {
        return 0;
      }
      const uint32_t generation = LITTLE_TO_HOST32(inode->i_generation);
      if (generation == 0xffffffffU) {
        // Reusing this slot would revive a previously exported handle.
        continue;
      }

      // Decode checks the same allocation lock and rejects links==0. Publish
      // the new generation before allocation, keeping creation private until
      // the directory insertion publishes its first link.
      ByteSet(inode, 0, m_InodeSize);
      inode->i_generation = HOST_TO_LITTLE32(generation + 1);
      writeInode(static_cast<uint32_t>(number));
      *byte |= bit;
      descriptor->bg_free_inodes_count =
          HOST_TO_LITTLE16(LITTLE_TO_HOST16(descriptor->bg_free_inodes_count) - 1);
      m_pSuperblock->s_free_inodes_count =
          HOST_TO_LITTLE32(LITTLE_TO_HOST32(m_pSuperblock->s_free_inodes_count) - 1);
      m_pDisk->write(1024ULL);
      writeBlock(LITTLE_TO_HOST32(descriptor->bg_inode_bitmap) + field);
      const uint32_t descriptors = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1;
      writeBlock(descriptors + (group * sizeof(GroupDesc)) / m_BlockSize);
      return static_cast<uint32_t>(number);
    }
  }
  return 0;
}
