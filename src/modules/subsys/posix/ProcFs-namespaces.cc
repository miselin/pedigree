/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/utility.h"

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "ProcFs.h"
#include "descriptor-path.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include "namespace-file.h"
#include "uts-namespace.h"

namespace {
constexpr uint32_t DirectoryPermissions = FILE_UR | FILE_UX | FILE_GR | FILE_GX | FILE_OR | FILE_OX;

bool taskNumber(const StringView& name, size_t& result) {
  if (!name.length() || name[0] == '0')
    return false;
  size_t value = 0;
  for (size_t i = 0; i < name.length(); ++i) {
    if (name[i] < '0' || name[i] > '9')
      return false;
    const size_t digit = name[i] - '0';
    if (value > (~size_t(0) - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  result = value;
  return true;
}

class NamespaceLink final : public Symlink {
 public:
  NamespaceLink(ProcFs& filesystem, uintptr_t inode, File* parent, const PosixUtsTarget& target)
      : Symlink(String("uts"), 0, 0, 0, inode, &filesystem, 0, parent), m_Target(target) {
    setPermissions(DirectoryPermissions | FILE_UW | FILE_GW | FILE_OW);
  }

  bool isPathLink() const override {
    return true;
  }

  bool followPath(FilesystemPathRef& result) override {
    UtsRef space;
    UtsStatus status = posix_uts_acquire_target(m_Target, space);
    if (status != UtsStatus::Success) {
      posix_uts_error(status);
      return false;
    }
    RetainedFile file;
    status = posix_uts_make_file(space, file);
    if (status != UtsStatus::Success) {
      posix_uts_error(status);
      return false;
    }
    auto* view = VFS::instance().mountView();
    if (!view) {
      SYSCALL_ERROR(NoSuchDevice);
      return false;
    }
    return view->anonymousPath(file.get(), result);
  }

  int followLink(char* buffer, size_t length) override {
    UtsRef space;
    const UtsStatus status = posix_uts_acquire_target(m_Target, space);
    if (status != UtsStatus::Success)
      return posix_uts_error(status);
    NormalStaticString name("uts:[");
    name.append(space->identity());
    name.append(']');
    const size_t copied = length < name.length() ? length : name.length();
    MemoryCopy(buffer, static_cast<const char*>(name), copied);
    return static_cast<int>(copied);
  }

 private:
  PosixUtsTarget m_Target;
};

class NamespaceDirectory final : public ProcFsDirectory {
 public:
  NamespaceDirectory(ProcFs& filesystem, File* parent, const PosixUtsTarget& target)
      : ProcFsDirectory(String("ns"), 0, 0, 0, filesystem.getNextInode(), &filesystem, 0, parent),
        m_Target(target),
        m_LinkInode(filesystem.getNextInode()) {
    setPermissions(DirectoryPermissions);
  }

 protected:
  LookupStatus resolveChild(const StringView& name, File*& child) override {
    child = nullptr;
    if (!(name == "uts"))
      return LookupStatus::NotFound;
    child = new NamespaceLink(*static_cast<ProcFs*>(getFilesystem()), m_LinkInode, this, m_Target);
    return child ? LookupStatus::Found : LookupStatus::IoError;
  }

  ReadStatus readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter,
                           void* context) override {
    if (!cookie) {
      DirectoryEntryView entry{StringView("uts"), m_LinkInode, EntryType::Symlink, 0, 1};
      if (!emitter(context, entry))
        return ReadStatus::Stopped;
      cookie = 1;
    }
    return ReadStatus::Complete;
  }

 private:
  PosixUtsTarget m_Target;
  const uintptr_t m_LinkInode;
};

class TaskDirectory final : public ProcFsDirectory {
 public:
  TaskDirectory(ProcFs& filesystem, File* parent,
                const SharedPointer<PosixNamespaceContext>& context)
      : ProcFsDirectory(String("task"), 0, 0, 0, filesystem.getNextInode(), &filesystem, 0, parent),
        m_Context(context) {
    setPermissions(DirectoryPermissions);
  }

  void invalidate(size_t taskId) {
    NormalStaticString name;
    name.append(taskId);
    ChildLease previous;
    if (lookupChild(HashedStringView(name), previous) == LookupStatus::Found) {
      // A retained task directory can outlive this cache and its process.
      // Detachment retains its parent before dropping the publication ref.
      static_cast<ProcFsDirectory*>(previous.get())->markDetached();
    }
    remove(HashedStringView(name));
  }

 protected:
  LookupStatus resolveChild(const StringView& name, File*& child) override {
    child = nullptr;
    size_t taskId = 0;
    PosixUtsTarget target;
    if (!taskNumber(name, taskId) || !m_Context->taskTarget(taskId, target))
      return LookupStatus::NotFound;
    auto& filesystem = *static_cast<ProcFs*>(getFilesystem());
    auto* directory =
        new ProcFsDirectory(String(name), 0, 0, 0, filesystem.getNextInode(), &filesystem, 0, this);
    if (!directory)
      return LookupStatus::IoError;
    directory->setPermissions(DirectoryPermissions);
    auto* namespaces = new NamespaceDirectory(filesystem, directory, target);
    if (!namespaces) {
      delete directory;
      return LookupStatus::IoError;
    }
    directory->addEntry(String("ns"), namespaces);
    child = directory;
    return LookupStatus::Found;
  }

  ReadStatus readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter,
                           void* context) override {
    size_t taskId = 0;
    PosixUtsTarget target;
    while (m_Context->nextTaskTarget(cookie, taskId, target)) {
      NormalStaticString name;
      name.append(taskId);
      ChildLease child;
      const LookupStatus status = lookupChild(HashedStringView(name), child);
      if (status == LookupStatus::IoError)
        return ReadStatus::IoError;
      if (status == LookupStatus::Found) {
        DirectoryEntryView entry{StringView(name, name.length()), child.get()->getInode(),
                                 EntryType::Directory, cookie, taskId};
        if (!emitter(context, entry))
          return ReadStatus::Stopped;
      }
      cookie = taskId;
    }
    return ReadStatus::Complete;
  }

 private:
  SharedPointer<PosixNamespaceContext> m_Context;
};

class ExecutableLink final : public Symlink {
 public:
  ExecutableLink(ProcFs& filesystem, File* parent)
      : Symlink(String("exe"), 0, 0, 0, filesystem.getNextInode(), &filesystem, 0, parent) {
    setPermissions(DirectoryPermissions | FILE_UW | FILE_GW | FILE_OW);
  }

  int followLink(char* buffer, size_t length) override {
    auto* thread = Processor::information().getCurrentThread();
    auto* process = thread ? thread->getParent() : nullptr;
    auto* subsystem = process && process->getType() == Process::Posix
                          ? static_cast<PosixSubsystem*>(process->getSubsystem())
                          : nullptr;
    String target;
    if (!subsystem || !subsystem->executablePath(target)) {
      SYSCALL_ERROR(DoesNotExist);
      return -1;
    }
    const size_t copied = length < target.length() ? length : target.length();
    MemoryCopy(buffer, target.cstr(), copied);
    return static_cast<int>(copied);
  }
};

class ProcessDirectory final : public ProcFsDirectory {
 public:
  ProcessDirectory(ProcFs& filesystem, const String& name,
                   const SharedPointer<PosixNamespaceContext>& context)
      : ProcFsDirectory(name, 0, 0, 0, filesystem.getNextInode(), &filesystem, 0,
                        filesystem.getRoot()),
        m_Context(context) {}

  bool initialise(ProcFs& filesystem, size_t pid) {
    if (!m_Context)
      return true;
    PosixUtsTarget leader;
    if (!m_Context->leaderTarget(leader))
      return false;
    auto* namespaces = new NamespaceDirectory(filesystem, this, leader);
    if (!namespaces)
      return false;
    addEntry(String("ns"), namespaces);
    m_Tasks = new TaskDirectory(filesystem, this, m_Context);
    if (!m_Tasks)
      return false;
    addEntry(String("task"), m_Tasks);
    auto* descriptors = posix_make_descriptor_directory(filesystem, this, pid, m_Context);
    if (!descriptors)
      return false;
    addEntry(String("fd"), descriptors);
    auto* executable = new ExecutableLink(filesystem, this);
    if (!executable)
      return false;
    addEntry(executable->getName(), executable);
    return true;
  }

  void invalidate(const SharedPointer<PosixNamespaceContext>& context, size_t taskId) {
    if (context.get() != m_Context.get() || !m_Tasks)
      return;
    m_Tasks->invalidate(taskId);
  }

 private:
  SharedPointer<PosixNamespaceContext> m_Context;
  TaskDirectory* m_Tasks = nullptr;
};

class SelfLink final : public Symlink {
 public:
  SelfLink(ProcFs& filesystem, bool thread)
      : Symlink(String(thread ? "thread-self" : "self"), 0, 0, 0, filesystem.getNextInode(),
                &filesystem, 0, filesystem.getRoot()),
        m_Thread(thread) {
    setPermissions(DirectoryPermissions | FILE_UW | FILE_GW | FILE_OW);
  }

  int followLink(char* buffer, size_t length) override {
    NormalStaticString target;
    if (!name(target))
      return -1;
    const size_t copied = length < target.length() ? length : target.length();
    MemoryCopy(buffer, static_cast<const char*>(target), copied);
    return static_cast<int>(copied);
  }

 private:
  bool name(NormalStaticString& result) const {
    auto* thread = Processor::information().getCurrentThread();
    auto* process = thread ? thread->getParent() : nullptr;
    if (!process || process->getType() != Process::Posix) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    result.append(process->getId());
    if (m_Thread) {
      result.append("/task/");
      result.append(thread->getTaskId());
    }
    return true;
  }
  const bool m_Thread;
};
}  // namespace

bool ProcFs::initialiseNamespaceLinks() {
  auto* self = new SelfLink(*this, false);
  auto* threadSelf = new SelfLink(*this, true);
  if (!self || !threadSelf) {
    delete self;
    delete threadSelf;
    return false;
  }
  m_pRoot->addEntry(self->getName(), self);
  m_pRoot->addEntry(threadSelf->getName(), threadSelf);
  return true;
}

ProcFsDirectory* ProcFs::createProcessDirectory(PosixProcess* process) {
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  auto context = subsystem ? subsystem->namespaceContext() : SharedPointer<PosixNamespaceContext>();
  NormalStaticString name;
  name.append(process->getId());
  auto* directory = new ProcessDirectory(*this, String(name, name.length()), context);
  if (directory && !directory->initialise(*this, process->getId())) {
    delete directory;
    directory = nullptr;
  }
  return directory;
}

void ProcFs::invalidateNamespaceTask(const SharedPointer<PosixNamespaceContext>& context,
                                     size_t pid, size_t taskId) {
  if (!context || !pid || !taskId)
    return;
  NormalStaticString name;
  name.append(pid);
  Directory::ChildLease directory;
  if (m_pRoot->lookupChild(HashedStringView(name), directory) != Directory::LookupStatus::Found)
    return;
  // Numeric root entries are created only by createProcessDirectory; retaining
  // that entry also retains its resident task directory during invalidation.
  static_cast<ProcessDirectory*>(directory.get())->invalidate(context, taskId);
}

void procfsInvalidateNamespaceTask(const SharedPointer<PosixNamespaceContext>& context, size_t pid,
                                   size_t taskId) {
  auto& vfs = VFS::instance();
  Filesystem* key = vfs.getFilesystemAt(String("/media/proc"));
  VFS::MountOperation mount;
  if (!vfs.acquireMount(key, mount))
    return;
  static_cast<ProcFs*>(mount.filesystem())->invalidateNamespaceTask(context, pid, taskId);
}
