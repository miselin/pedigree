/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Filesystem.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/LazyEvaluate.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Directory.h"
#include "File.h"
#include "Symlink.h"
#include "VFS.h"

Filesystem::Filesystem() : m_bReadOnly(false), m_pDisk(0) {}

Mutex Filesystem::m_StructureLock;

Filesystem::~Filesystem() = default;

FileHandleStatus Filesystem::encodeFileHandle(File&, FileHandle& handle) {
  handle = FileHandle();
  return FileHandleStatus::Unsupported;
}

FileHandleStatus Filesystem::decodeFileHandle(const FileHandle&, RetainedFile& file) {
  file.reset();
  return FileHandleStatus::Unsupported;
}

FileHandleStatus Filesystem::fileHandleFsid(FileSystemId& id) {
  id = FileSystemId();
  return FileHandleStatus::Unsupported;
}

namespace {
class InodeRetirementDrain {
 public:
  ~InodeRetirementDrain() {
    if (m_File)
      m_File.get()->finishInodeRetirement();
  }
  bool retain(File* file) {
    if (!file)
      return true;
    if (!file->retainVfsReference()) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    m_File.adopt(file);
    return true;
  }

 private:
  RetainedFile m_File;
};

class TrueRootLease {
 public:
  explicit TrueRootLease(Filesystem* filesystem)
#if !defined(VFS_STANDALONE) && THREADS
      : m_ProcessLease(), m_pRoot(filesystem->getRoot()) {
    Process* process = Processor::information().getCurrentThread()->getParent();
    File* processRoot = process->acquireRootFile(m_ProcessLease);
    if (processRoot) {
      m_pRoot = processRoot;
    }
  }
#else
      : m_pRoot(filesystem->getRoot()) {
  }
#endif

  File* get() const {
    return m_pRoot;
  }

