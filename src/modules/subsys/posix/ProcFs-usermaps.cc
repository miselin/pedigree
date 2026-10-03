/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/utility.h"

#include "ProcFs.h"
#include "modules/system/vfs/VFS.h"
#include "user-namespace.h"

namespace {
enum class Kind { Uid, Gid, Setgroups };
const char* name(Kind kind) {
  return kind == Kind::Uid ? "uid_map" : kind == Kind::Gid ? "gid_map" : "setgroups";
}

class UserMapFile final : public File {
 public:
  UserMapFile(ProcFs& filesystem, File* parent, Kind kind, const UserNamespaceRef& space,
              uint32_t uid, uint32_t gid)
      : File(String(name(kind)), 0, 0, 0, filesystem.getNextInode(), &filesystem, 0, parent),
        m_Kind(kind),
        m_Space(space),
        m_Viewer(posix_user_namespace(*Processor::information().getCurrentThread())) {
    setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_OR);
    setUidOnly(uid);
    setGidOnly(gid);
  }
  bool allowMapping(bool, bool, bool&) override {
    SYSCALL_ERROR(NoSuchDevice);
    return false;
  }

 protected:
  bool isBytewise() const override {
    return true;
  }
  bool allowResize(size_t, size_t) override {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  uint64_t readBytewise(uint64_t offset, uint64_t size, uintptr_t buffer, bool) override {
    auto bytes = UniqueArray<char>::allocate(4096);
    if (!bytes) {
      SYSCALL_ERROR(OutOfMemory);
      return 0;
    }
    size_t length;
    if (m_Kind == Kind::Setgroups) {
      const bool allow = !m_Space || m_Space->groupsAllowed();
      length = allow ? 6 : 5;
      MemoryCopy(bytes.get(), allow ? "allow\n" : "deny\n", length);
    } else if (!m_Space) {
      static const char initial[] = "0 0 4294967295\n";
      length = sizeof(initial) - 1;
      MemoryCopy(bytes.get(), initial, length);
    } else {
      length = m_Space->readMap(m_Kind == Kind::Gid, m_Viewer, bytes.get(), 4096);
    }
    if (offset >= length) {
      return 0;
    }
    if (size > length - offset) {
      size = length - offset;
    }
    MemoryCopy(reinterpret_cast<void*>(buffer), bytes.get() + offset, size);
    return size;
  }
  uint64_t writeBytewise(uint64_t offset, uint64_t size, uintptr_t buffer, bool) override {
    if (!m_Space) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return 0;
    }
    if (offset || size >= 4096) {
      SYSCALL_ERROR(InvalidArgument);
      return 0;
    }
    const char* bytes = reinterpret_cast<const char*>(buffer);
    const int result = m_Kind == Kind::Setgroups
                           ? m_Space->writeSetgroups(bytes, size)
                           : m_Space->writeMap(m_Kind == Kind::Gid, bytes, size);
    return result < 0 ? 0 : result;
  }

 private:
  const Kind m_Kind;
  const UserNamespaceRef m_Space, m_Viewer;
};

class UserMapNode final : public File {
 public:
  UserMapNode(ProcFs& filesystem, File* parent, Process& process, Kind kind)
      : File(String(name(kind)), 0, 0, 0, filesystem.getNextInode(), &filesystem, 0, parent),
        m_Kind(kind),
        m_ProcessId(process.getId()),
        m_Identity(process.pidIdentity()) {
    setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_OR);
    setUidOnly(process.getEffectiveUserId());
    setGidOnly(process.getEffectiveGroupId());
  }
  File* openForDescriptor(RetainedFile& owner) override {
    TerminationDeferral lifetime;
    Scheduler::ProcessLease process;
    Process::ThreadLease task;
    if (!Scheduler::instance().acquireProcessById(process, m_ProcessId) ||
        process->pidIdentity() != m_Identity ||
        !process->acquireThreadByUserspaceId(task, process->getUserspaceId(), nullptr)) {
      SYSCALL_ERROR(NoSuchProcess);
      return nullptr;
    }
    // Capture at open, so unshare changes subsequent opens while an existing
    // descriptor continues to address the namespace it originally opened.
    auto* file = new UserMapFile(*static_cast<ProcFs*>(getFilesystem()), getParent(), m_Kind,
                                 posix_user_namespace(*task.get()), process->getEffectiveUserId(),
                                 process->getEffectiveGroupId());
    if (!file || !VFS::instance().tryTrackFile(file)) {
      delete file;
      SYSCALL_ERROR(OutOfMemory);
      return nullptr;
    }
    owner.adopt(file);
    return file;
  }

 private:
  const Kind m_Kind;
  const size_t m_ProcessId;
  const SharedPointer<UserspacePid> m_Identity;
};
}  // namespace

bool procfsAddUserMaps(ProcFs& filesystem, ProcFsDirectory& directory, Process& process) {
  constexpr Kind kinds[] = {Kind::Uid, Kind::Gid, Kind::Setgroups};
  for (Kind kind : kinds) {
    auto* file = new UserMapNode(filesystem, &directory, process, kind);
    if (!file) {
      return false;
    }
    directory.addEntry(file->getName(), file);
  }
  return true;
}
