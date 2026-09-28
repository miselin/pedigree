/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_UTILITIES_RINGQUEUE_H
#define PEDIGREE_KERNEL_UTILITIES_RINGQUEUE_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/assert.h"

/** Bounded FIFO storage. The owner supplies synchronization and lifetime. */
template <class T, size_t preallocatedSize = 0>
class RingQueue {
 public:
  explicit RingQueue(size_t capacity) : m_Capacity(capacity) {
    assert(capacity <= preallocatedSize);
  }

  size_t count() const {
    return m_Count;
  }
  size_t capacity() const {
    return m_Capacity;
  }

  size_t push(const T* items, size_t count) {
    if (count > m_Capacity - m_Count) {
      count = m_Capacity - m_Count;
    }
    for (size_t i = 0; i < count; ++i) {
      m_Items[m_Write] = items[i];
      if (++m_Write == m_Capacity) {
        m_Write = 0;
      }
    }
    m_Count += count;
    return count;
  }

  size_t pop(T* items, size_t count) {
    if (count > m_Count) {
      count = m_Count;
    }
    for (size_t i = 0; i < count; ++i) {
      items[i] = m_Items[m_Read];
      if (++m_Read == m_Capacity) {
        m_Read = 0;
      }
    }
    m_Count -= count;
    return count;
  }

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(RingQueue);

  const size_t m_Capacity;
  T m_Items[preallocatedSize] = {};
  size_t m_Read = 0;
  size_t m_Write = 0;
  size_t m_Count = 0;
};

/** Preserve incremental allocation for queues without inline storage. */
template <class T>
class RingQueue<T, 0> {
 public:
  explicit RingQueue(size_t capacity) : m_Capacity(capacity) {}

  size_t count() const {
    return m_Items.count();
  }
  size_t capacity() const {
    return m_Capacity;
  }

  size_t push(const T* items, size_t count) {
    if (count > m_Capacity - m_Items.count()) {
      count = m_Capacity - m_Items.count();
    }
    for (size_t i = 0; i < count; ++i) {
      m_Items.pushBack(items[i]);
    }
    return count;
  }

  size_t pop(T* items, size_t count) {
    if (count > m_Items.count()) {
      count = m_Items.count();
    }
    for (size_t i = 0; i < count; ++i) {
      items[i] = m_Items.popFront();
    }
    return count;
  }

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(RingQueue);

  const size_t m_Capacity;
  List<T> m_Items;
};

#endif
