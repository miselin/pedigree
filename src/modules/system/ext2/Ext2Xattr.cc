/* Copyright (c) 2026, Pedigree Developers. */
#include "Ext2Xattr.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2Filesystem.h"
#include "Ext2Node.h"
#include "ext2.h"

namespace Ext2Ea {
namespace {
size_t align4(size_t size) {
  return (size + 3) & ~size_t(3);
}
const Entry* entryAt(const void* block, size_t offset) {
  return reinterpret_cast<const Entry*>(static_cast<const uint8_t*>(block) + offset);
}
int compare(unsigned index, const char* name, size_t length, const Entry& entry) {
  if (index != entry.nameIndex)
    return index < entry.nameIndex ? -1 : 1;
  if (length != entry.nameLength)
    return length < entry.nameLength ? -1 : 1;
  const auto* other = reinterpret_cast<const uint8_t*>(&entry + 1);
  for (size_t n = 0; n < length; ++n) {
    const auto character = static_cast<uint8_t>(name[n]);
    if (character != other[n])
      return character < other[n] ? -1 : 1;
  }
  return 0;
}
bool last(const Entry* entry) {
  uint32_t end;
  MemoryCopy(&end, entry, sizeof(end));
  return !end;
}
XattrStatus suffix(const StringView& name) {
  if (!name.length() || name.length() > Xattr::MaximumNameLength)
    return XattrStatus::Range;
  for (size_t n = 0; n < name.length(); ++n) {
    if (!name[n])
      return XattrStatus::Invalid;
  }
  if (name.length() < 5 || !name.substring(0, 5).compare("user.", 5))
    return XattrStatus::Unsupported;
  return name.length() == 5 ? XattrStatus::Invalid : XattrStatus::Success;
}
uint32_t entryHash(const Entry& entry, const uint8_t* value) {
  uint32_t hash = 0;
  const auto* name = reinterpret_cast<const uint8_t*>(&entry + 1);
  for (size_t n = 0; n < entry.nameLength; ++n)
    hash = (hash << 5) ^ (hash >> 27) ^ name[n];
  for (size_t n = 0; n < align4(LITTLE_TO_HOST32(entry.valueLength)); n += 4) {
    uint32_t word;
    MemoryCopy(&word, value + n, sizeof(word));
    hash = (hash << 16) ^ (hash >> 16) ^ LITTLE_TO_HOST32(word);
  }
  return hash;
}
}  // namespace

XattrStatus validate(const void* block, size_t size) {
  if (!block)
    return XattrStatus::Success;
  if (size < sizeof(Header) + 4 || size > 4096)
    return XattrStatus::IoError;
  const auto* header = static_cast<const Header*>(block);
  if (LITTLE_TO_HOST32(header->magic) != Magic || LITTLE_TO_HOST32(header->blocks) != 1 ||
      !LITTLE_TO_HOST32(header->references) || LITTLE_TO_HOST32(header->references) > 1024)
    return XattrStatus::IoError;
  for (uint32_t reserved : header->reserved) {
    if (reserved)
      return XattrStatus::Unsupported;
  }
  size_t cursor = sizeof(Header), valuesBegin = size;
  const Entry* previous = nullptr;
  while (cursor <= size - 4) {
    const Entry* entry = entryAt(block, cursor);
    if (last(entry))
      return cursor + 4 <= valuesBegin ? XattrStatus::Success : XattrStatus::IoError;
    if (size - cursor < sizeof(Entry))
      return XattrStatus::IoError;
    const size_t span = align4(sizeof(Entry) + entry->nameLength);
    if (span > size - cursor - 4)
      return XattrStatus::IoError;
    if (!entry->nameIndex)
      return XattrStatus::IoError;
    if (entry->valueBlock)
      return XattrStatus::Unsupported;
    if (entry->nameIndex == User) {
      if (!entry->nameLength || entry->nameLength > Xattr::MaximumNameLength - 5)
        return XattrStatus::IoError;
      const auto* name = reinterpret_cast<const uint8_t*>(entry + 1);
      for (size_t n = 0; n < entry->nameLength; ++n) {
        if (!name[n])
          return XattrStatus::IoError;
      }
    }
    if (previous && compare(previous->nameIndex, reinterpret_cast<const char*>(previous + 1),
                            previous->nameLength, *entry) >= 0)
      return XattrStatus::IoError;
    const size_t length = LITTLE_TO_HOST32(entry->valueLength);
    const size_t offset = LITTLE_TO_HOST16(entry->valueOffset);
    if (length > size || offset > size || length > size - offset ||
        (length && ((offset & 3) || align4(length) > size - offset)))
      return XattrStatus::IoError;
    if (length) {
      for (size_t prior = sizeof(Header); prior < cursor;) {
        const auto* other = entryAt(block, prior);
        const size_t otherLength = LITTLE_TO_HOST32(other->valueLength);
        const size_t otherOffset = LITTLE_TO_HOST16(other->valueOffset);
        if (otherLength && offset < otherOffset + align4(otherLength) &&
            otherOffset < offset + align4(length))
          return XattrStatus::IoError;
        prior += align4(sizeof(Entry) + other->nameLength);
      }
      if (offset < valuesBegin)
        valuesBegin = offset;
    }
    previous = entry;
    cursor += span;
  }
  return XattrStatus::IoError;
}

XattrStatus get(const void* block, size_t size, const StringView& name, void* output,
                size_t capacity, size_t& required) {
  required = 0;
  auto status = suffix(name);
  if (status != XattrStatus::Success)
    return status;
  status = validate(block, size);
  if (status != XattrStatus::Success || !block)
    return status == XattrStatus::Success ? XattrStatus::Missing : status;
  for (size_t cursor = sizeof(Header); !last(entryAt(block, cursor));) {
    const Entry* entry = entryAt(block, cursor);
    if (!compare(User, name.str() + 5, name.length() - 5, *entry)) {
      required = LITTLE_TO_HOST32(entry->valueLength);
      if (capacity && capacity < required)
        return XattrStatus::Range;
      if (capacity && required && !output)
        return XattrStatus::Invalid;
      if (capacity && required)
        MemoryCopy(output,
                   static_cast<const uint8_t*>(block) + LITTLE_TO_HOST16(entry->valueOffset),
                   required);
      return XattrStatus::Success;
    }
    cursor += align4(sizeof(Entry) + entry->nameLength);
  }
  return XattrStatus::Missing;
}

XattrStatus list(const void* block, size_t size, void* output, size_t capacity, size_t& required) {
  required = 0;
  if (capacity && !output)
    return XattrStatus::Invalid;
  auto status = validate(block, size);
  if (status != XattrStatus::Success || !block)
    return status;
  for (size_t cursor = sizeof(Header); !last(entryAt(block, cursor));) {
    const Entry* entry = entryAt(block, cursor);
    if (entry->nameIndex == User)
      required += 6 + entry->nameLength;
    cursor += align4(sizeof(Entry) + entry->nameLength);
  }
  if (capacity && capacity < required)
    return XattrStatus::Range;
  if (!capacity)
    return XattrStatus::Success;
  auto* destination = static_cast<char*>(output);
  for (size_t cursor = sizeof(Header); !last(entryAt(block, cursor));) {
    const Entry* entry = entryAt(block, cursor);
    if (entry->nameIndex == User) {
      MemoryCopy(destination, "user.", 5);
      MemoryCopy(destination + 5, entry + 1, entry->nameLength);
      destination[5 + entry->nameLength] = 0;
      destination += 6 + entry->nameLength;
    }
    cursor += align4(sizeof(Entry) + entry->nameLength);
  }
  return XattrStatus::Success;
}

XattrStatus rebuild(const void* oldBlock, size_t size, const StringView& name, const void* value,
                    size_t length, unsigned flags, bool remove, void* replacement, bool& empty) {
  auto status = suffix(name);
  if (status != XattrStatus::Success)
    return status;
  if (flags & ~(Xattr::Create | Xattr::Replace))
    return XattrStatus::Invalid;
  if (length > size)
    return XattrStatus::Range;
  if (!replacement || (length && !value))
    return XattrStatus::Invalid;
  status = validate(oldBlock, size);
  if (status != XattrStatus::Success)
    return status;
  bool exists = false;
  if (oldBlock) {
    for (size_t cursor = sizeof(Header); !last(entryAt(oldBlock, cursor));) {
      const auto* entry = entryAt(oldBlock, cursor);
      exists |= !compare(User, name.str() + 5, name.length() - 5, *entry);
      cursor += align4(sizeof(Entry) + entry->nameLength);
    }
  }
  if (exists && (flags & Xattr::Create))
    return XattrStatus::Exists;
  if (!exists && (remove || (flags & Xattr::Replace)))
    return XattrStatus::Missing;
  ByteSet(replacement, 0, size);
  auto* header = static_cast<Header*>(replacement);
  header->magic = HOST_TO_LITTLE32(Magic);
  header->blocks = header->references = HOST_TO_LITTLE32(1);
  size_t cursor = sizeof(Header), valueEnd = size;
  uint32_t hash = 0;
  bool unshareable = false;
  auto emit = [&](unsigned index, const char* entryName, size_t nameLength, const void* bytes,
                  size_t valueLength) {
    const size_t entrySize = align4(sizeof(Entry) + nameLength), valueSize = align4(valueLength);
    if (valueSize > valueEnd || cursor + entrySize + 4 > valueEnd - valueSize)
      return false;
    valueEnd -= valueSize;
    auto* entry = reinterpret_cast<Entry*>(static_cast<uint8_t*>(replacement) + cursor);
    entry->nameIndex = index;
    entry->nameLength = nameLength;
    entry->valueOffset = HOST_TO_LITTLE16(valueLength ? valueEnd : 0);
    entry->valueLength = HOST_TO_LITTLE32(valueLength);
    MemoryCopy(entry + 1, entryName, nameLength);
    auto* destination = static_cast<uint8_t*>(replacement) + valueEnd;
    if (valueLength)
      MemoryCopy(destination, bytes, valueLength);
    const uint32_t entryValueHash = entryHash(*entry, destination);
    entry->hash = HOST_TO_LITTLE32(entryValueHash);
    unshareable |= !entryValueHash;
    hash = (hash << 16) ^ (hash >> 16) ^ entryValueHash;
    cursor += entrySize;
    return true;
  };
  bool inserted = remove;
  if (oldBlock) {
    for (size_t offset = sizeof(Header); !last(entryAt(oldBlock, offset));) {
      const auto* entry = entryAt(oldBlock, offset);
      const int order = compare(User, name.str() + 5, name.length() - 5, *entry);
      if (!inserted && order <= 0) {
        if (!emit(User, name.str() + 5, name.length() - 5, value, length))
          return XattrStatus::NoSpace;
        inserted = true;
      }
      if (order &&
          !emit(entry->nameIndex, reinterpret_cast<const char*>(entry + 1), entry->nameLength,
                static_cast<const uint8_t*>(oldBlock) + LITTLE_TO_HOST16(entry->valueOffset),
                LITTLE_TO_HOST32(entry->valueLength)))
        return XattrStatus::NoSpace;
      offset += align4(sizeof(Entry) + entry->nameLength);
    }
  }
  if (!inserted && !emit(User, name.str() + 5, name.length() - 5, value, length))
    return XattrStatus::NoSpace;
  header->hash = HOST_TO_LITTLE32(unshareable ? 0 : hash);
  empty = cursor == sizeof(Header);
  return XattrStatus::Success;
}
}  // namespace Ext2Ea

