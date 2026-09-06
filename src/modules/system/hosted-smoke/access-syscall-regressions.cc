/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/utility.h"

#include <fcntl.h>
#include <unistd.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/UnixFilesystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"

namespace {
constexpr size_t AccessDescriptor = 97;
constexpr size_t DirectoryDescriptor = 98;
constexpr int PreservedErrno = 149;

class AccessSymlink final : public Symlink {
 public:
  AccessSymlink(const String& name, const String& target, Filesystem* filesystem, File* parent)
      : Symlink(name, 0, 0, 0, 2, filesystem, target.length(), parent), m_Target(target) {}

 protected:
  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    if (location >= m_Target.length()) {
      return 0;
    }
    if (size > m_Target.length() - location) {
      size = m_Target.length() - location;
    }
    MemoryCopy(reinterpret_cast<void*>(buffer), m_Target.cstr() + location, size);
    return size;
  }

 private:
  String m_Target;
};

struct AccessContext {
  AccessContext()
      : validation(false),
        pathResolution(false),
        credentials(false),
        emptyPath(false),
        symlinks(false),
        usercopy(false),
        returned(0) {}

  bool validation;
  bool pathResolution;
  bool credentials;
  bool emptyPath;
  bool symlinks;
  bool usercopy;
  Atomic<size_t> returned;
};

bool allocateUserMapping(Process* process, size_t length, uintptr_t& address) {
  address = 0;
  if (!process->allocateUserRange(Process::UserRegion::Normal, length, address)) {
    return false;
  }

  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapAnon(
      mappedAddress, length, MemoryMappedObject::Read | MemoryMappedObject::Write);
  if (!mapping || mappedAddress != address) {
    MemoryMapManager::instance().remove(address, length);
    process->freeUserRange(Process::UserRegion::Normal, address, length);
    address = 0;
    return false;
  }
  return true;
}

bool expectFailure(Thread* thread, int result, size_t error) {
  return result == -1 && thread->getErrno() == error;
}

bool expectSuccess(Thread* thread, int result) {
  return result == 0 && thread->getErrno() == PreservedErrno;
}

int accessWorker(void* parameter) {
  AccessContext* context = reinterpret_cast<AccessContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t address = 0;
  if (!allocateUserMapping(process, pageSize, address)) {
    context->returned += 1;
    return 1;
  }

  constexpr char AbsolutePath[] = "/access-file";
  constexpr char RelativePath[] = "access-file";
  constexpr char MissingPath[] = "/access-missing";
  constexpr char SymlinkPath[] = "/access-link";
  char* absolutePath = reinterpret_cast<char*>(address);
  char* relativePath = absolutePath + sizeof(AbsolutePath);
  char* missingPath = relativePath + sizeof(RelativePath);
  char* symlinkPath = missingPath + sizeof(MissingPath);
  char* emptyPath = symlinkPath + sizeof(SymlinkPath);
  MemoryCopy(absolutePath, AbsolutePath, sizeof(AbsolutePath));
  MemoryCopy(relativePath, RelativePath, sizeof(RelativePath));
  MemoryCopy(missingPath, MissingPath, sizeof(MissingPath));
  MemoryCopy(symlinkPath, SymlinkPath, sizeof(SymlinkPath));
  *emptyPath = 0;

  thread->setErrno(0);
  const bool invalidMode =
      expectFailure(thread, posix_faccessat(AT_FDCWD, absolutePath, 8, 0), Error::InvalidArgument);
  thread->setErrno(0);
  const bool invalidFlags = expectFailure(
      thread, posix_faccessat(AT_FDCWD, absolutePath, F_OK, 0x40000000), Error::InvalidArgument);
  context->validation = invalidMode && invalidFlags;

  thread->setErrno(PreservedErrno);
  const bool absoluteIgnoresFd = expectSuccess(thread, posix_faccessat(-1, absolutePath, F_OK, 0));
  thread->setErrno(0);
  const bool relativeRejectsFd =
      expectFailure(thread, posix_faccessat(-1, relativePath, F_OK, 0), Error::BadFileDescriptor);
  thread->setErrno(0);
  const bool relativeRejectsFile = expectFailure(
      thread, posix_faccessat(AccessDescriptor, relativePath, F_OK, 0), Error::NotADirectory);
  thread->setErrno(PreservedErrno);
  const bool relativeUsesDirectory =
      expectSuccess(thread, posix_faccessat(DirectoryDescriptor, relativePath, F_OK, 0));
  thread->setErrno(0);
  const bool missingIsEnoent =
      expectFailure(thread, posix_faccessat(-1, missingPath, F_OK, 0), Error::DoesNotExist);
  context->pathResolution = absoluteIgnoresFd && relativeRejectsFd && relativeRejectsFile &&
                            relativeUsesDirectory && missingIsEnoent;

  thread->setErrno(0);
  const bool realDenied = expectFailure(thread, posix_faccessat(AT_FDCWD, absolutePath, R_OK, 0),
                                        Error::PermissionDenied);
  thread->setErrno(PreservedErrno);
  const bool effectiveAllowed =
      expectSuccess(thread, posix_faccessat(AT_FDCWD, absolutePath, R_OK, AT_EACCESS));
  context->credentials = realDenied && effectiveAllowed && process->getUserId() == 100 &&
                         process->getEffectiveUserId() == 200 && process->getGroupId() == 10 &&
                         process->getEffectiveGroupId() == 20;

  thread->setErrno(0);
  const bool emptyNeedsFlag = expectFailure(
      thread, posix_faccessat(AccessDescriptor, emptyPath, F_OK, 0), Error::DoesNotExist);
  thread->setErrno(PreservedErrno);
  const bool emptyDescriptorExists =
      expectSuccess(thread, posix_faccessat(AccessDescriptor, emptyPath, F_OK, AT_EMPTY_PATH));
  thread->setErrno(PreservedErrno);
  const bool emptyCwdExists =
      expectSuccess(thread, posix_faccessat(AT_FDCWD, emptyPath, F_OK, AT_EMPTY_PATH));
  thread->setErrno(0);
  const bool emptyRealDenied =
      expectFailure(thread, posix_faccessat(AccessDescriptor, emptyPath, R_OK, AT_EMPTY_PATH),
                    Error::PermissionDenied);
  thread->setErrno(PreservedErrno);
  const bool emptyEffectiveAllowed = expectSuccess(
      thread, posix_faccessat(AccessDescriptor, emptyPath, R_OK, AT_EMPTY_PATH | AT_EACCESS));
  thread->setErrno(0);
  const bool emptyRejectsFd = expectFailure(
      thread, posix_faccessat(-1, emptyPath, F_OK, AT_EMPTY_PATH), Error::BadFileDescriptor);
  context->emptyPath = emptyNeedsFlag && emptyDescriptorExists && emptyCwdExists &&
                       emptyRealDenied && emptyEffectiveAllowed && emptyRejectsFd;

  thread->setErrno(0);
  const bool followsByDefault = expectFailure(
      thread, posix_faccessat(AT_FDCWD, symlinkPath, W_OK, 0), Error::PermissionDenied);
  thread->setErrno(PreservedErrno);
  const bool nofollowChecksLink =
      expectSuccess(thread, posix_faccessat(AT_FDCWD, symlinkPath, W_OK, AT_SYMLINK_NOFOLLOW));
  context->symlinks = followsByDefault && nofollowChecksLink;

  thread->setErrno(0);
  context->usercopy =
      expectFailure(thread, posix_faccessat(AT_FDCWD, nullptr, F_OK, 0), Error::BadAddress);

  MemoryMapManager::instance().remove(address, pageSize);
  process->freeUserRange(Process::UserRegion::Normal, address, pageSize);
  context->returned += 1;
  return 0;
}

