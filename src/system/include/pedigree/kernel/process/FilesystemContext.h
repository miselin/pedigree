/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_PROCESS_FILESYSTEMCONTEXT_H
#define PEDIGREE_PROCESS_FILESYSTEMCONTEXT_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/utility.h"

class File;

/** Inert provider-owned path ownership; copying never enters a VFS operation. */
class EXPORTED_PUBLIC FilesystemPath {
 public:
  virtual ~FilesystemPath() = default;
  // Borrowed for the lifetime of the path reference, including after detach.
  virtual File* node() const = 0;
  virtual const void* provider() const = 0;
};
using FilesystemPathRef = SharedPointer<FilesystemPath>;

struct EXPORTED_PUBLIC FilesystemContextSnapshot {
  FilesystemPathRef root;
  FilesystemPathRef cwd;
  uint64_t contextGeneration = 0;
  uint64_t topologyGeneration = 0;
};

class EXPORTED_PUBLIC FilesystemContextOwner;
class EXPORTED_PUBLIC FilesystemContext {
 public:
  virtual ~FilesystemContext() = default;
  virtual bool snapshot(FilesystemContextSnapshot& result) const = 0;
  // The provider enrolls the unpublished child before returning success.
  virtual bool forkForProcess(FilesystemContextOwner& result) const = 0;
  virtual void retireProcessOwner() = 0;
};
using FilesystemContextRef = SharedPointer<FilesystemContext>;

/** One Process or unpublished fork staging scope owns registry retirement. */
class EXPORTED_PUBLIC FilesystemContextOwner {
 public:
  FilesystemContextOwner() = default;
  FilesystemContextOwner(FilesystemContextOwner&& other) noexcept
      : m_Context(pedigree_std::move(other.m_Context)) {}
  FilesystemContextOwner& operator=(FilesystemContextOwner&& other) noexcept {
    if (this != &other) {
      reset();
      m_Context = pedigree_std::move(other.m_Context);
    }
    return *this;
  }
  ~FilesystemContextOwner() {
    reset();
  }

  FilesystemContextRef reference() const {
    return m_Context;
  }
  explicit operator bool() const {
    return static_cast<bool>(m_Context);
  }
  void reset() {
    FilesystemContextRef retired(pedigree_std::move(m_Context));
    if (retired)
      retired->retireProcessOwner();
  }
  // Providers call this only after enrollment, with the original control block.
  static FilesystemContextOwner adopt(FilesystemContextRef&& context) {
    FilesystemContextOwner result;
    result.m_Context = pedigree_std::move(context);
    return result;
  }

 private:
  FilesystemContextOwner(const FilesystemContextOwner&) = delete;
  FilesystemContextOwner& operator=(const FilesystemContextOwner&) = delete;
  FilesystemContextRef m_Context;
};

#endif
