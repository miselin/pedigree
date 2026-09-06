/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include <errno.h>

#include "PosixSubsystem.h"
#include "ipc-common.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/VFS.h"
#include "sysv-shm-syscalls.h"

namespace {
constexpr size_t MaximumSegments = 128;
constexpr size_t MaximumSegmentBytes = 16 * 1024 * 1024;
constexpr size_t MaximumTotalBytes = 64 * 1024 * 1024;
constexpr size_t MaximumAttachments = 128;
constexpr int Create = 01000, Exclusive = 02000, HugePages = 04000, NoReserve = 010000;
constexpr int ReadOnly = 010000, Round = 020000, Remap = 040000, Executable = 0100000;
constexpr int Remove = 0, Set = 1, Stat = 2, Info = 3;
constexpr int Lock = 11, Unlock = 12, SegmentStat = 13, SegmentInfo = 14, SegmentStatAny = 15;
constexpr unsigned Destroyed = 01000, Locked = 02000;

struct SegmentStatus {
  PosixIpc::Permission permission;
  uint64_t size;
  int64_t attachTime, detachTime, changeTime;
  int32_t creator, lastPid;
  uint64_t attachments, unused[2];
};
static_assert(sizeof(SegmentStatus) == 112, "Linux amd64 shmid_ds layout");
static_assert(__builtin_offsetof(SegmentStatus, attachments) == 88, "Linux shm_nattch offset");
struct SegmentLimits {
  uint64_t maximumSize, minimumSize, segmentCount, attachments, totalPages, unused[4];
};
struct SegmentUsage {
  int32_t usedIds, padding;
  uint64_t totalPages, residentPages, swappedPages, swapAttempts, swapSuccesses;
};
static_assert(sizeof(SegmentLimits) == 72 && sizeof(SegmentUsage) == 48, "Linux shm info layout");

struct Segment {
  Segment(int identifier, size_t slot, int32_t key, size_t size, size_t roundedSize, unsigned mode)
      : id(identifier),
        slot(slot),
        roundedSize(roundedSize),
        lockedPages(0),
        pendingAttachments(0),
        status(),
        filesystem(),
        file(new RamFile(String("shm"), identifier, &filesystem, nullptr)) {
    PosixIpc::initialize(status.permission, key, mode, identifier / MaximumSegments);
    status.size = size;
    status.creator = PosixIpc::process()->getId();
    status.changeTime = Time::getTime();
    VFS::instance().trackFile(file);
  }
  ~Segment() {
    unlock();
    VFS::instance().untrackFile(file);
  }
  void unlock() {
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    while (lockedPages) {
      file->returnPhysicalPage(--lockedPages * pageSize);
    }
    status.permission.mode &= ~Locked;
  }
  bool lock() {
    if (status.permission.mode & Locked) {
      return true;
    }
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    for (size_t offset = 0; offset < roundedSize; offset += pageSize) {
      if (file->read(offset, pageSize, 0) != pageSize || file->getPhysicalPage(offset) == ~0UL) {
        unlock();
        SYSCALL_ERROR(OutOfMemory);
        return false;
      }
      ++lockedPages;
    }
    status.permission.mode |= Locked;
    return true;
  }
  int id;
  size_t slot, roundedSize, lockedPages, pendingAttachments;
  SegmentStatus status;
  RamFs filesystem;
  RamFile* file;
};

class ShmAttachment;
// Mapping teardown invokes attachment callbacks synchronously. The recursive
// operation gate protects this registry as well as mapping publication/removal.
SharedPointer<Segment> segments[MaximumSegments];
List<ShmAttachment*> attachments;
size_t totalBytes = 0;
uint32_t nextSequence = 0;

void retire(const SharedPointer<Segment>& segment) {
  if ((segment->status.permission.mode & Destroyed) && !segment->status.attachments &&
      !segment->pendingAttachments && segments[segment->slot] == segment) {
    totalBytes -= segment->roundedSize;
    segments[segment->slot].reset();
  }
}

class ShmAttachment final : public MappingAttachment {
 public:
  explicit ShmAttachment(const SharedPointer<Segment>& segment)
      : m_Segment(segment), m_Base(0), m_Pid(0), m_Active(false) {}
  ~ShmAttachment() override {
    MemoryMapManager::OperationGuard guard(MemoryMapManager::instance());
    if (!m_Active) {
      return;
    }
    for (auto it = attachments.begin(); it != attachments.end(); ++it) {
      if (*it == this) {
        attachments.erase(it);
        break;
      }
    }
    assert(m_Segment->status.attachments);
    --m_Segment->status.attachments;
    m_Segment->status.detachTime = Time::getTime();
    m_Segment->status.lastPid = m_Pid;
    retire(m_Segment);
  }
  void activate(uintptr_t base, size_t pid) {
    m_Base = base;
    m_Pid = pid;
    m_Active = true;
    ++m_Segment->status.attachments;
    m_Segment->status.attachTime = Time::getTime();
    m_Segment->status.lastPid = PosixIpc::process()->getId();
    attachments.pushBack(this);
  }
  uintptr_t baseAddress() const override {
    return m_Base;
  }
  void relocate(uintptr_t newBase) override {
    m_Base = newBase;
  }
  size_t pid() const {
    return m_Pid;
  }
  SharedPointer<MappingAttachment> clone(Process* target) override {
    auto* copy = new ShmAttachment(m_Segment);
    copy->activate(m_Base, target->getId());
    return SharedPointer<MappingAttachment>(copy);
  }

