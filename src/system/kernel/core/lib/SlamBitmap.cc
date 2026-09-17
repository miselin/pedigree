#include "pedigree/kernel/core/SlamBitmap.h"

void SlamBitmap::useMemory(void* memory, size_t entryCount, size_t pageCount) {
  m_Entries = static_cast<Entry*>(memory);
  m_EntryCount = entryCount;
  m_PageCount = pageCount;
}

uint64_t SlamBitmap::reservedBits(size_t entry) const {
  return m_Entries[entry].reserved;
}

uintptr_t SlamBitmap::metadataAddress(size_t entry) const {
  return reinterpret_cast<uintptr_t>(&m_Entries[entry]);
}

size_t SlamBitmap::findFreeRun(size_t pageCount) const {
  size_t runStart = 0;
  size_t runLength = 0;
  for (size_t entryIndex = 0; entryIndex < m_EntryCount; ++entryIndex) {
    const size_t entryBase = entryIndex * 64;
    const size_t bits = (m_PageCount - entryBase < 64) ? m_PageCount - entryBase : 64;
    const uint64_t bitmap = m_Entries[entryIndex].reserved;
    if (!bitmap) {
      if (!runLength) runStart = entryBase;
      runLength += bits;
      if (runLength >= pageCount) return runStart;
      continue;
    }
    if (bitmap == ~uint64_t(0)) {
      runLength = 0;
      continue;
    }
    for (size_t bit = 0; bit < bits; ++bit) {
      if (bitmap & (uint64_t(1) << bit))
        runLength = 0;
      else {
        if (!runLength) runStart = entryBase + bit;
        if (++runLength >= pageCount) return runStart;
      }
    }
  }
  return static_cast<size_t>(-1);
}

void SlamBitmap::reserve(size_t firstPage, size_t pageCount) {
  for (size_t i = 0; i < pageCount; ++i)
    m_Entries[(firstPage + i) / 64].reserved |= uint64_t(1) << ((firstPage + i) % 64);
}

void SlamBitmap::release(size_t firstPage, size_t pageCount) {
  for (size_t i = 0; i < pageCount; ++i) {
    const uint64_t bit = uint64_t(1) << ((firstPage + i) % 64);
    Entry& entry = m_Entries[(firstPage + i) / 64];
    entry.mapped &= ~bit;
    entry.ready &= ~bit;
    entry.reserved &= ~bit;
  }
}

void SlamBitmap::setMapped(size_t page) {
  m_Entries[page / 64].mapped |= uint64_t(1) << (page % 64);
}

void SlamBitmap::clearMappedAndReady(size_t page) {
  const uint64_t bit = uint64_t(1) << (page % 64);
  Entry& entry = m_Entries[page / 64];
  entry.mapped &= ~bit;
  entry.ready &= ~bit;
}

bool SlamBitmap::isReserved(size_t page) const {
  return m_Entries[page / 64].reserved & (uint64_t(1) << (page % 64));
}

bool SlamBitmap::isMapped(size_t page) const {
  return m_Entries[page / 64].mapped & (uint64_t(1) << (page % 64));
}

bool SlamBitmap::isReady(size_t page) const {
  return m_Entries[page / 64].ready & (uint64_t(1) << (page % 64));
}

void SlamBitmap::setReady(size_t page) {
  m_Entries[page / 64].ready |= uint64_t(1) << (page % 64);
}
