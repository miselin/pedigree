/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include "MountView-internal.h"
#include "Symlink.h"
#if THREADS && !defined(STANDALONE_MUTEXES)
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

namespace {
// Declared before attempt-local owners so their retirement cannot select errno.
class ResolutionAttempt {
 public:
#if THREADS && !defined(STANDALONE_MUTEXES)
  ResolutionAttempt()
      : m_Thread(Processor::information().getCurrentThread()),
        m_Error(m_Thread ? m_Thread->getErrno() : 0) {}
  ~ResolutionAttempt() {
    if (m_Thread)
      m_Thread->setErrno(m_Error);
  }
  void failure() {
    if (m_Thread)
      m_Error = m_Thread->getErrno();
  }

 private:
  TerminationDeferral m_Lifetime;
  Thread* const m_Thread;
  size_t m_Error;
#else
  void failure() {}
#endif
};
}  // namespace

bool VfsMountView::State::cross(const FilesystemPathRef& reference, FilesystemPathRef& result) {
  auto* current = path(reference);
  if (!current)
    return false;
  VfsAttachmentRef target;
  {
    LockGuard<Mutex> guard(graph);
    auto* row = at(*current);
    if (row)
      target = row->attachment;
  }
  if (target)
    return makePath(target, target->root, result);
  result = reference;
  return true;
}

bool VfsMountView::State::parent(const FilesystemPathRef& reference, FilesystemPathRef& result,
                                 const FilesystemPathRef& boundary) {
  auto* current = path(reference);
  if (!current)
    return false;
  if (current->node() == current->attachment->root) {
    VfsAttachmentRef parentAttachment;
    SharedPointer<VfsNodeReference> covered;
    {
      LockGuard<Mutex> guard(graph);
      auto* row = find(current->attachment->id);
      if (row) {
        parentAttachment = row->parent;
        covered = row->covered;
      }
    }
    if (!parentAttachment) {
      // A detached mount's root never escapes through the global boot root.
      result = reference;
      return true;
    }
    FilesystemPathRef mountpoint;
    if (!covered || !makePath(parentAttachment, covered->get(), mountpoint))
      return false;
    if (view.samePath(mountpoint, boundary)) {
      result = pedigree_std::move(mountpoint);
      return true;
    }
    return parent(mountpoint, result, boundary);
  }
  File::ParentLease retained;
  String unused;
  current->node()->getNamespace(retained, unused);
  if (!retained.get()) {
    result = reference;
    return true;
  }
  return makePath(current->attachment, retained.get(), result);
}

bool VfsMountView::State::beneath(const FilesystemPathRef& descendant,
                                  const FilesystemPathRef& ancestor) {
  FilesystemPathRef current = descendant;
  // Directory ancestry is acyclic, but bound corrupted backend parent chains.
  for (size_t depth = 0; depth < 4096; ++depth) {
    if (view.samePath(current, ancestor))
      return true;
    FilesystemPathRef next;
    if (!parent(current, next) || view.samePath(current, next))
      return false;
    current = pedigree_std::move(next);
  }
  return false;
}

bool VfsMountView::State::follow(const FilesystemContextSnapshot& context,
                                 const FilesystemPathRef& selected, const ResolveOptions& options,
                                 FilesystemPathRef& result, size_t& links) {
  // An empty internal pathname means follow this already retained final node.
  return walk(context, selected, String(), options, result, links);
}

