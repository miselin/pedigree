/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef VFS_H
#define VFS_H
#include "pedigree/kernel/linker/KernelElf.h"

#include <config.h>

#if THREADS
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/process/WaitQueue.h"
#endif
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/FilesystemContext.h"
#include "pedigree/kernel/process/FilesystemCredentials.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/LruCache.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Filesystem.h"

class Process;
class Disk;
class File;
class StringView;
class VfsMountState;
class VfsFilesystemPin;
class VfsMountView;

/** Set to zero to disable the builtin VFS LRU caches. */
#define VFS_WITH_LRU_CACHES 0

/** This class implements a single-root virtual filesystem namespace. */
class EXPORTED_PUBLIC VFS {
 public:
  class EXPORTED_PUBLIC NamespaceMutation {
   public:
    explicit NamespaceMutation(VFS& vfs);
    ~NamespaceMutation();
    uint64_t generation() const;
    bool protects(const VFS& vfs) const {
      return &m_Vfs == &vfs;
    }

   private:
    NamespaceMutation(const NamespaceMutation&) = delete;
    NamespaceMutation& operator=(const NamespaceMutation&) = delete;
    VFS& m_Vfs;
    LockGuard<Mutex> m_Lock;
  };

  /** Odd while a namespace writer owns admission; readers never hold it for I/O. */
  uint64_t namespaceGeneration() const;
  VfsMountView* mountView() const;
  /** Terminal-only: detach a drained namespace and return attachment-owned backends. */
  bool shutdownMountView(Vector<Filesystem*>& ownedBackings);
  bool initialiseMountView();

  static Module::UnloadAdmission unloadAdmission(bool terminal);
  class MountOperation;
  class FilesystemPin;
  class EXPORTED_PUBLIC MountIdentity {
   public:
    MountIdentity();
    MountIdentity(const MountIdentity& other);
    MountIdentity(MountIdentity&& other) noexcept;
    ~MountIdentity();
    MountIdentity& operator=(const MountIdentity& other);
    MountIdentity& operator=(MountIdentity&& other) noexcept;
    uint32_t id() const;
    explicit operator bool() const;
    bool acquire(MountOperation& operation) const;
    bool pin(FilesystemPin& pin) const;
    bool subscribeRetirement(const SharedPointer<FileEventObserver>& observer,
                             FileEventSubscription& subscription) const;

   private:
    friend class VFS;
    friend class MountOperation;
    friend class FilesystemPin;
    SharedPointer<VfsMountState> m_State;
  };

  class EXPORTED_PUBLIC MountOperation {
   public:
    MountOperation();
    MountOperation(MountOperation&& other) noexcept;
    ~MountOperation();
    MountOperation& operator=(MountOperation&& other) noexcept;
    Filesystem* filesystem() const;
    uint32_t id() const;
    MountIdentity identity() const;
    explicit operator bool() const;
    void reset();

   private:
    friend class VFS;
    friend class MountIdentity;
    MountOperation(const MountOperation&) = delete;
    MountOperation& operator=(const MountOperation&) = delete;
    TerminationDeferral m_Lifetime;
    SharedPointer<VfsMountState> m_State;
    OperationBarrier::Lease m_Admission;
  };

  /** Compares key without dereferencing it, then admits live filesystem access. */
  bool acquireMount(Filesystem* key, MountOperation& operation) const;

  /**
   * Retains backing storage for long-lived paths without a thread-owned scope.
   * Copies share an admission; ordinary unregistration fails while any remain.
   * This identifies a filesystem, not a particular namespace attachment.
   */
  class EXPORTED_PUBLIC FilesystemPin {
   public:
    FilesystemPin();
    FilesystemPin(const FilesystemPin& other);
    FilesystemPin(FilesystemPin&& other) noexcept;
    ~FilesystemPin();
    FilesystemPin& operator=(const FilesystemPin& other);
    FilesystemPin& operator=(FilesystemPin&& other) noexcept;
    Filesystem* filesystem() const;
    MountIdentity identity() const;
    explicit operator bool() const;
    void reset();

   private:
    friend class VFS;
    friend class MountIdentity;
    SharedPointer<VfsFilesystemPin> m_Pin;
  };

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  class EXPORTED_PUBLIC HostedRootViewScope {
   public:
    HostedRootViewScope();
    ~HostedRootViewScope();
    bool open(Filesystem* filesystem);
    bool installContext(Process& process);
    bool close();
    VfsMountView* view() const {
      return m_View;
    }

   private:
    HostedRootViewScope(const HostedRootViewScope&) = delete;
    HostedRootViewScope& operator=(const HostedRootViewScope&) = delete;
    TerminationDeferral m_Lifetime;
    VFS& m_Vfs;
    Filesystem* m_Filesystem = nullptr;
    Filesystem* m_PreviousRoot = nullptr;
    VfsMountView* m_PreviousView = nullptr;
    VfsMountView* m_View = nullptr;
    FilesystemPin m_PreviousRootPin;
    FilesystemContextRef m_PreviousContext;
    bool m_Installed = false;
  };
#endif

