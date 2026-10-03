/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/FilesystemAccess.h"
#include "pedigree/kernel/syscallError.h"

#include "MountView-internal.h"
#ifndef VFS_STANDALONE
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

namespace {
#ifndef VFS_STANDALONE
Subsystem* currentPolicy() {
  auto* thread = Processor::information().getCurrentThread();
  auto* process = thread ? thread->getParent() : nullptr;
  auto* subsystem = process ? process->getSubsystem() : nullptr;
  return subsystem && subsystem->filesystemConstrained() ? subsystem : nullptr;
}
#endif
uint64_t creationAccess(File* node) {
  if (node->isDirectory()) {
    return FilesystemAccess::MakeDir;
  }
  if (node->isSymlink()) {
    return FilesystemAccess::MakeSym;
  }
  if (node->isSocket()) {
    return FilesystemAccess::MakeSock;
  }
  if (node->isPipe() || node->isFifo()) {
    return FilesystemAccess::MakeFifo;
  }
  if (node->isBlockDevice()) {
    return FilesystemAccess::MakeBlock;
  }
  if (!node->supportsRegularFileOperations()) {
    return FilesystemAccess::MakeChar;
  }
  return FilesystemAccess::MakeReg;
}
}  // namespace

bool VfsMountView::State::ancestors(const FilesystemPathRef& selected,
                                    Vector<FilesystemPathRef>& result) {
  FilesystemPathRef current = selected;
  for (size_t depth = 0; depth < 4096; ++depth) {
    auto* item = path(current);
    if (!item || !result.tryReserve(result.count() + 1)) {
      return false;
    }
    result.pushBack(current);
    if (!item->node()->isDirectory() && item->lookupParent && item->lookupName.length()) {
      Directory::ChildLease selectedName;
      const auto found = Directory::fromFile(item->lookupParent->node())
                             ->lookupChild(HashedStringView(item->lookupName), selectedName);
      // A retained open path can outlive its name. Its direct inode rules still
      // apply, but its previous directory must not grant a new open after rename.
      if (found != Directory::LookupStatus::Found || selectedName.get() != item->node()) {
        return true;
      }
    }
    FilesystemPathRef next;
    if (item->node() == item->attachment->root) {
      VfsAttachmentRef parentAttachment;
      SharedPointer<VfsNodeReference> covered;
      {
        LockGuard<Mutex> guard(graph);
        auto* row = find(item->attachment->id);
        if (row) {
          parentAttachment = row->parent;
          covered = row->covered;
        }
      }
      if (!parentAttachment) {
        return true;
      }
      if (!covered || !makePath(parentAttachment, covered->get(), next)) {
        return false;
      }
    } else if (!parent(current, next)) {
      return false;
    }
    if (view.samePath(current, next)) {
      return true;
    }
    current = pedigree_std::move(next);
  }
  return false;
}

uint64_t VfsMountView::filesystemAccess(const FilesystemPathRef& path,
                                        const VFS::NamespaceMutation* writer) {
  auto* owner = fromPath(path);
  if (owner && owner != this) {
    return owner->filesystemAccess(path, writer);
  }
#ifndef VFS_STANDALONE
  auto* policy = currentPolicy();
  if (!policy) {
    return ~uint64_t(0);
  }
  if (!writer) {
    VFS::NamespaceMutation admission(m_Vfs);
    return filesystemAccess(path, &admission);
  }
  if (!writer->protects(m_Vfs) || !m_State || !m_State->nodePath(path)) {
    return 0;
  }
  // The subsystem distinguishes internal objects from missing path provenance.
  if (!m_State->path(path)) {
    return policy->filesystemAccess(&path, 1);
  }
  Vector<FilesystemPathRef> ancestry;
  if (!m_State->ancestors(path, ancestry)) {
    return 0;
  }
  return policy->filesystemAccess(ancestry.begin(), ancestry.count());
#else
  return ~uint64_t(0);
#endif
}

bool VfsMountView::checkFilesystemAccess(const FilesystemPathRef& path, uint64_t access,
                                         const VFS::NamespaceMutation* writer) {
  auto* owner = fromPath(path);
  if (owner && owner != this) {
    return owner->checkFilesystemAccess(path, access, writer);
  }
  constexpr uint64_t mutations =
      FilesystemAccess::WriteFile | FilesystemAccess::RemoveDir | FilesystemAccess::RemoveFile |
      FilesystemAccess::MakeChar | FilesystemAccess::MakeDir | FilesystemAccess::MakeReg |
      FilesystemAccess::MakeSock | FilesystemAccess::MakeFifo | FilesystemAccess::MakeBlock |
      FilesystemAccess::MakeSym | FilesystemAccess::Truncate;
  if ((access & mutations) && !writable(path)) {
    return false;
  }
  if ((access & FilesystemAccess::Execute) && (mountFlags(path) & NoExec)) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  if ((filesystemAccess(path, writer) & access) == access) {
    return true;
  }
  SYSCALL_ERROR(PermissionDenied);
  return false;
}

bool VfsMountView::authorizeRemove(const FilesystemPathRef& parent, File* node,
                                   const VFS::NamespaceMutation& writer) {
  return checkFilesystemAccess(
      parent, node->isDirectory() ? FilesystemAccess::RemoveDir : FilesystemAccess::RemoveFile,
      &writer);
}

bool VfsMountView::authorizeLink(const FilesystemPathRef& parent, const FilesystemPathRef& target,
                                 const VFS::NamespaceMutation& writer) {
#ifndef VFS_STANDALONE
  if (currentPolicy()) {
    auto* source = m_State->path(target);
    Directory::ChildLease selected;
    if (!source || !source->lookupParent || !source->lookupName.length() ||
        Directory::fromFile(source->lookupParent->node())
                ->lookupChild(HashedStringView(source->lookupName), selected) !=
            Directory::LookupStatus::Found ||
        selected.get() != source->node()) {
      SYSCALL_ERROR(PermissionDenied);
      return false;
    }
  }
#endif
  FilesystemPathRef oldParent;
  return m_State->parent(target, oldParent) &&
         authorizeRename(oldParent, target->node(), parent, nullptr, writer, false);
}

bool VfsMountView::authorizeRename(const FilesystemPathRef& oldParent, File* source,
                                   const FilesystemPathRef& newParent, File* replaced,
                                   const VFS::NamespaceMutation& writer, bool removeSource) {
  if ((removeSource && !authorizeRemove(oldParent, source, writer)) ||
      !checkFilesystemAccess(newParent, creationAccess(source), &writer) ||
      (replaced && !authorizeRemove(newParent, replaced, writer))) {
    return false;
  }
#ifndef VFS_STANDALONE
  auto* policy = currentPolicy();
  if (!policy || samePath(oldParent, newParent)) {
    return true;
  }
  FilesystemPathRef selected;
  if (!m_State->makePath(m_State->path(oldParent)->attachment, source, selected)) {
    return false;
  }
  static_cast<VfsPath*>(selected.get())->lookupParent = oldParent;
  Vector<FilesystemPathRef> from, to;
  if (!m_State->ancestors(selected, from) || !to.tryReserve(1)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  to.pushBack(selected);
  if (!m_State->ancestors(newParent, to)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (!checkFilesystemAccess(oldParent, FilesystemAccess::Refer, &writer) ||
      !checkFilesystemAccess(newParent, FilesystemAccess::Refer, &writer) ||
      !policy->filesystemReparent(from.begin(), from.count(), to.begin(), to.count())) {
    SYSCALL_ERROR(CrossDeviceLink);
    return false;
  }
#endif
  return true;
}
