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

#ifndef MEMORY_MAPPED_FILE_H
#define MEMORY_MAPPED_FILE_H
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/FilesystemContext.h"
#include "pedigree/kernel/process/MemoryPressureManager.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/processor/UserMemoryPolicy.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/new"

#include <config.h>

#include "SwapStore.h"

class File;
class Process;
class VirtualAddressSpace;
class VfsUserMemoryPolicy;
using FileResidencyAccess = bool (*)(File*, void*);

struct EXPORTED_PUBLIC FileMappingOrigin {
  uint64_t openIdentity = 0;
  bool writableOpen = false;
  FilesystemPathRef openingPath{};
};

/** One logical mapping attachment, retained by every surviving fragment. */
class MappingAttachment {
 public:
  virtual ~MappingAttachment() {}
  virtual uintptr_t baseAddress() const = 0;
  virtual void relocate(uintptr_t newBase) = 0;
  virtual SharedPointer<MappingAttachment> clone(Process* target) = 0;
};

/** \addtogroup vfs
    @{ */

/**
 * \page mmap_main Memory Mapped Files
 * Pedigree supports memory mapped files to allow mapping File objects into
 * the address space.
 *
 * \section mmap_overview Overview
 * Pedigree's memory mapped file support includes support for both anonymous and
 * proper file-backed mappings. An anonymous mapping is one that only backs onto
 * memory (there is no file behind it). A file-backed mapping backs onto a file
 * and demand-maps it as necessary.
 *
 * File memory maps can be mapped shared or copy-on-write. Shared maps allow
 * writes directly to the file in memory (and need to be synced to appear on
 * disk). Copy-on-write maps never affect the file.
 *
 * \section mmap_vfs VFS Requirements
 * Memory mapped files do not work out-of-the-box with every filesystem. In
 * particular, filesystems that cannot guarantee page-aligned (and page-size)
 * blocks returned from File::readBlock will not work immediately.
 *
 * The basic requirements for a memory mapped file to work correctly are:
 * - File::read must be able to accept a NULL buffer, which will prime
 *     File::readBlock, rather than read into a buffer.
 * - File::readBlock must return a page-aligned, page-size block.
 * - File::pinBlock must be able to pin a block so it cannot be freed.
 * - File::unpinBlock must be able to unpin a block so it can be freed.
 *
 * Optionally, the following can be provided to enable extra functionality:
 * - File::sync(size_t, bool) which syncs a block back to disk (or some other
 *     backing store, if the File is not backed by disk).
 * - File::writeBlock which triggers a write of a block back to disk (or some
 *     other backing store).
 *
 * \section mmap_trap Trap Handling
 * At first, no mappings for the file exist in the address space. When a process
 * attempts to access a mapping, a page fault is triggered which is handled by
 * MemoryMapManager::trap.
 *
 * For anonymous memory maps, reads get mapped to a single zero page. This
 * means anonymous memory maps that have not been written to use minimal amounts
 * of memory. When an anonymous memory map is written to, a new page is mapped
 * and zeroed.
 *
 * For file memory maps, reads get mapped to the physical page for the virtual
 * page returned by File::readBlock. It is for this reason that File::pinBlock
 * is necessary -- the physical page cannot disappear underneath the file, as
 * this would cause the mapping's contents to change (and leak data from other
 * processes).
 *
 * A file memory map that is mapped shared will allow this physical page to be
 * modified on write. A copy-on-write file memory map will trigger a copy of the
 * page, and further writes will go to the copy of this page.
 */

/** \file
    \brief Memory-mapped file interface

    Provides a mechanism for mapping Files into the address space.

    \todo Handle writing of files, not just reading. */

/**
 * Generic base for a memory mapped file or object.
 *
 * Provides the interface for implementation, while centralising common
 * functionality (such as extents, CoW flags, shared state, etc)
 */
class MemoryMappedObject {
  friend class MemoryMapManager;

 private:
  /** Default constructor, don't use. */
  MemoryMappedObject();

 public:
  /** Permissions to assign to a mapping when it is created. */
  typedef int Permissions;

  static const int None = 0x0;
  static const int Read = 0x1;
  static const int Write = 0x2;
  static const int Exec = 0x4;

