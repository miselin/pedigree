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

Filesystem::~Filesystem() = default;

namespace {
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

bool Filesystem::removeChild(File* parent, const String& filename, File* expected) {
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
      target.get()->retainDetachedParent();
      return true;
    }
  }

  // Ephemeral overlays belong to the VFS namespace, not to the backing
  // filesystem whose directory they appear in. Classification and removal
  // share the same namespace critical section as backing removal.
  if (directory->removeEphemeralFileLocked(HashedStringView(filename), target.get())) {
    target.get()->retainDetachedParent();
    return true;
  }

  if (!removeNode(parent, filename, target.get())) {
    return false;
  }

  target.get()->retainDetachedParent();
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
  File* parent = pNode->getParent();

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
