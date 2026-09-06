/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_VFS_FILEHANDLE_H
#define PEDIGREE_VFS_FILEHANDLE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/types.h"

class File;

struct FileHandle {
  uint32_t length = 0;
  int32_t type = 0;
  uint8_t bytes[128] = {};
};

struct FileSystemId {
  uint32_t words[2] = {};
};

enum class FileHandleStatus { Success, Unsupported, Stale, Invalid, NoMemory, IoError };

/** Owns exactly one existing VFS-tracked File reference. */
class EXPORTED_PUBLIC RetainedFile {
 public:
  RetainedFile();
  RetainedFile(RetainedFile&& other) noexcept;
  ~RetainedFile();
  RetainedFile& operator=(RetainedFile&& other) noexcept;
  File* get() const {
    return m_File;
  }
  explicit operator bool() const {
    return m_File != nullptr;
  }
  void adopt(File* file);
  void reset();

 private:
  RetainedFile(const RetainedFile&) = delete;
  RetainedFile& operator=(const RetainedFile&) = delete;
  TerminationDeferral m_Lifetime;
  File* m_File = nullptr;
};

#endif
