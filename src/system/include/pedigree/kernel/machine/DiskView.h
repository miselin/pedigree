/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef MACHINE_DISKVIEW_H
#define MACHINE_DISKVIEW_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/DiskPaging.h"
#include "pedigree/kernel/utilities/BufferView.h"

class Disk;
class DiskWriteView;

/** Owns a cache reference. Pointers obtained from a view expire with the view.
 * Callers retain their enclosing I/O termination scope across blocking operations.
 */
class EXPORTED_PUBLIC DiskReadView {
 public:
  DiskReadView();
  DiskReadView(DiskReadView&& other) noexcept;
  DiskReadView& operator=(DiskReadView&& other) noexcept;
  ~DiskReadView();
  DiskReadView(const DiskReadView&) = delete;
  DiskReadView& operator=(const DiskReadView&) = delete;

  /** For immutable storage whose lifetime exceeds the view, such as a zero page. */
  static DiskReadView borrowed(const void* data, size_t size);

  explicit operator bool() const {
    return m_Data != nullptr;
  }
  const void* data() const {
    return m_Data;
  }
  size_t size() const {
    return m_Size;
  }
  bool truncate(size_t length) {
    if (length > m_Size)
      return false;
    m_Size = length;
    return true;
  }
  bool copyTo(void* destination, size_t length, size_t offset = 0) const {
    if ((!destination && length) || offset > m_Size || length > m_Size - offset)
      return false;
    if (length)
      MemoryCopy(destination, m_Data + offset, length);
    return true;
  }
  template <typename T>
  bool readAt(T& into, size_t offset = 0) const {
    static_assert(__is_trivially_copyable(T), "Disk values must be trivially copyable");
    if (offset > m_Size || sizeof(T) > m_Size - offset)
      return false;
    // Packed on-disk values need not have the host type's alignment.
    __builtin_memcpy(&into, m_Data + offset, sizeof(T));
    return true;
  }
  void reset();

 private:
  friend class Disk;
  friend class DiskWriteView;
  DiskReadView(Disk* owner, uint64_t location, BufferView view, bool writable, DiskUse&& use);

  Disk* m_Owner;
  uint64_t m_Location;
  const uint8_t* m_Data;
  size_t m_Size;
  bool m_Writable;
  DiskUse m_Use;
};

/** A mutable loan. Returning the last loan ends temporary checksum discovery. */
class EXPORTED_PUBLIC DiskWriteView {
 public:
  DiskWriteView() = default;
  DiskWriteView(DiskWriteView&&) noexcept = default;
  DiskWriteView& operator=(DiskWriteView&&) noexcept = default;
  DiskWriteView(const DiskWriteView&) = delete;
  DiskWriteView& operator=(const DiskWriteView&) = delete;

  explicit operator bool() const {
    return static_cast<bool>(m_View);
  }
  void* data() const {
    return const_cast<void*>(m_View.data());
  }
  size_t size() const {
    return m_View.size();
  }
  bool truncate(size_t length) {
    return m_View.truncate(length);
  }
  template <typename T>
  bool readAt(T& into, size_t offset = 0) const {
    return m_View.readAt(into, offset);
  }
  template <typename T>
  bool writeAt(const T& value, size_t offset = 0) {
    static_assert(__is_trivially_copyable(T), "Disk values must be trivially copyable");
    if (offset > size() || sizeof(T) > size() - offset)
      return false;
    __builtin_memcpy(static_cast<uint8_t*>(data()) + offset, &value, sizeof(T));
    return true;
  }
  bool copyFrom(const void* source, size_t length, size_t offset = 0) {
    if ((!source && length) || offset > size() || length > size() - offset)
      return false;
    if (length)
      MemoryCopy(static_cast<uint8_t*>(data()) + offset, source, length);
    return true;
  }
  void reset() {
    m_View.reset();
  }

 private:
  friend class Disk;
  DiskWriteView(Disk* owner, uint64_t location, BufferView view, DiskUse&& use)
      : m_View(owner, location, view, true, static_cast<DiskUse&&>(use)) {}
  DiskReadView m_View;
};

#endif