  /** Constructor - bring up common metadata. */
  MemoryMappedObject(uintptr_t address, bool bCopyOnWrite, size_t length, Permissions perms,
                     Permissions maximumPerms = Read | Write | Exec)
      : m_bCopyOnWrite(bCopyOnWrite),
        m_Address(address),
        m_Length(length),
        m_Permissions(perms),
        m_MaximumPermissions(maximumPerms),
        m_Attachment(),
        m_OwnerProcess(nullptr),
        m_OwnsMappings(true),
        m_LockMode(MemoryLockMode::None) {}

  virtual ~MemoryMappedObject();

  /**
   * Clones the existing metadata of this object into another.
   *
   * Returns a MemoryMappedObject that exactly matches this one,
   * but which can be used for reference in a new address space.
   * Used for address space clones.
   *
   * Note that mappings are automatically cloned - only clone
   * metadata in this method.
   */
  virtual MemoryMappedObject* clone() = 0;

  /**
   * Splits the metadata of this object at the given address and
   * creates a new MemoryMappedObject that begins at the page of
   * the split.
   */
  virtual MemoryMappedObject* split(uintptr_t at) = 0;

  /** Prepared slices transfer existing page ownership only after publication. */
  virtual MemoryMappedObject* stageSlice(uintptr_t source, size_t sourceLength,
                                         uintptr_t destination, size_t destinationLength) = 0;
  void setMappingOwnership(bool ownsMappings) {
    m_OwnsMappings = ownsMappings;
  }
  virtual void releaseDetachedPage(uintptr_t oldAddress,
                                   const VirtualAddressSpace::DetachedPage& page) = 0;
  virtual void discardRange(VirtualAddressSpace& space, uintptr_t base, size_t length) = 0;
  virtual File* backingFile() const {
    return nullptr;
  }
  virtual bool backingRangeValid(uintptr_t source, size_t length) const {
    return true;
  }
  virtual bool resident(VirtualAddressSpace& space, uintptr_t address, FileResidencyAccess access,
                        void* credentials);

  /**
   * Removes pages from the start of this MemoryMappedObject.
   *
   * To remove pages from the middle or end of a MemoryMappedObject,
   * use split() and then remove()/unmap() on the returned object).
   *
   * \return true if the remove() has effectively removed the
   *         entire MemoryMappedObject, false otherwise.
   */
  virtual bool remove(size_t length) = 0;

  /**
   * Sets permissions on this object.
   *
   * Resident pages retain their contents and ownership.
   */
  virtual void setPermissions(Permissions perms) = 0;

  virtual bool preparePermissions(uintptr_t base, size_t length, Permissions perms) {
    return true;
  }

  /**
   * Sync back the given page to a backing store, if one exists.
   */
  virtual bool sync(uintptr_t at, bool async) {
    return true;
  }

  /** Invalidate cached state without discarding private modifications. */
  virtual void invalidate(uintptr_t at) {}

  virtual bool sharedBacking(uintptr_t at, uintptr_t& identity, size_t& offset) const {
    return false;
  }

  virtual bool usesBacking(uintptr_t identity) const {
    return false;
  }
  virtual bool beyondBackingEnd(uintptr_t at) const {
    return false;
  }
  virtual void discardFilePages(VirtualAddressSpace& space, size_t end) {}

  /**
   * Unmaps existing mappings in this object from the address space.
   *
   * Implementations are expected to track these as necessary for
   * the implementation.
   */
  virtual void unmap() = 0;

  /**
   * Trap entry
   *
   * Implement this in your implementation to actually perform
   * the mapping of memory into the address space.
   * \return true if the trap was successful, false otherwise.
   */
  virtual bool trap(VirtualAddressSpace& space, uintptr_t address, bool bWrite,
                    PopulationStatus* population = nullptr) = 0;
  virtual PopulationStatus prepareResidentAccess(VirtualAddressSpace& space, uintptr_t address) {
    return PopulationStatus::Success;
  }
  PopulationStatus populatePage(VirtualAddressSpace& space, uintptr_t address);
  MemoryLockMode lockMode() const {
    return m_LockMode;
  }

