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

#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
constexpr uint8_t ElfClass32 = 1;
constexpr uint8_t ElfClass64 = 2;
constexpr uint8_t ElfDataLittleEndian = 1;
constexpr uint8_t ElfDataBigEndian = 2;
constexpr uint8_t ElfCurrentVersion = 1;

static_assert(sizeof(Elf_Off) <= sizeof(size_t));
static_assert(sizeof(Elf_Addr) <= sizeof(uintptr_t));
static_assert(sizeof(Elf_Xword) <= sizeof(uintptr_t));

bool isExpectedMachine(Elf_Half machine) {
#if X64 || defined(MACH_HOSTED)
  return machine == 62;  // EM_X86_64
#elif ARM64
  return machine == 183;  // EM_AARCH64
#elif ARMV7
  return machine == 40;  // EM_ARM
#elif X86
  return machine == 3;  // EM_386
#else
  return false;
#endif
}

bool rangeWithinFile(Elf_Off offset, Elf_Xword size, size_t fileSize) {
  if (offset > fileSize) {
    return false;
  }

  return size <= (fileSize - static_cast<size_t>(offset));
}

bool loadPageRange(Elf_Addr address, Elf_Xword size, uintptr_t& start, uintptr_t& end) {
  const uintptr_t maximum = ~uintptr_t{0};
  const uintptr_t pageMask = TargetInfo::getPageOffsetMask();
  const uintptr_t nativeAddress = static_cast<uintptr_t>(address);
  const uintptr_t nativeSize = static_cast<uintptr_t>(size);
  if (nativeSize > (maximum - nativeAddress)) {
    return false;
  }

  const uintptr_t pageOffset = nativeAddress & pageMask;
  if (nativeSize > (maximum - pageOffset)) {
    return false;
  }

  start = nativeAddress & ~pageMask;
  end = nativeAddress + nativeSize;
  if (end & pageMask) {
    if (end > (maximum - pageMask)) {
      return false;
    }
    end = (end + pageMask) & ~pageMask;
  }

  return true;
}
}  // namespace

Elf::ExecutableValidationResult Elf::validateExecutableHeader(const uint8_t* pBuffer, size_t length,
                                                              size_t fileSize,
                                                              ExecutableMetadata& metadata) {
  metadata = ExecutableMetadata{};
  if (!pBuffer || length < sizeof(ElfHeader_t) || fileSize < sizeof(ElfHeader_t)) {
    return ExecutableValidationResult::Malformed;
  }

  ElfHeader_t header;
  MemoryCopy(&header, pBuffer, sizeof(header));
  if (header.ident[0] != 0x7f || header.ident[1] != 'E' || header.ident[2] != 'L' ||
      header.ident[3] != 'F') {
    return ExecutableValidationResult::Malformed;
  }

  const uint8_t expectedClass = BITS_32 ? ElfClass32 : ElfClass64;
  const uint8_t expectedData =
      TargetInfo::isLittleEndian() ? ElfDataLittleEndian : ElfDataBigEndian;
  if (header.ident[4] != expectedClass || header.ident[5] != expectedData ||
      !isExpectedMachine(header.machine)) {
    return ExecutableValidationResult::WrongArchitecture;
  }

  if (header.ident[6] != ElfCurrentVersion || header.version != ElfCurrentVersion) {
    return ExecutableValidationResult::Malformed;
  }
  if (header.type != ET_EXEC && header.type != ET_DYN) {
    return ExecutableValidationResult::UnsupportedType;
  }
  if (header.ehsize != sizeof(ElfHeader_t) || header.phentsize != sizeof(ElfProgramHeader_t) ||
      !header.phnum ||
      header.phnum > (MaximumProgramHeaderTableSize / sizeof(ElfProgramHeader_t))) {
    return ExecutableValidationResult::Malformed;
  }

  const size_t programHeaderSize = static_cast<size_t>(header.phnum) * sizeof(ElfProgramHeader_t);
  if (header.phoff < sizeof(ElfHeader_t) ||
      !rangeWithinFile(header.phoff, programHeaderSize, fileSize)) {
    return ExecutableValidationResult::Malformed;
  }

  metadata.type = header.type;
  metadata.entryPoint = static_cast<uintptr_t>(header.entry);
  metadata.programHeaderOffset = static_cast<size_t>(header.phoff);
  metadata.programHeaderCount = header.phnum;
  metadata.programHeaderSize = programHeaderSize;
  return ExecutableValidationResult::Valid;
}

