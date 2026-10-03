/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/syscallError.h"

#include "MountView-internal.h"
#if THREADS && !defined(STANDALONE_MUTEXES)
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

VfsNodeReference::VfsNodeReference(Directory::ChildLease& child)
    : m_Node(child.releaseOwnership()),
      m_Ownership(m_Node ? Ownership::Tracked : Ownership::Borrowed) {}
VfsNodeReference::VfsNodeReference(VfsNodeReference&& other) noexcept
    : m_Node(other.m_Node), m_Ownership(other.m_Ownership) {
  other.m_Node = nullptr;
  other.m_Ownership = Ownership::Borrowed;
}
VfsNodeReference& VfsNodeReference::operator=(VfsNodeReference&& other) noexcept {
  if (this != &other) {
    reset();
    m_Node = other.m_Node;
    m_Ownership = other.m_Ownership;
    other.m_Node = nullptr;
    other.m_Ownership = Ownership::Borrowed;
  }
  return *this;
}
VfsNodeReference::~VfsNodeReference() {
  reset();
}
void VfsNodeReference::reset() {
  if (m_Ownership == Ownership::Borrowed) {
    m_Node = nullptr;
    return;
  }
#if THREADS && !defined(STANDALONE_MUTEXES)
  TerminationDeferral lifetime;
#endif
  File* node = m_Node;
  const auto ownership = m_Ownership;
  m_Node = nullptr;
  m_Ownership = Ownership::Borrowed;
  if (ownership == Ownership::Tracked) {
    VFS::instance().untrackFile(node);
  } else {
    node->releaseVfsReference();
  }
}
bool VfsNodeReference::retain(File* node, Filesystem* backing) {
  if (m_Node || !node || !backing || node->getFilesystem() != backing)
    return false;
  const bool retained = node->retainVfsReference();
  if (!retained && node != backing->getRoot()) {
    return false;
  }
  m_Node = node;
  m_Ownership = retained ? Ownership::Virtual : Ownership::Borrowed;
  return true;
}

bool VfsNodeReference::retainAnonymous(File* node) {
  if (m_Node || !node || node->isDirectory() || !node->retainVfsReference())
    return false;
  m_Node = node;
  m_Ownership = Ownership::Virtual;
  return true;
}

VfsBacking::~VfsBacking() {
#if THREADS && !defined(STANDALONE_MUTEXES)
  TerminationDeferral lifetime;
#endif
  // Keep our pin until admission closes, so retirement cannot delete storage
  // while this attachment still owns it. Graph rows retire outside graph/writer locks.
  if (owningRegistry && !owningRegistry->retireOwnedFilesystem(pin.filesystem()))
    FATAL("Owned attachment lost its backing registration");
  pin.reset();
}

VfsPath::VfsPath(VfsMountView& owner, const VfsAttachmentRef& mounted, VfsNodeReference&& retained)
    : kind(Kind::Mounted), view(owner), attachment(mounted), file(pedigree_std::move(retained)) {
  view.retain();
  attachment->paths += 1;
}
VfsPath::VfsPath(VfsMountView& owner, VfsNodeReference&& retained)
    : kind(Kind::Anonymous), view(owner), attachment(), file(pedigree_std::move(retained)) {
  view.retain();
  view.m_State->anonymousPaths += 1;
}
VfsPath::~VfsPath() {
#if THREADS && !defined(STANDALONE_MUTEXES)
  TerminationDeferral lifetime;
  Thread* thread = Processor::information().getCurrentThread();
  const size_t error = thread ? thread->getErrno() : 0;
#endif
  file.reset();
  if (kind == Kind::Mounted) {
    if ((attachment->paths -= 1) == 0)
      view.m_State->reapDetached();
  } else {
    view.m_State->anonymousPaths -= 1;
  }
  attachment.reset();
  lookupParent.reset();
  view.release();
#if THREADS && !defined(STANDALONE_MUTEXES)
  if (thread)
    thread->setErrno(error);
#endif
}

