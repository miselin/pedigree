/* Copyright (c) 2026, Pedigree Developers. */
#include "descriptor-path.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/utility.h"

#include "DevFs.h"
#include "PosixSubsystem.h"
#include "ProcFs.h"
#include "fanotify-syscalls.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/Symlink.h"
#include "signalfd-syscalls.h"
#include "timerfd-syscalls.h"

namespace {
constexpr uint32_t DirectoryPermissions = FILE_UR | FILE_UX | FILE_GR | FILE_GX | FILE_OR | FILE_OX;
constexpr uint32_t LinkPermissions =
    FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR | FILE_OW | FILE_OX;

class DescriptorOwner {
 public:
  DescriptorOwner(size_t pid, const SharedPointer<PosixNamespaceContext>& identity)
      : m_Pid(pid), m_Identity(identity) {}

  PosixSubsystem* acquire() const {
    auto* thread = Processor::information().getCurrentThread();
    auto* process = thread ? thread->getParent() : nullptr;
    if (!process || process->getType() != Process::Posix ||
        process->getUserspaceId() != m_Pid) {
      // Cross-process fd access needs a ptrace access policy; self access
      // remains safe without exposing another process's retained objects.
      SYSCALL_ERROR(PermissionDenied);
      return nullptr;
    }
    auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
    if (!subsystem || subsystem->namespaceContext().get() != m_Identity.get()) {
      SYSCALL_ERROR(DoesNotExist);
      return nullptr;
    }
    return subsystem;
  }

 private:
  size_t m_Pid;
  SharedPointer<PosixNamespaceContext> m_Identity;
};

bool descriptorNumber(const StringView& name, size_t& number) {
  if (!name.length() || (name.length() > 1 && name[0] == '0'))
    return false;
  number = 0;
  for (size_t i = 0; i < name.length(); ++i) {
    const char ch = name[i];
    if (ch < '0' || ch > '9' || number > (~size_t(0) - (ch - '0')) / 10)
      return false;
    number = number * 10 + ch - '0';
  }
  return true;
}

class DescriptorLink final : public Symlink {
 public:
  DescriptorLink(ProcFs& filesystem, File* parent, const String& name, const DescriptorOwner& owner,
                 size_t fd)
      : Symlink(name, 0, 0, 0, filesystem.getNextInode(), &filesystem, 0, parent),
        m_Owner(owner),
        m_Fd(fd) {
    setPermissions(LinkPermissions);
  }

  bool isPathLink() const override {
    return true;
  }

  bool followPath(FilesystemPathRef& result) override {
    DescriptorLease descriptor;
    if (!acquire(descriptor))
      return false;
    auto path = descriptor->openingPath();
    if (path) {
      result = path;
      return true;
    }
    auto* view = VFS::instance().mountView();
    File* file = descriptor->getFile();
    if (!view || !file) {
      SYSCALL_ERROR(NoSuchDevice);
      return false;
    }
    return view->anonymousPath(file, result);
  }

  File* followLinkRetained(Directory::ChildLease&) override {
    // File-only traversal cannot preserve the descriptor's opening attachment.
    SYSCALL_ERROR(OperationNotSupported);
    return nullptr;
  }

  int followLink(char* buffer, size_t length) override {
    DescriptorLease descriptor;
    if (!acquire(descriptor))
      return -1;
    String target;
    if (descriptor->getTimerFdImpl())
      target.assign("anon_inode:[timerfd]");
    else if (descriptor->getSignalFdImpl())
      target.assign("anon_inode:[signalfd]");
    else if (descriptor->getFanotifyImpl())
      target.assign("anon_inode:[fanotify]");
    else if (File* file = descriptor->getFile()) {
      auto path = descriptor->openingPath();
      auto* view = VFS::instance().mountView();
      if (path && view && view->attachmentId(path)) {
        auto context =
            Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
        FilesystemContextSnapshot snapshot;
        if (!context || !context->snapshot(snapshot) || !view->formatPath(snapshot, path, target))
          return -1;
      } else {
        target = String("/");
        target += file->getName();
      }
      if (!file->getAttributes().links)
        target += " (deleted)";
    } else {
      SYSCALL_ERROR(NoSuchDevice);
      return -1;
    }
    const size_t copied = length < target.length() ? length : target.length();
    MemoryCopy(buffer, target.cstr(), copied);
    return static_cast<int>(copied);
  }

 private:
  bool acquire(DescriptorLease& descriptor) const {
    auto* subsystem = m_Owner.acquire();
    if (!subsystem)
      return false;
    if (!subsystem->acquireFileDescriptor(m_Fd, descriptor)) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    return true;
  }
  DescriptorOwner m_Owner;
  size_t m_Fd;
};

class DescriptorDirectory final : public ProcFsDirectory {
 public:
  DescriptorDirectory(ProcFs& filesystem, File* parent, size_t pid,
                      const SharedPointer<PosixNamespaceContext>& identity)
      : ProcFsDirectory(String("fd"), 0, 0, 0, filesystem.getNextInode(), &filesystem, 0, parent),
        m_Owner(pid, identity) {
    setPermissions(DirectoryPermissions);
  }

 protected:
  bool cacheResolvedChildren() const override {
    return false;
  }

  LookupStatus resolveChild(const StringView& name, File*& child) override {
    child = nullptr;
    size_t fd;
    if (!descriptorNumber(name, fd))
      return LookupStatus::NotFound;
    auto* subsystem = m_Owner.acquire();
    if (!subsystem)
      return LookupStatus::NotFound;
    DescriptorLease descriptor;
    if (!subsystem->acquireFileDescriptor(fd, descriptor))
      return LookupStatus::NotFound;
    child =
        new DescriptorLink(*static_cast<ProcFs*>(getFilesystem()), this, String(name), m_Owner, fd);
    return child ? LookupStatus::Found : LookupStatus::IoError;
  }

  ReadStatus readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter,
                           void* context) override {
    auto* subsystem = m_Owner.acquire();
    if (!subsystem)
      return ReadStatus::IoError;
    size_t fd;
    DescriptorLease descriptor;
    while (subsystem->acquireNextFileDescriptor(cookie, fd, descriptor)) {
      NormalStaticString name;
      name.append(fd);
      DirectoryEntryView entry{StringView(name, name.length()),
                               static_cast<ProcFs*>(getFilesystem())->getNextInode(),
                               EntryType::Symlink, cookie, fd + 1};
      if (!emitter(context, entry))
        return ReadStatus::Stopped;
      cookie = fd + 1;
    }
    return ReadStatus::Complete;
  }

 private:
  DescriptorOwner m_Owner;
};

class DevFdLink final : public Symlink {
 public:
  DevFdLink(DevFs& filesystem, File* parent)
      : Symlink(String("fd"), 0, 0, 0, filesystem.getNextInode(), &filesystem, 12, parent) {
    m_sTarget.assign("/proc/self/fd");
    setPermissions(LinkPermissions);
  }
};
}  // namespace

File* posix_make_descriptor_directory(ProcFs& filesystem, File* parent, size_t pid,
                                      const SharedPointer<PosixNamespaceContext>& identity) {
  return new DescriptorDirectory(filesystem, parent, pid, identity);
}

File* posix_make_dev_fd_link(DevFs& filesystem, File* parent) {
  return new DevFdLink(filesystem, parent);
}