  /**
   * Release memory that can be released.
   *
   * Default implementation returns 'no pages released'.
   */
  virtual bool supportsPageOut(VirtualAddressSpace& space, uintptr_t address) {
    return false;
  }
  virtual SwapStatus pageOutAt(VirtualAddressSpace& space, uintptr_t address, bool& released) {
    return SwapStatus::Unsupported;
  }
  virtual bool reclaimAnonymousPage(VirtualAddressSpace& space) {
    return false;
  }
  virtual SwapStatus restoreSwapPages(VirtualAddressSpace& space) {
    return SwapStatus::Success;
  }
  virtual bool compact() {
    return false;
  }

  /**
   * Determines if the given address is within this object's mapping.
   */
  bool matches(uintptr_t address) {
    return (m_Address <= address) && (address < (m_Address + m_Length));
  }

  /**
   * Getter for base address.
   */
  uintptr_t address() const {
    return m_Address;
  }

  /**
   * Getter for length.
   */
  size_t length() const {
    return m_Length;
  }

  /** Permissions granted to this mapping. */
  Permissions permissions() const {
    return m_Permissions;
  }

  Permissions maximumPermissions() const {
    return m_MaximumPermissions;
  }

 protected:
  /**
   * Is this a Copy-on-Write mapping?
   *
   * A non-copy-on-write mapping will cause writes to hit the
   * backing store directly. This makes sense for some types of
   * mapping and not for others.
   */
  bool m_bCopyOnWrite;

  /**
   * Base address of this mapping.
   *
   * The region from base -> base+length will trap into this
   * object when an access attempt is made that faults.
   */
  uintptr_t m_Address;

  /**
   * Size of the object.
   *
   * To clarify, this is the size of the mapping itself, not of
   * the entire backing object.
   */
  size_t m_Length;

  /**
   * Permissions for mappings created by this object.
   *
   * For example, 'None' would mean that a trap will NEVER succeed,
   * and that no premapping will be done. 'Read' might mean that
   * writes will never magically map in a page for writing to.
   *
   * 'Exec' only works on systems that support this (eg, x86_64).
   */
  Permissions m_Permissions;
  Permissions m_MaximumPermissions;
  SharedPointer<MappingAttachment> m_Attachment;
  // The mapping gate protects registration; paging additionally admits a
  // short scheduler lease before dereferencing this process identity.
  Process* m_OwnerProcess;
  bool m_OwnsMappings;
  MemoryLockMode m_LockMode;
};

/**
 * Anonymous memory map object.
 *
 * Anonymous memory maps do not map back to an actual file; they back
 * onto a common page full of zeroes with forced copy-on-write. This
 * is perfect for mapping in large .bss sections in binaries or for
 * getting huge amounts of zeroed memory.
 */
class EXPORTED_PUBLIC AnonymousMemoryMap : public MemoryMappedObject {
 public:
  AnonymousMemoryMap(uintptr_t address, size_t length, Permissions perms);

  virtual ~AnonymousMemoryMap() override;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  static void setCloneFailureForTest(ssize_t after);
#endif

  virtual MemoryMappedObject* clone() override;
  virtual MemoryMappedObject* split(uintptr_t at) override;
  MemoryMappedObject* stageSlice(uintptr_t source, size_t sourceLength, uintptr_t destination,
                                 size_t destinationLength) override;
  void releaseDetachedPage(uintptr_t oldAddress,
                           const VirtualAddressSpace::DetachedPage& page) override;
  void discardRange(VirtualAddressSpace& space, uintptr_t base, size_t length) override;
  virtual bool remove(size_t length) override;

  virtual void setPermissions(MemoryMappedObject::Permissions perms) override;

  virtual void unmap() override;

  virtual bool trap(VirtualAddressSpace& space, uintptr_t address, bool bWrite,
                    PopulationStatus* population = nullptr) override;
  bool supportsPageOut(VirtualAddressSpace& space, uintptr_t address) override;
  SwapStatus pageOutAt(VirtualAddressSpace& space, uintptr_t address, bool& released) override;
  bool reclaimAnonymousPage(VirtualAddressSpace& space) override {
    return pageOut(space);
  }
  SwapStatus restoreSwapPages(VirtualAddressSpace& space) override {
    return restoreAll(space);
  }
  PopulationStatus prepareResidentAccess(VirtualAddressSpace& space, uintptr_t address) override;

