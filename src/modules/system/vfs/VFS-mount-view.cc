/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include "MountView.h"

VFS::NamespaceMutation::NamespaceMutation(VFS& vfs) : m_Vfs(vfs), m_Lock(vfs.m_PathMutationLock) {
  __atomic_add_fetch(&m_Vfs.m_PathGeneration, 1, __ATOMIC_ACQ_REL);
}
VFS::NamespaceMutation::~NamespaceMutation() {
  __atomic_add_fetch(&m_Vfs.m_PathGeneration, 1, __ATOMIC_RELEASE);
}
uint64_t VFS::NamespaceMutation::generation() const {
  return m_Vfs.namespaceGeneration();
}
uint64_t VFS::namespaceGeneration() const {
  return __atomic_load_n(&m_PathGeneration, __ATOMIC_ACQUIRE);
}
VfsMountView* VFS::mountView() const {
  return __atomic_load_n(&m_MountView, __ATOMIC_ACQUIRE);
}
bool VFS::shutdownMountView(Vector<Filesystem*>& ownedBackings) {
  auto* view = mountView();
  if (!view)
    return true;
  if (!view->shutdown(ownedBackings))
    return false;
  __atomic_store_n(&m_MountView, static_cast<VfsMountView*>(nullptr), __ATOMIC_RELEASE);
  delete view;
  return true;
}
bool VFS::initialiseMountView() {
  if (mountView())
    return true;
  auto view = UniquePointer<VfsMountView>::adopt(new VfsMountView(*this));
  if (!view || !view.get()->initialise(getRootFilesystem()))
    return false;
  FilesystemContextOwner bootstrap;
  if (!view.get()->createBootContext(bootstrap))
    return false;
  auto context = bootstrap.reference();
  Vector<MountSnapshot> mounts;
  Vector<FilesystemPathRef> importedPoints;
  getMounts(mounts);
  if (!importedPoints.tryReserve(mounts.count())) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  VfsMountView::ResolveOptions options;
  options.requireDirectory = true;
  options.crossFinalMount = false;
  for (const auto& mount : mounts) {
    if (!mount.path.length() || mount.path == "/")
      continue;
    Filesystem* backing = getFilesystemAt(mount.path);
    FilesystemPathRef point;
    if (!backing ||
        !view.get()->resolve(context, FilesystemPathRef(), mount.path, options, point) ||
        !view.get()->attach(context, point, backing))
      return false;
    importedPoints.pushBack(pedigree_std::move(point));
  }
  bootstrap.reset();
  context.reset();
  {
    NamespaceMutation writer(*this);
    if (m_MountView) {
      SYSCALL_ERROR(DeviceBusy);
      return false;
    }
    // Once imported, mount edges have one authority; stale raw reparses must
    // not survive backing retirement or offer a second lookup namespace.
    for (const auto& point : importedPoints)
      Directory::fromFile(point->node())->setReparsePoint(nullptr);
    __atomic_store_n(&m_MountView, view.releaseOwnership(), __ATOMIC_RELEASE);
  }
  return true;
}
