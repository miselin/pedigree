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

DiskReadView::DiskReadView()
    : m_Owner(nullptr), m_Location(0), m_Data(nullptr), m_Size(0), m_Writable(false), m_Use() {}

DiskReadView::DiskReadView(Disk* owner, uint64_t location, BufferView view, bool writable,
                           DiskUse&& use)
    : m_Owner(owner),
      m_Location(location),
      m_Data(static_cast<const uint8_t*>(view.data())),
      m_Size(view.size()),
      m_Writable(writable),
      m_Use(static_cast<DiskUse&&>(use)) {}

DiskReadView::DiskReadView(DiskReadView&& other) noexcept : DiskReadView() {
  *this = static_cast<DiskReadView&&>(other);
}

DiskReadView& DiskReadView::operator=(DiskReadView&& other) noexcept {
  if (this != &other) {
    reset();
    m_Owner = other.m_Owner;
    m_Location = other.m_Location;
    m_Data = other.m_Data;
    m_Size = other.m_Size;
    m_Writable = other.m_Writable;
    m_Use = static_cast<DiskUse&&>(other.m_Use);
    other.m_Owner = nullptr;
    other.m_Data = nullptr;
    other.m_Size = 0;
  }
  return *this;
}

DiskReadView::~DiskReadView() {
  reset();
}

DiskReadView DiskReadView::borrowed(const void* data, size_t size) {
  DiskReadView view;
  if (data) {
    view.m_Data = static_cast<const uint8_t*>(data);
    view.m_Size = size;
  }
  return view;
}

void DiskReadView::reset() {
  if (m_Owner) {
    TerminationDeferral lifetime;
    m_Owner->releaseView(m_Location, m_Writable);
    m_Owner = nullptr;
    m_Data = nullptr;
    m_Size = 0;
    m_Use.reset();
    return;
  }
  m_Data = nullptr;
  m_Size = 0;
  m_Use.reset();
}

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

DiskReadView Disk::readView(uint64_t location) {
  TerminationDeferral lifetime;
  DiskUse use;
  if (!acquireUse(use))
    return {};
  uint64_t token = location;
  const auto view = acquireView(location, false, token);
  return view ? DiskReadView(this, token, view, false, static_cast<DiskUse&&>(use))
              : DiskReadView();
}

DiskWriteView Disk::writeView(uint64_t location) {
  TerminationDeferral lifetime;
  DiskUse use;
  if (!acquireUse(use))
    return {};
  uint64_t token = location;
  const auto view = acquireView(location, true, token);
  return view ? DiskWriteView(this, token, view, static_cast<DiskUse&&>(use)) : DiskWriteView();
}

BufferView Disk::acquireView(uint64_t location, bool writable, uint64_t& token) {
  token = location;
  return read(location);
}

void Disk::releaseView(uint64_t location, bool writable) {
  if (writable)
    write(location);
  unpin(location);
}

bool Disk::readIntoBatch(ReadBuffer* buffers, size_t count) {
  if (count > MaxReadBuffers || (count && !buffers))
    return false;
  for (size_t i = 0; i < count; ++i)
    buffers[i].complete = false;
  if (!count)
    return true;
  TerminationDeferral lifetime;
  DiskUse diskUse;
  if (!acquireUse(diskUse))
    return false;
  bool success = true;
  for (size_t i = 0; i < count; ++i) {
    auto& request = buffers[i];
    request.complete = readInto(request.location, request.buffer, request.length);
    success &= request.complete;
  }
  return success;
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
    const DiskReadView view = readView(aligned);
    if (!view)
      return false;
    if (view.size() <= offset) {
      return false;
    }
    const size_t available = view.size() - offset;
    const size_t chunk = length < available ? length : available;
    view.copyTo(output, chunk, offset);
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

bool Disk::writeFromBatch(WriteBuffer* buffers, size_t count) {
  if (count > MaxWriteBuffers || (count && !buffers))
    return false;
  for (size_t i = 0; i < count; ++i)
    buffers[i].complete = false;
  bool succeeded = true;
  for (size_t i = 0; i < count; ++i) {
    auto& buffer = buffers[i];
    buffer.complete = writeFrom(buffer.location, buffer.buffer, buffer.length);
    succeeded = buffer.complete && succeeded;
  }
  return succeeded;
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

bool Disk::zero(uint64_t location, size_t length) {
  if (location > getSize() || length > getSize() - location)
    return false;
  while (length) {
    const uint64_t base = location - location % 512;
    const size_t within = location - base;
    auto view = read(base);
    if (!view)
      return false;
    if (within >= view.size()) {
      unpin(base);
      return false;
    }
    const size_t available = view.size() - within;
    const size_t amount = length < available ? length : available;
    ByteSet(reinterpret_cast<void*>(view.address() + within), 0, amount);
    write(base);
    unpin(base);
    location += amount;
    length -= amount;
  }
  return true;
}
