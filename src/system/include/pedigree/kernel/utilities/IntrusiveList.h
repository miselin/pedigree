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

#ifndef KERNEL_UTILITIES_INTRUSIVE_LIST_H
#define KERNEL_UTILITIES_INTRUSIVE_LIST_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/assert.h"

/** A caller-owned link embedded in an IntrusiveList element. */
template <typename T>
struct IntrusiveListNode {
  IntrusiveListNode* next() {
    return m_Next;
  }

  IntrusiveListNode* previous() {
    return m_Previous;
  }

  IntrusiveListNode* m_Next = nullptr;
  IntrusiveListNode* m_Previous = nullptr;
  T* value = nullptr;
};

/**
 * Allocation-free doubly-linked list.
 *
 * Elements remain owned by the caller and must outlive their membership in
 * the list. The selected node member belongs exclusively to one list while
 * linked. A type can participate in multiple lists by providing a distinct
 * node member for each list.
 */
template <typename T, IntrusiveListNode<T> T::* Member>
class EXPORTED_PUBLIC IntrusiveList {
  static_assert(Member != nullptr, "IntrusiveList requires a valid node member");

  using node_t = IntrusiveListNode<T>;

  template <typename Value, node_t* (node_t::*Forward)(), node_t* (node_t::*Backward)()>
  class IteratorType {
   public:
    IteratorType() : m_Node(nullptr) {}

    explicit IteratorType(node_t* node) : m_Node(node) {}

    Value& operator*() const {
      return *m_Node->value;
    }

    Value* operator->() const {
      return m_Node->value;
    }

    IteratorType& operator++() {
      m_Node = (m_Node->*Forward)();
      return *this;
    }

    IteratorType operator++(int) {
      IteratorType old(*this);
      ++(*this);
      return old;
    }

    IteratorType& operator--() {
      m_Node = (m_Node->*Backward)();
      return *this;
    }

    IteratorType operator--(int) {
      IteratorType old(*this);
      --(*this);
      return old;
    }

    template <typename OtherValue, node_t* (node_t::*OtherForward)(),
              node_t* (node_t::*OtherBackward)()>
    bool operator==(const IteratorType<OtherValue, OtherForward, OtherBackward>& other) const {
      return m_Node == other.__getNode();
    }

    template <typename OtherValue, node_t* (node_t::*OtherForward)(),
              node_t* (node_t::*OtherBackward)()>
    bool operator!=(const IteratorType<OtherValue, OtherForward, OtherBackward>& other) const {
      return !(*this == other);
    }

    node_t* __getNode() const {
      return m_Node;
    }

   private:
    node_t* m_Node;
  };

 public:
  using Iterator = IteratorType<T, &node_t::next, &node_t::previous>;
  using ConstIterator = IteratorType<const T, &node_t::next, &node_t::previous>;
  using ReverseIterator = IteratorType<T, &node_t::previous, &node_t::next>;
  using ConstReverseIterator = IteratorType<const T, &node_t::previous, &node_t::next>;

  IntrusiveList() : m_Count(0), m_Empty() {
    m_Empty.m_Next = &m_Empty;
    m_Empty.m_Previous = &m_Empty;
  }

  IntrusiveList(const IntrusiveList&) = delete;
  IntrusiveList& operator=(const IntrusiveList&) = delete;

  ~IntrusiveList() {
    clear();
  }

  size_t size() const {
    return m_Count;
  }

  size_t count() const {
    return m_Count;
  }

  bool empty() const {
    return !m_Count;
  }

  void pushBack(T& value) {
    insertBefore(m_Empty, value);
  }

  void pushFront(T& value) {
    insertBefore(*m_Empty.m_Next, value);
  }

  T* popBack() {
    if (empty())
      return nullptr;

    return remove(*m_Empty.m_Previous);
  }

  T* popFront() {
    if (empty())
      return nullptr;

    return remove(*m_Empty.m_Next);
  }

  Iterator erase(Iterator& iterator) {
    node_t* node = iterator.__getNode();
    if (node == &m_Empty)
      return end();

    node_t* next = node->m_Next;
    remove(*node);
    return Iterator(next);
  }

  ReverseIterator erase(ReverseIterator& iterator) {
    node_t* node = iterator.__getNode();
    if (node == &m_Empty)
      return rend();

    node_t* previous = node->m_Previous;
    remove(*node);
    return ReverseIterator(previous);
  }

  Iterator begin() {
    return Iterator(m_Empty.m_Next);
  }

  ConstIterator begin() const {
    return ConstIterator(m_Empty.m_Next);
  }

  Iterator end() {
    return Iterator(&m_Empty);
  }

  ConstIterator end() const {
    return ConstIterator(const_cast<node_t*>(&m_Empty));
  }

  ReverseIterator rbegin() {
    return ReverseIterator(m_Empty.m_Previous);
  }

  ConstReverseIterator rbegin() const {
    return ConstReverseIterator(m_Empty.m_Previous);
  }

  ReverseIterator rend() {
    return ReverseIterator(&m_Empty);
  }

  ConstReverseIterator rend() const {
    return ConstReverseIterator(const_cast<node_t*>(&m_Empty));
  }

  void clear() {
    while (!empty())
      remove(*m_Empty.m_Next);
  }

 private:
  void insertBefore(node_t& position, T& value) {
    node_t& node = value.*Member;
    assert(!node.m_Next && !node.m_Previous);

    node.value = &value;
    node.m_Next = &position;
    node.m_Previous = position.m_Previous;
    position.m_Previous->m_Next = &node;
    position.m_Previous = &node;
    ++m_Count;
  }

  T* remove(node_t& node) {
    assert(&node != &m_Empty);
    assert(node.m_Next && node.m_Previous);

    node.m_Previous->m_Next = node.m_Next;
    node.m_Next->m_Previous = node.m_Previous;

    T* value = node.value;
    node.m_Next = nullptr;
    node.m_Previous = nullptr;
    node.value = nullptr;
    --m_Count;
    return value;
  }

  size_t m_Count;
  node_t m_Empty;
};

#endif
