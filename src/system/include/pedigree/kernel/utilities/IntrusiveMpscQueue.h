/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_UTILITIES_INTRUSIVEMPSCQUEUE_H
#define KERNEL_UTILITIES_INTRUSIVEMPSCQUEUE_H
#include "pedigree/kernel/compiler.h"

#include <config.h>

template <typename Node, Node* Node::* NextMember>
class IntrusiveMpscQueueTestAccess;

/**
 * Allocation-free intrusive queue for any number of producers and one
 * consumer.
 *
 * Storage is bounded by the caller-owned nodes supplied to push(). The caller
 * must reserve one node as the queue's permanent stub and keep every queued
 * node alive. A node may not be published again until pop() returns it as an
 * Item. The link member must be naturally pointer-aligned and belongs
 * exclusively to the queue while its node is published.
 *
 * Producers never wait: they atomically replace the head and then link their
 * predecessor. The consumer also never waits. It reports Transient when a
 * producer is between those two operations, allowing an IRQ drain loop to
 * defer another attempt instead of spinning inside hard-interrupt context.
 */
template <typename Node, Node* Node::* NextMember>
class IntrusiveMpscQueue {
  static_assert(__atomic_always_lock_free(sizeof(Node*), nullptr),
                "IntrusiveMpscQueue requires lock-free pointer atomics");
  static_assert(alignof(Node) >= alignof(Node*),
                "IntrusiveMpscQueue nodes require natural pointer alignment");
  static_assert(NextMember != nullptr, "IntrusiveMpscQueue requires a valid intrusive link member");

 public:
  enum class PopResult {
    Item,
    Empty,
    Transient,
  };

  /**
   * Constructs an empty queue using the stub as permanent internal storage.
   * The stub must outlive the queue and must never be passed to push().
   */
  explicit IntrusiveMpscQueue(Node& stub) : m_Head(&stub), m_Tail(&stub), m_Stub(&stub) {
    storeNext(&stub, nullptr, __ATOMIC_RELAXED);
  }

  /** Publishes one caller-owned node in constant time. */
  void push(Node& node) {
    Node* previous = beginPush(node);
    finishPush(node, previous);
  }

  /**
   * Attempts to remove one node without waiting.
   *
   * Item makes out immediately safe for the consumer to reuse, destroy, or
   * publish again. Empty is a point-in-time observation and can immediately
   * become stale. Transient means an in-progress producer owns the missing
   * predecessor link, so the consumer should retry later. Both non-Item
   * results set out to nullptr.
   */
  MUST_USE_RESULT PopResult pop(Node*& out) {
    out = nullptr;

    Node* tail = m_Tail;
    Node* next = loadNext(tail);

    if (tail == m_Stub) {
      if (!next) {
        if (loadHead() == tail) {
          return PopResult::Empty;
        }
        return PopResult::Transient;
      }

      m_Tail = next;
      tail = next;
      next = loadNext(tail);
    }

    if (next) {
      m_Tail = next;
      out = tail;
      return PopResult::Item;
    }

    if (tail != loadHead()) {
      return PopResult::Transient;
    }

    // Moving the producer head onto the permanent stub prevents a later
    // producer from retaining the returned node as its predecessor.
    push(*m_Stub);
    next = loadNext(tail);
    if (!next) {
      // A producer can win the head exchange immediately before the stub
      // rotation and still be responsible for this final link.
      return PopResult::Transient;
    }

    m_Tail = next;
    out = tail;
    return PopResult::Item;
  }

 private:
  friend class IntrusiveMpscQueueTestAccess<Node, NextMember>;

  IntrusiveMpscQueue(const IntrusiveMpscQueue&) = delete;
  IntrusiveMpscQueue& operator=(const IntrusiveMpscQueue&) = delete;

  Node* beginPush(Node& node) {
    // Clearing the reused link before the release exchange prevents the
    // consumer from following a link left over from an earlier lifetime.
    storeNext(&node, nullptr, __ATOMIC_RELAXED);
    return __atomic_exchange_n(&m_Head, &node, __ATOMIC_ACQ_REL);
  }

  static void finishPush(Node& node, Node* previous) {
    // This is the payload publication edge consumed by loadNext().
    storeNext(previous, &node, __ATOMIC_RELEASE);
  }

  Node* loadHead() const {
    return __atomic_load_n(&m_Head, __ATOMIC_ACQUIRE);
  }

  static Node* loadNext(Node* node) {
    return __atomic_load_n(&(node->*NextMember), __ATOMIC_ACQUIRE);
  }

  static void storeNext(Node* node, Node* next, int memoryOrder) {
    __atomic_store_n(&(node->*NextMember), next, memoryOrder);
  }

  Node* m_Head;
  Node* m_Tail;
  Node* const m_Stub;
};

#if defined(TESTSUITE) || (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS)
/** Constructs otherwise unreachable producer publication states in tests. */
template <typename Node, Node* Node::* NextMember>
class IntrusiveMpscQueueTestAccess {
 public:
  using Queue = IntrusiveMpscQueue<Node, NextMember>;

  struct Publication {
    Node* node;
    Node* previous;
  };

  static Publication beginPush(Queue& queue, Node& node) {
    return Publication{&node, queue.beginPush(node)};
  }

  static void finishPush(Queue& queue, const Publication& publication) {
    queue.finishPush(*publication.node, publication.previous);
  }

  static bool consumerSeesLastNode(const Queue& queue) {
    return queue.m_Tail != queue.m_Stub && !queue.loadNext(queue.m_Tail) &&
           queue.m_Tail == queue.loadHead();
  }

  static void rotateStub(Queue& queue) {
    queue.push(*queue.m_Stub);
  }
};
#endif

#endif