bool VfsMountView::State::walk(const FilesystemContextSnapshot& context,
                               const FilesystemPathRef& start, const String& pathname,
                               const ResolveOptions& options, FilesystemPathRef& result,
                               size_t& links) {
  const bool selectedOnly = !pathname.length();
  FilesystemPathRef current = !selectedOnly && pathname[0] == '/' ? context.root : start;
  if (!nodePath(current) || !path(context.root)) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  if (!selectedOnly && !path(current)) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  bool trailingSlash = !selectedOnly && pathname[pathname.length() - 1] == '/';
  bool followCurrent = selectedOnly;
  bool crossCurrent = false;
  StringView pending = pathname.view();
  UniqueArray<char> pendingStorage, linkStorage;
  size_t offset = 0;
  for (;;) {
    if (followCurrent && current->node()->isSymlink()) {
      if (++links > 40) {
        SYSCALL_ERROR(LoopExists);
        return false;
      }
      auto* link = Symlink::fromFile(current->node());
      if (link->isPathLink()) {
        FilesystemPathRef target;
        if (!link->followPath(target))
          return false;
        if (!nodePath(target)) {
          SYSCALL_ERROR(CrossDeviceLink);
          return false;
        }
        current = pedigree_std::move(target);
        // A typed jump keeps its opening attachment, even beneath an overmount.
        crossCurrent = false;
        continue;
      }
      if (!path(current)) {
        SYSCALL_ERROR(LoopExists);
        return false;
      }
      if (!linkStorage) {
        linkStorage = UniqueArray<char>::allocate(4096);
        if (!linkStorage) {
          SYSCALL_ERROR(OutOfMemory);
          return false;
        }
      }
      const int length = link->followLink(linkStorage.get(), 4096);
      if (length < 0)
        return false;
      if (!length) {
        SYSCALL_ERROR(DoesNotExist);
        return false;
      }
      if (length >= 4096) {
        SYSCALL_ERROR(NameTooLong);
        return false;
      }
      FilesystemPathRef relative;
      if (!parent(current, relative))
        return false;
      const size_t targetLength = static_cast<size_t>(length);
      const size_t remaining = pending.length() - offset;
      const size_t separator = remaining ? 1 : 0;
      if (remaining > ~size_t(0) - targetLength - separator) {
        SYSCALL_ERROR(NameTooLong);
        return false;
      }
      const size_t combined = targetLength + separator + remaining;
      auto expanded = UniqueArray<char>::allocate(combined);
      if (!expanded) {
        SYSCALL_ERROR(OutOfMemory);
        return false;
      }
      MemoryCopy(expanded.get(), linkStorage.get(), targetLength);
      if (remaining) {
        expanded.get()[targetLength] = '/';
        MemoryCopy(expanded.get() + targetLength + 1, pending.str() + offset, remaining);
      } else if (linkStorage.get()[targetLength - 1] == '/') {
        trailingSlash = true;
      }
      current = linkStorage.get()[0] == '/' ? context.root : relative;
      // Keep just the pending suffix, rather than a call frame for each link.
      pendingStorage = pedigree_std::move(expanded);
      pending = StringView(pendingStorage.get(), combined);
      offset = 0;
      followCurrent = false;
      crossCurrent = false;
      continue;
    }
    if (crossCurrent && path(current)) {
      FilesystemPathRef crossed;
      if (!cross(current, crossed))
        return false;
      current = pedigree_std::move(crossed);
    }
    crossCurrent = false;
    while (offset < pending.length() && pending[offset] == '/')
      ++offset;
    if (offset == pending.length())
      break;
    const size_t begin = offset;
    while (offset < pending.length() && pending[offset] != '/')
      ++offset;
    StringView component = pending.substring(begin, offset);
    size_t next = offset;
    while (next < pending.length() && pending[next] == '/')
      ++next;
    const bool final = next == pending.length();
    if (!path(current) || !current->node()->isDirectory()) {
      SYSCALL_ERROR(NotADirectory);
      return false;
    }
    if (!VFS::checkAccess(current->node(), false, false, true))
      return false;
    followCurrent = false;
    if (component == ".") {
      offset = next;
      continue;
    }
    if (component == "..") {
      if (!view.samePath(current, context.root)) {
        FilesystemPathRef above;
        if (!parent(current, above, context.root))
          return false;
        current = pedigree_std::move(above);
      }
      offset = next;
      continue;
    }
    Directory::ChildLease child;
    const auto status =
        Directory::fromFile(current->node())->lookupChild(HashedStringView(component), child);
    if (status != Directory::LookupStatus::Found) {
      syscallError(status == Directory::LookupStatus::IoError ? Error::IoError
                   : status == Directory::LookupStatus::Retry ? Error::NoMoreProcesses
                                                              : Error::DoesNotExist);
      return false;
    }
    FilesystemPathRef candidate;
    if (!makePath(path(current)->attachment, child.get(), candidate))
      return false;
    current = pedigree_std::move(candidate);
    offset = next;
    followCurrent = !final || options.followFinal || trailingSlash;
    crossCurrent = !final || options.crossFinalMount;
  }
  if ((options.requireDirectory || trailingSlash) && !current->node()->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  result = pedigree_std::move(current);
  return true;
}

bool VfsMountView::State::resolve(const FilesystemContextSnapshot& context,
                                  const FilesystemPathRef& start, const String& pathname,
                                  const ResolveOptions& options, FilesystemPathRef& result,
                                  const VFS::NamespaceMutation* writer) {
  if (!pathname.length()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  if (writer && !writer->protects(view.m_Vfs)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  for (;;) {
    ResolutionAttempt attempt;
    const uint64_t generation = view.m_Vfs.namespaceGeneration();
    if (!writer && (generation & 1)) {
      // Wait for admission instead of spinning while a writer performs I/O.
      VFS::NamespaceMutation completed(view.m_Vfs);
      continue;
    }
    FilesystemPathRef found;
    size_t links = 0;
    const bool success = walk(context, start, pathname, options, found, links);
    if (generation != view.m_Vfs.namespaceGeneration())
      continue;
    if (!success) {
      attempt.failure();
      return false;
    }
    result = pedigree_std::move(found);
    return true;
  }
}

bool VfsMountView::resolve(const FilesystemContextRef& context, const FilesystemPathRef& start,
                           const String& pathname, const ResolveOptions& options,
                           FilesystemPathRef& result) {
  if (!m_State || !context) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  for (;;) {
    ResolutionAttempt attempt;
    FilesystemContextSnapshot snapshot;
    if (!context->snapshot(snapshot)) {
      SYSCALL_ERROR(DoesNotExist);
      attempt.failure();
      return false;
    }
    FilesystemPathRef found;
    const bool success =
        m_State->resolve(snapshot, start ? start : snapshot.cwd, pathname, options, found);
    bool coherent;
    {
      LockGuard<Mutex> guard(m_State->graph);
      VfsFilesystemContext* live = nullptr;
      coherent = m_State->context(context, live) &&
                 live->generation == snapshot.contextGeneration &&
                 m_State->topology == snapshot.topologyGeneration;
    }
    if (!coherent)
      continue;
    if (!success) {
      attempt.failure();
      return false;
    }
    result = pedigree_std::move(found);
    return true;
  }
}

bool VfsMountView::follow(const FilesystemContextRef& context, const FilesystemPathRef& selected,
                          FilesystemPathRef& result) {
  if (!m_State || !context || !m_State->nodePath(selected)) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  for (;;) {
    ResolutionAttempt attempt;
    const uint64_t generation = m_Vfs.namespaceGeneration();
    if (generation & 1) {
      VFS::NamespaceMutation completed(m_Vfs);
      continue;
    }
    FilesystemContextSnapshot snapshot;
    if (!context->snapshot(snapshot)) {
      SYSCALL_ERROR(DoesNotExist);
      attempt.failure();
      return false;
    }
    FilesystemPathRef found;
    size_t links = 0;
    ResolveOptions options;
    const bool success = m_State->follow(snapshot, selected, options, found, links);
    if (generation != m_Vfs.namespaceGeneration())
      continue;
    bool coherent;
    {
      LockGuard<Mutex> guard(m_State->graph);
      VfsFilesystemContext* live = nullptr;
      coherent = m_State->context(context, live) &&
                 live->generation == snapshot.contextGeneration &&
                 m_State->topology == snapshot.topologyGeneration;
    }
    if (!coherent)
      continue;
    if (!success) {
      attempt.failure();
      return false;
    }
    result = pedigree_std::move(found);
    return true;
  }
}

bool VfsMountView::resolveParent(const FilesystemContextRef& context,
                                 const FilesystemPathRef& start, const String& pathname,
                                 FilesystemPathRef& parent, String& basename) {
  if (!pathname.length()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  size_t end = pathname.length();
  while (end > 1 && pathname[end - 1] == '/')
    --end;
  size_t slash = end;
  while (slash && pathname[slash - 1] != '/')
    --slash;
  String name(pathname.view().substring(slash, end));
  if (name.length() > 255) {
    SYSCALL_ERROR(NameTooLong);
    return false;
  }
  String prefix(slash ? pathname.view().substring(0, slash) : StringView("."));
  ResolveOptions options;
  options.requireDirectory = true;
  FilesystemPathRef resolved;
  if (!resolve(context, start, prefix, options, resolved))
    return false;
  parent = pedigree_std::move(resolved);
  basename = pedigree_std::move(name);
  return true;
}
