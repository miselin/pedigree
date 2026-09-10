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

#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/String.h"

Disk::Disk() : m_Endpoint(nullptr) {
  m_SpecificType.assign("Generic Disk", 13);
}

Disk::Disk(Device* p) : Device(p), m_Endpoint(nullptr) {}

Disk::~Disk() {
  retireEndpoint();
}

Disk* Disk::physicalDisk() {
  return this;
}

bool Disk::acquireUse(DiskUse& use) {
  Disk* physical = physicalDisk();
  if (!physical) {
    use.reset();
    return false;
  }
  if (!physical->m_Endpoint) {
    use.reset();
    return true;
  }
  return DiskEndpoints::acquire(physical->m_Endpoint, physical, use);
}

uint32_t Disk::endpointId() {
  Disk* physical = physicalDisk();
  return physical ? DiskEndpoints::id(physical->m_Endpoint, physical) : 0;
}

void Disk::reserveEndpoint() {
  if (!m_Endpoint)
    m_Endpoint = DiskEndpoints::reserve(this);
}
void Disk::publishEndpoint() {
  DiskEndpoints::publish(m_Endpoint);
}
void Disk::retireEndpoint() {
  DiskEndpoints::retire(m_Endpoint, this);
}
bool Disk::tryCloseEndpoint() {
  return DiskEndpoints::tryClose(m_Endpoint, this);
}
void Disk::reopenEndpoint() {
  DiskEndpoints::reopen(m_Endpoint, this);
}
PagingStatus Disk::preparePagingTransport(PagingTransport*& transport) {
  transport = nullptr;
  return PagingStatus::Unsupported;
}

Device::Type Disk::getType() {
  return Device::Disk;
}

Disk::SubType Disk::getSubType() {
  return ATA;
}

void Disk::getName(String& str) {
  str.assign("Generic disk", 13);
}

void Disk::dump(String& str) {
  str.assign("Generic disk", 13);
}

BufferView Disk::read(uint64_t location) {
  return BufferView();
}

bool Disk::readInto(uint64_t location, void* buffer, size_t length) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;
  if ((!buffer && length) || location > getSize() || length > getSize() - location)
    return false;
  if (!length)
    return true;

  auto* output = static_cast<uint8_t*>(buffer);
  while (length) {
    const uint64_t aligned = location - location % 512;
    const size_t offset = location - aligned;
    const BufferView view = read(aligned);
    if (!view)
      return false;
    if (view.size() <= offset) {
      unpin(aligned);
      return false;
    }
    const size_t available = view.size() - offset;
    const size_t chunk = length < available ? length : available;
    MemoryCopy(output, view.subview(offset, chunk).data(), chunk);
    unpin(aligned);
    output += chunk;
    location += chunk;
    length -= chunk;
  }
  return true;
}

bool Disk::writeFrom(uint64_t location, const void* buffer, size_t length) {
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;
  if ((!buffer && length) || location > getSize() || length > getSize() - location)
    return false;
  if (!length)
    return true;

  const auto* input = static_cast<const uint8_t*>(buffer);
  while (length) {
    const uint64_t aligned = location - location % 512;
    const size_t offset = location - aligned;
    const BufferView view = read(aligned);
    if (!view)
      return false;
    if (view.size() <= offset) {
      unpin(aligned);
      return false;
    }
    const size_t available = view.size() - offset;
    const size_t chunk = length < available ? length : available;
    MemoryCopy(view.subview(offset, chunk).data(), input, chunk);
    write(aligned);
    const bool written = sync(aligned, false);
    unpin(aligned);
    if (!written)
      return false;
    input += chunk;
    location += chunk;
    length -= chunk;
  }
  return true;
}

bool Disk::syncData() {
  return true;
}

bool Disk::readViews(uint64_t location, size_t length, BufferViewSequence& views) {
  if (!views.empty() || location > getSize() || length > (getSize() - location)) {
    return false;
  }

  size_t remaining = length;
  uint64_t current = location;
  while (remaining) {
    const BufferView view = read(current);
    if (!view || view.empty()) {
      unpinViews(location, views);
      return false;
    }

    const size_t chunk = view.size() < remaining ? view.size() : remaining;
    if (!views.append(view.first(chunk))) {
      unpin(current);
      unpinViews(location, views);
      return false;
    }

    current += chunk;
    remaining -= chunk;
  }
  return true;
}

void Disk::unpinViews(uint64_t location, BufferViewSequence& views) {
  uint64_t current = location;
  for (size_t i = 0; i < views.count(); ++i) {
    const BufferView view = views[i];
    unpin(current);
    current += view.size();
  }
  views.clear();
}

void Disk::write(uint64_t location) {}

void Disk::align(uint64_t location) {}

size_t Disk::getSize() const {
  return 0;
}

size_t Disk::getBlockSize() const {
  return 0;
}

bool Disk::cacheIsCritical() {
  return false;
}

void Disk::flush(uint64_t location) {}

bool Disk::sync(uint64_t location, bool async) {
  return false;
}

bool Disk::syncPages(const uint64_t* locations, size_t count) {
  if (count > MaxSyncPages || (count && !locations))
    return false;
  for (size_t i = 0; i < count; ++i) {
    if (locations[i] >= getSize())
      return false;
  }
  bool succeeded = true;
  for (size_t i = 0; i < count; ++i)
    succeeded = sync(locations[i], false) && succeeded;
  return succeeded;
}

bool Disk::syncAll() {
  return false;
}

bool Disk::retireCachePage(uint64_t location) {
  return false;
}

size_t Disk::getNativeBlockSize() const {
  return 512;
}
