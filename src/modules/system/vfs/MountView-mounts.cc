/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/syscallError.h"

#include "MountView-internal.h"

bool VfsMountView::State::attach(const FilesystemPathRef& covered, VFS::FilesystemPin&& pin,
                                 const VFS::NamespaceMutation& writer, BackingOwnership ownership) {
  auto* point = path(covered);
  if (!writer.protects(view.m_Vfs) || !point || !point->node()->isDirectory() || !pin) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (point->node() == point->attachment->root) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  if (Directory::fromFile(point->node())->isDetached()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  uint64_t id;
  {
    LockGuard<Mutex> guard(graph);
    if (!contains(covered) || at(*point)) {
      SYSCALL_ERROR(DeviceBusy);
      return false;
    }
    if (nextId > 0x7fffffffU) {
      SYSCALL_ERROR(OutOfMemory);
      return false;
    }
    id = nextId++;
  }
  auto attachment = VfsAttachmentRef::tryAdopt(new VfsAttachment(pedigree_std::move(pin), id));
  auto node = SharedPointer<VfsNodeReference>::tryAdopt(new VfsNodeReference);
  auto row = UniquePointer<VfsAttachmentRow>::adopt(new VfsAttachmentRow);
  if (!attachment || !node || !row) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (!node->retain(point->node(), point->attachment->backing.filesystem())) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  row.get()->attachment = attachment;
  row.get()->parent = point->attachment;
  row.get()->covered = node;
  {
    LockGuard<Mutex> guard(graph);
    attachment->owningRegistry = ownership == BackingOwnership::Attachment ? &view.m_Vfs : nullptr;
    row.get()->next = attachments;
    attachments = row.releaseOwnership();
    ++topology;
  }
  return true;
}

bool VfsMountView::attach(const FilesystemContextRef& context, const FilesystemPathRef& covered,
                          Filesystem* backing, BackingOwnership ownership) {
  VFS::FilesystemPin pin;
  if (!m_State || !m_Vfs.pinFilesystem(backing, pin)) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  FilesystemContextSnapshot snapshot;
  VFS::NamespaceMutation writer(m_Vfs);
  if (!context || !context->snapshot(snapshot) || !m_State->beneath(covered, snapshot.root)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  return m_State->attach(covered, pedigree_std::move(pin), writer, ownership);
}

void VfsMountView::State::reapDetached() {
#if THREADS && !defined(STANDALONE_MUTEXES)
  TerminationDeferral lifetime;
#endif
  VfsAttachmentRow* retired = nullptr;
  {
    LockGuard<Mutex> guard(graph);
    // Graph rows own covered-node references, not user paths. A disconnected
    // tree may drain only when none of its attachments has a retained path.
    for (;;) {
      VfsAttachmentRow* detached = nullptr;
      for (auto* candidate = attachments; candidate; candidate = candidate->next) {
        if (candidate->parent || candidate->attachment->id == rootId)
          continue;
        bool used = false;
        for (auto* row = attachments; row && !used; row = row->next) {
          auto* ancestor = row;
          while (ancestor && ancestor != candidate)
            ancestor = ancestor->parent ? find(ancestor->parent->id) : nullptr;
          if (ancestor && row->attachment->paths)
            used = true;
        }
        if (!used) {
          detached = candidate;
          break;
        }
      }
      if (!detached)
        break;
      // Remove leaves first, keeping parent rows discoverable for the next
      // ancestry check. Actual node/backing destruction happens after unlock.
      while (true) {
        VfsAttachmentRow** selected = nullptr;
        for (auto** link = &attachments; *link; link = &(*link)->next) {
          auto* row = *link;
          auto* ancestor = row;
          while (ancestor && ancestor != detached)
            ancestor = ancestor->parent ? find(ancestor->parent->id) : nullptr;
          if (!ancestor)
            continue;
          bool child = false;
          for (auto* check = attachments; check; check = check->next)
            if (check->parent.get() == row->attachment.get())
              child = true;
          if (!child) {
            selected = link;
            break;
          }
        }
        if (!selected)
          FATAL("Cyclic detached attachment graph");
        auto* row = *selected;
        const bool last = row == detached;
        *selected = row->next;
        row->next = retired;
        retired = row;
        if (last)
          break;
      }
      ++topology;
    }
  }
  while (retired) {
    auto* next = retired->next;
    delete retired;
    retired = next;
  }
}

bool VfsMountView::detach(const FilesystemContextRef& context, const String& target, bool lazy) {
  if (!m_State || !context) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  VfsAttachmentRef retiredParent;
  SharedPointer<VfsNodeReference> retiredCovered;
  FilesystemPathRef mounted;
  FilesystemContextSnapshot snapshot;
  VFS::NamespaceMutation writer(m_Vfs);
  ResolveOptions options;
  options.requireDirectory = true;
  if (!context->snapshot(snapshot) ||
      !m_State->resolve(snapshot, snapshot.cwd, target, options, mounted, &writer))
    return false;
  auto* path = m_State->path(mounted);
  {
    LockGuard<Mutex> guard(m_State->graph);
    auto* row = path ? m_State->find(path->attachment->id) : nullptr;
    if (!row || !m_State->contains(mounted) || path->node() != path->attachment->root) {
      SYSCALL_ERROR(InvalidArgument);
      return false;
    }
    if (row->attachment->id == m_State->rootId) {
      SYSCALL_ERROR(DeviceBusy);
      return false;
    }
    if (!lazy) {
      // The lookup above owns exactly one newly materialized mount-root path.
      if (row->attachment->paths != 1 || mounted.refcount() != 1) {
        SYSCALL_ERROR(DeviceBusy);
        return false;
      }
      for (auto* child = m_State->attachments; child; child = child->next) {
        if (child->parent.get() == row->attachment.get()) {
          SYSCALL_ERROR(DeviceBusy);
          return false;
        }
      }
    }
    retiredParent = pedigree_std::move(row->parent);
    retiredCovered = pedigree_std::move(row->covered);
    ++m_State->topology;
  }
  return true;
}

bool VfsMountView::pivot(const FilesystemContextRef& context, const String& newRoot,
                         const String& putOld) {
  if (!m_State || !context) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  // All ownership which may retire backend state precedes the writer guard.
  FilesystemContextSnapshot snapshot;
  FilesystemPathRef newPath, oldPath;
  VfsAttachmentRef retiredNewParent, retiredOldParent;
  SharedPointer<VfsNodeReference> retiredNewCovered, retiredOldCovered;
  SharedPointer<VfsNodeReference> putOldNode;
  Vector<FilesystemPathRef> retiredPaths;
  VFS::NamespaceMutation writer(m_Vfs);
  ResolveOptions options;
  options.requireDirectory = true;
  if (!context->snapshot(snapshot) ||
      !m_State->resolve(snapshot, snapshot.cwd, newRoot, options, newPath, &writer) ||
      !m_State->resolve(snapshot, snapshot.cwd, putOld, options, oldPath, &writer))
    return false;
  auto* callerRoot = m_State->path(snapshot.root);
  auto* nextRoot = m_State->path(newPath);
  auto* oldMountpoint = m_State->path(oldPath);
  if (!callerRoot || !nextRoot || !oldMountpoint) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (samePath(newPath, oldPath)) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  if (callerRoot->node() != callerRoot->attachment->root ||
      nextRoot->node() != nextRoot->attachment->root ||
      callerRoot->attachment.get() == nextRoot->attachment.get() ||
      oldMountpoint->attachment.get() == callerRoot->attachment.get() ||
      !m_State->beneath(newPath, snapshot.root) || !m_State->beneath(oldPath, newPath)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (Directory::fromFile(oldMountpoint->node())->isDetached()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  if (oldMountpoint->node() == oldMountpoint->attachment->root) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  size_t count;
  {
    LockGuard<Mutex> guard(m_State->graph);
    count = m_State->contextCount;
    auto* previous = m_State->find(callerRoot->attachment->id);
    auto* next = m_State->find(nextRoot->attachment->id);
    if (!previous || !next || !m_State->contains(newPath) || !m_State->contains(oldPath) ||
        m_State->at(*oldMountpoint)) {
      SYSCALL_ERROR(DeviceBusy);
      return false;
    }
  }
  if (count > ~size_t(0) / 2 || !retiredPaths.tryReserve(count * 2)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  putOldNode = SharedPointer<VfsNodeReference>::tryAdopt(new VfsNodeReference);
  if (!putOldNode) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (!putOldNode->retain(oldMountpoint->node(), oldMountpoint->attachment->backing.filesystem())) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  {
    LockGuard<Mutex> guard(m_State->graph);
    auto* previous = m_State->find(callerRoot->attachment->id);
    auto* next = m_State->find(nextRoot->attachment->id);
    // Writer admission freezes graph edges, enrollment and backend ancestry.
    // Every operation below is an existing reference increment or move.
    retiredNewParent = pedigree_std::move(next->parent);
    retiredNewCovered = pedigree_std::move(next->covered);
    retiredOldParent = pedigree_std::move(previous->parent);
    retiredOldCovered = pedigree_std::move(previous->covered);
    next->parent = retiredOldParent;
    next->covered = retiredOldCovered;
    previous->parent = oldMountpoint->attachment;
    previous->covered = putOldNode;
    if (m_State->rootId == previous->attachment->id)
      m_State->rootId = next->attachment->id;
    for (auto* row = m_State->contexts; row; row = row->next) {
      auto* fs = static_cast<VfsFilesystemContext*>(row->context.get());
      bool changed = false;
      if (samePath(fs->root, snapshot.root)) {
        retiredPaths.pushBack(pedigree_std::move(fs->root));
        fs->root = newPath;
        changed = true;
      }
      if (samePath(fs->cwd, snapshot.root)) {
        retiredPaths.pushBack(pedigree_std::move(fs->cwd));
        fs->cwd = newPath;
        changed = true;
      }
      if (changed)
        ++fs->generation;
    }
    ++m_State->topology;
  }
  return true;
}

bool VfsMountView::detachBackingForShutdown(Filesystem* backing) {
#if THREADS && !defined(STANDALONE_MUTEXES)
  TerminationDeferral lifetime;
#endif
  if (!m_State || !backing)
    return true;
  VfsAttachmentRow* retired = nullptr;
  {
    VFS::NamespaceMutation writer(m_Vfs);
    LockGuard<Mutex> guard(m_State->graph);
    for (auto* row = m_State->attachments; row; row = row->next) {
      if (row->attachment->backing.filesystem() != backing)
        continue;
      if (row->attachment->id == m_State->rootId || row->attachment->paths) {
        SYSCALL_ERROR(DeviceBusy);
        return false;
      }
      for (auto* child = m_State->attachments; child; child = child->next) {
        if (child->parent.get() == row->attachment.get() &&
            child->attachment->backing.filesystem() != backing) {
          SYSCALL_ERROR(DeviceBusy);
          return false;
        }
      }
    }
    for (auto** link = &m_State->attachments; *link;) {
      auto* row = *link;
      if (row->attachment->backing.filesystem() != backing) {
        link = &row->next;
        continue;
      }
      *link = row->next;
      row->next = retired;
      retired = row;
    }
    ++m_State->topology;
  }
  while (retired) {
    auto* next = retired->next;
    delete retired;
    retired = next;
  }
  return true;
}
