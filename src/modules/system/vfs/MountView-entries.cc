/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/syscallError.h"

#include "MountView-internal.h"

namespace {
bool validParent(const FilesystemPathRef& parent, const String& name, const VfsMountView* view) {
  if (!parent || parent->provider() != view || !parent->node()->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  if (!name.length() || name == "." || name == "..") {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  for (size_t i = 0; i < name.length(); ++i) {
    if (name[i] == '/') {
      SYSCALL_ERROR(InvalidArgument);
      return false;
    }
  }
  if (Directory::fromFile(parent->node())->isDetached()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  if (parent->node()->getFilesystem()->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  return VFS::checkAccess(parent->node(), false, true, true);
}
bool absent(const FilesystemPathRef& parent, const String& name) {
  Directory::ChildLease existing;
  auto status = Directory::fromFile(parent->node())->lookupChild(HashedStringView(name), existing);
  if (status == Directory::LookupStatus::NotFound)
    return true;
  syscallError(status == Directory::LookupStatus::Found   ? Error::FileExists
               : status == Directory::LookupStatus::Retry ? Error::NoMoreProcesses
                                                          : Error::IoError);
  return false;
}
}  // namespace

bool VfsMountView::isMountpoint(File* node) const {
  if (!m_State || !node)
    return false;
  LockGuard<Mutex> guard(m_State->graph);
  for (auto* row = m_State->attachments; row; row = row->next)
    if (row->covered && row->covered->get() == node)
      return true;
  return false;
}

bool VfsMountView::createFile(const FilesystemPathRef& parent, const String& name, uint32_t mask) {
  return validParent(parent, name, this) && absent(parent, name) &&
         parent->node()->getFilesystem()->createFile(parent->node(), name, mask);
}
bool VfsMountView::createDirectory(const FilesystemPathRef& parent, const String& name,
                                   uint32_t mask) {
  return validParent(parent, name, this) && absent(parent, name) &&
         parent->node()->getFilesystem()->createDirectory(parent->node(), name, mask);
}
bool VfsMountView::createSymlink(const FilesystemPathRef& parent, const String& name,
                                 const String& value) {
  return validParent(parent, name, this) && absent(parent, name) &&
         parent->node()->getFilesystem()->createSymlink(parent->node(), name, value);
}
bool VfsMountView::createLink(const FilesystemPathRef& parent, const String& name,
                              const FilesystemPathRef& target) {
  if (!validParent(parent, name, this))
    return false;
  auto* source = m_State->path(target);
  auto* destination = m_State->path(parent);
  if (!source || source->attachment.get() != destination->attachment.get()) {
    SYSCALL_ERROR(CrossDeviceLink);
    return false;
  }
  // Directory ancestry must remain a tree for both path walking and pivot.
  if (target->node()->isDirectory()) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  if (target->node()->isPipe() || target->node()->isFifo() || target->node()->isSocket()) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  return absent(parent, name) &&
         parent->node()->getFilesystem()->createLink(parent->node(), name, target->node());
}
bool VfsMountView::remove(const FilesystemPathRef& parent, const String& name, File* expected) {
  return validParent(parent, name, this) &&
         parent->node()->getFilesystem()->removeChild(parent->node(), name, expected);
}
bool VfsMountView::rename(const FilesystemPathRef& oldParent, const String& oldName,
                          const FilesystemPathRef& newParent, const String& newName, bool noReplace,
                          bool sourceMustBeDirectory) {
  if (!validParent(oldParent, oldName, this) || !validParent(newParent, newName, this))
    return false;
  if (m_State->path(oldParent)->attachment.get() != m_State->path(newParent)->attachment.get()) {
    SYSCALL_ERROR(CrossDeviceLink);
    return false;
  }
  return oldParent->node()->getFilesystem()->renameChildren(
      oldParent->node(), oldName, newParent->node(), newName, noReplace, sourceMustBeDirectory);
}
