/* Copyright (c) 2026, Pedigree Developers. */
#include "namespace-file.h"
#include "pedigree/kernel/syscallError.h"

#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/VFS.h"

namespace {
class NamespaceFilesystem final : public RamFs {
 public:
  NamespaceFilesystem() {
    setProcessOwnership(false);
  }
};
NamespaceFilesystem filesystem;

class NamespaceFile final : public File {
 public:
  explicit NamespaceFile(const UtsRef& space)
      : File(String("uts"), 0, 0, 0, static_cast<uintptr_t>(space->identity()), &filesystem, 0,
             nullptr),
        m_Space(space) {
    Attributes attributes;
    attributes.uid = attributes.gid = 0;
    attributes.permissions = FILE_UR | FILE_GR | FILE_OR;
    updateAttributes(attributes, Owner | Group | Permissions);
  }
  const UtsRef& space() const {
    return m_Space;
  }
  bool isSeekable() const override {
    return false;
  }
  bool allowMapping(bool, bool, bool&) override {
    SYSCALL_ERROR(NoSuchDevice);
    return false;
  }

 protected:
  bool allowResize(size_t, size_t) override {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  bool isBytewise() const override {
    return true;
  }
  uint64_t readBytewise(uint64_t, uint64_t, uintptr_t, bool) override {
    SYSCALL_ERROR(InvalidArgument);
    return 0;
  }
  uint64_t writeBytewise(uint64_t, uint64_t, uintptr_t, bool) override {
    SYSCALL_ERROR(InvalidArgument);
    return 0;
  }

 private:
  UtsRef m_Space;
};
}  // namespace

UtsStatus posix_uts_make_file(const UtsRef& space, RetainedFile& result) {
  if (!space)
    return UtsStatus::Missing;
  auto* file = new NamespaceFile(space);
  if (!file)
    return UtsStatus::NoMemory;
  if (!VFS::instance().tryTrackFile(file)) {
    delete file;
    return UtsStatus::NoMemory;
  }
  result.adopt(file);
  return UtsStatus::Success;
}

bool posix_uts_file_namespace(File* file, UtsRef& result) {
  if (!file || file->getFilesystem() != &filesystem)
    return false;
  result = static_cast<NamespaceFile*>(file)->space();
  return true;
}