 private:
  SharedPointer<Segment> m_Segment;
  uintptr_t m_Base;
  size_t m_Pid;
  bool m_Active;
};

SharedPointer<Segment> findSegment(int id, bool index = false) {
  if (id >= 0) {
    const size_t slot = index ? static_cast<size_t>(id) : id % MaximumSegments;
    if (slot < MaximumSegments && segments[slot] && (index || segments[slot]->id == id)) {
      return segments[slot];
    }
  }
  SYSCALL_ERROR(InvalidArgument);
  return SharedPointer<Segment>();
}

int information(int command, void* buffer) {
  int highest = 0;
  SegmentUsage usage = {};
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  for (size_t i = 0; i < MaximumSegments; ++i) {
    if (segments[i]) {
      highest = i;
      ++usage.usedIds;
      usage.totalPages += segments[i]->roundedSize / pageSize;
      usage.residentPages += segments[i]->file->getAttributes().blocks * 512 / pageSize;
    }
  }
  if (command == Info) {
    const SegmentLimits limits = {
        MaximumSegmentBytes,          1, MaximumSegments, MaximumAttachments,
        MaximumTotalBytes / pageSize, {}};
    if (PosixSubsystem::copyToUser(buffer, &limits, sizeof(limits))) {
      return highest;
    }
  } else if (PosixSubsystem::copyToUser(buffer, &usage, sizeof(usage))) {
    return highest;
  }
  SYSCALL_ERROR(BadAddress);
  return -1;
}
}  // namespace

