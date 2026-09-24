/*
 * Copyright (c) 2026 Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
bool fail(const char* detail) {
  ERROR("HOSTED-ELF-VALIDATION-TEST: FAIL: " << detail);
  return false;
}

class ExecutableValidationFixture final : public Elf {
 public:
  using Header = ElfHeader_t;
  using ProgramHeader = ElfProgramHeader_t;
  using Metadata = ExecutableMetadata;
  using Result = ExecutableValidationResult;

  static constexpr size_t ImageSize = 8192;
  static constexpr size_t ProgramHeaderCapacity = 3;

  ExecutableValidationFixture() {
    reset();
  }

  void reset(Elf_Half type = ET_EXEC, size_t programHeaderCount = 1) {
    ByteSet(&header, 0, sizeof(header));
    ByteSet(programHeaders, 0, sizeof(programHeaders));
    ByteSet(image, 0, sizeof(image));
    terminateInterpreter = true;

    header.ident[0] = 0x7f;
    header.ident[1] = 'E';
    header.ident[2] = 'L';
    header.ident[3] = 'F';
    header.ident[4] = BITS_32 ? 1 : 2;
    header.ident[5] = TargetInfo::isLittleEndian() ? 1 : 2;
    header.ident[6] = 1;
    header.type = type;
#if X64 || defined(MACH_HOSTED)
    header.machine = 62;  // EM_X86_64
#endif
    header.version = 1;
    header.entry = type == ET_DYN ? 0x100 : 0x400100;
    header.phoff = sizeof(header);
    header.ehsize = sizeof(header);
    header.phentsize = sizeof(programHeaders[0]);
    header.phnum = programHeaderCount;

    ProgramHeader& load = programHeaders[0];
    load.type = PT_LOAD;
    load.flags = PF_R | PF_X;
    load.offset = 0;
    load.vaddr = type == ET_DYN ? 0 : 0x400000;
    load.filesz = ImageSize;
    load.memsz = ImageSize;
    load.align = TargetInfo::getPageSize();
  }

  void addInterpreter(size_t index = 1, size_t offset = 512, size_t size = sizeof(Interpreter)) {
    header.phnum = index + 1;
    ProgramHeader& interpreter = programHeaders[index];
    ByteSet(&interpreter, 0, sizeof(interpreter));
    interpreter.type = PT_INTERP;
    interpreter.offset = offset;
    interpreter.filesz = size;
    interpreter.memsz = size;
    interpreter.align = 1;
  }

  Result validate(size_t programHeaderLength = ~size_t{0}, size_t interpreterLength = ~size_t{0}) {
    Metadata metadata{};
    return validate(metadata, programHeaderLength, interpreterLength);
  }

  Result validate(Metadata& metadata, size_t programHeaderLength = ~size_t{0},
                  size_t interpreterLength = ~size_t{0}) {
    sync();
    metadata = Metadata{};
    Result result = Elf::validateExecutableHeader(image + 1, sizeof(Header), ImageSize, metadata);
    if (result != Result::Valid) {
      return result;
    }

    const size_t actualProgramHeaderLength =
        programHeaderLength == ~size_t{0} ? metadata.programHeaderSize : programHeaderLength;
    result = Elf::validateExecutableProgramHeaders(image + 1 + metadata.programHeaderOffset,
                                                   actualProgramHeaderLength, ImageSize, metadata);
    if (result != Result::Valid || !metadata.hasInterpreter) {
      return result;
    }

    const size_t actualInterpreterLength =
        interpreterLength == ~size_t{0} ? metadata.interpreterSize : interpreterLength;
    return Elf::validateExecutableInterpreter(image + 1 + metadata.interpreterOffset,
                                              actualInterpreterLength, metadata);
  }

  Result validateHeader(size_t length = sizeof(Header), size_t fileSize = ImageSize) {
    sync();
    Metadata metadata{};
    return Elf::validateExecutableHeader(image + 1, length, fileSize, metadata);
  }

  Header header;
  ProgramHeader programHeaders[ProgramHeaderCapacity];
  bool terminateInterpreter;

 private:
  static constexpr char Interpreter[] = "/lib/ld.so";

  void sync() {
    ByteSet(image, 0, sizeof(image));
    uint8_t* const data = image + 1;
    MemoryCopy(data, &header, sizeof(header));

    const size_t count =
        header.phnum < ProgramHeaderCapacity ? header.phnum : ProgramHeaderCapacity;
    const size_t programHeaderSize = count * sizeof(programHeaders[0]);
    if (header.phoff <= ImageSize && programHeaderSize <= (ImageSize - header.phoff)) {
      MemoryCopy(data + header.phoff, programHeaders, programHeaderSize);
    }

    for (size_t i = 0; i < count; ++i) {
      const ProgramHeader& programHeader = programHeaders[i];
      if (programHeader.type != PT_INTERP || programHeader.offset >= ImageSize) {
        continue;
      }

      const size_t available = ImageSize - programHeader.offset;
      const size_t copySize = sizeof(Interpreter) < available ? sizeof(Interpreter) : available;
      MemoryCopy(data + programHeader.offset, Interpreter, copySize);
      if (!terminateInterpreter && programHeader.filesz && programHeader.filesz <= available) {
        data[programHeader.offset + programHeader.filesz - 1] = 'x';
      }
    }
  }

  // Every validation stage is deliberately exercised with unaligned input.
  alignas(Elf_Xword) uint8_t image[ImageSize + 1];
};

constexpr char ExecutableValidationFixture::Interpreter[];

bool expect(Elf::ExecutableValidationResult actual, Elf::ExecutableValidationResult expected,
            const char* detail) {
  return actual == expected || fail(detail);
}

#define EXPECT_RESULT(actual, expected, detail)  \
  do {                                           \
    if (!expect((actual), (expected), (detail))) \
      return false;                              \
  } while (0)

bool validImagesAndMetadata() {
  using Fixture = ExecutableValidationFixture;
  using Result = Elf::ExecutableValidationResult;

  Fixture fixture;
  Fixture::Metadata metadata{};
  EXPECT_RESULT(fixture.validate(metadata), Result::Valid, "valid ET_EXEC rejected");
  if (metadata.type != ET_EXEC || metadata.entryPoint != 0x400100 ||
      metadata.programHeaderOffset != sizeof(Fixture::Header) || metadata.programHeaderCount != 1 ||
      metadata.programHeaderSize != sizeof(Fixture::ProgramHeader) ||
      metadata.loadStart != 0x400000 || metadata.loadEnd != 0x400000 + Fixture::ImageSize ||
      metadata.hasInterpreter) {
    return fail("valid ET_EXEC metadata was incorrect");
  }

  fixture.reset(ET_DYN);
  EXPECT_RESULT(fixture.validate(metadata), Result::Valid, "valid zero-based ET_DYN rejected");
  if (metadata.loadStart != 0 || metadata.loadEnd != Fixture::ImageSize) {
    return fail("valid ET_DYN metadata was incorrect");
  }

  fixture.reset(ET_EXEC, 2);
  fixture.programHeaders[0].filesz = TargetInfo::getPageSize();
  fixture.programHeaders[0].memsz = TargetInfo::getPageSize();
  fixture.programHeaders[1].type = PT_LOAD;
  fixture.programHeaders[1].flags = PF_R;
  fixture.programHeaders[1].offset = TargetInfo::getPageSize();
  fixture.programHeaders[1].vaddr = 0x400000 + TargetInfo::getPageSize();
  fixture.programHeaders[1].filesz = TargetInfo::getPageSize();
  fixture.programHeaders[1].memsz = TargetInfo::getPageSize();
  fixture.programHeaders[1].align = TargetInfo::getPageSize();
  EXPECT_RESULT(fixture.validate(), Result::Valid, "non-overlapping load segments rejected");
  return true;
}

bool headerValidation() {
  using Fixture = ExecutableValidationFixture;
  using Result = Elf::ExecutableValidationResult;

  Fixture fixture;
  Fixture::Metadata metadata{};
  EXPECT_RESULT(
      Elf::validateExecutableHeader(nullptr, sizeof(Fixture::Header), Fixture::ImageSize, metadata),
      Result::Malformed, "null ELF header accepted");
  EXPECT_RESULT(fixture.validateHeader(sizeof(Fixture::Header) - 1), Result::Malformed,
                "truncated ELF header accepted");
  EXPECT_RESULT(fixture.validateHeader(sizeof(Fixture::Header), sizeof(Fixture::Header) - 1),
                Result::Malformed, "ELF header outside file accepted");

  fixture.reset();
  fixture.header.ident[0] = 0;
  EXPECT_RESULT(fixture.validateHeader(), Result::Malformed, "bad ELF magic accepted");
  fixture.reset();
  fixture.header.ident[4] = BITS_32 ? 2 : 1;
  EXPECT_RESULT(fixture.validateHeader(), Result::WrongArchitecture, "wrong ELF class accepted");
  fixture.reset();
  fixture.header.ident[5] = TargetInfo::isLittleEndian() ? 2 : 1;
  EXPECT_RESULT(fixture.validateHeader(), Result::WrongArchitecture,
                "wrong ELF byte order accepted");
  fixture.reset();
  fixture.header.ident[6] = 0;
  EXPECT_RESULT(fixture.validateHeader(), Result::Malformed, "bad ident version accepted");
  fixture.reset();
  fixture.header.version = 0;
  EXPECT_RESULT(fixture.validateHeader(), Result::Malformed, "bad ELF version accepted");
  fixture.reset();
  fixture.header.type = ET_REL;
  EXPECT_RESULT(fixture.validateHeader(), Result::UnsupportedType, "ET_REL accepted for execution");
  fixture.reset();
  fixture.header.machine = 0;
  EXPECT_RESULT(fixture.validateHeader(), Result::WrongArchitecture, "wrong ELF machine accepted");
  fixture.reset();
  --fixture.header.ehsize;
  EXPECT_RESULT(fixture.validateHeader(), Result::Malformed, "bad ELF header size accepted");
  fixture.reset();
  --fixture.header.phentsize;
  EXPECT_RESULT(fixture.validateHeader(), Result::Malformed, "bad program header size accepted");
  fixture.reset();
  fixture.header.phnum = 0;
  EXPECT_RESULT(fixture.validateHeader(), Result::Malformed, "empty program header table accepted");
  fixture.reset();
  fixture.header.phnum = (Elf::MaximumProgramHeaderTableSize / sizeof(Fixture::ProgramHeader)) + 1;
  EXPECT_RESULT(fixture.validateHeader(), Result::Malformed,
                "oversized program header table accepted");
  fixture.reset();
  fixture.header.phoff = sizeof(Fixture::Header) - 1;
  EXPECT_RESULT(fixture.validateHeader(), Result::Malformed,
                "overlapping ELF and program headers accepted");
  fixture.reset();
  fixture.header.phoff = Fixture::ImageSize - 1;
  EXPECT_RESULT(fixture.validateHeader(), Result::Malformed,
                "program header table past EOF accepted");
  fixture.reset();
  fixture.header.phoff = ~Elf_Off{0} - 8;
  EXPECT_RESULT(fixture.validateHeader(sizeof(Fixture::Header), ~size_t{0}), Result::Malformed,
                "overflowing program header table accepted");
  return true;
}

bool loadSegmentValidation() {
  using Fixture = ExecutableValidationFixture;
  using Result = Elf::ExecutableValidationResult;

  Fixture fixture;
  EXPECT_RESULT(fixture.validate(sizeof(Fixture::ProgramHeader) - 1), Result::Malformed,
                "truncated program header buffer accepted");
  fixture.reset();
  fixture.programHeaders[0].type = PT_NOTE;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "ELF without a load segment accepted");
  fixture.reset();
  fixture.programHeaders[0].filesz = 0;
  fixture.programHeaders[0].memsz = 0;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "ELF without a nonempty load accepted");
  fixture.reset();
  fixture.programHeaders[0].memsz = Fixture::ImageSize - 1;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "load filesz greater than memsz accepted");
  fixture.reset();
  fixture.programHeaders[0].offset = Fixture::ImageSize - 8;
  fixture.programHeaders[0].filesz = 16;
  fixture.programHeaders[0].memsz = 16;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "load segment past EOF accepted");
  fixture.reset(ET_EXEC, 2);
  fixture.programHeaders[1].type = PT_NOTE;
  fixture.programHeaders[1].offset = Fixture::ImageSize - 8;
  fixture.programHeaders[1].filesz = 16;
  EXPECT_RESULT(fixture.validate(), Result::Malformed,
                "file-backed non-load segment past EOF accepted");
  fixture.reset();
  fixture.programHeaders[0].vaddr = ~Elf_Addr{0} - 16;
  fixture.programHeaders[0].offset =
      fixture.programHeaders[0].vaddr & TargetInfo::getPageOffsetMask();
  fixture.programHeaders[0].filesz = 0;
  fixture.programHeaders[0].memsz = 32;
  fixture.programHeaders[0].align = 1;
  fixture.header.entry = fixture.programHeaders[0].vaddr;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "overflowing load address accepted");
  fixture.reset();
  fixture.programHeaders[0].vaddr = ~Elf_Addr{0} - TargetInfo::getPageOffsetMask();
  fixture.programHeaders[0].filesz = 0;
  fixture.programHeaders[0].memsz = TargetInfo::getPageOffsetMask();
  fixture.header.entry = fixture.programHeaders[0].vaddr;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "overflowing page rounding accepted");
  fixture.reset();
  fixture.programHeaders[0].align = 3;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "non-power-of-two load alignment accepted");
  fixture.reset();
  fixture.programHeaders[0].offset = 1;
  fixture.programHeaders[0].filesz = 128;
  fixture.programHeaders[0].memsz = TargetInfo::getPageSize();
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "misaligned load segment accepted");
  fixture.reset();
  fixture.programHeaders[0].align = 1;
  fixture.programHeaders[0].offset = 1;
  fixture.programHeaders[0].filesz = 128;
  fixture.programHeaders[0].memsz = TargetInfo::getPageSize();
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "page-incongruent load segment accepted");
  fixture.reset();
  fixture.header.entry = 0x500000;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "unmapped entry point accepted");
  fixture.reset();
  fixture.programHeaders[0].flags = PF_R;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "non-executable entry point accepted");

  fixture.reset(ET_DYN);
  fixture.programHeaders[0].vaddr = TargetInfo::getPageSize();
  fixture.header.entry = TargetInfo::getPageSize() + 0x100;
  EXPECT_RESULT(fixture.validate(), Result::UnsupportedLayout, "nonzero-based ET_DYN accepted");

  fixture.reset(ET_EXEC, 2);
  fixture.programHeaders[0].filesz = TargetInfo::getPageSize() / 2;
  fixture.programHeaders[0].memsz = TargetInfo::getPageSize() / 2;
  fixture.programHeaders[0].align = 1;
  fixture.programHeaders[1].type = PT_LOAD;
  fixture.programHeaders[1].flags = PF_R;
  fixture.programHeaders[1].offset = TargetInfo::getPageSize() / 2;
  fixture.programHeaders[1].vaddr = 0x400000 + (TargetInfo::getPageSize() / 2);
  fixture.programHeaders[1].filesz = TargetInfo::getPageSize() / 2;
  fixture.programHeaders[1].memsz = TargetInfo::getPageSize() / 2;
  fixture.programHeaders[1].align = 1;
  EXPECT_RESULT(fixture.validate(), Result::UnsupportedLayout,
                "page-overlapping load segments accepted");
  return true;
}

bool interpreterValidation() {
  using Fixture = ExecutableValidationFixture;
  using Result = Elf::ExecutableValidationResult;

  Fixture fixture;
  Fixture::Metadata metadata{};
  fixture.addInterpreter();
  EXPECT_RESULT(fixture.validate(metadata), Result::Valid, "valid PT_INTERP rejected");
  if (!metadata.hasInterpreter || metadata.interpreterOffset != 512 || !metadata.interpreterSize) {
    return fail("valid PT_INTERP metadata was incorrect");
  }

  fixture.reset();
  fixture.addInterpreter(1, 512, 1);
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "one-byte PT_INTERP accepted");
  fixture.reset();
  fixture.addInterpreter(1, 512, Elf::MaximumInterpreterSize + 1);
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "oversized PT_INTERP accepted");
  fixture.reset();
  fixture.addInterpreter(1, Fixture::ImageSize - 1, 2);
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "PT_INTERP past EOF accepted");
  fixture.reset();
  fixture.addInterpreter();
  fixture.addInterpreter(2, 640);
  EXPECT_RESULT(fixture.validate(), Result::MultipleInterpreters, "duplicate PT_INTERP accepted");
  fixture.reset();
  fixture.addInterpreter();
  fixture.terminateInterpreter = false;
  EXPECT_RESULT(fixture.validate(), Result::Malformed, "unterminated PT_INTERP accepted");
  fixture.reset();
  fixture.addInterpreter();
  EXPECT_RESULT(fixture.validate(~size_t{0}, 1), Result::Malformed,
                "truncated PT_INTERP buffer accepted");
  fixture.reset();
  fixture.addInterpreter(1, 512, Elf::MaximumInterpreterSize);
  EXPECT_RESULT(fixture.validate(), Result::Valid, "maximum-size PT_INTERP rejected");
  return true;
}

#undef EXPECT_RESULT
}  // namespace

bool runHostedElfValidationRegressions() {
  if (!validImagesAndMetadata() || !headerValidation() || !loadSegmentValidation() ||
      !interpreterValidation()) {
    return false;
  }

  NOTICE("HOSTED-ELF-VALIDATION-TEST: PASS");
  return true;
}
