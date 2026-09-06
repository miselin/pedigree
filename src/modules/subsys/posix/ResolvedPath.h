/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_RESOLVEDPATH_H
#define POSIX_RESOLVEDPATH_H
#include "pedigree/kernel/process/FilesystemContext.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/utility.h"

// Syscall-local ownership; stored process and OFD paths remain inert references.
class EXPORTED_PUBLIC ResolvedPath {
 public:
  ~ResolvedPath();
  File* get() const {
    return m_Path ? m_Path->node() : nullptr;
  }
  explicit operator bool() const {
    return static_cast<bool>(m_Path);
  }
  const FilesystemPathRef& path() const {
    return m_Path;
  }
  void retain(const FilesystemPathRef& path);
  void reset();
  void swap(ResolvedPath& other) {
    auto temporary = pedigree_std::move(m_Path);
    m_Path = pedigree_std::move(other.m_Path);
    other.m_Path = pedigree_std::move(temporary);
  }

 private:
  TerminationDeferral m_Lifetime;
  FilesystemPathRef m_Path;
};
#endif