Elf::ExecutableValidationResult Elf::validateExecutableProgramHeaders(
    const uint8_t* pBuffer, size_t length, size_t fileSize, ExecutableMetadata& metadata) {
  if (!pBuffer || !metadata.programHeaderCount ||
      metadata.programHeaderCount > (MaximumProgramHeaderTableSize / sizeof(ElfProgramHeader_t))) {
    return ExecutableValidationResult::Malformed;
  }

  const size_t expectedSize = metadata.programHeaderCount * sizeof(ElfProgramHeader_t);
  if (metadata.programHeaderSize != expectedSize || length < expectedSize ||
      !rangeWithinFile(metadata.programHeaderOffset, expectedSize, fileSize)) {
    return ExecutableValidationResult::Malformed;
  }

  metadata.loadStart = ~uintptr_t{0};
  metadata.loadEnd = 0;
  metadata.interpreterOffset = 0;
  metadata.interpreterSize = 0;
  metadata.hasInterpreter = false;

  const uintptr_t pageMask = TargetInfo::getPageOffsetMask();
  bool hasLoadSegment = false;
  bool entryPointCovered = false;
  for (size_t i = 0; i < metadata.programHeaderCount; ++i) {
    ElfProgramHeader_t header;
    MemoryCopy(&header, pBuffer + (i * sizeof(header)), sizeof(header));

    if (header.type != PT_NULL && header.filesz &&
        !rangeWithinFile(header.offset, header.filesz, fileSize)) {
      return ExecutableValidationResult::Malformed;
    }

    if (header.type == PT_INTERP) {
      if (metadata.hasInterpreter) {
        return ExecutableValidationResult::MultipleInterpreters;
      }
      if (header.filesz < 2 || header.filesz > MaximumInterpreterSize ||
          !rangeWithinFile(header.offset, header.filesz, fileSize)) {
        return ExecutableValidationResult::Malformed;
      }

      metadata.hasInterpreter = true;
      metadata.interpreterOffset = static_cast<size_t>(header.offset);
      metadata.interpreterSize = static_cast<size_t>(header.filesz);
    }

    if (header.type != PT_LOAD) {
      continue;
    }
    if (!rangeWithinFile(header.offset, header.filesz, fileSize) || header.filesz > header.memsz) {
      return ExecutableValidationResult::Malformed;
    }
    if (header.align > 1 &&
        ((header.align & (header.align - 1)) ||
         ((header.vaddr & (header.align - 1)) != (header.offset & (header.align - 1))))) {
      return ExecutableValidationResult::Malformed;
    }
    if ((header.vaddr & pageMask) != (header.offset & pageMask)) {
      return ExecutableValidationResult::Malformed;
    }
    if (!header.memsz) {
      continue;
    }

    uintptr_t pageStart = 0;
    uintptr_t pageEnd = 0;
    if (!loadPageRange(header.vaddr, header.memsz, pageStart, pageEnd)) {
      return ExecutableValidationResult::Malformed;
    }

    for (size_t j = 0; j < i; ++j) {
      ElfProgramHeader_t previous;
      MemoryCopy(&previous, pBuffer + (j * sizeof(previous)), sizeof(previous));
      if (previous.type != PT_LOAD || !previous.memsz) {
        continue;
      }

      uintptr_t previousStart = 0;
      uintptr_t previousEnd = 0;
      if (!loadPageRange(previous.vaddr, previous.memsz, previousStart, previousEnd)) {
        return ExecutableValidationResult::Malformed;
      }
      if (pageStart < previousEnd && previousStart < pageEnd) {
        return ExecutableValidationResult::UnsupportedLayout;
      }
    }

    hasLoadSegment = true;
    if (pageStart < metadata.loadStart) {
      metadata.loadStart = pageStart;
    }
    if (pageEnd > metadata.loadEnd) {
      metadata.loadEnd = pageEnd;
    }

    if ((header.flags & PF_X) && metadata.entryPoint >= header.vaddr &&
        (metadata.entryPoint - header.vaddr) < header.memsz) {
      entryPointCovered = true;
    }
  }

  if (!hasLoadSegment || !entryPointCovered) {
    return ExecutableValidationResult::Malformed;
  }
  if (metadata.type == ET_DYN && metadata.loadStart != 0) {
    return ExecutableValidationResult::UnsupportedLayout;
  }

  return ExecutableValidationResult::Valid;
}

Elf::ExecutableValidationResult Elf::validateExecutableInterpreter(
    const uint8_t* pBuffer, size_t length, const ExecutableMetadata& metadata) {
  if (!metadata.hasInterpreter) {
    return ExecutableValidationResult::Valid;
  }
  if (!pBuffer || metadata.interpreterSize < 2 ||
      metadata.interpreterSize > MaximumInterpreterSize || length < metadata.interpreterSize ||
      pBuffer[metadata.interpreterSize - 1] != 0) {
    return ExecutableValidationResult::Malformed;
  }

  return ExecutableValidationResult::Valid;
}
