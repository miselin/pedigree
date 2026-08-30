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
#include "pedigree/kernel/utilities/String.h"

Disk::Disk() {
  m_SpecificType.assign("Generic Disk", 13);
}

Disk::Disk(Device* p) : Device(p) {}

Disk::~Disk() {}

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

bool Disk::retireCachePage(uint64_t location) {
  return false;
}
