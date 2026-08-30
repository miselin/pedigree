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

#ifndef KERNEL_UTILITIES_BUFFERVIEW_H
#define KERNEL_UTILITIES_BUFFERVIEW_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/lib.h"

/** A mutable, non-owning view over a contiguous byte range. */
class BufferView {
 public:
  BufferView() : m_Data(0), m_Size(0) {}

  BufferView(void* data, size_t size) : m_Data(reinterpret_cast<uintptr_t>(data)), m_Size(size) {
    assert(data || !size);
  }

  static BufferView fromAddress(uintptr_t address, size_t size) {
    return BufferView(reinterpret_cast<void*>(address), size);
  }

  explicit operator bool() const {
    return m_Data != 0;
  }

  void* data() const {
    return reinterpret_cast<void*>(m_Data);
  }

  uintptr_t address() const {
    return m_Data;
  }

  size_t size() const {
    return m_Size;
  }

  bool empty() const {
    return m_Size == 0;
  }

  uint8_t& operator[](size_t offset) const {
    assert(offset < m_Size);
    return reinterpret_cast<uint8_t*>(m_Data)[offset];
  }

  BufferView operator+(size_t offset) const {
    assert(offset <= m_Size);
    if (offset > m_Size) {
      return BufferView();
    }
    return fromAddress(m_Data + offset, m_Size - offset);
  }

  BufferView& operator+=(size_t offset) {
    assert(offset <= m_Size);
    if (offset > m_Size) {
      m_Data = 0;
      m_Size = 0;
      return *this;
    }
    m_Data += offset;
    m_Size -= offset;
    return *this;
  }

  BufferView subview(size_t offset, size_t length) const {
    assert(offset <= m_Size && length <= (m_Size - offset));
    if (offset > m_Size || length > (m_Size - offset)) {
      return BufferView();
    }
    return fromAddress(m_Data + offset, length);
  }

  BufferView first(size_t length) const {
    return subview(0, length);
  }

  template <typename T>
  T* as(size_t offset = 0) const {
    assert(offset <= m_Size && sizeof(T) <= (m_Size - offset));
    if (offset > m_Size || sizeof(T) > (m_Size - offset)) {
      return nullptr;
    }
    const uintptr_t address = m_Data + offset;
    assert((address % alignof(T)) == 0);
    if ((address % alignof(T)) != 0) {
      return nullptr;
    }
    return reinterpret_cast<T*>(address);
  }

 private:
  uintptr_t m_Data;
  size_t m_Size;
};

/**
 * A mutable, non-owning sequence of discontiguous byte ranges.
 *
 * The caller owns both the descriptor storage and every buffer referenced by
 * it. This class only provides bounded traversal and copying across the
 * supplied views.
 */
class BufferViewSequence {
 public:
  BufferViewSequence(BufferView* storage, size_t capacity)
      : m_Storage(storage), m_Capacity(capacity), m_Count(0), m_Size(0) {
    assert(storage || !capacity);
  }

  bool append(const BufferView& view) {
    assert(view && !view.empty());
    if (!view || view.empty() || m_Count >= m_Capacity ||
        view.size() > (~static_cast<size_t>(0) - m_Size)) {
      return false;
    }

    m_Storage[m_Count++] = view;
    m_Size += view.size();
    return true;
  }

  void clear() {
    m_Count = 0;
    m_Size = 0;
  }

  bool empty() const {
    return m_Count == 0;
  }

  size_t count() const {
    return m_Count;
  }

  size_t size() const {
    return m_Size;
  }

  BufferView operator[](size_t index) const {
    assert(index < m_Count);
    return index < m_Count ? m_Storage[index] : BufferView();
  }

  bool copyTo(void* destination, size_t length, size_t offset = 0) const {
    assert(destination || !length);
    assert(offset <= m_Size && length <= (m_Size - offset));
    if ((!destination && length) || offset > m_Size || length > (m_Size - offset)) {
      return false;
    }

    uint8_t* output = reinterpret_cast<uint8_t*>(destination);
    size_t remaining = length;
    for (size_t i = 0; i < m_Count && remaining; ++i) {
      const BufferView view = m_Storage[i];
      if (offset >= view.size()) {
        offset -= view.size();
        continue;
      }

      const size_t available = view.size() - offset;
      const size_t chunk = available < remaining ? available : remaining;
      MemoryCopy(output, reinterpret_cast<const uint8_t*>(view.data()) + offset, chunk);
      output += chunk;
      remaining -= chunk;
      offset = 0;
    }
    return remaining == 0;
  }

  bool copyFrom(const void* source, size_t length, size_t offset = 0) const {
    assert(source || !length);
    assert(offset <= m_Size && length <= (m_Size - offset));
    if ((!source && length) || offset > m_Size || length > (m_Size - offset)) {
      return false;
    }

    const uint8_t* input = reinterpret_cast<const uint8_t*>(source);
    size_t remaining = length;
    for (size_t i = 0; i < m_Count && remaining; ++i) {
      const BufferView view = m_Storage[i];
      if (offset >= view.size()) {
        offset -= view.size();
        continue;
      }

      const size_t available = view.size() - offset;
      const size_t chunk = available < remaining ? available : remaining;
      MemoryCopy(reinterpret_cast<uint8_t*>(view.data()) + offset, input, chunk);
      input += chunk;
      remaining -= chunk;
      offset = 0;
    }
    return remaining == 0;
  }

 private:
  BufferView* m_Storage;
  size_t m_Capacity;
  size_t m_Count;
  size_t m_Size;
};

#endif