 private:
  friend class MemoryMapManager;
  struct Page {
    uintptr_t address = 0;
    SwapReference slot;
    bool pagingBlocked = false;
  };
  bool pageOut(VirtualAddressSpace& space);
  SwapStatus restorePage(VirtualAddressSpace& space, Page& page);
  SwapStatus restoreAll(VirtualAddressSpace& space);
  static physical_uintptr_t m_Zero;
  static bool initialisePhysicalPage(physical_uintptr_t physical);

  void unmapUnlocked();

  /** List of existing virtual addresses we've mapped in. */
  List<Page> m_Mappings;
};

/**
 * File map object.
 *
 * File maps actually provide a backing file for a memory region. These
 * are used for loading binaries or for opening files for reading without
 * the overhead of read/write syscalls (assuming the syscall is more
 * expensive than a page fault).
 */
class MemoryMappedFile : public MemoryMappedObject {
  friend class MemoryMapManager;

 public:
  MemoryMappedFile(
      uintptr_t address, size_t length, size_t offset, File* backing, bool bCopyOnWrite,
      Permissions perms, Permissions maximumPerms = Read | Write | Exec,
      const SharedPointer<MappingAttachment>& attachment = SharedPointer<MappingAttachment>(),
      const FileMappingOrigin& origin = {});

  virtual ~MemoryMappedFile() override;

  virtual MemoryMappedObject* clone() override;
  virtual MemoryMappedObject* split(uintptr_t at) override;
  MemoryMappedObject* stageSlice(uintptr_t source, size_t sourceLength, uintptr_t destination,
                                 size_t destinationLength) override;
  void releaseDetachedPage(uintptr_t oldAddress,
                           const VirtualAddressSpace::DetachedPage& page) override;
  void discardRange(VirtualAddressSpace& space, uintptr_t base, size_t length) override;
  File* backingFile() const override {
    return m_pBacking;
  }
  bool backingRangeValid(uintptr_t source, size_t length) const override;
  bool resident(VirtualAddressSpace& space, uintptr_t address, FileResidencyAccess access,
                void* credentials) override;
  virtual bool remove(size_t length) override;

  virtual void setPermissions(MemoryMappedObject::Permissions perms) override;

  virtual bool sync(uintptr_t at, bool async) override;
  virtual void invalidate(uintptr_t at) override;
  virtual bool sharedBacking(uintptr_t at, uintptr_t& identity, size_t& offset) const override;
  virtual bool usesBacking(uintptr_t identity) const override;
  virtual bool beyondBackingEnd(uintptr_t at) const override;
  virtual void discardFilePages(VirtualAddressSpace& space, size_t end) override;
  virtual bool preparePermissions(uintptr_t base, size_t length, Permissions perms) override;

  virtual void unmap() override;

  virtual bool trap(VirtualAddressSpace& space, uintptr_t address, bool bWrite,
                    PopulationStatus* population = nullptr) override;

  /**
   * Syncs back all dirty pages to their respective backing store.
   *
   * Compacting begins by doing a pass to find any pages that can be
   * synced and then unpinned, allowing a future Cache eviction.
   * Should that pass successfully free memory, the compact will be
   * considered successful.
   *
   * If that pass is not successful, read-only pages are evicted. At this
   * stage, this is done in a linear fashion; there is no LRU or other
   * type of algorithm at play.
   * \todo Improve this.
   *
   * \return true if at least one page was released, false otherwise.
   */
  virtual bool compact() override;

 private:
  void unmapUnlocked();

  void releaseDetachedPageUnlocked(uintptr_t oldAddress,
                                   const VirtualAddressSpace::DetachedPage& page);

  /** Track a new mapping. */
  void trackMapping(uintptr_t, physical_uintptr_t);

  /** Stop tracking a mapping. */
  void untrackMapping(uintptr_t);

  /** Get a specific mapping. */
  physical_uintptr_t getMapping(uintptr_t);

  /** Get the number of mappings we currently have. */
  size_t getMappingCount();

  /** Clear all mappings. */
  void clearMappings();