XattrStatus Ext2Node::getXattr(const StringView& name, void* output, size_t capacity,
                               size_t& required) {
  required = 0;
  LockGuard<Mutex> inodeGuard(m_State->writebackLock);
  LockGuard<Mutex> allocationGuard(m_pExt2Fs->m_WriteLock);
  if (!m_State->allocationValid)
    return XattrStatus::IoError;
  auto status = m_pExt2Fs->attributeFormatStatus();
  if (status != XattrStatus::Success)
    return status;
  Ext2Filesystem::AttributeRetirement block;
  status = m_pExt2Fs->readAttributeBlockLocked(m_pInode, block);
  return status == XattrStatus::Success
             ? Ext2Ea::get(reinterpret_cast<const void*>(block.buffer), m_pExt2Fs->m_BlockSize,
                           name, output, capacity, required)
             : status;
}
XattrStatus Ext2Node::listXattrs(void* output, size_t capacity, size_t& required) {
  required = 0;
  LockGuard<Mutex> inodeGuard(m_State->writebackLock);
  LockGuard<Mutex> allocationGuard(m_pExt2Fs->m_WriteLock);
  if (!m_State->allocationValid)
    return XattrStatus::IoError;
  auto status = m_pExt2Fs->attributeFormatStatus();
  if (status != XattrStatus::Success)
    return status;
  Ext2Filesystem::AttributeRetirement block;
  status = m_pExt2Fs->readAttributeBlockLocked(m_pInode, block);
  return status == XattrStatus::Success
             ? Ext2Ea::list(reinterpret_cast<const void*>(block.buffer), m_pExt2Fs->m_BlockSize,
                            output, capacity, required)
             : status;
}

