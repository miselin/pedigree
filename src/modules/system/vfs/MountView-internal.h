/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_VFS_MOUNTVIEW_INTERNAL_H
#define PEDIGREE_VFS_MOUNTVIEW_INTERNAL_H
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include "MountView.h"

// Unlike ChildLease/RetainedFile, this owner may outlive the acquiring thread.
class VfsNodeReference {
 public:
  VfsNodeReference() = default;
  VfsNodeReference(VfsNodeReference&& other) noexcept;
  VfsNodeReference& operator=(VfsNodeReference&& other) noexcept;
  ~VfsNodeReference();
  bool retain(File* node, Filesystem* backing);
  bool retainAnonymous(File* node);
  void reset();
  File* get() const {
    return m_Node;
  }

 private:
  VfsNodeReference(const VfsNodeReference&) = delete;
  VfsNodeReference& operator=(const VfsNodeReference&) = delete;
  File* m_Node = nullptr;
  bool m_Tracked = false;
};

class VfsAttachment {
 public:
  VfsAttachment(VFS::FilesystemPin&& pin, uint64_t identity)
      : backing(pedigree_std::move(pin)), root(backing.filesystem()->getRoot()), id(identity) {}
  ~VfsAttachment();
  VFS* owningRegistry = nullptr;
  VFS::FilesystemPin backing;
  File* const root;
  const uint64_t id;
  Atomic<size_t> paths{0};
};
using VfsAttachmentRef = SharedPointer<VfsAttachment>;

class VfsPath final : public FilesystemPath {
 public:
  enum class Kind { Mounted, Anonymous };
  VfsPath(VfsMountView& owner, const VfsAttachmentRef& attachment, VfsNodeReference&& node);
  VfsPath(VfsMountView& owner, VfsNodeReference&& anonymousNode);
  const Kind kind;
  ~VfsPath() override;
  File* node() const override {
    return file.get();
  }
  const void* provider() const override {
    return &view;
  }
  VfsMountView& view;
  VfsAttachmentRef attachment;
  VfsNodeReference file;
};

struct VfsAttachmentRow {
  VfsAttachmentRef attachment;
  VfsAttachmentRef parent;
  SharedPointer<VfsNodeReference> covered;
  VfsAttachmentRow* next = nullptr;
};
struct VfsContextRow {
  FilesystemContextRef context;
  VfsContextRow* next = nullptr;
};

class VfsFilesystemContext final : public FilesystemContext {
 public:
  explicit VfsFilesystemContext(VfsMountView& owner) : view(owner) {}
  ~VfsFilesystemContext() override;
  bool snapshot(FilesystemContextSnapshot& result) const override;
  bool forkForProcess(FilesystemContextOwner& result) const override;
  void retireProcessOwner() override;

  VfsMountView& view;
  FilesystemPathRef root;
  FilesystemPathRef cwd;
  uint64_t generation = 1;
  VfsContextRow* registration = nullptr;
};

struct VfsMountView::State {
  explicit State(VfsMountView& owner) : view(owner) {}
  ~State();
  VfsMountView& view;
  Mutex graph;
  Atomic<size_t> anonymousPaths{0};
  VfsAttachmentRow* attachments = nullptr;
  VfsContextRow* contexts = nullptr;
  size_t contextCount = 0;
  uint64_t topology = 1;
  uint64_t rootId = 0;
  uint64_t nextId = 1;

  VfsAttachmentRow* find(uint64_t id) const;
  VfsAttachmentRow* at(const VfsPath& path) const;
  bool contains(const FilesystemPathRef& path) const;
  VfsPath* nodePath(const FilesystemPathRef& reference) const;
  VfsPath* path(const FilesystemPathRef& reference) const;
  bool makePath(const VfsAttachmentRef& attachment, File* node, FilesystemPathRef& result);
  bool context(const FilesystemContextRef& reference, VfsFilesystemContext*& result) const;
  bool createContext(const VfsFilesystemContext* parent, FilesystemContextOwner& result);
  bool cross(const FilesystemPathRef& path, FilesystemPathRef& result);
  bool parent(const FilesystemPathRef& path, FilesystemPathRef& result,
              const FilesystemPathRef& boundary = FilesystemPathRef());
  bool beneath(const FilesystemPathRef& descendant, const FilesystemPathRef& ancestor);
  bool format(const FilesystemContextSnapshot& context, const FilesystemPathRef& path,
              String& result, const VFS::NamespaceMutation& writer);
  bool resolve(const FilesystemContextSnapshot& context, const FilesystemPathRef& start,
               const String& pathname, const ResolveOptions& options, FilesystemPathRef& result,
               const VFS::NamespaceMutation* writer = nullptr);
  bool follow(const FilesystemContextSnapshot& context, const FilesystemPathRef& selected,
              const ResolveOptions& options, FilesystemPathRef& result, size_t& links);
  bool walk(const FilesystemContextSnapshot& context, const FilesystemPathRef& start,
            const String& pathname, const ResolveOptions& options, FilesystemPathRef& result,
            size_t& links);
  bool attach(const FilesystemPathRef& covered, VFS::FilesystemPin&& pin,
              const VFS::NamespaceMutation& writer, BackingOwnership ownership);
  void reapDetached();
};
#endif
