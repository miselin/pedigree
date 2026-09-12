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

/** One shared view. Filesystem registration and storage retirement stay in VFS. */
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
  };

  explicit VfsMountView(VFS& vfs);
  ~VfsMountView();
  bool initialise(Filesystem* bootRoot);
  bool createBootContext(FilesystemContextOwner& result);
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
              Filesystem* backing, BackingOwnership ownership = BackingOwnership::External);
  bool detach(const FilesystemContextRef& context, const String& target, bool lazy);
  bool detachBackingForShutdown(Filesystem* backing);
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
  VfsMountView(const VfsMountView&) = delete;
  VfsMountView& operator=(const VfsMountView&) = delete;
};

#endif