  /** Backing file. */
  File* m_pBacking;

  /** Offset within the file that this mapping begins at. */
  size_t m_Offset;

  /** List of existing mappings. */
  Tree<uintptr_t, physical_uintptr_t> m_Mappings;
  FileMappingOrigin m_Origin;

  /**
   * Lock for anything to do with the memory mapped file.
   *
   * Backing-file reads and synchronisation may block, so this must be a
   * sleeping lock rather than a Spinlock.
   */
  Mutex m_Lock;

  /** Whether this mapping retained an established VFS File owner. */
  bool m_bVfsLease;
};

/**
 * This class is a multiplexing trap handler, to handle traps for
 * MemoryMappedObjects, dispatching them to the right place.
 */
class EXPORTED_PUBLIC MemoryMapManager : public MemoryTrapHandler, public MemoryPressureHandler {
  friend class PosixSubsystem;
  friend class VfsUserMemoryPolicy;

 public:
  enum class Placement {
    Hint,
    FixedReplace,
    FixedNoReplace,
  };

  enum class MapStatus {
    Success,
    NoMemory,
    AddressInUse,
    PolicyDenied,
    LockLimit,
  };

  enum class VmStatus { Success, InvalidRange, Unmapped, Unsupported, NoMemory, LockLimit };
  enum class FileRemapStatus {
    Success,
    InvalidRange,
    Unsupported,
    NoMemory,
    PolicyDenied,
    LockLimit,
    PermissionDenied
  };
  FileRemapStatus remapFilePages(uintptr_t base, size_t length, size_t byteOffset, bool nonblock);
  struct RemapRequest {
    uintptr_t source, destination;
    size_t oldLength, newLength;
    bool mayMove, fixed;
  };
  VmStatus remap(const RemapRequest& request, uintptr_t& result);
  VmStatus residency(uintptr_t base, size_t length, unsigned char* kernelVector,
                     FileResidencyAccess access, void* credentials);
  VmStatus discard(uintptr_t base, size_t length);

  enum class UserPageCopyStatus { Success, Inaccessible, NoMemory, IoError, Unsupported };
  UserPageCopyStatus copyUserPage(VirtualAddressSpace& space, uintptr_t userAddress,
                                  void* kernelBuffer, size_t bytes, bool write);

  PopulationStatus populateMemory(VirtualAddressSpace& space, uintptr_t base, size_t length);
  void bindMemoryLockPolicy(VirtualAddressSpace& space);
  MemoryLockStatus lockMemory(VirtualAddressSpace& space, uintptr_t base, size_t length,
                              MemoryLockMode mode, bool privileged);
  MemoryLockStatus lockAllMemory(VirtualAddressSpace& space, bool current,
                                 MemoryLockMode currentMode, MemoryLockMode futureMode,
                                 bool privileged);
  bool hasLockedMemory(VirtualAddressSpace& space, uintptr_t base, size_t length);