bool closeDescriptor(PosixSubsystem* subsystem, size_t fd) {
  DescriptorLease descriptor;
  return subsystem->acquireFileDescriptor(fd, descriptor) &&
         subsystem->closeFileDescriptor(fd, descriptor);
}

bool accessSemantics(Process* kernelProcess) {
  Filesystem* priorRoot = VFS::instance().getRootFilesystem();
  if (priorRoot) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL faccessat2-semantics: "
        "the isolated access fixture requires an empty hosted root namespace");
    return false;
  }

  UnixFilesystem* filesystem = new UnixFilesystem;
  Filesystem* displacedRoot = VFS::instance().swapRootFilesystemForHostedTest(filesystem);
  File* root = filesystem->getRoot();
  UnixDirectory* directory = static_cast<UnixDirectory*>(Directory::fromFile(root));

  File* target = new File(String("access-file"), 0, 0, 0, 1, filesystem, 0, root);
  target->setUid(200);
  target->setGid(20);
  target->setPermissions(FILE_UR);
  const bool targetAdded = directory->addEntry(target->getName(), target);

  AccessSymlink* link =
      new AccessSymlink(String("access-link"), String("/access-file"), filesystem, root);
  link->setUid(100);
  link->setGid(10);
  link->setPermissions(FILE_UW);
  const bool linkAdded = directory->addEntry(link->getName(), link);

  PosixProcess* process = new PosixProcess(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  subsystem->setAbi(PosixSubsystem::LinuxAbi);
  process->setCwd(root);
  process->setUserId(100);
  process->setEffectiveUserId(200);
  process->setGroupId(10);
  process->setEffectiveGroupId(20);
  subsystem->addFileDescriptor(AccessDescriptor,
                               new FileDescriptor(target, 0, AccessDescriptor, 0, O_RDONLY));
  subsystem->addFileDescriptor(DirectoryDescriptor,
                               new FileDescriptor(root, 0, DirectoryDescriptor, 0, O_RDONLY));

  AccessContext context;
  Thread* worker = new Thread(process, accessWorker, &context, nullptr, false, true, true);
  worker->setName("hosted faccessat2 semantics");
  const bool started = targetAdded && linkAdded && displacedRoot == priorRoot && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  bool passed = started && joined && context.returned == 1 && context.validation &&
                context.pathResolution && context.credentials && context.emptyPath &&
                context.symlinks && context.usercopy;
  passed = closeDescriptor(subsystem, AccessDescriptor) && passed;
  passed = closeDescriptor(subsystem, DirectoryDescriptor) && passed;
  process->setCwd(nullptr);
  delete process;

  Filesystem* removedRoot = VFS::instance().swapRootFilesystemForHostedTest(priorRoot);
  passed = removedRoot == filesystem && passed;
  delete filesystem;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL faccessat2-semantics: "
        "validation, resolution, credential, empty-path, symlink, or usercopy behavior regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS faccessat2-semantics");
  return true;
}
}  // namespace

bool runHostedAccessSyscallRegressions(Process* process) {
  return accessSemantics(process);
}
