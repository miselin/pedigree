/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_MEMFD_FILE_H
#define POSIX_MEMFD_FILE_H

#include "modules/system/ramfs/RamFs.h"

namespace LinuxMemFd {
constexpr unsigned CloseOnExec = 1, AllowSealing = 2;
constexpr unsigned Seal = 1, Shrink = 2, Grow = 4, Write = 8, FutureWrite = 16;
constexpr unsigned AllSeals = Seal | Shrink | Grow | Write | FutureWrite;
constexpr size_t MaximumNameLength = 249;
}  // namespace LinuxMemFd

class EXPORTED_PUBLIC MemFdFile final : public RamFile {
 public:
  MemFdFile(const String& name, bool allowSealing, size_t uid, size_t gid);
  ~MemFdFile() override = default;

  static MemFdFile* fromFile(File* file);
  Attributes getAttributes() const override;
  int getSeals();
  int addSeals(unsigned seals);
  bool allowMapping(bool shared, bool writeRequested, bool& mayWrite) override;

 protected:
  bool prepareWrite(uint64_t location, uint64_t size) override;
  bool allowResize(size_t oldSize, size_t newSize) override;

 private:
  unsigned m_Seals;
};

#endif
