/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/syscallError.h"

#include "MountView-internal.h"

bool VfsMountView::bind(const FilesystemContextRef& context, const FilesystemPathRef& source,
                        const FilesystemPathRef& target, bool recursive) {
  struct Copy {
    uint64_t original;
    VfsAttachmentRow* row = nullptr;
  };
  struct StagedRows {
    VfsAttachmentRow* first = nullptr;
    ~StagedRows() {
      while (first) {
        auto* next = first->next;
        delete first;
        first = next;
      }
    }
  } staged;
  Vector<Copy> copies;
  FilesystemContextSnapshot snapshot;
  Vector<VfsAttachmentRow> rows;
  VFS::NamespaceMutation writer(m_Vfs);
  auto* sourceView = fromPath(source);
  auto* sourceState = sourceView ? sourceView->m_State : nullptr;
  auto* from = sourceState ? sourceState->path(source) : nullptr;
  auto* to = m_State->path(target);
  if (fromContext(context) != this || !from || !to || !context->snapshot(snapshot) ||
      !m_State->beneath(target, snapshot.root) ||
      from->node()->isDirectory() != to->node()->isDirectory()) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (!sourceState->contains(source) || !m_State->contains(target) || m_State->at(*to)) {
    SYSCALL_ERROR(DeviceBusy);
    return false;
  }
  if (!sourceState->snapshotRows(rows)) {
    return false;
  }
  const size_t capacity = rows.count() + 1;
  if (!copies.tryReserve(capacity)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  Copy primary;
  primary.original = from->attachment->id;
  primary.row = new VfsAttachmentRow;
  if (primary.row) {
    primary.row->next = staged.first;
    staged.first = primary.row;
  }
  auto covered = SharedPointer<VfsNodeReference>::tryAdopt(new VfsNodeReference);
  if (!primary.row || !covered ||
      !covered->retain(to->node(), to->attachment->backing->pin.filesystem()) ||
      !m_State->makeAttachment(from->attachment->backing, from->node(), from->attachment->flags,
                               primary.row->attachment)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  primary.row->parent = to->attachment;
  primary.row->attachment->lockedFlags = from->attachment->lockedFlags;
  if (sourceView->ownerNamespace() != ownerNamespace()) {
    primary.row->attachment->lockedFlags |= from->attachment->flags;
  }
  primary.row->covered = covered;
  copies.pushBack(pedigree_std::move(primary));
  if (recursive && from->node()->isDirectory()) {
    for (auto& item : rows) {
      auto* row = &item;
      if (row->attachment.get() == from->attachment.get()) {
        continue;
      }
      FilesystemPathRef mounted;
      if (!sourceState->makePath(row->attachment, row->attachment->root, mounted)) {
        return false;
      }
      if (!sourceState->contains(mounted) || !sourceState->beneath(mounted, source)) {
        continue;
      }
      Copy copied;
      copied.original = row->attachment->id;
      copied.row = new VfsAttachmentRow;
      if (copied.row) {
        copied.row->next = staged.first;
        staged.first = copied.row;
      }
      if (!copied.row || !m_State->makeAttachment(row->attachment->backing, row->attachment->root,
                                                  row->attachment->flags, copied.row->attachment)) {
        return false;
      }
      copied.row->covered = row->covered;
      copied.row->attachment->lockedFlags = row->attachment->lockedFlags;
      copied.row->attachment->lockedMount = row->attachment->lockedMount;
      if (sourceView->ownerNamespace() != ownerNamespace()) {
        copied.row->attachment->lockedFlags |= row->attachment->flags;
        copied.row->attachment->lockedMount = true;
      }
      copies.pushBack(pedigree_std::move(copied));
    }
    for (size_t i = 1; i < copies.count(); ++i) {
      VfsAttachmentRow* original = nullptr;
      for (auto& row : rows) {
        if (row.attachment->id == copies[i].original) {
          original = &row;
          break;
        }
      }
      for (auto& parent : copies) {
        if (original->parent && parent.original == original->parent->id) {
          copies[i].row->parent = parent.row->attachment;
          break;
        }
      }
      if (!copies[i].row->parent) {
        SYSCALL_ERROR(InvalidArgument);
        return false;
      }
    }
  }
  {
    LockGuard<Mutex> guard(m_State->graph);
    for (auto& copy : copies) {
      copy.row->next = m_State->attachments;
      m_State->attachments = copy.row;
    }
    staged.first = nullptr;
    ++m_State->topology;
  }
  return true;
}

uint64_t VfsMountView::mountFlags(const FilesystemPathRef& path) const {
  auto* selected = m_State ? m_State->path(path) : nullptr;
  return selected ? uint64_t(selected->attachment->flags) : 0;
}

VfsMountView::WriteLease::~WriteLease() {
  if (auto* view = fromPath(m_Path)) {
    auto* path = view->m_State->path(m_Path);
    if (path) {
      path->attachment->writers -= 1;
    }
  }
}

bool VfsMountView::WriteLease::acquire(const FilesystemPathRef& reference) {
  if (m_Path) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  auto* view = fromPath(reference);
  if (!view) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  LockGuard<Mutex> guard(view->m_State->graph);
  if (!view->writable(reference)) {
    return false;
  }
  auto* path = view->m_State->path(reference);
  if (path) {
    path->attachment->writers += 1;
  }
  m_Path = reference;
  return true;
}

bool VfsMountView::retainWrite(const FilesystemPathRef& path,
                               SharedPointer<FilesystemWriteLease>& result) {
  auto lease = SharedPointer<FilesystemWriteLease>::tryAdopt(new WriteLease);
  if (!lease) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (!static_cast<WriteLease*>(lease.get())->acquire(path)) {
    return false;
  }
  result = pedigree_std::move(lease);
  return true;
}

bool VfsMountView::writable(const FilesystemPathRef& path) const {
  if (!m_State || !m_State->nodePath(path)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  auto* filesystem = path->node()->getFilesystem();
  if ((mountFlags(path) & ReadOnly) || (filesystem && filesystem->isReadOnly())) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  return true;
}

bool VfsMountView::remount(const FilesystemContextRef& context, const FilesystemPathRef& target,
                           uint64_t flags, bool recursive) {
  if (flags & ~SupportedMountFlags) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  Vector<VfsAttachmentRef> selected;
  FilesystemContextSnapshot snapshot;
  Vector<VfsAttachmentRow> rows;
  VFS::NamespaceMutation writer(m_Vfs);
  auto* mounted = m_State->path(target);
  if (fromContext(context) != this || !mounted || !context->snapshot(snapshot) ||
      mounted->node() != mounted->attachment->root || !m_State->contains(target) ||
      !m_State->beneath(target, snapshot.root)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (!m_State->snapshotRows(rows)) {
    return false;
  }
  for (auto& item : rows) {
    auto* row = &item;
    bool included = row->attachment.get() == mounted->attachment.get();
    if (recursive && !included) {
      for (auto* parent = row; parent && parent->parent;) {
        if (parent->parent.get() == mounted->attachment.get()) {
          included = true;
          break;
        }
        VfsAttachmentRow* next = nullptr;
        for (auto& candidate : rows) {
          if (candidate.attachment.get() == parent->parent.get()) {
            next = &candidate;
            break;
          }
        }
        parent = next;
      }
    }
    if (included) {
      if (!selected.tryReserve(selected.count() + 1)) {
        SYSCALL_ERROR(OutOfMemory);
        return false;
      }
      selected.pushBack(row->attachment);
    }
  }
  {
    LockGuard<Mutex> guard(m_State->graph);
    for (auto& attachment : selected) {
      if ((flags & attachment->lockedFlags) != attachment->lockedFlags) {
        SYSCALL_ERROR(NotEnoughPermissions);
        return false;
      }
      if ((flags & ReadOnly) && !(attachment->flags & ReadOnly) && attachment->writers) {
        SYSCALL_ERROR(DeviceBusy);
        return false;
      }
    }
    for (auto& attachment : selected) {
      auto* mountedAttachment = attachment.get();
      if (!mountedAttachment) {
        FATAL("Missing prepared mount attachment");
      } else {
        mountedAttachment->flags = flags;
      }
    }
    ++m_State->topology;
  }
  return true;
}

bool VfsMountView::State::attach(const FilesystemPathRef& covered, VFS::FilesystemPin&& pin,
                                 const VFS::NamespaceMutation& writer, BackingOwnership ownership,
                                 uint64_t flags) {
  VFS::MountOperation operation;
  if (!pin.identity().acquire(operation)) {
    SYSCALL_ERROR(DeviceDoesNotExist);
    return false;
  }
  auto* point = path(covered);
  if (!writer.protects(view.m_Vfs) || !point || !point->node()->isDirectory() || !pin) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (Directory::fromFile(point->node())->isDetached()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  {
    LockGuard<Mutex> guard(graph);
    if (!contains(covered) || at(*point)) {
      SYSCALL_ERROR(DeviceBusy);
      return false;
    }
  }
  auto backing = VfsBackingRef::tryAdopt(new VfsBacking(pedigree_std::move(pin)));
  VfsAttachmentRef attachment;
  if (!backing ||
      !makeAttachment(backing, backing->pin.filesystem()->getRoot(), flags, attachment)) {
    return false;
  }
  auto node = SharedPointer<VfsNodeReference>::tryAdopt(new VfsNodeReference);
  auto row = UniquePointer<VfsAttachmentRow>::adopt(new VfsAttachmentRow);
  if (!attachment || !node || !row) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (!node->retain(point->node(), point->attachment->backing->pin.filesystem())) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  row.get()->attachment = attachment;
  row.get()->parent = point->attachment;
  row.get()->covered = node;
  {
    LockGuard<Mutex> guard(graph);
    attachment->backing->owningRegistry =
        ownership == BackingOwnership::Attachment ? &view.m_Vfs : nullptr;
    row.get()->next = attachments;
    attachments = row.releaseOwnership();
    ++topology;
  }
  return true;
}

bool VfsMountView::attach(const FilesystemContextRef& context, const FilesystemPathRef& covered,
                          Filesystem* backing, BackingOwnership ownership, uint64_t flags) {
  if (flags & ~SupportedMountFlags) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  VFS::FilesystemPin pin;
  if (!m_State) {
    ERROR("VfsMountView::attach: no internal state");
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  if (!m_Vfs.pinFilesystem(backing, pin)) {
    ERROR("VfsMountView::attach: failed to pin filesystem");
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  FilesystemContextSnapshot snapshot;
  VFS::NamespaceMutation writer(m_Vfs);
  if (!context) {
    ERROR("VfsMountView::attach: no context");
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (!context->snapshot(snapshot)) {
    ERROR("VfsMountView::attach: failed to snapshot");
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (!m_State->beneath(covered, snapshot.root)) {
    ERROR("VfsMountView::attach: not an ancestor of " << snapshot.root->node()->getFullPath());
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  if (!m_State->attach(covered, pedigree_std::move(pin), writer, ownership, flags)) {
    ERROR("VfsMountView::attach: internal attach failed");
    return false;
  }

  return true;
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
  options.requireDirectory = false;
  if (!context->snapshot(snapshot) ||
      !m_State->resolve(snapshot, snapshot.cwd, target, options, mounted, &writer))
    return false;
  if (!m_State->cross(mounted, mounted)) {
    return false;
  }
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
    if (row->attachment->lockedMount) {
      SYSCALL_ERROR(NotEnoughPermissions);
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
  const bool stacked = samePath(newPath, oldPath);
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
  if (!stacked && oldMountpoint->node() == oldMountpoint->attachment->root) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  size_t count;
  {
    LockGuard<Mutex> guard(m_State->graph);
    count = m_State->contextCount;
    auto* previous = m_State->find(callerRoot->attachment->id);
    auto* next = m_State->find(nextRoot->attachment->id);
    if (next && next->attachment->lockedMount) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return false;
    }
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
  if (!putOldNode->retain(oldMountpoint->node(),
                          oldMountpoint->attachment->backing->pin.filesystem())) {
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
      if (row->attachment->backing->pin.filesystem() != backing)
        continue;
      if (row->attachment->id == m_State->rootId || row->attachment->paths) {
        SYSCALL_ERROR(DeviceBusy);
        return false;
      }
      for (auto* child = m_State->attachments; child; child = child->next) {
        if (child->parent.get() == row->attachment.get() &&
            child->attachment->backing->pin.filesystem() != backing) {
          SYSCALL_ERROR(DeviceBusy);
          return false;
        }
      }
    }
    for (auto** link = &m_State->attachments; *link;) {
      auto* row = *link;
      if (row->attachment->backing->pin.filesystem() != backing) {
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

bool VfsMountView::detachBackingForRemoval(Filesystem* backing) {
  Vector<VfsAttachmentRef> parents;
  Vector<SharedPointer<VfsNodeReference>> covered;
  {
    VFS::NamespaceMutation writer(m_Vfs);
    LockGuard<Mutex> guard(m_State->graph);
    for (auto* row = m_State->attachments; row; row = row->next) {
      if (row->attachment->backing->pin.filesystem() != backing) {
        continue;
      }
      if (row->attachment->id == m_State->rootId) {
        SYSCALL_ERROR(DeviceBusy);
        return false;
      }
      parents.pushBack(pedigree_std::move(row->parent));
      covered.pushBack(pedigree_std::move(row->covered));
      row->attachment->backing->owningRegistry = nullptr;
    }
    ++m_State->topology;
  }
  m_State->reapDetached();
  return true;
}

bool VfsMountView::shutdown(Vector<Filesystem*>& ownedBackings) {
  VfsAttachmentRow* retired;
  {
    VFS::NamespaceMutation writer(m_Vfs);
    LockGuard<Mutex> guard(m_State->graph);
    if (m_State->contexts || m_State->anonymousPaths) {
      SYSCALL_ERROR(DeviceBusy);
      return false;
    }
    size_t ownedCount = 0;
    for (auto* row = m_State->attachments; row; row = row->next) {
      if (row->attachment->paths) {
        SYSCALL_ERROR(DeviceBusy);
        return false;
      }
      size_t localOwners = 0;
      for (auto* other = m_State->attachments; other; other = other->next) {
        if (other->attachment->backing.get() == row->attachment->backing.get()) {
          ++localOwners;
        }
      }
      if (row->attachment->backing.refcount() > localOwners) {
        SYSCALL_ERROR(DeviceBusy);
        return false;
      }
      if (row->attachment->backing->owningRegistry)
        ++ownedCount;
    }
    if (!ownedBackings.tryReserve(ownedBackings.count() + ownedCount)) {
      SYSCALL_ERROR(OutOfMemory);
      return false;
    }
    for (auto* row = m_State->attachments; row; row = row->next) {
      if (row->attachment->backing->owningRegistry) {
        ownedBackings.pushBack(row->attachment->backing->pin.filesystem());
        // The terminal owner must check sync before deleting this backend.
        // Clearing the shared attachment also handles multiple bind rows.
        row->attachment->backing->owningRegistry = nullptr;
      }
    }
    retired = m_State->attachments;
    m_State->attachments = nullptr;
    m_State->rootId = 0;
    ++m_State->topology;
  }
  while (retired) {
    auto* next = retired->next;
    delete retired;
    retired = next;
  }
  return true;
}