  bool pinFilesystem(Filesystem* key, FilesystemPin& pin) const;
  bool diskMount(uint32_t id, MountIdentity& identity) const;
  bool snapshotDiskMounts(Vector<MountIdentity>& mounts) const;

  /** Callback type, called when a disk is mounted or unmounted. */
  typedef void (*MountCallback)();

  /** Detached metadata for one published filesystem. */
  struct MountSnapshot {
    MountSnapshot() : hasDisk(false) {}
    MountSnapshot(const String& stableName, const String& path, bool hasDisk,
                  const String& diskParentName, const String& diskName)
        : stableName(stableName),
          path(path),
          hasDisk(hasDisk),
          diskParentName(diskParentName),
          diskName(diskName) {}

    String stableName;
    String path;
    bool hasDisk;
    String diskParentName;
    String diskName;
  };

  /** Constructor */
  VFS();
  /** Destructor */
  ~VFS();

  /** Returns the singleton VFS instance. */
  static VFS& instance();

  /** Probe and register a filesystem backed by a disk. */
  bool mount(Disk* pDisk, String& stableName, Filesystem** pMountedFs = nullptr);

  /** Register a filesystem for mounting under /media/<stable-name>. */
  String registerFilesystem(Filesystem* pFs, const String& preferredStableName);

  /**
   * Remove a registered filesystem and optionally destroy it.
   * When canDelete is true, the caller must provide exclusive ownership;
   * success consumes the pointer, while false leaves it unconsumed. When
   * canDelete is false, ownership always remains external. Live FilesystemPins
   * reject unregistration without changing publication or closing admission.
   */
  // Terminal failures after admission closes leave the unregistered backend
  // alive for diagnosis; callers must halt instead of resuming filesystem use.
  bool unregisterFilesystem(Filesystem* pFs, bool canDelete = true, bool terminal = false);
  /** Surrenders an attachment-owned registration. Existing admissions drain
      asynchronously; the last release deletes the backend outside VFS locks. */
  bool retireOwnedFilesystem(Filesystem* filesystem);

  /** Select the filesystem that supplies the root namespace. */
  bool setRootFilesystem(Filesystem* pFs);

  /** Returns a borrowed pointer; the lookup does not pin filesystem lifetime. */
  Filesystem* getRootFilesystem() const;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  /** Replace the hosted test namespace without registering or attaching it. */
  Filesystem* swapRootFilesystemForHostedTest(Filesystem* pFs);
#endif

  /** Obtain the canonical mount path for a filesystem. */
  bool getMountPath(Filesystem* pFs, String& path) const;

  /**
   * Find the filesystem mounted at an exact absolute path.
   * The returned pointer is borrowed and is not pinned against unregistration.
   */
  Filesystem* getFilesystemAt(const String& path) const;

  /** Copies a detached snapshot of all mounted filesystems. */
  void getMounts(Vector<MountSnapshot>& mounts) const;

  /** Pins one registered backing filesystem before calling its sync operation. */
  Filesystem::SyncStatus syncFilesystem(Filesystem* key);

  /** Pins all registered filesystems, attempts each, and returns the first error. */
  Filesystem::SyncStatus syncAll();

  /** Attempts to obtain a File for a specific path. */
  File* find(const String& path, File* pStartNode = 0);

  /** Find a File while retaining its VFS lifetime in result. */
  File* findRetained(const String& path, Directory::ChildLease& result, File* pStartNode = nullptr);

  /** Attempts to create a file. */
  bool createFile(const String& path, uint32_t mask, File* pStartNode = 0);

  /** Attempts to create a directory. */
  bool createDirectory(const String& path, uint32_t mask, File* pStartNode = 0);

  /** Attempts to create a symlink. */
  bool createSymlink(const String& path, const String& value, File* pStartNode = 0);

  /** Attempts to create a hard link. */
  bool createLink(const String& path, File* target, File* pStartNode = 0);

  /** Attempts to remove a file/directory/symlink. WILL FAIL IF DIRECTORY NOT
   * EMPTY */
  bool remove(const String& path, File* pStartNode = 0);

  /** Remove a path only if its terminal entry still has the expected identity. */
  bool remove(const String& path, File* pStartNode, File* expected);

  bool rename(const String& oldPath, File* oldStart, const String& newPath, File* newStart,
              bool noReplace = false);

  /** Adds a filesystem probe callback - this is called when a device is
   * mounted. Duplicate registration is idempotent and revives an entry closed
   * by a deferred removal which has no external drainer. */
  void addProbeCallback(Filesystem::ProbeCallback callback);

  /** Removes a filesystem probe callback before its implementation unloads. */
  bool removeProbeCallback(Filesystem::ProbeCallback callback);

  /** Adds a mount callback - the function is called when a disk is mounted or
      unmounted. Duplicate registration is idempotent and revives an entry
      closed by a deferred removal which has no external drainer. */
  void addMountCallback(MountCallback callback);

