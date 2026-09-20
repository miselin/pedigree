#include "pedigree/kernel/core/SlamBitmap.h"
#include "pedigree/kernel/utilities/utility.h"

void SlamBitmap::useMemory(void* memory, size_t entryCount, size_t pageCount) {
  m_reserved = static_cast<uint64_t*>(adjust_pointer(memory, 0));
  m_mapped = static_cast<uint64_t*>(adjust_pointer(memory, entryCount * sizeof(uint64_t)));
  m_ready = static_cast<uint64_t*>(adjust_pointer(memory, entryCount * sizeof(uint64_t) * 2));
  // m_Entries = static_cast<Entry*>(memory);
  m_EntryCount = entryCount;
  m_PageCount = pageCount;
  m_FirstFreeEntry = 0;
}

uint64_t SlamBitmap::reservedBits(size_t entry) const {
  return m_reserved[entry];  // m_Entries[entry].reserved;
}

uintptr_t SlamBitmap::metadataAddress(size_t entry) const {
  // return reinterpret_cast<uintptr_t>(&m_Entries[entry]);
  // TODO: check this is sensible?
  return reinterpret_cast<uintptr_t>(&m_reserved[entry]);
}

static bool hasFreePage(uint64_t reserved, size_t firstPage, size_t pageCount) {
  const size_t bits = pageCount - firstPage < 64 ? pageCount - firstPage : 64;
  const uint64_t valid = bits == 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
  return (reserved & valid) != valid;
}

size_t SlamBitmap::findFreeRun(size_t pageCount) const {
  size_t runStart = 0;
  size_t runLength = 0;
  for (size_t entryIndex = m_FirstFreeEntry; entryIndex < m_EntryCount; ++entryIndex) {
    const size_t entryBase = entryIndex * 64;
    const size_t bits = (m_PageCount - entryBase < 64) ? m_PageCount - entryBase : 64;
    const uint64_t bitmap = m_reserved[entryIndex];  // m_Entries[entryIndex].reserved;
    if (!bitmap) {
      if (!runLength)
        runStart = entryBase;
      runLength += bits;
      if (runLength >= pageCount)
        return runStart;
      continue;
    }
    if (bitmap == ~uint64_t(0)) {
      runLength = 0;
      continue;
    }
    for (size_t bit = 0; bit < bits;) {
      const uint64_t remaining = bitmap >> bit;
      if (remaining & 1) {
        runLength = 0;
        bit += (remaining == ~uint64_t(0)) ? bits - bit : __builtin_ctzll(~remaining);
      } else {
        const size_t freeBits = remaining ? __builtin_ctzll(remaining) : bits - bit;
        const size_t span = freeBits < bits - bit ? freeBits : bits - bit;
        if (!runLength)
          runStart = entryBase + bit;
        runLength += span;
        if (runLength >= pageCount)
          return runStart;
        bit += span;
      }
    }
  }
  return static_cast<size_t>(-1);
}

void SlamBitmap::reserve(size_t firstPage, size_t pageCount) {
  for (size_t i = 0; i < pageCount; ++i)
    // m_Entries[(firstPage + i) / 64].reserved |= uint64_t(1) << ((firstPage + i) % 64);
    m_reserved[(firstPage + i) / 64] |= uint64_t(1) << ((firstPage + i) % 64);

  while (m_FirstFreeEntry < m_EntryCount &&
         !hasFreePage(m_reserved[m_FirstFreeEntry], m_FirstFreeEntry * 64, m_PageCount))
    ++m_FirstFreeEntry;
}

void SlamBitmap::release(size_t firstPage, size_t pageCount) {
  for (size_t i = 0; i < pageCount; ++i) {
    const uint64_t bit = uint64_t(1) << ((firstPage + i) % 64);
    size_t index = (firstPage + i) / 64;
    m_mapped[index] &= ~bit;
    m_ready[index] &= ~bit;
    m_reserved[index] &= ~bit;
    if (index < m_FirstFreeEntry)
      m_FirstFreeEntry = index;
    /*
     Entry& entry = m_Entries[(firstPage + i) / 64];
     entry.mapped &= ~bit;
     entry.ready &= ~bit;
     entry.reserved &= ~bit;
     */
  }
}

void SlamBitmap::setMapped(size_t page) {
  // m_Entries[page / 64].mapped |= uint64_t(1) << (page % 64);
  m_mapped[page / 64] |= uint64_t(1) << (page % 64);
}

void SlamBitmap::clearMappedAndReady(size_t page) {
  const uint64_t bit = uint64_t(1) << (page % 64);
  size_t index = page / 64;
  m_mapped[index] &= ~bit;
  m_ready[index] &= ~bit;
  /*
  Entry& entry = m_Entries[page / 64];
  entry.mapped &= ~bit;
  entry.ready &= ~bit;
  */
}

bool SlamBitmap::isReserved(size_t page) const {
  // return m_Entries[page / 64].reserved & (uint64_t(1) << (page % 64));
  return m_reserved[page / 64] & (uint64_t(1) << (page % 64));
}

bool SlamBitmap::isMapped(size_t page) const {
  // return m_Entries[page / 64].mapped & (uint64_t(1) << (page % 64));
  return m_mapped[page / 64] & (uint64_t(1) << (page % 64));
}

bool SlamBitmap::isReady(size_t page) const {
  // return m_Entries[page / 64].ready & (uint64_t(1) << (page % 64));
  return m_ready[page / 64] & (uint64_t(1) << (page % 64));
}

void SlamBitmap::setReady(size_t page) {
  // m_Entries[page / 64].ready |= uint64_t(1) << (page % 64);
  m_ready[page / 64] |= uint64_t(1) << (page % 64);
}
