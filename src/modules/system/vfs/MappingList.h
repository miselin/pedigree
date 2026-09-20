/* Copyright (c) 2026, Pedigree Developers. */
#ifndef VFS_MAPPING_LIST_H
#define VFS_MAPPING_LIST_H

#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Tree.h"

/** Append-ordered, non-owning mapping registry with logarithmic point lookup.
 * Callers serialize access and keep published ranges disjoint. Mapping starts
 * stay fixed until removal; shrinking a length in place is safe. */
template <class Object>
class MappingList {
 public:
  using Iterator = typename List<Object*>::Iterator;
  using ReverseIterator = typename List<Object*>::ReverseIterator;

  MappingList() = default;

  size_t count() const {
    return m_Objects.count();
  }
  Iterator begin() {
    return m_Objects.begin();
  }
  Iterator end() {
    return m_Objects.end();
  }
  ReverseIterator rbegin() {
    return m_Objects.rbegin();
  }
  ReverseIterator rend() {
    return m_Objects.rend();
  }

  bool tryPushBack(Object* object) {
    assert(object && !m_HasReservation);
    if (!reserveBack(object->address()))
      return false;
    publishBack(object);
    return true;
  }

  /** Reserve both nodes before a fallible split/clone can alter its source. */
  bool reserveBack(uintptr_t address) {
    assert(!m_HasReservation);
    if (m_Index.contains(address) || !m_Index.tryInsert(address, nullptr))
      return false;
    if (!m_Objects.tryPushBack(nullptr)) {
      m_Index.remove(address);
      return false;
    }
    m_ReservedAddress = address;
    m_HasReservation = true;
    return true;
  }

  void publishBack(Object* object) {
    assert(m_HasReservation && object && object->address() == m_ReservedAddress);
    Object** slot = m_Index.find(m_ReservedAddress);
    assert(slot && !*slot);
    *slot = object;
    *m_Objects.rbegin() = object;
    m_HasReservation = false;
  }

  Object* popBack() {
    Object* object = m_Objects.popBack();
    if (m_HasReservation) {
      assert(!object);
      m_Index.remove(m_ReservedAddress);
      m_HasReservation = false;
    } else if (object) {
      m_Index.remove(object->address());
    }
    return object;
  }

  Iterator erase(Iterator& it) {
    assert(!m_HasReservation && *it);
    m_Index.remove((*it)->address());
    return m_Objects.erase(it);
  }
  ReverseIterator erase(ReverseIterator& it) {
    assert(!m_HasReservation && *it);
    m_Index.remove((*it)->address());
    return m_Objects.erase(it);
  }

  Object* find(uintptr_t address, size_t* objectVisits = nullptr) const {
    if (objectVisits)
      *objectVisits = 0;
    uintptr_t start;
    Object* object;
    if (!m_Index.floorBound(address, start, object))
      return nullptr;
    // A split can reenter before publishing its reserved suffix. The source
    // still owns that range until the split changes its length.
    if (!object && (!start || !m_Index.floorBound(start - 1, start, object)))
      return nullptr;
    if (!object)
      return nullptr;
    if (objectVisits)
      *objectVisits = 1;
    return object->matches(address) ? object : nullptr;
  }

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(MappingList);

  List<Object*> m_Objects;
  Tree<uintptr_t, Object*> m_Index;
  uintptr_t m_ReservedAddress = 0;
  bool m_HasReservation = false;
};

#endif
