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

#ifndef KERNEL_BOOTSTRAPINFO_H
#define KERNEL_BOOTSTRAPINFO_H
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

#include <config.h>

/** @addtogroup kernel
 * @{ */

class BootstrapStruct_t;

extern BootstrapStruct_t* g_pBootstrapInfo EXPORTED_PUBLIC;

#define BOOTSTRAP_FLAG_CMDLINE 0x001
#define BOOTSTRAP_FLAG_MODULES 0x002
#define BOOTSTRAP_FLAG_ELF 0x004
#define BOOTSTRAP_FLAG_MEMORY_MAP 0x008
#define BOOTSTRAP_FLAG_ACPI 0x010
#define BOOTSTRAP_FLAG_SMBIOS 0x020
#define BOOTSTRAP_FLAG_FRAMEBUFFER 0x040
#define BOOTSTRAP_FLAG_UEFI 0x080

#if HOSTED
// Required to specify C linkage for the hosted bootstrap friend declaration.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmain"
#endif
extern "C" int main(int argc, char* argv[]);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

class EXPORTED_PUBLIC BootstrapStruct_t {
#if HOSTED
  friend int ::main(int argc, char* argv[]);
#endif

 public:
  BootstrapStruct_t();

  bool isInitrdLoaded() const;
  uint8_t* getInitrdAddress() const;
  size_t getInitrdSize() const;

  char* getCommandLine() const;

  size_t getSectionHeaderCount() const;
  size_t getSectionHeaderEntrySize() const;
  size_t getSectionHeaderStringTableIndex() const;
  uintptr_t getSectionHeaders() const;

  void* getMemoryMap() const;
  uint64_t getMemoryMapEntryAddress(void* opaque) const;
  uint64_t getMemoryMapEntryLength(void* opaque) const;
  uint32_t getMemoryMapEntryType(void* opaque) const;
  void* nextMemoryMapEntry(void* opaque) const;

  size_t getModuleCount() const;
  void* getModuleBase() const;

  typedef uint64_t bootstrap_uintptr_t;

  typedef struct {
    bootstrap_uintptr_t base;
    bootstrap_uintptr_t end;
    bootstrap_uintptr_t name_ptr;
    bootstrap_uintptr_t pad;
  } PACKED Module;

  typedef struct {
    uint32_t size;
    uint64_t address;
    uint64_t length;
    uint32_t type;
  } MemoryMapEntry;

  void setMemoryMap(const MemoryMapEntry* entries, size_t count);
  void setModules(const Module* modules, size_t count);
  void setCommandLine(const char* text);
  void setUefi();
  void setAcpiRsdp(uintptr_t address);

  const Module* getModuleArray() const {
    return reinterpret_cast<const Module*>(getModuleBase());
  }

  uintptr_t getAcpiRsdp() const;
  uintptr_t getSmbios() const;
  bool isUefi() const;

  struct FramebufferInfo {
    uint64_t address;
    // Format 0 has red in the low byte; format 1 has blue in the low byte.
    uint32_t width, height, pitch, bpp, format;
  };
  bool getFramebuffer(FramebufferInfo& info) const;

 private:
  uint32_t flags;
  uint32_t reserved;
  uint32_t mods_count;
  bootstrap_uintptr_t mods_addr;
  uint32_t num;
  uint32_t size;
  uint32_t shndx;
  bootstrap_uintptr_t addr;
  bootstrap_uintptr_t mmap_addr;
  uint32_t mmap_length;
  uint32_t mmap_entry_size;
  bootstrap_uintptr_t cmdline;
  bootstrap_uintptr_t acpi_rsdp;
  bootstrap_uintptr_t smbios;
  bootstrap_uintptr_t framebuffer;
  uint32_t framebuffer_width;
  uint32_t framebuffer_height;
  uint32_t framebuffer_pitch;
  uint32_t framebuffer_bpp;
  uint32_t framebuffer_format;
} PACKED;

/** @} */

#endif
