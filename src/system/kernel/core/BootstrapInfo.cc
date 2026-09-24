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

#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

static_assert(sizeof(BootstrapStruct_t::MemoryMapEntry) == 32,
              "bootstrap memory-map entries must match the UEFI wire format");
static_assert(sizeof(BootstrapStruct_t) == 108, "bootstrap wire layout must match the UEFI loader");

BootstrapStruct_t::BootstrapStruct_t() {
  flags = 0;
  reserved = 0;
}

void BootstrapStruct_t::setMemoryMap(const MemoryMapEntry* entries, size_t count) {
  mmap_addr = reinterpret_cast<uintptr_t>(entries);
  mmap_entry_size = sizeof(MemoryMapEntry);
  mmap_length = count * mmap_entry_size;
  flags |= BOOTSTRAP_FLAG_MEMORY_MAP;
}

void BootstrapStruct_t::setModules(const Module* modules, size_t count) {
  mods_addr = reinterpret_cast<uintptr_t>(modules);
  mods_count = count;
  flags |= BOOTSTRAP_FLAG_MODULES;
}

void BootstrapStruct_t::setCommandLine(const char* text) {
  cmdline = reinterpret_cast<uintptr_t>(text);
  flags |= BOOTSTRAP_FLAG_CMDLINE;
}

void BootstrapStruct_t::setUefi() {
  flags |= BOOTSTRAP_FLAG_UEFI;
}

void BootstrapStruct_t::setAcpiRsdp(uintptr_t address) {
  acpi_rsdp = address;
  flags |= BOOTSTRAP_FLAG_ACPI;
}

bool BootstrapStruct_t::isInitrdLoaded() const {
  if (flags & BOOTSTRAP_FLAG_MODULES)
    return (mods_count != 0) && mods_addr;
  else
    return false;
}

uint8_t* BootstrapStruct_t::getInitrdAddress() const {
  const Module* modules = getModuleArray();
  if (isInitrdLoaded() && modules)
    return reinterpret_cast<uint8_t*>(modules[0].base);
  else
    return 0;
}

size_t BootstrapStruct_t::getInitrdSize() const {
  const Module* modules = getModuleArray();
  if (isInitrdLoaded() && modules)
    return modules[0].end - modules[0].base;
  else
    return 0;
}

char* BootstrapStruct_t::getCommandLine() const {
  if (flags & BOOTSTRAP_FLAG_CMDLINE)
    return reinterpret_cast<char*>(cmdline);
  else
    return 0;
}

size_t BootstrapStruct_t::getSectionHeaderCount() const {
  if (flags & BOOTSTRAP_FLAG_ELF)
    return num;
  else
    return 0;
}

size_t BootstrapStruct_t::getSectionHeaderEntrySize() const {
  if (flags & BOOTSTRAP_FLAG_ELF)
    return size;
  else
    return 0;
}

size_t BootstrapStruct_t::getSectionHeaderStringTableIndex() const {
  if (flags & BOOTSTRAP_FLAG_ELF)
    return shndx;
  else
    return 0;
}

uintptr_t BootstrapStruct_t::getSectionHeaders() const {
  if (flags & BOOTSTRAP_FLAG_ELF)
    return addr;
  else
    return 0;
}

void* BootstrapStruct_t::getMemoryMap() const {
  if (flags & BOOTSTRAP_FLAG_MEMORY_MAP)
    return reinterpret_cast<void*>(mmap_addr);
  else
    return 0;
}

uint64_t BootstrapStruct_t::getMemoryMapEntryAddress(void* opaque) const {
  if (!opaque)
    return 0;

  MemoryMapEntry* entry = reinterpret_cast<MemoryMapEntry*>(opaque);
  return entry->address;
}

uint64_t BootstrapStruct_t::getMemoryMapEntryLength(void* opaque) const {
  if (!opaque)
    return 0;

  MemoryMapEntry* entry = reinterpret_cast<MemoryMapEntry*>(opaque);
  return entry->length;
}

uint32_t BootstrapStruct_t::getMemoryMapEntryType(void* opaque) const {
  if (!opaque)
    return 0;

  MemoryMapEntry* entry = reinterpret_cast<MemoryMapEntry*>(opaque);
  return entry->type;
}

void* BootstrapStruct_t::nextMemoryMapEntry(void* opaque) const {
  if (!opaque)
    return 0;

  MemoryMapEntry* entry = reinterpret_cast<MemoryMapEntry*>(opaque);
  uintptr_t entry_addr = reinterpret_cast<uintptr_t>(opaque);
  const uintptr_t entry_size = mmap_entry_size ? mmap_entry_size : sizeof(MemoryMapEntry);
  void* new_opaque = reinterpret_cast<void*>(entry_addr + entry_size);

  if (reinterpret_cast<uintptr_t>(new_opaque) >= (mmap_addr + mmap_length))
    return 0;
  else
    return new_opaque;
}

size_t BootstrapStruct_t::getModuleCount() const {
  if (flags & BOOTSTRAP_FLAG_MODULES)
    return mods_count;
  else
    return 0;
}

void* BootstrapStruct_t::getModuleBase() const {
  if (flags & BOOTSTRAP_FLAG_MODULES)
    return reinterpret_cast<void*>(mods_addr);
  else
    return 0;
}

uintptr_t BootstrapStruct_t::getAcpiRsdp() const {
  return (flags & BOOTSTRAP_FLAG_ACPI) ? acpi_rsdp : 0;
}

uintptr_t BootstrapStruct_t::getSmbios() const {
  return (flags & BOOTSTRAP_FLAG_SMBIOS) ? smbios : 0;
}

bool BootstrapStruct_t::isUefi() const {
  return (flags & BOOTSTRAP_FLAG_UEFI) != 0;
}

bool BootstrapStruct_t::getFramebuffer(FramebufferInfo& info) const {
  if (!(flags & BOOTSTRAP_FLAG_FRAMEBUFFER) || !framebuffer || !framebuffer_width ||
      !framebuffer_height || framebuffer_bpp != 32 || framebuffer_format > 1 ||
      framebuffer_width > ~uint32_t(0) / 4 || framebuffer_pitch < framebuffer_width * 4 ||
      (framebuffer_pitch & 3) ||
      static_cast<uint64_t>(framebuffer_pitch) * framebuffer_height > ~uint64_t(0) - framebuffer)
    return false;
  info = {framebuffer,       framebuffer_width, framebuffer_height,
          framebuffer_pitch, framebuffer_bpp,   framebuffer_format};
  return true;
}
