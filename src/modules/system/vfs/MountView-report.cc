/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/syscallError.h"

#include "MountView-internal.h"

bool VfsMountView::State::format(const FilesystemContextSnapshot& context,
                                 const FilesystemPathRef& reference, String& result,
                                 const VFS::NamespaceMutation& writer) {
  if (!writer.protects(view.m_Vfs) || !path(reference) || !path(context.root)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  String suffix;
  FilesystemPathRef current = reference;
  for (size_t depth = 0; depth < 4096; ++depth) {
    if (view.samePath(current, context.root)) {
      result = suffix.length() ? pedigree_std::move(suffix) : String("/");
      return true;
    }
    auto* entry = path(current);
    if (entry->node() == entry->attachment->root) {
      VfsAttachmentRef parent;
      SharedPointer<VfsNodeReference> covered;
      {
        LockGuard<Mutex> guard(graph);
        auto* row = find(entry->attachment->id);
        if (row) {
          parent = row->parent;
          covered = row->covered;
        }
      }
      if (!parent || !covered) {
        SYSCALL_ERROR(DoesNotExist);
        return false;
      }
      FilesystemPathRef mountpoint;
      if (!makePath(parent, covered->get(), mountpoint))
        return false;
      current = pedigree_std::move(mountpoint);
      continue;
    }
    File::ParentLease parent;
    String name;
    entry->node()->getNamespace(parent, name);
    if (!parent.get() || !name.length()) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    String component("/");
    component += name;
    component += suffix;
    if (component.length() >= 4096) {
      SYSCALL_ERROR(NameTooLong);
      return false;
    }
    suffix = pedigree_std::move(component);
    FilesystemPathRef next;
    if (!makePath(entry->attachment, parent.get(), next))
      return false;
    current = pedigree_std::move(next);
  }
  SYSCALL_ERROR(LoopExists);
  return false;
}

bool VfsMountView::formatPath(const FilesystemContextSnapshot& context,
                              const FilesystemPathRef& path, String& result) {
  if (!m_State)
    return false;
  VFS::NamespaceMutation writer(m_Vfs);
  {
    LockGuard<Mutex> guard(m_State->graph);
    if (context.topologyGeneration != m_State->topology) {
      SYSCALL_ERROR(NoMoreProcesses);
      return false;
    }
  }
  return m_State->format(context, path, result, writer);
}

bool VfsMountView::snapshotMounts(const FilesystemContextRef& context,
                                  Vector<MountSnapshot>& result) {
  if (!m_State || !context)
    return false;
  struct RetainedRow {
    VfsAttachmentRef attachment;
    uint64_t parentId = 0;
  };
  Vector<RetainedRow> rows;
  Vector<MountSnapshot> snapshots;
  FilesystemContextSnapshot fs;
  VFS::NamespaceMutation writer(m_Vfs);
  if (!context->snapshot(fs))
    return false;
  size_t count = 0;
  {
    LockGuard<Mutex> guard(m_State->graph);
    for (auto* row = m_State->attachments; row; row = row->next)
      ++count;
  }
  if (!rows.tryReserve(count) || !snapshots.tryReserve(count)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  {
    LockGuard<Mutex> guard(m_State->graph);
    for (auto* row = m_State->attachments; row; row = row->next) {
      RetainedRow copy;
      copy.attachment = row->attachment;
      copy.parentId = row->parent ? row->parent->id : row->attachment->id;
      rows.pushBack(pedigree_std::move(copy));
    }
  }
  for (auto& row : rows) {
    FilesystemPathRef root;
    MountSnapshot snapshot;
    if (!m_State->makePath(row.attachment, row.attachment->root, root))
      return false;
    if (m_State->path(fs.root)->attachment.get() == row.attachment.get()) {
      snapshot.path = String("/");
    } else {
      if (!m_State->beneath(root, fs.root))
        continue;
      if (!m_State->format(fs, root, snapshot.path, writer))
        return false;
    }
    snapshot.id = row.attachment->id;
    snapshot.parentId = row.parentId;
    snapshot.backing = row.attachment->backing.identity();
    snapshots.pushBack(pedigree_std::move(snapshot));
  }
  result.swap(snapshots);
  return true;
}
