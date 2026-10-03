/* Copyright (c) 2026, Pedigree Developers. */
#include "landlock.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "memfd-file.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/MountView.h"
#include "namespace-file.h"
#include "sandbox-state.h"

namespace {
constexpr size_t MaximumRules = 4096;
constexpr size_t MaximumLayers = 16;
constexpr int BadDescriptorState = 77;
constexpr uint64_t FileRights = LandlockAccess::Execute | LandlockAccess::ReadFile |
                                LandlockAccess::WriteFile | LandlockAccess::Truncate;

class LandlockResult {
 public:
  ~LandlockResult() {
    syscallError(error);
  }
  int finish(int value) {
    error = value < 0 ? Processor::information().getCurrentThread()->getErrno() : 0;
    return value;
  }

 private:
  TerminationDeferral lifetime;
  size_t error = 0;
};

PosixSubsystem* subsystem() {
  auto* process = Processor::information().getCurrentThread()->getParent();
  return process->getType() == Process::Posix
             ? static_cast<PosixSubsystem*>(process->getSubsystem())
             : nullptr;
}

class RulesetFilesystem final : public RamFs {
 public:
  RulesetFilesystem() {
    setProcessOwnership(false);
  }
};
RulesetFilesystem rulesetFilesystem;

class RulesetFile final : public File {
 public:
  explicit RulesetFile(uint64_t requested)
      : File(String("landlock-ruleset"), 0, 0, 0, 0, &rulesetFilesystem, 0, nullptr),
        handled(requested) {}
  Mutex lock;
  Vector<LandlockRule> rules;
  const uint64_t handled;

  bool isSeekable() const override {
    return false;
  }
  bool allowMapping(bool, bool, bool&) override {
    SYSCALL_ERROR(NoSuchDevice);
    return false;
  }

