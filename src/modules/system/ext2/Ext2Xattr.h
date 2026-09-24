/* Copyright (c) 2026, Pedigree Developers. */
#ifndef EXT2_XATTR_H
#define EXT2_XATTR_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StringView.h"

#include "modules/system/vfs/ExtendedAttributes.h"

namespace Ext2Ea {
constexpr uint32_t Magic = 0xea020000;
constexpr unsigned User = 1;
struct Header {
  uint32_t magic, references, blocks, hash, reserved[4];
};
struct Entry {
  uint8_t nameLength, nameIndex;
  uint16_t valueOffset;
  uint32_t valueBlock, valueLength, hash;
} __attribute__((packed));
static_assert(sizeof(Header) == 32, "Ext2 EA header");
static_assert(sizeof(Entry) == 16, "Ext2 EA entry");

XattrStatus validate(const void* block, size_t size);
XattrStatus get(const void* block, size_t size, const StringView& name, void* output,
                size_t capacity, size_t& required);
XattrStatus list(const void* block, size_t size, void* output, size_t capacity, size_t& required);
XattrStatus rebuild(const void* oldBlock, size_t size, const StringView& name, const void* value,
                    size_t length, unsigned flags, bool remove, void* replacement, bool& empty);
}  // namespace Ext2Ea
#endif