 private:
#if !defined(VFS_STANDALONE) && THREADS
  Process::FileContextLease m_ProcessLease;
#endif
  File* m_pRoot;
};

bool targetAbsentForCreate(File* parent, const String& filename) {
  if (!filename.length() || filename == "." || filename == "..") {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (!parent->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }

  Directory::ChildLease existing;
  const Directory::LookupStatus status =
      Directory::fromFile(parent)->lookupChild(HashedStringView(filename), existing);
  if (status == Directory::LookupStatus::NotFound) {
    return true;
  }
  if (status == Directory::LookupStatus::IoError) {
    SYSCALL_ERROR(IoError);
  } else {
    SYSCALL_ERROR(FileExists);
  }
  return false;
}
}  // namespace

File* Filesystem::find(const StringView& path) {
  return findNode(nullptr, path);
}

File* Filesystem::find(const String& path) {
  return findNode(nullptr, path.view());
}

File* Filesystem::find(const StringView& path, File* pStartNode) {
  assert(pStartNode != nullptr);
  File* a = findNode(pStartNode, path);
  return a;
}

File* Filesystem::find(const String& path, File* pStartNode) {
  return find(path.view(), pStartNode);
}

File* Filesystem::findRetained(const StringView& path, Directory::ChildLease& result,
                               File* pStartNode) {
  TrueRootLease rootLease(this);
  File* trueRoot = rootLease.get();
  if (!pStartNode) {
    pStartNode = trueRoot;
  }

  File* retained = nullptr;
  File* found = findNode(pStartNode, path, pStartNode, trueRoot, &retained);
  if (!found) {
    return nullptr;
  }

  Directory::ChildLease replacement;
  if (retained) {
    replacement.adopt(retained);
  }
  result.swap(replacement);
  return found;
}

bool Filesystem::createFile(const StringView& path, uint32_t mask, File* pStartNode) {
  TrueRootLease startLease(this);
  if (!pStartNode) {
    pStartNode = startLease.get();
  }

  String filename;
  Directory::ChildLease parentLease;
  File* retainedParent = nullptr;
  File* pParent = findParent(path, pStartNode, filename, &retainedParent);
  if (retainedParent)
    parentLease.adopt(retainedParent);

  // Check the parent existed.
  if (!pParent) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  if (!targetAbsentForCreate(pParent, filename))
    return false;

  // Are we allowed to make the file?
  if (!VFS::checkAccess(pParent, false, true, true)) {
    return false;
  }

  // May need to create on a different filesytem (if the traversal crossed
  // over to a different fs)
  Filesystem* pFs = pParent->getFilesystem();

  // Now make the file.
  return pFs->createFile(pParent, filename, mask);
}

bool Filesystem::createDirectory(const StringView& path, uint32_t mask, File* pStartNode) {
  TrueRootLease startLease(this);
  if (!pStartNode) {
    pStartNode = startLease.get();
  }

  String filename;
  Directory::ChildLease parentLease;
  File* retainedParent = nullptr;
  File* pParent = findParent(path, pStartNode, filename, &retainedParent);
  if (retainedParent)
    parentLease.adopt(retainedParent);

  // Check the parent existed.
  if (!pParent) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  if (!targetAbsentForCreate(pParent, filename))
    return false;

  // Are we allowed to make the file?
  if (!VFS::checkAccess(pParent, false, true, true)) {
    return false;
  }

  // May need to create on a different filesytem (if the traversal crossed
  // over to a different fs)
  Filesystem* pFs = pParent->getFilesystem();

  // Now make the directory.
  return pFs->createDirectory(pParent, filename, mask);
}

bool Filesystem::createSymlink(const StringView& path, const String& value, File* pStartNode) {
  TrueRootLease startLease(this);
  if (!pStartNode) {
    pStartNode = startLease.get();
  }

  String filename;
  Directory::ChildLease parentLease;
  File* retainedParent = nullptr;
  File* pParent = findParent(path, pStartNode, filename, &retainedParent);
  if (retainedParent)
    parentLease.adopt(retainedParent);

  // Check the parent existed.
  if (!pParent) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  if (!targetAbsentForCreate(pParent, filename))
    return false;

  // Are we allowed to make the file?
  if (!VFS::checkAccess(pParent, false, true, true)) {
    return false;
  }

  // May need to create on a different filesytem (if the traversal crossed
  // over to a different fs)
  Filesystem* pFs = pParent->getFilesystem();

  // Now make the symlink.
  return pFs->createSymlink(pParent, filename, value);
}

bool Filesystem::createLink(const StringView& path, File* target, File* pStartNode) {
  TrueRootLease startLease(this);
  if (!pStartNode) {
    pStartNode = startLease.get();
  }

  String filename;
  Directory::ChildLease parentLease;
  File* retainedParent = nullptr;
  File* pParent = findParent(path, pStartNode, filename, &retainedParent);
  if (retainedParent)
    parentLease.adopt(retainedParent);

  // Check the parent existed.
  if (!pParent) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  if (!targetAbsentForCreate(pParent, filename))
    return false;

  // Are we allowed to make the file?
  if (!VFS::checkAccess(pParent, false, true, true)) {
    return false;
  }

  // Links can't cross filesystems (symlinks can, though).
  if (this != target->getFilesystem()) {
    SYSCALL_ERROR(CrossDeviceLink);
    return false;
  }

  // May need to create on a different filesytem (if the traversal crossed
  // over to a different fs)
  Filesystem* pFs = pParent->getFilesystem();

  // Now make the symlink.
  return pFs->createLink(pParent, filename, target);
}

bool Filesystem::remove(const StringView& path, File* pStartNode) {
  return remove(path, pStartNode, nullptr);
}

bool Filesystem::remove(const StringView& path, File* pStartNode, File* expected) {
  TrueRootLease startLease(this);
  if (!pStartNode) {
    pStartNode = startLease.get();
  }

  String filename;
  Directory::ChildLease parentLease;
  File* retainedParent = nullptr;
  File* pParent = findParent(path, pStartNode, filename, &retainedParent);
  if (retainedParent)
    parentLease.adopt(retainedParent);

  // Check the parent existed.
  if (!pParent) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  // Dot entries are traversal operators, not removable directory entries.
  // In-memory filesystems may represent them explicitly, so reject them
  // before lookup rather than relying on individual drivers to do so.
  if (!filename.length() || filename == "." || filename == "..") {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  // Are we allowed to delete the file?
  if (!VFS::checkAccess(pParent, false, true, true)) {
    return false;
  }

  // May need to create on a different filesytem (if the traversal crossed
  // over to a different fs)
  Filesystem* pFs = pParent->getFilesystem();
  return pFs->removeChild(pParent, filename, expected);
}

bool Filesystem::remove(File* parent, File* file) {
  if (!file) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  return removeChild(parent, file->getName(), file);
}

bool Filesystem::rename(const StringView& oldPath, File* oldStart, const StringView& newPath,
                        File* newStart) {
  if (!oldPath.length() || !newPath.length()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  InodeRetirementDrain retirement;
  LockGuard<Mutex> structureGuard(m_StructureLock);
  TrueRootLease rootLease(this);
  if (!oldStart) {
    oldStart = rootLease.get();
  }
  if (!newStart) {
    newStart = rootLease.get();
  }

  String oldName;
  String newName;
  Directory::ChildLease oldParentLease;
  Directory::ChildLease newParentLease;
  File* retained = nullptr;
  File* oldParentFile = findParent(oldPath, oldStart, oldName, &retained);
  if (retained) {
    oldParentLease.adopt(retained);
  }
  retained = nullptr;
  File* newParentFile = findParent(newPath, newStart, newName, &retained);
  if (retained) {
    newParentLease.adopt(retained);
  }
  if (!oldParentFile || !newParentFile) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  if (!oldParentFile->isDirectory() || !newParentFile->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  if (!oldName.length() || !newName.length() || oldName == "." || oldName == ".." ||
      newName == "." || newName == "..") {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  Filesystem* filesystem = oldParentFile->getFilesystem();
  if (filesystem != newParentFile->getFilesystem()) {
    SYSCALL_ERROR(CrossDeviceLink);
    return false;
  }
  if (filesystem->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  if (!VFS::checkAccess(oldParentFile, false, true, true) ||
      !VFS::checkAccess(newParentFile, false, true, true)) {
    return false;
  }

  Directory* oldParent = Directory::fromFile(oldParentFile);
  Directory* newParent = Directory::fromFile(newParentFile);
  const bool oldFirst =
      reinterpret_cast<uintptr_t>(oldParent) < reinterpret_cast<uintptr_t>(newParent);
  Directory* first = oldFirst ? oldParent : newParent;
  Directory* second = oldFirst ? newParent : oldParent;
  LockGuard<Mutex> firstGuard(first->namespaceMutationLock());
  LockGuard<Mutex> secondGuard(second->namespaceMutationLock(), second != first);
  if (oldParent->isDetached() || newParent->isDetached()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  Directory::ChildLease sourceLease;
  Directory::ChildLease replacedLease;
  const auto sourceStatus = oldParent->lookupChild(HashedStringView(oldName), sourceLease);
  if (sourceStatus != Directory::LookupStatus::Found) {
    syscallError(sourceStatus == Directory::LookupStatus::IoError ? Error::IoError
                                                                  : Error::DoesNotExist);
    return false;
  }
  File* source = sourceLease.get();
  if (oldParent == newParent && oldName == newName) {
    return true;
  }
  const auto replacedStatus = newParent->lookupChild(HashedStringView(newName), replacedLease);
  if (replacedStatus == Directory::LookupStatus::IoError) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  File* replaced = replacedLease.get();
  if (source->getFilesystem() != filesystem ||
      (replaced && replaced->getFilesystem() != filesystem)) {
    SYSCALL_ERROR(CrossDeviceLink);
    return false;
  }
  if (replaced &&
      (source == replaced || (source->getInode() && source->getInode() == replaced->getInode()))) {
    return true;
  }
  if ((oldPath[oldPath.length() - 1] == '/' && !source->isDirectory()) ||
      (newPath[newPath.length() - 1] == '/' && !source->isDirectory())) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  if (replaced && replaced->isDirectory() != source->isDirectory()) {
    syscallError(replaced->isDirectory() ? Error::IsADirectory : Error::NotADirectory);
    return false;
  }
  Directory* sourceDirectory = source->isDirectory() ? Directory::fromFile(source) : nullptr;
  Directory* replacedDirectory =
      replaced && replaced->isDirectory() ? Directory::fromFile(replaced) : nullptr;
  if ((sourceDirectory && sourceDirectory->getReparsePoint()) ||
      (replacedDirectory && replacedDirectory->getReparsePoint())) {
    SYSCALL_ERROR(DeviceBusy);
    return false;
  }
  if (sourceDirectory) {
    File* ancestor = newParent;
    File::ParentLease ancestorLease;
    while (ancestor) {
      if (ancestor == source) {
        SYSCALL_ERROR(InvalidArgument);
        return false;
      }
      File::ParentLease next;
      String unused;
      ancestor->getNamespace(next, unused);
      ancestorLease.swap(next);
      ancestor = ancestorLease.get();
    }
  }
  if (replacedDirectory == oldParent || replacedDirectory == newParent) {
    SYSCALL_ERROR(NotEmpty);
    return false;
  }

  LockGuard<Mutex> sourceGuard(sourceDirectory ? sourceDirectory->namespaceMutationLock()
                                               : oldParent->namespaceMutationLock(),
                               sourceDirectory != nullptr);
  LockGuard<Mutex> replacedGuard(replacedDirectory ? replacedDirectory->namespaceMutationLock()
                                                   : oldParent->namespaceMutationLock(),
                                 replacedDirectory != nullptr);
  if (replacedDirectory) {
    bool empty = false;
    if (replacedDirectory->isEmpty(empty) != Directory::ReadStatus::Complete) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    if (!empty) {
      SYSCALL_ERROR(NotEmpty);
      return false;
    }
  }

  Directory::NameReservation oldReservation;
  Directory::NameReservation newReservation;
  if (!oldParent->reserveRenameEntry(oldName, oldReservation) ||
      !newParent->reserveRenameEntry(newName, newReservation)) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  bool sourceEphemeral = false;
  bool replacedEphemeral = false;
  {
    LockGuard<Mutex> guard(oldParent->m_CacheLock);
    sourceEphemeral = oldParent->m_EphemeralEntries.lookup(oldName).hasValue();
  }
  if (replaced) {
    LockGuard<Mutex> guard(newParent->m_CacheLock);
    replacedEphemeral = newParent->m_EphemeralEntries.lookup(newName).hasValue();
  }
  if (sourceEphemeral) {
    if (newName.length() > 255) {
      SYSCALL_ERROR(NameTooLong);
      return false;
    }
    if (sourceDirectory) {
      SYSCALL_ERROR(OperationNotSupported);
      return false;
    }
    // Overlay names have no backing record. Remove a backing victim before
    // the non-fallible namespace publication, keeping both names reserved.
    if (replaced && !replacedEphemeral &&
        (!retirement.retain(replaced) || !filesystem->removeNode(newParent, newName, replaced))) {
      return false;
    }
  } else if (!retirement.retain(replacedEphemeral ? nullptr : replaced) ||
             !filesystem->renameNode(oldParent, oldName, source, newParent, newName,
                                     replacedEphemeral ? nullptr : replaced)) {
    return false;
  }
  if (replaced) {
    replaced->retainDetachedParent();
    if (replacedDirectory) {
      replacedDirectory->markDetached();
    }
  }
  source->moveNamespace(newName, newParent);
  if (sourceDirectory) {
    __atomic_store_n(&sourceDirectory->m_ParentInode, newParent->getInode(), __ATOMIC_RELEASE);
  }
  oldParent->moveReservedEntry(oldReservation, newParent, newReservation, source);
  oldReservation.complete(Directory::LookupStatus::NotFound);
  newReservation.complete(Directory::LookupStatus::Found);
  if (replaced) {
    replaced->publishEvent(FileEvents::DeletedSelf);
  }
  return true;
}

bool Filesystem::renameNode(Directory*, const String&, File*, Directory*, const String&, File*) {
  SYSCALL_ERROR(OperationNotSupported);
  return false;
}

bool Filesystem::removeChild(File* parent, const String& filename, File* expected) {
  InodeRetirementDrain retirement;
  LockGuard<Mutex> structureGuard(m_StructureLock);
  if (!parent || !parent->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  if (!filename.length() || filename == "." || filename == "..") {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  Directory* directory = Directory::fromFile(parent);
  LockGuard<Mutex> namespaceGuard(directory->namespaceMutationLock());

  auto publishRemoval = [&](File* target) {
    // Publish the detached state before terminal event delivery. The event
    // source then linearizes subscription closure with its final snapshot.
    target->retainDetachedParent();
    directory->publishEvent(FileEvents::Removed, filename.view(), target->isDirectory());
    // TODO: FileEventSource currently follows a VFS namespace node. Linux
    // retires an inode watch only after its final link/open lifecycle; open
    // unlink and hard-link aliases need a shared inode-identity event source.
    target->publishEvent(FileEvents::DeletedSelf);
  };

  Directory::ChildLease target;
  const Directory::LookupStatus lookup = directory->lookupChild(HashedStringView(filename), target);
  if (lookup != Directory::LookupStatus::Found) {
    if (lookup == Directory::LookupStatus::IoError) {
      SYSCALL_ERROR(IoError);
    } else {
      SYSCALL_ERROR(DoesNotExist);
    }
    return false;
  }
  if (expected && target.get() != expected) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  if (target.get()->isDirectory()) {
    Directory* childDirectory = Directory::fromFile(target.get());
    LockGuard<Mutex> childNamespaceGuard(childDirectory->namespaceMutationLock());
    bool empty = false;
    const Directory::ReadStatus status = childDirectory->isEmpty(empty);
    if (status != Directory::ReadStatus::Complete) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    if (!empty) {
      SYSCALL_ERROR(NotEmpty);
      return false;
    }

    // Ephemeral directories never reach a filesystem driver, so the VFS
    // must complete their detachment while both namespace boundaries are
    // held. Backing drivers recheck emptiness after acquiring this lock.
    if (directory->removeEphemeralFileLocked(HashedStringView(filename), target.get())) {
      childDirectory->markDetached();
      publishRemoval(target.get());
      return true;
    }
  }

  // Ephemeral overlays belong to the VFS namespace, not to the backing
  // filesystem whose directory they appear in. Classification and removal
  // share the same namespace critical section as backing removal.
  if (directory->removeEphemeralFileLocked(HashedStringView(filename), target.get())) {
    publishRemoval(target.get());
    return true;
  }

  if (!retirement.retain(target.get()) || !removeNode(parent, filename, target.get())) {
    return false;
  }

  publishRemoval(target.get());
  return true;
}

File* Filesystem::findNode(File* pNode, StringView path) {
  TrueRootLease rootLease(this);
  File* trueRoot = rootLease.get();
  if (!pNode) {
    pNode = trueRoot;
  }
  return findNode(pNode, path, pNode, trueRoot, nullptr);
}

File* Filesystem::findNode(File* pNode, StringView path, File* stableStart, File* trueRoot,
                           File** retainedResult) {
  if (UNLIKELY(path.length() == 0)) {
    if (retainedResult && !*retainedResult) {
      if (VFS::instance().retainTrackedFile(pNode)) {
        *retainedResult = pNode;
      } else if (pNode != stableStart && pNode != trueRoot) {
        return nullptr;
      }
    }
    return pNode;
  }

  // If the pathname has a leading slash, cd to root and remove it.
  else if (path[0] == '/') {
    pNode = trueRoot;
    path = path.substring(1, path.length());
  }

  // Grab the next filename component.
  size_t i = 0;
  size_t nExtra = 0;
  while ((i < path.length()) && path[i] != '/') {
    i = path.nextCharacter(i);
  }
  while (i < path.length()) {
    size_t n = path.nextCharacter(i);
    if (n >= path.length()) {
      break;
    } else if (path[n] == '/') {
      i = n;
      ++nExtra;
    } else {
      break;
    }
  }

  StringView currentComponent = path.substring(0, i - nExtra);
  StringView restOfPath = path.substring(path.nextCharacter(i), path.length());

  // At this point 'currentComponent' contains the token to search for.
  // 'restOfPath' contains the path for the next recursion (or nil).

  // If 'path' is zero-lengthed, ignore and recurse.
  if (currentComponent.length() == 0) {
    return findNode(pNode, restOfPath, stableStart, trueRoot, retainedResult);
  }

  // Firstly, if the current node is a symlink, follow it.
  /// \todo do we need to do permissions checks at each intermediate step?
  Directory::ChildLease followedLease;
  while (pNode->isSymlink()) {
    Directory::ChildLease nextLease;
    pNode = Symlink::fromFile(pNode)->followLinkRetained(nextLease);
    if (!pNode) {
      return nullptr;
    }
    followedLease.swap(nextLease);
  }

  // Next, if the current node isn't a directory, die.
  if (!pNode->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return 0;
  }

  bool dot = currentComponent == ".";
  bool dotdot = currentComponent == "..";
  File::ParentLease parentLease;
  String unusedName;
  if (dotdot) {
    pNode->getNamespace(parentLease, unusedName);
  }
  File* parent = parentLease.get();

  // '.' section, or '..' with no parent, or '..' and we're at the root.
  if (dot || (dotdot && !parent) || (dotdot && pNode == trueRoot)) {
    return findNode(pNode, restOfPath, stableStart, trueRoot, retainedResult);
  } else if (dotdot) {
    return findNode(parent, restOfPath, stableStart, trueRoot, retainedResult);
  }

  Directory* pDir = Directory::fromFile(pNode);
  if (!pDir) {
    SYSCALL_ERROR(NotADirectory);
    return 0;
  }

  // Is this a reparse point? If so we need to change where we perform the
  // next lookup.
  Directory* reparse = pDir->getReparsePoint();
  if (reparse) {
    String fullPath, reparseFullPath;
    pDir->getFullPath(fullPath);
    pDir->getFullPath(reparseFullPath);
    WARNING("VFS: found reparse point at '"
            << fullPath << "', following it (new target: " << reparseFullPath << ")");
    pDir = reparse;
  }

  // Are we allowed to access files in this directory?
  if (!VFS::checkAccess(pNode, false, false, true)) {
    return 0;
  }

  Directory::ChildLease child;
  Directory::LookupStatus lookup = pDir->lookupChild(HashedStringView(currentComponent), child);
  if (lookup == Directory::LookupStatus::Found) {
    return findNode(child.get(), restOfPath, stableStart, trueRoot, retainedResult);
  }
  if (lookup == Directory::LookupStatus::IoError) {
    SYSCALL_ERROR(IoError);
  }
  return nullptr;
}

File* Filesystem::findParent(StringView path, File* pStartNode, String& filename,
                             File** retainedParent) {
  if (retainedParent) {
    *retainedParent = nullptr;
  }
  TrueRootLease rootLease(this);
  File* trueRoot = rootLease.get();

  // If the final character of the string is '/', this log falls apart. So,
  // check for that and chomp it. But, we also need to not do that for e.g.
  // path == '/'.
  if (path.length() > 1 && path[path.length() - 1] == '/') {
    path = path.substring(0, path.length() - 1);
  }

  // Work forwards to the end of the path string, attempting to find the last
  // '/'.
  ssize_t lastSlash = -1;
  for (ssize_t i = path.length() - 1; i >= 0; i = path.prevCharacter(i)) {
    if (path[i] == '/') {
      lastSlash = i;
      break;
    }
  }

  // Now, if there were no slashes, the parent node is pStartNode.
  File* parentNode = nullptr;
  if (lastSlash == -1) {
    filename = path.toString();
    parentNode = pStartNode;
    if (retainedParent && VFS::instance().retainTrackedFile(parentNode)) {
      *retainedParent = parentNode;
    }
  } else {
    // Else split the filename off from the rest of the path and follow it.
    filename = path.substring(path.nextCharacter(lastSlash), path.length()).toString();
    path = path.substring(0, lastSlash);
    if (lastSlash == 0) {
      parentNode = trueRoot;
      if (retainedParent && VFS::instance().retainTrackedFile(parentNode)) {
        *retainedParent = parentNode;
      }
    } else {
      parentNode = findNode(pStartNode, path, pStartNode, trueRoot, retainedParent);
    }
  }

  // Handle immediate parent node being a reparse point.
  if (parentNode) {
    if (parentNode->isDirectory()) {
      File* reparseNode = Directory::fromFile(parentNode)->getReparsePoint();
      if (reparseNode) {
        if (retainedParent && *retainedParent) {
          VFS::instance().untrackFile(*retainedParent);
          *retainedParent = nullptr;
        }
        if (retainedParent && VFS::instance().retainTrackedFile(reparseNode)) {
          *retainedParent = reparseNode;
        }
        parentNode = reparseNode;
      }
    }
  }

  return parentNode;
}

bool Filesystem::createLink(File* parent, const String& filename, File* target) {
  // Default stubbed implementation, works for filesystems that can't handle
  // hard links.
  return false;
}
