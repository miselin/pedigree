/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_UTILITIES_UNIQUERESOURCE_H
#define PEDIGREE_KERNEL_UTILITIES_UNIQUERESOURCE_H
#include "pedigree/kernel/compiler.h"

#include <config.h>

/**
 * Owns one resource released by a stateless, compile-time policy.
 *
 * Releaser must provide `static void release(T *)` and must consume the
 * resource on every return; a fallible release must escalate or use another
 * ownership type. The policy is not stored, so this wrapper has the same
 * representation as the pointer it owns. It is intended for lexical ownership
 * of C handles and similar resources. Release runs synchronously and must be
 * legal in the destruction context; any wider protocol shutdown or concurrent
 * drain remains an explicit operation.
 */
template <class T, class Releaser>
class UniqueResource {
 public:
  UniqueResource() : m_Resource(nullptr) {}

  UniqueResource(UniqueResource&& other) : m_Resource(other.release()) {}

  ~UniqueResource() {
    reset();
  }

  UniqueResource& operator=(UniqueResource&& other) {
    if (this != &other) {
      reset();
      m_Resource = other.release();
    }
    return *this;
  }

  /** Takes ownership of a resource returned by an acquisition API. */
  MUST_USE_RESULT static UniqueResource adopt(T* resource) {
    return UniqueResource(resource);
  }

  T* get() const {
    return m_Resource;
  }

  T* operator->() const {
    return m_Resource;
  }

  T& operator*() const {
    return *m_Resource;
  }

  explicit operator bool() const {
    return m_Resource != nullptr;
  }

  /** Transfers ownership to a caller or another lifetime domain. */
  MUST_USE_RESULT T* release() {
    T* resource = m_Resource;
    m_Resource = nullptr;
    return resource;
  }

  /** Releases the current resource, then adopts its replacement. */
  void reset(T* replacement = nullptr) {
    if (replacement == m_Resource) {
      return;
    }
    T* resource = m_Resource;
    m_Resource = nullptr;
    if (resource) {
      Releaser::release(resource);
    }
    m_Resource = replacement;
  }

 private:
  explicit UniqueResource(T* resource) : m_Resource(resource) {}

  UniqueResource(const UniqueResource&) = delete;
  UniqueResource& operator=(const UniqueResource&) = delete;

  T* m_Resource;
};

#endif
