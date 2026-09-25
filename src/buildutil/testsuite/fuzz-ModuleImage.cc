/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/linker/ModuleImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace {
constexpr size_t ImageBytes = 1024;
constexpr size_t DynamicOffset = 0x100;
constexpr size_t DynsymOffset = 0x180;
constexpr size_t DynstrOffset = 0x1a0;
constexpr size_t SymtabOffset = 0x1b0;
constexpr size_t StrtabOffset = 0x1d0;
constexpr size_t ShstrtabOffset = 0x1e0;
constexpr size_t SectionOffset = 0x200;
constexpr size_t SectionCount = 7;

using Image = std::array<uint8_t, ImageBytes>;

template <typename T>
void put(Image& image, size_t offset, const T& value) {
  std::memcpy(image.data() + offset, &value, sizeof(value));
}

Image structuredImage() {
  static_assert(DynamicOffset + 5 * sizeof(ModuleImage::Dynamic) <= DynsymOffset);
  static_assert(DynsymOffset + sizeof(ModuleImage::Symbol) <= DynstrOffset);
  static_assert(SymtabOffset + sizeof(ModuleImage::Symbol) <= StrtabOffset);
  static_assert(SectionOffset + SectionCount * sizeof(ModuleImage::Section) <= ImageBytes);

  Image image{};
  ModuleImage::Header header{};
  header.ident[0] = 0x7f;
  header.ident[1] = 'E';
  header.ident[2] = 'L';
  header.ident[3] = 'F';
  header.ident[4] = 2;
  header.ident[5] = 1;
  header.ident[6] = 1;
  header.type = ET_DYN;
  header.machine = 62;
  header.version = 1;
  header.phoff = sizeof(header);
  header.shoff = SectionOffset;
  header.ehsize = sizeof(header);
  header.phentsize = sizeof(ModuleImage::Segment);
  header.phnum = 2;
  header.shentsize = sizeof(ModuleImage::Section);
  header.shnum = SectionCount;
  header.shstrndx = 1;
  put(image, 0, header);

  ModuleImage::Segment segment{};
  segment.type = PT_LOAD;
  segment.flags = PF_R | PF_X;
  segment.filesz = ImageBytes;
  segment.memsz = ImageBytes;
  segment.align = 4096;
  put(image, header.phoff, segment);
  segment = {};
  segment.type = PT_DYNAMIC;
  segment.flags = PF_R;
  segment.offset = DynamicOffset;
  segment.vaddr = DynamicOffset;
  segment.filesz = 5 * sizeof(ModuleImage::Dynamic);
  segment.memsz = segment.filesz;
  put(image, header.phoff + sizeof(segment), segment);

  const auto putDynamic = [&](size_t index, int64_t tag, uint64_t value) {
    ModuleImage::Dynamic dynamic{};
    dynamic.tag = tag;
    dynamic.un.val = value;
    put(image, DynamicOffset + index * sizeof(dynamic), dynamic);
  };
  putDynamic(0, DT_SYMTAB, DynsymOffset);
  putDynamic(1, DT_STRTAB, DynstrOffset);
  putDynamic(2, DT_STRSZ, 1);
  putDynamic(3, DT_SYMENT, sizeof(ModuleImage::Symbol));

  ModuleImage::Section section{};
  section.type = SHT_STRTAB;
  section.offset = ShstrtabOffset;
  section.size = 1;
  put(image, SectionOffset + sizeof(section), section);
  section = {};
  section.type = SHT_DYNAMIC;
  section.flags = SHF_ALLOC;
  section.addr = DynamicOffset;
  section.offset = DynamicOffset;
  section.size = 5 * sizeof(ModuleImage::Dynamic);
  section.entsize = sizeof(ModuleImage::Dynamic);
  put(image, SectionOffset + 2 * sizeof(section), section);
  section = {};
  section.type = SHT_DYNSYM;
  section.flags = SHF_ALLOC;
  section.addr = DynsymOffset;
  section.offset = DynsymOffset;
  section.size = sizeof(ModuleImage::Symbol);
  section.link = 4;
  section.entsize = sizeof(ModuleImage::Symbol);
  put(image, SectionOffset + 3 * sizeof(section), section);
  section = {};
  section.type = SHT_STRTAB;
  section.flags = SHF_ALLOC;
  section.addr = DynstrOffset;
  section.offset = DynstrOffset;
  section.size = 1;
  put(image, SectionOffset + 4 * sizeof(section), section);
  section = {};
  section.type = SHT_SYMTAB;
  section.offset = SymtabOffset;
  section.size = sizeof(ModuleImage::Symbol);
  section.link = 6;
  section.entsize = sizeof(ModuleImage::Symbol);
  put(image, SectionOffset + 5 * sizeof(section), section);
  section = {};
  section.type = SHT_STRTAB;
  section.offset = StrtabOffset;
  section.size = 1;
  put(image, SectionOffset + 6 * sizeof(section), section);

  // The empty symbol tables reach metadata validation without creating a loadable module.
  return image;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size <= ModuleImage::MaximumImageBytes) {
    ModuleImage plan;
    plan.preflight(data, size);
  }

  static const Image baseline = structuredImage();
  Image image = baseline;
  if (size > 2) {
    const size_t offset = ((static_cast<size_t>(data[0]) << 8) | data[1]) % image.size();
    const size_t bytes = std::min(size - 2, image.size() - offset);
    std::memcpy(image.data() + offset, data + 2, bytes);
  }
  ModuleImage plan;
  plan.preflight(image.data(), image.size());
  return 0;
}