 protected:
  bool allowResize(size_t, size_t) override {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  bool isBytewise() const override {
    return true;
  }
  uint64_t readBytewise(uint64_t, uint64_t, uintptr_t, bool) override {
    SYSCALL_ERROR(InvalidArgument);
    return 0;
  }
  uint64_t writeBytewise(uint64_t, uint64_t, uintptr_t, bool) override {
    SYSCALL_ERROR(InvalidArgument);
    return 0;
  }
};

RulesetFile* acquireRuleset(int fd, DescriptorLease& descriptor) {
  auto* current = subsystem();
  if (!current || fd < 0 || !current->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return nullptr;
  }
  auto* file = descriptor->getFile();
  if (!file || file->getFilesystem() != &rulesetFilesystem) {
    syscallError(BadDescriptorState);
    return nullptr;
  }
  return static_cast<RulesetFile*>(file);
}

uint64_t inodeIdentity(File* file) {
  const auto attributes = file->getAttributes();
  return attributes.inode ? attributes.inode : file->getInode();
}

bool sameObject(const LandlockRule& rule, const FilesystemPathRef& path) {
  if (!rule.anchor || !path) {
    return false;
  }
  File* candidate = path->node();
  File* anchor = rule.anchor->node();
  return candidate == anchor || (candidate->getFilesystem() == anchor->getFilesystem() &&
                                 rule.inode && inodeIdentity(candidate) == rule.inode);
}
}  // namespace

uint64_t LandlockDomain::layerAccess(const FilesystemPathRef* ancestry, size_t count) const {
  uint64_t allowed = ~handled;
  for (size_t i = 0; i < count; ++i) {
    for (const auto& rule : rules) {
      if (sameObject(rule, ancestry[i])) {
        allowed |= rule.access;
      }
    }
  }
  return allowed;
}

bool PosixSubsystem::filesystemConstrained() const {
  return static_cast<bool>(posix_sandbox_domain());
}

uint64_t PosixSubsystem::filesystemAccess(const FilesystemPathRef* ancestry, size_t count) const {
  auto* view = count ? VfsMountView::fromPath(ancestry[0]) : nullptr;
  if (count == 1 && ancestry[0] && view && !view->attachmentId(ancestry[0])) {
    File* file = ancestry[0]->node();
    UtsRef space;
    if (MemFdFile::fromFile(file) || (file->isPipe() && !file->isFifo()) || file->isSocket() ||
        file->getFilesystem() == &rulesetFilesystem || posix_uts_file_namespace(file, space)) {
      return ~uint64_t(0);
    }
    return 0;
  }
  uint64_t allowed = ~uint64_t(0);
  for (auto domain = posix_sandbox_domain(); domain; domain = domain->previous) {
    allowed &= domain->layerAccess(ancestry, count);
  }
  return allowed;
}

bool PosixSubsystem::filesystemReparent(const FilesystemPathRef* source, size_t sourceCount,
                                        const FilesystemPathRef* destination,
                                        size_t destinationCount) const {
  for (auto domain = posix_sandbox_domain(); domain; domain = domain->previous) {
    const uint64_t before = domain->layerAccess(source, sourceCount);
    const uint64_t after = domain->layerAccess(destination, destinationCount);
    if (after & ~before & LandlockAccess::All) {
      return false;
    }
  }
  return true;
}

bool posix_landlock_check(const FilesystemPathRef& path, uint64_t requested) {
  if (!posix_sandbox_domain()) {
    return true;
  }
  auto* view = VfsMountView::fromPath(path);
  if (!view || !path) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  return view->checkFilesystemAccess(path, requested);
}

bool posix_landlock_open(const FilesystemPathRef& path, int flags, bool& allowTruncate) {
  allowTruncate = true;
  if ((flags & O_PATH) || !posix_sandbox_domain()) {
    return true;
  }
  auto* view = VfsMountView::fromPath(path);
  if (!path || !view) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  const uint64_t allowed = view->filesystemAccess(path);
  allowTruncate = allowed & LandlockAccess::Truncate;
  File* file = path->node();
  uint64_t required = 0;
  if (file->isDirectory()) {
    required = LandlockAccess::ReadDir;
  } else {
    if ((flags & O_ACCMODE) != O_WRONLY) {
      required |= LandlockAccess::ReadFile;
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
      required |= LandlockAccess::WriteFile;
    }
    if ((flags & O_TRUNC) && file->supportsRegularFileOperations()) {
      required |= LandlockAccess::Truncate;
    }
  }
  if ((allowed & required) != required) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  return true;
}

int posix_landlock_create_ruleset(const void* attributes, size_t size, unsigned flags) {
  LandlockResult result;
  if (flags) {
    if (flags != 1 || attributes || size) {
      SYSCALL_ERROR(InvalidArgument);
      return result.finish(-1);
    }
    return result.finish(3);
  }
  if (!attributes) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  if (size < sizeof(uint64_t) || size > 4096) {
    syscallError(size > 4096 ? Error::TooBig : Error::InvalidArgument);
    return result.finish(-1);
  }
  uint64_t handled;
  if (!PosixSubsystem::copyFromUser(&handled, attributes, sizeof(handled))) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  uint8_t bytes[64];
  for (size_t offset = sizeof(handled); offset < size;) {
    const size_t amount = size - offset < sizeof(bytes) ? size - offset : sizeof(bytes);
    const uintptr_t base = reinterpret_cast<uintptr_t>(attributes);
    if (base > ~uintptr_t(0) - offset ||
        !PosixSubsystem::copyFromUser(bytes, reinterpret_cast<const void*>(base + offset),
                                      amount)) {
      SYSCALL_ERROR(BadAddress);
      return result.finish(-1);
    }
    for (size_t i = 0; i < amount; ++i) {
      if (bytes[i]) {
        SYSCALL_ERROR(TooBig);
        return result.finish(-1);
      }
    }
    offset += amount;
  }
  if (!handled || (handled & ~LandlockAccess::All)) {
    syscallError(handled ? Error::InvalidArgument : Error::NoMessage);
    return result.finish(-1);
  }
  auto* current = subsystem();
  auto* file = current ? new RulesetFile(handled) : nullptr;
  if (!file || !VFS::instance().tryTrackFile(file)) {
    delete file;
    SYSCALL_ERROR(OutOfMemory);
    return result.finish(-1);
  }
  RetainedFile retained;
  retained.adopt(file);
  auto* descriptor = new FileDescriptor(file, 0, 0xffffffff, FD_CLOEXEC, O_RDWR);
  if (!descriptor || !descriptor->acquireOpenFileDescription()) {
    delete descriptor;
    SYSCALL_ERROR(OutOfMemory);
    return result.finish(-1);
  }
  DescriptorLease published;
  return result.finish(static_cast<int>(current->installFileDescriptor(descriptor, published)));
}

int posix_landlock_add_rule(int ruleset, int type, const void* attributes, unsigned flags) {
  LandlockResult result;
  if (flags || type != 1) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  DescriptorLease rulesetDescriptor;
  auto* file = acquireRuleset(ruleset, rulesetDescriptor);
  if (!file) {
    return result.finish(-1);
  }
  struct __attribute__((packed)) PathRule {
    uint64_t allowed;
    int32_t parentFd;
  } input;
  static_assert(sizeof(input) == 12, "Linux Landlock path rule ABI");
  if (!PosixSubsystem::copyFromUser(&input, attributes, sizeof(input))) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  if (!input.allowed || (input.allowed & ~file->handled)) {
    syscallError(input.allowed ? Error::InvalidArgument : Error::NoMessage);
    return result.finish(-1);
  }
  DescriptorLease parent;
  if (input.parentFd < 0 || !subsystem()->acquireFileDescriptor(input.parentFd, parent)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return result.finish(-1);
  }
  auto anchor = parent->openingPath();
  auto* view = VfsMountView::fromPath(anchor);
  if (!anchor || !view || !view->attachmentId(anchor) || anchor->node()->isSymlink()) {
    syscallError(BadDescriptorState);
    return result.finish(-1);
  }
  if (!anchor->node()->isDirectory() && (input.allowed & ~FileRights)) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  LandlockRule rule{anchor, inodeIdentity(anchor->node()), input.allowed};
  LockGuard<Mutex> guard(file->lock);
  for (auto& existing : file->rules) {
    if (sameObject(existing, anchor)) {
      existing.access |= input.allowed;
      return result.finish(0);
    }
  }
  if (file->rules.count() >= MaximumRules || !file->rules.tryReserve(file->rules.count() + 1)) {
    SYSCALL_ERROR(OutOfMemory);
    return result.finish(-1);
  }
  file->rules.pushBack(pedigree_std::move(rule));
  return result.finish(0);
}

int posix_landlock_restrict_self(int ruleset, unsigned flags) {
  LandlockResult result;
  if (flags) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  if (!posix_no_new_privs()) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return result.finish(-1);
  }
  DescriptorLease descriptor;
  auto* file = acquireRuleset(ruleset, descriptor);
  if (!file) {
    return result.finish(-1);
  }
  auto previous = posix_sandbox_domain();
  if (previous && previous->depth >= MaximumLayers) {
    SYSCALL_ERROR(TooBig);
    return result.finish(-1);
  }
  auto domain = SharedPointer<LandlockDomain>::tryAdopt(new LandlockDomain);
  if (!domain) {
    SYSCALL_ERROR(OutOfMemory);
    return result.finish(-1);
  }
  domain->previous = previous;
  domain->depth = previous ? previous->depth + 1 : 1;
  // REFER is always denied unless explicitly granted, including ABI-1 rulesets.
  domain->handled = file->handled | LandlockAccess::Refer;
  {
    LockGuard<Mutex> guard(file->lock);
    if (!domain->rules.tryReserve(file->rules.count())) {
      SYSCALL_ERROR(OutOfMemory);
      return result.finish(-1);
    }
    for (const auto& rule : file->rules) {
      domain->rules.pushBack(rule);
    }
  }
  return result.finish(posix_sandbox_restrict(domain) ? 0 : -1);
}
