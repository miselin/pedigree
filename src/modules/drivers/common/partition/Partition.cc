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

#include "Partition.h"
#include "pedigree/kernel/processor/types.h"

Partition::Partition(const String& type, uint64_t start, uint64_t length)
    : m_Type(type), m_Start(start), m_Length(length), m_AlignmentLock(), m_bAligned(false) {
  setSpecificType(String("partition"));
}

Partition::~Partition() {}

bool Partition::containsRange(uint64_t location, size_t length) const {
  const auto* parent = static_cast<const Disk*>(getParent());
  if (!parent || location > m_Length || length > m_Length - location ||
      location > ~uint64_t{0} - m_Start)
    return false;
  const uint64_t translated = m_Start + location;
  return translated <= parent->getSize() && length <= parent->getSize() - translated;
}

bool Partition::readInto(uint64_t location, void* buffer, size_t length) {
  if (!containsRange(location, length))
    return false;
  auto* parent = static_cast<Disk*>(getParent());
  ensureAligned(parent);
  return parent->readInto(m_Start + location, buffer, length);
}

bool Partition::readIntoBatch(ReadBuffer* buffers, size_t count) {
  if (count > MaxReadBuffers || (count && !buffers))
    return false;
  for (size_t i = 0; i < count; ++i)
    buffers[i].complete = false;
  if (!count)
    return true;
  ReadBuffer translated[MaxReadBuffers];
  for (size_t i = 0; i < count; ++i) {
    if (!containsRange(buffers[i].location, buffers[i].length))
      return false;
    translated[i] = buffers[i];
    translated[i].location += m_Start;
  }
  auto* parent = static_cast<Disk*>(getParent());
  ensureAligned(parent);
  const bool success = parent->readIntoBatch(translated, count);
  for (size_t i = 0; i < count; ++i)
    buffers[i].complete = translated[i].complete;
  return success;
}

bool Partition::writeFrom(uint64_t location, const void* buffer, size_t length) {
  if (!containsRange(location, length))
    return false;
  auto* parent = static_cast<Disk*>(getParent());
  ensureAligned(parent);
  return parent->writeFrom(m_Start + location, buffer, length);
}

bool Partition::syncData() {
  auto* parent = static_cast<Disk*>(getParent());
  return parent && parent->syncData();
}

bool Partition::syncPages(const uint64_t* locations, size_t count) {
  if (count > MaxSyncPages || (count && !locations))
    return false;
  if (!count)
    return true;
  Disk* parent = static_cast<Disk*>(getParent());
  if (!parent)
    return false;
  uint64_t translated[MaxSyncPages];
  for (size_t i = 0; i < count; ++i) {
    if (!containsCachePage(locations[i]) || locations[i] > ~uint64_t(0) - m_Start)
      return false;
    translated[i] = locations[i] + m_Start;
    if (translated[i] >= parent->getSize())
      return false;
  }
  ensureAligned(parent);
  return parent->syncPages(translated, count);
}

bool Partition::syncAll() {
  Disk* parent = static_cast<Disk*>(getParent());
  // A device-wide drain is stronger than the partition's persistence boundary.
  return parent && parent->syncAll();
}

uint64_t Partition::getStart() {
  return m_Start;
}