bool Ext2Node::decodeAllocation(const Inode& inode, uint32_t blockSize, uint32_t& blocks,
                                bool& inlineSymlink) {
  if (blockSize < 512 || blockSize > 4096 || (blockSize & (blockSize - 1)))
    return false;
  const uint32_t sectors = LITTLE_TO_HOST32(inode.i_blocks);
  const uint32_t eaSectors = inode.i_file_acl ? blockSize / 512 : 0;
  if (sectors < eaSectors || (sectors - eaSectors) % (blockSize / 512))
    return false;
  blocks = (sectors - eaSectors) / (blockSize / 512);
  inlineSymlink = (LITTLE_TO_HOST16(inode.i_mode) & 0xf000) == EXT2_S_IFLNK && !blocks;
  return !inlineSymlink || LITTLE_TO_HOST32(inode.i_size) <= sizeof(inode.i_block);
}

bool Ext2Node::encodeAllocation(uint32_t data, uint32_t indirect, bool hasEa, uint32_t blockSize,
                                uint32_t& sectors) {
  const uint64_t count = (static_cast<uint64_t>(data) + indirect + hasEa) * (blockSize / 512);
  if (count > 0xffffffffULL)
    return false;
  sectors = count;
  return true;
}

bool Ext2Node::isInlineSymlink() const {
  uint32_t blocks = 0;
  bool inlineSymlink = false;
  return decodeAllocation(*m_pInode, m_pExt2Fs->m_BlockSize, blocks, inlineSymlink) &&
         inlineSymlink;
}

void Ext2Node::updateAllocatedSectorCount() {
  uint32_t sectors;
  const bool valid = encodeAllocation(m_State->allocatedDataBlocks, m_nMetadataBlocks,
                                      m_pInode->i_file_acl != 0, m_pExt2Fs->m_BlockSize, sectors);
  if (!valid) {
    panic("Ext2: allocated sector count exceeds inode capacity");
  }
  m_pInode->i_blocks = HOST_TO_LITTLE32(sectors);
}