  /** Removes a mount callback before its implementation unloads. */
  bool removeMountCallback(MountCallback callback);

  /** Checks if the current user can access the given file. */
  static bool checkAccess(File* pFile, bool bRead, bool bWrite, bool bExecute);
  static bool checkAccess(File* file, bool read, bool write, bool execute,
                          const FilesystemCredentials& credentials);

  /** Checks access using an explicit, immutable credential snapshot. */
  static bool checkAccess(File* pFile, bool bRead, bool bWrite, bool bExecute, int64_t userId,
                          int64_t groupId, const Vector<int64_t>& supplementalGroups);

  /** \brief Track a File object that exists.
   * It is necessary to keep track of File objects, or at least those that
   * are stored in Directory caches and Filesystem objects, such that they
   * can be correctly tidied up when no more usages exist.
   *
   * It is almost never safe to directly delete a File pointer so this helps
   * maintain that safety.
   */
  void trackFile(File* pFile);
  /** Fallible initial publication; success owns one additional tracked reference. */
  bool tryTrackFile(File* pFile);

  /** Retain an already tracked File without publishing an untracked pointer. */
  MUST_USE_RESULT bool retainTrackedFile(File* pFile);

  /**
   * Stop tracking a File object, destroying by default if it is no longer
   * tracked by any other owners.
   * \return true if the File object was deleted (or would have been,
   *        if destroy == false)
   */
  bool untrackFile(File* pFile, bool destroy = true);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  using RetainTrackedFileHook = void (*)(File* pFile);
  static void setRetainTrackedFileHookForHostedTest(RetainTrackedFileHook hook);
  const void* trackedFilesLockAddressForHostedTest() const {
    return static_cast<const Semaphore*>(&m_TrackedFilesLock);
  }
#endif

 private:
  struct MountInfo {
    MountInfo(const String& stableName, const String& path,
              const SharedPointer<VfsMountState>& state);
    ~MountInfo();

    String stableName;
    String path;
    SharedPointer<VfsMountState> state;
  };

  typedef Tree<Filesystem*, MountInfo*> MountTable;

  struct CallbackState {
#if THREADS
    size_t sequence = 0;
    size_t inFlight = 0;
    size_t removers = 0;
    bool enabled = true;
    bool draining = false;
    uintptr_t debugAddress = 0;
    WaitQueue drainWaiters;
#endif
  };

  struct ProbeCallbackItem {
    explicit ProbeCallbackItem(Filesystem::ProbeCallback callback) : callback(callback) {}

    CallbackState state;
    Filesystem::ProbeCallback callback;
  };

  struct MountCallbackItem {
    explicit MountCallbackItem(MountCallback callback) : callback(callback) {}

    CallbackState state;
    MountCallback callback;
  };

#if THREADS
  struct ActiveInvocation {
    CallbackState* state;
    void* owner;
    ActiveInvocation* next;
  };

  ProbeCallbackItem* acquireProbeCallback(size_t& afterSequence, size_t boundary,
                                          ActiveInvocation& invocation);
  MountCallbackItem* acquireMountCallback(size_t& afterSequence, size_t boundary,
                                          ActiveInvocation& invocation);
  void finishCallback(CallbackState* state, ActiveInvocation& invocation);
  void drainProbeCallback(ProbeCallbackItem* item);
  void drainMountCallback(MountCallbackItem* item);
  void dispatchMountCallbacks(void* owner);
  bool isCallbackInvocation(void* owner) const;
#endif

  File* resolveStartNode(const String& path, File* pStartNode);
  String registerFilesystemLocked(Filesystem* pFs, const String& preferredStableName);
  String getUniqueStableNameLocked(const String& preferredName) const;
  bool attachFilesystem(Filesystem* pRootFs, Filesystem* pFs, const String& path);
  void attachRegisteredFilesystemsLocked();

  /** The static instance object. */
  static VFS m_Instance;

  /** A static File object representing an invalid file */
  static File* m_EmptyFile;

  mutable Mutex m_PathMutationLock;
  uint64_t m_PathGeneration;
  VfsMountView* m_MountView;
  mutable Mutex m_MountMutationLock;
  mutable Mutex m_MountTableLock;

  Filesystem* m_pRootFilesystem;
  MountTable m_Mounts;

  List<ProbeCallbackItem*> m_ProbeCallbacks;
  List<MountCallbackItem*> m_MountCallbacks;

#if THREADS
  Spinlock m_CallbackLock;
  size_t m_NextCallbackSequence;
  ActiveInvocation* m_pActiveCallbacks;
  bool m_CallbacksClosing;
#endif

  LruCache<String, File*> m_FindCache;

  Mutex m_TrackedFilesLock;
  Tree<File*, size_t> m_TrackedFiles;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  static RetainTrackedFileHook m_RetainTrackedFileHook;
#endif
};

#endif