VfsMountView::VfsMountView(VFS& vfs) : m_State(new State(*this)), m_Vfs(vfs) {}
void VfsMountView::retain() {
  __atomic_add_fetch(&m_References, 1, __ATOMIC_RELAXED);
}
void VfsMountView::release() {
  if (__atomic_sub_fetch(&m_References, 1, __ATOMIC_ACQ_REL) == 0 && m_Automatic) {
    delete this;
  }
}
VfsMountView* VfsMountView::fromContext(const FilesystemContextRef& context) {
  return context ? static_cast<VfsMountView*>(const_cast<void*>(context->provider())) : nullptr;
}
VfsMountView* VfsMountView::fromPath(const FilesystemPathRef& path) {
  return path ? static_cast<VfsMountView*>(const_cast<void*>(path->provider())) : nullptr;
}
VfsMountView::~VfsMountView() {
  delete m_State;
}
VfsMountView::State::~State() {
  if (contexts || anonymousPaths)
    FATAL("Mount view destroyed with retained filesystem owners");
  auto* row = attachments;
  attachments = nullptr;
  while (row) {
    if (row->attachment->paths)
      FATAL("Mount view destroyed with retained paths");
    auto* next = row->next;
    delete row;
    row = next;
  }
}

VfsAttachmentRow* VfsMountView::State::find(uint64_t id) const {
  for (auto* row = attachments; row; row = row->next)
    if (row->attachment->id == id)
      return row;
  return nullptr;
}
VfsAttachmentRow* VfsMountView::State::at(const VfsPath& path) const {
  for (auto* row = attachments; row; row = row->next)
    if (row->parent.get() == path.attachment.get() && row->covered &&
        row->covered->get() == path.node())
      return row;
  return nullptr;
}
bool VfsMountView::State::snapshotRows(Vector<VfsAttachmentRow>& result) {
  LockGuard<Mutex> guard(graph);
  size_t count = 0;
  for (auto* row = attachments; row; row = row->next) {
    ++count;
  }
  if (!result.tryReserve(count)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  for (auto* row = attachments; row; row = row->next) {
    result.pushBack(*row);
  }
  return true;
}
VfsPath* VfsMountView::State::nodePath(const FilesystemPathRef& reference) const {
  auto* result = reference && reference->provider() == &view
                     ? static_cast<VfsPath*>(reference.get())
                     : nullptr;
  return result && &result->view == &view ? result : nullptr;
}
VfsPath* VfsMountView::State::path(const FilesystemPathRef& reference) const {
  auto* selected = nodePath(reference);
  return selected && selected->kind == VfsPath::Kind::Mounted ? selected : nullptr;
}
bool VfsMountView::State::contains(const FilesystemPathRef& reference) const {
  auto* p = path(reference);
  if (!p)
    return false;
  auto* row = find(p->attachment->id);
  size_t remaining = 1;
  for (auto* entry = attachments; entry; entry = entry->next)
    ++remaining;
  while (row && remaining--) {
    if (row->attachment->id == rootId)
      return true;
    row = row->parent ? find(row->parent->id) : nullptr;
  }
  return false;
}
bool VfsMountView::State::context(const FilesystemContextRef& reference,
                                  VfsFilesystemContext*& result) const {
  for (auto* row = contexts; row; row = row->next) {
    if (row->context.get() == reference.get()) {
      result = static_cast<VfsFilesystemContext*>(reference.get());
      return true;
    }
  }
  return false;
}
bool VfsMountView::State::makePath(const VfsAttachmentRef& attachment, File* node,
                                   FilesystemPathRef& result) {
  VfsNodeReference retained;
  if (!attachment || !retained.retain(node, attachment->backing->pin.filesystem())) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  return makePath(attachment, pedigree_std::move(retained), result);
}

bool VfsMountView::State::makeAttachment(const VfsBackingRef& backing, File* root, uint64_t flags,
                                         VfsAttachmentRef& result) {
  VfsNodeReference retained;
  if (!backing || !retained.retain(root, backing->pin.filesystem())) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  if (nextId > 0x7fffffffU) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  auto attachment = VfsAttachmentRef::tryAdopt(
      new VfsAttachment(backing, nextId++, pedigree_std::move(retained), flags));
  if (!attachment) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  result = pedigree_std::move(attachment);
  return true;
}

bool VfsMountView::forkNamespace(const FilesystemContextRef& context,
                                 FilesystemContextOwner& result, uint64_t ownerNamespace) {
  if (fromContext(context) != this || result) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  auto clone = UniquePointer<VfsMountView>::adopt(new VfsMountView(m_Vfs));
  if (!clone || !clone.get()->m_State) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  auto* destination = clone.get()->m_State;
  clone.get()->m_OwnerNamespace = ownerNamespace;
  auto reference = FilesystemContextRef::tryAdopt(new VfsFilesystemContext(*clone.get()));
  auto enrolled = UniquePointer<VfsContextRow>::adopt(new VfsContextRow);
  FilesystemContextSnapshot snapshot;
  Vector<VfsAttachmentRow> rows;
  if (!reference || !enrolled) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  {
    VFS::NamespaceMutation writer(m_Vfs);
    if (!context->snapshot(snapshot)) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    if (!m_State->snapshotRows(rows)) {
      return false;
    }
    // Preserve IDs within the clone so every edge and retained context can be
    // translated without a second independently maintained topology index.
    for (auto& item : rows) {
      auto* source = &item;
      VfsNodeReference root;
      if (!root.retain(source->attachment->root, source->attachment->backing->pin.filesystem())) {
        SYSCALL_ERROR(DoesNotExist);
        return false;
      }
      auto attachment = VfsAttachmentRef::tryAdopt(
          new VfsAttachment(source->attachment->backing, source->attachment->id,
                            pedigree_std::move(root), source->attachment->flags));
      auto row = UniquePointer<VfsAttachmentRow>::adopt(new VfsAttachmentRow);
      if (!attachment || !row) {
        SYSCALL_ERROR(OutOfMemory);
        return false;
      }
      attachment->lockedFlags = source->attachment->lockedFlags;
      attachment->lockedMount = source->attachment->lockedMount;
      if (ownerNamespace != m_OwnerNamespace) {
        attachment->lockedFlags |= source->attachment->flags;
        // A less privileged owner may detach the inherited tree as a whole,
        // but cannot remove an interior mount to expose what it conceals.
        attachment->lockedMount = source->attachment->id != m_State->rootId;
      }
      row.get()->attachment = attachment;
      row.get()->covered = source->covered;
      row.get()->next = destination->attachments;
      destination->attachments = row.releaseOwnership();
    }
    for (auto& item : rows) {
      auto* source = &item;
      if (source->parent) {
        destination->find(source->attachment->id)->parent =
            destination->find(source->parent->id)->attachment;
      }
    }
    destination->rootId = m_State->rootId;
    destination->nextId = m_State->nextId;
    auto* created = static_cast<VfsFilesystemContext*>(reference.get());
    auto* root = m_State->path(snapshot.root);
    auto* cwd = m_State->path(snapshot.cwd);
    if (!root || !destination->makePath(destination->find(root->attachment->id)->attachment,
                                        root->node(), created->root)) {
      return false;
    }
    if (cwd) {
      if (!destination->makePath(destination->find(cwd->attachment->id)->attachment, cwd->node(),
                                 created->cwd)) {
        return false;
      }
    } else {
      created->cwd = snapshot.cwd;
    }
    enrolled.get()->context = reference;
    created->registration = enrolled.get();
    destination->contexts = enrolled.releaseOwnership();
    destination->contextCount = 1;
  }
  auto* published = clone.releaseOwnership();
  published->m_Automatic = true;
  result = FilesystemContextOwner::adopt(pedigree_std::move(reference));
  published->m_State->reapDetached();
  published->release();
  return true;
}
bool VfsMountView::State::makePath(const VfsAttachmentRef& attachment, VfsNodeReference&& retained,
                                   FilesystemPathRef& result) {
  if (!attachment || !retained.get() ||
      retained.get()->getFilesystem() != attachment->backing->pin.filesystem()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  auto path =
      FilesystemPathRef::tryAdopt(new VfsPath(view, attachment, pedigree_std::move(retained)));
  if (!path) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  result = pedigree_std::move(path);
  return true;
}

bool VfsMountView::initialise(Filesystem* bootRoot) {
  if (!m_State || !bootRoot) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  VFS::FilesystemPin pin;
  if (!m_Vfs.pinFilesystem(bootRoot, pin)) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  auto backing = VfsBackingRef::tryAdopt(new VfsBacking(pedigree_std::move(pin)));
  VfsAttachmentRef attachment;
  if (!backing || !m_State->makeAttachment(backing, bootRoot->getRoot(), 0, attachment)) {
    return false;
  }
  auto row = UniquePointer<VfsAttachmentRow>::adopt(new VfsAttachmentRow);
  if (!attachment || !row) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  row.get()->attachment = attachment;
  VFS::NamespaceMutation writer(m_Vfs);
  LockGuard<Mutex> guard(m_State->graph);
  if (m_State->rootId) {
    SYSCALL_ERROR(DeviceBusy);
    return false;
  }
  m_State->rootId = 1;
  m_State->nextId = 2;
  m_State->attachments = row.releaseOwnership();
  return true;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool VfsMountView::quiescentForHostedTest() const {
  if (!m_State)
    return true;
  LockGuard<Mutex> guard(m_State->graph);
  if (m_State->contexts || m_State->anonymousPaths)
    return false;
  for (auto* row = m_State->attachments; row; row = row->next)
    if (row->attachment->paths)
      return false;
  return true;
}
#endif

bool VfsMountView::bootRootPath(FilesystemPathRef& result) {
  VfsAttachmentRef attachment;
  if (!m_State)
    return false;
  {
    LockGuard<Mutex> guard(m_State->graph);
    auto* row = m_State->find(m_State->rootId);
    if (!row)
      return false;
    attachment = row->attachment;
  }
  return m_State->makePath(attachment, attachment->root, result);
}
bool VfsMountView::createBootContext(FilesystemContextOwner& result) {
  return m_State && m_State->createContext(nullptr, result);
}
bool VfsMountView::State::createContext(const VfsFilesystemContext* parent,
                                        FilesystemContextOwner& result) {
#if THREADS && !defined(STANDALONE_MUTEXES)
  TerminationDeferral lifetime;
#endif
  if (result) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  auto reference = FilesystemContextRef::tryAdopt(new VfsFilesystemContext(view));
  auto row = UniquePointer<VfsContextRow>::adopt(new VfsContextRow);
  if (!reference || !row) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  auto* created = static_cast<VfsFilesystemContext*>(reference.get());
  row.get()->context = reference;
  FilesystemPathRef initial;
  VFS::NamespaceMutation writer(view.m_Vfs);
  if (!parent) {
    // Boot enrollment snapshots the current view root under the same writer
    // admission as publication, so it cannot resurrect a root moved by pivot.
    VfsAttachmentRef attachment;
    {
      LockGuard<Mutex> guard(graph);
      auto* root = find(rootId);
      if (!root) {
        SYSCALL_ERROR(DoesNotExist);
        return false;
      }
      attachment = root->attachment;
    }
    if (!makePath(attachment, attachment->root, initial))
      return false;
  }
  {
    LockGuard<Mutex> guard(graph);
    if (parent && !parent->registration) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    created->root = parent ? parent->root : initial;
    created->cwd = parent ? parent->cwd : initial;
    created->registration = row.get();
    row.get()->next = contexts;
    contexts = row.releaseOwnership();
    ++contextCount;
  }
  result = FilesystemContextOwner::adopt(pedigree_std::move(reference));
  return true;
}

VfsFilesystemContext::~VfsFilesystemContext() {
  if (registration) {
    FATAL("Filesystem context destroyed before owner retirement");
  }
  root.reset();
  cwd.reset();
  view.release();
}
bool VfsFilesystemContext::snapshot(FilesystemContextSnapshot& result) const {
  FilesystemContextSnapshot replacement;
  {
    LockGuard<Mutex> guard(view.m_State->graph);
    if (!registration)
      return false;
    replacement.root = root;
    replacement.cwd = cwd;
    replacement.contextGeneration = generation;
    replacement.topologyGeneration = view.m_State->topology;
  }
  result = pedigree_std::move(replacement);
  return true;
}
bool VfsFilesystemContext::forkForProcess(FilesystemContextOwner& result) const {
  return view.m_State->createContext(this, result);
}
void VfsFilesystemContext::retireProcessOwner() {
#if THREADS && !defined(STANDALONE_MUTEXES)
  TerminationDeferral lifetime;
#endif
  VfsContextRow* retired = nullptr;
  FilesystemPathRef retiredRoot, retiredCwd;
  {
    VFS::NamespaceMutation writer(view.m_Vfs);
    LockGuard<Mutex> guard(view.m_State->graph);
    if (!registration)
      return;
    auto** link = &view.m_State->contexts;
    while (*link && *link != registration)
      link = &(*link)->next;
    if (!*link)
      FATAL("Missing filesystem context enrollment");
    retired = *link;
    *link = retired->next;
    registration = nullptr;
    --view.m_State->contextCount;
    retiredRoot = pedigree_std::move(root);
    retiredCwd = pedigree_std::move(cwd);
  }
  delete retired;
}

bool VfsMountView::changeCwd(const FilesystemContextRef& context, const FilesystemPathRef& path) {
  auto* owner = fromPath(path);
  if (!owner || !owner->m_State->path(path) || !path->node()->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  if (!VFS::checkAccess(path->node(), false, false, true))
    return false;
  FilesystemPathRef retired;
  {
    VFS::NamespaceMutation writer(m_Vfs);
    LockGuard<Mutex> guard(m_State->graph);
    VfsFilesystemContext* target = nullptr;
    if (!m_State->context(context, target)) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    retired = pedigree_std::move(target->cwd);
    target->cwd = path;
    ++target->generation;
  }
  return true;
}
bool VfsMountView::changeRoot(const FilesystemContextRef& context, const FilesystemPathRef& path) {
  if (!m_State->path(path) || !path->node()->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  if (!VFS::checkAccess(path->node(), false, false, true))
    return false;
  FilesystemPathRef retired;
  {
    VFS::NamespaceMutation writer(m_Vfs);
    LockGuard<Mutex> guard(m_State->graph);
    VfsFilesystemContext* target = nullptr;
    if (!m_State->context(context, target)) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    retired = pedigree_std::move(target->root);
    target->root = path;
    ++target->generation;
  }
  return true;
}
uint64_t VfsMountView::attachmentId(const FilesystemPathRef& path) const {
  auto* concrete = m_State ? m_State->path(path) : nullptr;
  return concrete ? concrete->attachment->id : 0;
}
bool VfsMountView::samePath(const FilesystemPathRef& a, const FilesystemPathRef& b) const {
  auto* first = m_State ? m_State->nodePath(a) : nullptr;
  auto* second = m_State ? m_State->nodePath(b) : nullptr;
  return first && second && first->kind == second->kind &&
         first->attachment.get() == second->attachment.get() && first->node() == second->node();
}
bool VfsMountView::anonymousPath(File* node, FilesystemPathRef& result) {
  VfsNodeReference retained;
  if (!m_State || !retained.retainAnonymous(node)) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  auto path = FilesystemPathRef::tryAdopt(new VfsPath(*this, pedigree_std::move(retained)));
  if (!path) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  result = pedigree_std::move(path);
  return true;
}
bool VfsMountView::pathForNode(const FilesystemPathRef& sameAttachment, File* node,
                               FilesystemPathRef& result) {
  auto* path = m_State ? m_State->path(sameAttachment) : nullptr;
  if (!path) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  return m_State->makePath(path->attachment, node, result);
}
