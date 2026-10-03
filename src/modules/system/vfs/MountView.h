/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_VFS_MOUNTVIEW_H
#define PEDIGREE_VFS_MOUNTVIEW_H

#include "pedigree/kernel/process/FilesystemContext.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "VFS.h"

class VfsPath;
class VfsFilesystemContext;
class VfsAttachment;

/** One mount namespace. Filesystem registration and storage retirement stay in VFS. */
class EXPORTED_PUBLIC VfsMountView {
 public:
  struct ResolveOptions {
    bool followFinal = true;
    bool requireDirectory = false;
    bool crossFinalMount = true;
  };
  struct MountSnapshot {
    uint64_t id = 0;
    uint64_t parentId = 0;
    VFS::MountIdentity backing;
    String path;
    String root;
    uint64_t flags = 0;
  };
  enum MountFlags : uint64_t { ReadOnly = 1, NoSuid = 2, NoDev = 4, NoExec = 8 };
  static constexpr uint64_t SupportedMountFlags = ReadOnly | NoSuid | NoDev | NoExec;
  class WriteLease final : public FilesystemWriteLease {
   public:
    WriteLease() = default;
    ~WriteLease() override;
    bool acquire(const FilesystemPathRef& path);

   private:
    WriteLease(const WriteLease&) = delete;
    WriteLease& operator=(const WriteLease&) = delete;
    FilesystemPathRef m_Path;
  };
  static bool retainWrite(const FilesystemPathRef& path,
                          SharedPointer<FilesystemWriteLease>& result);

  explicit VfsMountView(VFS& vfs);
  ~VfsMountView();
  bool initialise(Filesystem* bootRoot);
  bool createBootContext(FilesystemContextOwner& result);
  static VfsMountView* fromContext(const FilesystemContextRef& context);
  static VfsMountView* fromPath(const FilesystemPathRef& path);
  bool forkNamespace(const FilesystemContextRef& context, FilesystemContextOwner& result,
                     uint64_t ownerNamespace = 0);
  uint64_t ownerNamespace() const {
    return m_OwnerNamespace;
  }
  bool resolve(const FilesystemContextRef& context, const FilesystemPathRef& start,
               const String& path, const ResolveOptions& options, FilesystemPathRef& result);
  bool follow(const FilesystemContextRef& context, const FilesystemPathRef& selected,
              FilesystemPathRef& result);
  bool resolveParent(const FilesystemContextRef& context, const FilesystemPathRef& start,
                     const String& path, FilesystemPathRef& parent, String& basename);
  bool changeCwd(const FilesystemContextRef& context, const FilesystemPathRef& path);
  bool changeRoot(const FilesystemContextRef& context, const FilesystemPathRef& path);
  bool formatPath(const FilesystemContextSnapshot& context, const FilesystemPathRef& path,
                  String& result);
  bool snapshotMounts(const FilesystemContextRef& context, Vector<MountSnapshot>& result);

  enum class BackingOwnership { External, Attachment };
  // Attachment ownership transfers only after successful graph publication.
  // Attachments identify a mounted root, independently of backing identity.
  bool attach(const FilesystemContextRef& context, const FilesystemPathRef& covered,
              Filesystem* backing, BackingOwnership ownership = BackingOwnership::External,
              uint64_t flags = 0);
  bool bind(const FilesystemContextRef& context, const FilesystemPathRef& source,
            const FilesystemPathRef& target, bool recursive);
  bool remount(const FilesystemContextRef& context, const FilesystemPathRef& target, uint64_t flags,
               bool recursive = false);
  uint64_t mountFlags(const FilesystemPathRef& path) const;
  bool writable(const FilesystemPathRef& path) const;
  bool detach(const FilesystemContextRef& context, const String& target, bool lazy);
  bool detachBackingForShutdown(Filesystem* backing);
  bool detachBackingForRemoval(Filesystem* backing);
  bool shutdown(Vector<Filesystem*>& ownedBackings);
  bool pivot(const FilesystemContextRef& context, const String& newRoot, const String& putOld);
  uint64_t attachmentId(const FilesystemPathRef& path) const;
  bool isMountpoint(File* node) const;
  bool createFile(const FilesystemPathRef& parent, const String& name, uint32_t mask);
  bool createDirectory(const FilesystemPathRef& parent, const String& name, uint32_t mask);
  bool createSymlink(const FilesystemPathRef& parent, const String& name, const String& value);
  bool createLink(const FilesystemPathRef& parent, const String& name,
                  const FilesystemPathRef& target);
  bool remove(const FilesystemPathRef& parent, const String& name, File* expected = nullptr);
  bool rename(const FilesystemPathRef& oldParent, const String& oldName,
              const FilesystemPathRef& newParent, const String& newName, bool noReplace,
              bool sourceMustBeDirectory = false);
  bool anonymousPath(File* retainedNode, FilesystemPathRef& result);
  bool pathForNode(const FilesystemPathRef& sameAttachment, File* retainedNode,
                   FilesystemPathRef& result);
  bool samePath(const FilesystemPathRef& first, const FilesystemPathRef& second) const;
  uint64_t filesystemAccess(const FilesystemPathRef& path,
                            const VFS::NamespaceMutation* writer = nullptr);
  bool checkFilesystemAccess(const FilesystemPathRef& path, uint64_t access,
                             const VFS::NamespaceMutation* writer = nullptr);
  bool authorizeRemove(const FilesystemPathRef& parent, File* node,
                       const VFS::NamespaceMutation& writer);
  bool authorizeLink(const FilesystemPathRef& parent, const FilesystemPathRef& target,
                     const VFS::NamespaceMutation& writer);
  bool authorizeRename(const FilesystemPathRef& oldParent, File* source,
                       const FilesystemPathRef& newParent, File* replaced,
                       const VFS::NamespaceMutation& writer, bool removeSource = true);
  Directory::AddStatus createEphemeral(const FilesystemPathRef& parent, File* node,
                                       uint64_t access);

  // Boot/fixture enrollment only; never resolves a raw File to a guessed mount.
  bool bootRootPath(FilesystemPathRef& result);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  bool quiescentForHostedTest() const;
#endif

 private:
  friend class VfsPath;
  friend class VfsFilesystemContext;
  struct State;
  State* m_State;
  VFS& m_Vfs;
  size_t m_References = 1;
  bool m_Automatic = false;
  uint64_t m_OwnerNamespace = 0;
  void retain();
  void release();
  VfsMountView(const VfsMountView&) = delete;
  VfsMountView& operator=(const VfsMountView&) = delete;
};

#endif