  /** Singleton instance */
  static MemoryMapManager& instance() {
    return m_Instance;
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  /** Deterministic contention control for the hosted teardown regression. */
  void acquireLifecycleGateForHostedTest();
  void releaseLifecycleGateForHostedTest();
  bool trapForHostedTest(uintptr_t address, bool bIsWrite, bool bWasPresent);
  const void* lifecycleGateAddressForHostedTest() const {
    return static_cast<const Semaphore*>(&m_LifecycleLock);
  }
#endif

  /**
   * Map in the given File.
   */
  MemoryMappedObject* mapFile(File* pFile, uintptr_t& address, size_t length,
                              MemoryMappedObject::Permissions perms, size_t offset = 0,
                              bool bCopyOnWrite = true);

  /** Retain an outer OperationGuard when using this query before publication. */
  bool hasSharedWriteCapability(File* backing);

  MemoryMappedObject* mapFile(
      File* pFile, uintptr_t& address, size_t length, MemoryMappedObject::Permissions perms,
      size_t offset, bool bCopyOnWrite, Placement placement, MapStatus* status,
      MemoryMappedObject::Permissions maximumPerms = MemoryMappedObject::Read |
                                                     MemoryMappedObject::Write |
                                                     MemoryMappedObject::Exec,
      const SharedPointer<MappingAttachment>& attachment = SharedPointer<MappingAttachment>(),
      MemoryLockMode requestedLock = MemoryLockMode::None, const FileMappingOrigin& origin = {});

  /**
   * Create a new anonymous memory mapping.
   */
  MemoryMappedObject* mapAnon(uintptr_t& address, size_t length,
                              MemoryMappedObject::Permissions perms);

  MemoryMappedObject* mapAnon(uintptr_t& address, size_t length,
                              MemoryMappedObject::Permissions perms, Placement placement,
                              MapStatus* status,
                              MemoryLockMode requestedLock = MemoryLockMode::None);

  /**
   * Registers the current address space's mappings with the target
   * process.
   * \param pTarget The process to clone into.
   */
  bool clone(Process* pTarget);
  SwapStatus pageOutRange(uintptr_t base, size_t length);
  SwapStatus activateSwap(uint32_t endpoint);
  SwapStatus deactivateSwap(uint32_t endpoint);
  SwapSnapshot swapSnapshot();
  bool operationOwnedByCurrentExecution();

  /**
   * Removes the given range from whatever objects might own them,
   * and will cross object boundaries if necessary.
   *
   * If this will result in a MemoryMappedObject being completely
   * unmapped, it will be removed.
   *
   * \return number of objects affected by this call.
   */
  size_t remove(uintptr_t base, size_t length);

  /**
   * Removes mappings and returns their virtual-address reservations to the
   * current process. Unmapped holes in the requested range are left alone.
   *
   * This is the munmap path. MAP_FIXED replacement uses remove() so that the
   * replacement inherits the reservations of the mappings it displaces.
   */
  size_t removeAndRelease(uintptr_t base, size_t length, VmStatus* status = nullptr);

  /** Find a logical attachment even when its first fragment was unmapped. */
  SharedPointer<MappingAttachment> findAttachment(uintptr_t base);

  /** Remove only this attachment's fragments, preserving replacement mappings. */
  size_t removeAttachment(const SharedPointer<MappingAttachment>& attachment);

  /**
   * Adjusts permissions across the given range, crossing object
   * boundaries if necessary.
   *
   * \return number of objects affected by this call.
   */
  enum class ProtectStatus { Success, Unmapped, AccessDenied, InvalidRange, Unsupported, NoMemory };

  size_t setPermissions(uintptr_t base, size_t length, MemoryMappedObject::Permissions perms,
                        ProtectStatus* status = nullptr);

  /**
   * Returns true if at least one memory mapped object is in the range
   * given, false otherwise.
   */
  bool contains(uintptr_t base, size_t length);

  /**
   * Returns true if memory-map objects cover the full range with all of the
   * requested permissions.
   */
  bool allows(uintptr_t base, size_t length, MemoryMappedObject::Permissions permissions);

  bool sharedBacking(Process* process, uintptr_t address, uintptr_t& identity, size_t& offset);

  bool faultIn(uintptr_t address, bool write);

  enum class FaultResolution { Unhandled, Resolved, BackingFault };
  FaultResolution resolveUserFault(uintptr_t address, bool write, bool wasPresent, bool execute);

  enum class ResizeStatus { Ready, NoMemory, Unsupported, Invalid };
  class PreparedFileResize {
   public:
    ~PreparedFileResize();
    const Cache::DiscardReference* loans() const;
    size_t loanCount() const;
    size_t totalLoans() const;
    void commit();

   private:
    friend class MemoryMapManager;
    struct Data;
    explicit PreparedFileResize(Data* data);
    NOT_COPYABLE_OR_ASSIGNABLE(PreparedFileResize);
    UniquePointer<Data> m_Data;
  };

  /** Caller retains writer, operation and backing-data locks through planning,
   * backend/cache preparation and commit. Preparation does not alter PTEs. */
  ResizeStatus prepareFileResize(File* file, size_t oldSize, size_t newSize,
                                 UniquePointer<PreparedFileResize>& result);

  /**
   * Syncs memory mapped objects within the given range back to
   * their backing store, if they have one.
   */
  bool sync(uintptr_t base, size_t length, bool async, int* error = nullptr);

  /** Invalidates backing-cache state where required by the mapping. */
  void invalidate(uintptr_t base, size_t length);

  /**
   * Removes the mappings for the given object from the address space.
   */
  void unmap(MemoryMappedObject* pObj);

  /**
   * Removes all mappings from this address space.
   */
  void unmapAll();

  /**
   * Trap handler, called when a fault takes place.
   */
  virtual bool trap(InterruptState& state, uintptr_t address, bool bIsWrite, bool bWasPresent);

  /**
   * Trigger a compact in all address spaces.
   *
   * This may switch in and out of several address spaces.
   */
  virtual bool compact();

  virtual const char* getMemoryPressureDescription() {
    return "Unmap safe pages from memory mapped files.";
  }

 protected:
  /**
   * Removes all mappings from the address space, unlocked.
   *
   * Requires callers to have acquired the lock by other means.
   */
  void unmapAllUnlocked();

  /**
   * Acquire the manager's operation gate.
   *
   * Take this before becoming unscheduleable when a caller needs to invoke
   * unmapAllUnlocked. Contention sleeps through the operation barrier rather
   * than spinning until the gate becomes available.
   */
  void acquireLock();

  /**
   * Release the manager's operation gate.
   */
  void releaseLock();

 public:
  /**
   * A terminal-safe, same-thread recursive gate for manager operations.
   *
   * The gate keeps object lists and object lifetimes stable while allowing
   * the short cache Spinlock to be released before invoking an object.
   * Try-only acquisition excludes recursive entry for pressure recovery.
   */
  class OperationGuard {
   public:
    explicit OperationGuard(MemoryMapManager& manager, bool tryOnly = false);
    ~OperationGuard();

    explicit operator bool() const {
      return m_Acquired;
    }

   private:
    NOT_COPYABLE_OR_ASSIGNABLE(OperationGuard);

    Uninterruptible m_EventDeferral;
    TerminationDeferral m_TerminationDeferral;
    MemoryMapManager& m_Manager;
    bool m_Acquired;
  };

 private:
  /** Default and only constructor. Registers with PageFaultHandler. */
  MemoryMapManager();
  ~MemoryMapManager();

  class LockPlan;
  MemoryLockStatus prepareManagedLocks(VirtualAddressSpace& space, uintptr_t base, size_t length,
                                       MemoryLockMode mode, bool all,
                                       UniquePointer<PreparedMemoryLock>& result);
  MemoryMappedObject* publishMapping(File* file, uintptr_t& address, size_t length,
                                     MemoryMappedObject::Permissions perms, size_t offset,
                                     bool copyOnWrite, Placement placement, MapStatus* status,
                                     MemoryMappedObject::Permissions maximumPerms,
                                     const SharedPointer<MappingAttachment>& attachment,
                                     MemoryLockMode requestedLock,
                                     const FileMappingOrigin& origin = {});
  void retireLockedPages(VirtualAddressSpace& space, size_t pages);

  void enterOperation();
  bool tryEnterOperation();
  void leaveOperation();

  size_t removeInternal(uintptr_t base, size_t length, bool releaseReservations,
                        VmStatus* status = nullptr);
  void releaseReservation(Process* process, VirtualAddressSpace& addressSpace, uintptr_t base,
                          size_t length);
  bool handleTrap(uintptr_t address, bool bIsWrite, bool bWasPresent, bool execute = false);

  enum Ops {
    Sync,
    Invalidate,
  };

  bool op(Ops what, uintptr_t base, size_t length, bool async);

  /** Singleton instance. */
  static MemoryMapManager m_Instance;

  typedef List<MemoryMappedObject*> MmObjectList;

  /** Cache of virtual address spaces -> MmObjectLists. */
  Tree<VirtualAddressSpace*, MmObjectList*> m_MmObjectLists;

  /** Lock for the cache. */
  Spinlock m_Lock;

  /**
   * Keeps the object-list topology and object lifetimes stable while an
   * operation is running without holding m_Lock across object callbacks.
   */
  Mutex m_LifecycleLock;

  /** Protects recursive lifecycle-gate ownership metadata. */
  Spinlock m_LifecycleStateLock;

  void* m_pLifecycleOwner;
  size_t m_LifecycleDepth;
};

/** @} */

#endif