int posix_shmget(int32_t key, size_t size, int flags) {
  MemoryMapManager::OperationGuard guard(MemoryMapManager::instance());
  if (flags & HugePages) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  if (flags & ~(0777 | Create | Exclusive | NoReserve)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  size_t freeSlot = MaximumSegments;
  for (size_t i = 0; i < MaximumSegments; ++i) {
    if (!segments[i]) {
      if (freeSlot == MaximumSegments) {
        freeSlot = i;
      }
      continue;
    }
    if (key && segments[i]->status.permission.key == key) {
      if ((flags & (Create | Exclusive)) == (Create | Exclusive)) {
        SYSCALL_ERROR(FileExists);
        return -1;
      }
      if (size > segments[i]->status.size) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      const unsigned access = ((flags >> 6) | (flags >> 3) | flags) & 7;
      if (!PosixIpc::allowed(segments[i]->status.permission, access)) {
        SYSCALL_ERROR(PermissionDenied);
        return -1;
      }
      return segments[i]->id;
    }
  }
  if (key && !(flags & Create)) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  if (!size || size > MaximumSegmentBytes) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  const size_t pageMask = PhysicalMemoryManager::getPageSize() - 1;
  const size_t roundedSize = (size + pageMask) & ~pageMask;
  if (freeSlot == MaximumSegments || roundedSize > MaximumTotalBytes - totalBytes ||
      nextSequence > (0x7fffffffU - freeSlot) / MaximumSegments) {
    SYSCALL_ERROR(NoSpaceLeftOnDevice);
    return -1;
  }
  const int id = nextSequence++ * MaximumSegments + freeSlot;
  SharedPointer<Segment> segment(new Segment(id, freeSlot, key, size, roundedSize, flags & 0777));
  if (!segment->file->resize(roundedSize)) {
    return -1;
  }
  totalBytes += roundedSize;
  segments[freeSlot] = segment;
  return id;
}

void* posix_shmat(int id, const void* requestedAddress, int flags) {
  MemoryMapManager& mappings = MemoryMapManager::instance();
  MemoryMapManager::OperationGuard guard(mappings);
  void* const failed = reinterpret_cast<void*>(~static_cast<uintptr_t>(0));
  if ((flags & ~(ReadOnly | Round | Remap | Executable)) ||
      ((flags & Remap) && !requestedAddress)) {
    SYSCALL_ERROR(InvalidArgument);
    return failed;
  }
  auto segment = findSegment(id);
  if (!segment) {
    return failed;
  }
  unsigned access = 4 | (flags & ReadOnly ? 0 : 2) | (flags & Executable ? 1 : 0);
  if (!PosixIpc::allowed(segment->status.permission, access)) {
    SYSCALL_ERROR(PermissionDenied);
    return failed;
  }
  size_t count = 0;
  const size_t pid = PosixIpc::process()->getId();
  for (auto it = attachments.begin(); it != attachments.end(); ++it) {
    count += (*it)->pid() == pid;
  }
  if (count >= MaximumAttachments) {
    syscallError(EMFILE);
    return failed;
  }
  uintptr_t address = reinterpret_cast<uintptr_t>(requestedAddress);
  const size_t pageMask = PhysicalMemoryManager::getPageSize() - 1;
  if (flags & Round) {
    address &= ~pageMask;
  }
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  if ((requestedAddress && !address) || (address & pageMask) ||
      (address && (address < space.getUserStart() || address >= space.getKernelStart() ||
                   segment->roundedSize > space.getKernelStart() - address))) {
    SYSCALL_ERROR(InvalidArgument);
    return failed;
  }
  const auto placement = !requestedAddress ? MemoryMapManager::Placement::Hint
                         : flags & Remap   ? MemoryMapManager::Placement::FixedReplace
                                           : MemoryMapManager::Placement::FixedNoReplace;
  MemoryMappedObject::Permissions permissions = MemoryMappedObject::Read;
  if (!(flags & ReadOnly)) {
    permissions |= MemoryMappedObject::Write;
  }
  if (flags & Executable) {
    permissions |= MemoryMappedObject::Exec;
  }
  auto* owner = new ShmAttachment(segment);
  SharedPointer<MappingAttachment> attachment(owner);
  MemoryMapManager::MapStatus status;
  // A replacement may remove this segment's last older attachment. Keep its
  // identifier published until this new attachment either commits or fails.
  ++segment->pendingAttachments;
  if (!mappings.mapFile(segment->file, address, segment->roundedSize, permissions, 0, false,
                        placement, &status, permissions, attachment)) {
    --segment->pendingAttachments;
    retire(segment);
    syscallError(status == MemoryMapManager::MapStatus::AddressInUse ? Error::InvalidArgument
                                                                     : Error::OutOfMemory);
    return failed;
  }
  owner->activate(address, pid);
  --segment->pendingAttachments;
  return reinterpret_cast<void*>(address);
}

int posix_shmdt(const void* address) {
  MemoryMapManager& mappings = MemoryMapManager::instance();
  MemoryMapManager::OperationGuard guard(mappings);
  auto attachment = mappings.findAttachment(reinterpret_cast<uintptr_t>(address));
  if (!attachment || !mappings.removeAttachment(attachment)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return 0;
}

int posix_shmctl(int id, int command, void* buffer) {
  MemoryMapManager::OperationGuard guard(MemoryMapManager::instance());
  command &= ~0x100;
  if (command == Info || command == SegmentInfo) {
    return information(command, buffer);
  }
  const bool index = command == SegmentStat || command == SegmentStatAny;
  auto segment = findSegment(id, index);
  if (!segment) {
    return -1;
  }
  if (command == Stat || index) {
    if (command != SegmentStatAny && !PosixIpc::allowed(segment->status.permission, 4)) {
      SYSCALL_ERROR(PermissionDenied);
      return -1;
    }
    if (!PosixSubsystem::copyToUser(buffer, &segment->status, sizeof(segment->status))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return index ? segment->id : 0;
  }
  if (command != Remove && command != Set && command != Lock && command != Unlock) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!PosixIpc::owner(segment->status.permission)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  if (command == Remove) {
    segment->status.permission.mode |= Destroyed;
    segment->status.permission.key = 0;
    retire(segment);
  } else if (command == Lock) {
    if (!segment->lock()) {
      return -1;
    }
  } else if (command == Unlock) {
    segment->unlock();
  } else {
    SegmentStatus input = {};
    if (!PosixSubsystem::copyFromUser(&input, buffer, sizeof(input))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (input.permission.uid == 0xffffffffU || input.permission.gid == 0xffffffffU) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    segment->status.permission.uid = input.permission.uid;
    segment->status.permission.gid = input.permission.gid;
    segment->status.permission.mode =
        (segment->status.permission.mode & ~0777U) | (input.permission.mode & 0777);
    segment->status.changeTime = Time::getTime();
  }
  return 0;
}
