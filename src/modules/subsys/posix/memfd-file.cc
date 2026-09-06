/* Copyright (c) 2026, Pedigree Developers. */
#include "memfd-file.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/assert.h"

#include "modules/system/vfs/MemoryMappedFile.h"

namespace {
class MemFdFilesystem final : public RamFs {
 public:
  MemFdFilesystem() {
    setProcessOwnership(false);
  }
};

MemFdFilesystem filesystem;
uintptr_t nextInode = 0;
}  // namespace

MemFdFile::MemFdFile(const String& name, bool allowSealing, size_t uid, size_t gid)
    : RamFile(name, __atomic_add_fetch(&nextInode, uintptr_t(1), __ATOMIC_RELAXED), &filesystem,
              nullptr),
      m_Seals(allowSealing ? 0 : LinuxMemFd::Seal) {
  assert(getInode());
  Attributes initial;
  initial.uid = uid;
  initial.gid = gid;
  initial.permissions = 0777;
  initial.accessed = initial.modified = initial.changed = Time::getTime();
  updateAttributes(initial, Owner | Group | Permissions | AccessTime | ModifyTime | ChangeTime);
}

MemFdFile* MemFdFile::fromFile(File* file) {
  return file && file->getFilesystem() == &filesystem ? static_cast<MemFdFile*>(file) : nullptr;
}

File::Attributes MemFdFile::getAttributes() const {
  Attributes attributes = RamFile::getAttributes();
  attributes.links = 0;
  return attributes;
}

int MemFdFile::getSeals() {
  LockGuard<Mutex> guard(dataMutationLock());
  return static_cast<int>(m_Seals);
}

int MemFdFile::addSeals(unsigned seals) {
  if (seals & ~LinuxMemFd::AllSeals) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  TerminationDeferral lifetime;
  auto writer = lockWrites();
  MemoryMapManager& mappings = MemoryMapManager::instance();
  MemoryMapManager::OperationGuard mappingGuard(mappings);
  unsigned previous;
  {
    LockGuard<Mutex> guard(dataMutationLock());
    previous = m_Seals;
    if (previous & LinuxMemFd::Seal) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return -1;
    }
  }
  // Keep the mapping gate through publication, while avoiding a data-lock ->
  // mapping-object lock order. All seal writers also retain the WriteGuard.
  if ((seals & LinuxMemFd::Write) && !(previous & LinuxMemFd::Write) &&
      mappings.hasSharedWriteCapability(this)) {
    SYSCALL_ERROR(DeviceBusy);
    return -1;
  }
  {
    LockGuard<Mutex> guard(dataMutationLock());
    m_Seals |= seals;
  }
  return 0;
}

bool MemFdFile::allowMapping(bool shared, bool writeRequested, bool& mayWrite) {
  LockGuard<Mutex> guard(dataMutationLock());
  if (shared && (m_Seals & (LinuxMemFd::Write | LinuxMemFd::FutureWrite))) {
    if (writeRequested) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return false;
    }
    mayWrite = false;
  }
  return true;
}

bool MemFdFile::prepareWrite(uint64_t location, uint64_t size) {
  if (!size)
    return true;
  if (m_Seals & (LinuxMemFd::Write | LinuxMemFd::FutureWrite)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  const uint64_t currentSize = getSize();
  if ((m_Seals & LinuxMemFd::Grow) && (location > currentSize || size > currentSize - location)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  return RamFile::prepareWrite(location, size);
}

bool MemFdFile::allowResize(size_t oldSize, size_t newSize) {
  if ((newSize < oldSize && (m_Seals & LinuxMemFd::Shrink)) ||
      (newSize > oldSize && (m_Seals & LinuxMemFd::Grow))) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  return true;
}
