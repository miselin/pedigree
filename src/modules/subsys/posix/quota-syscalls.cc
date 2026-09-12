/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/FilesystemCredentials.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>

#include "DevFs-block.h"
#include "file-handle-syscalls.h"
#include "modules/system/vfs/Quota.h"
#include "quota-syscalls.h"

namespace {
constexpr uint32_t Sync = 0x800001, Enable = 0x800002, Disable = 0x800003;
constexpr uint32_t GetFormat = 0x800004, GetInfo = 0x800005, SetInfo = 0x800006;
constexpr uint32_t GetQuota = 0x800007, SetQuota = 0x800008;
static_assert(sizeof(QuotaRecord) == 72, "Linux dqblk size");
static_assert(__builtin_offsetof(QuotaRecord, valid) == 64, "Linux dqblk validity mask");

struct QuotaResult {
  QuotaResult(int result)
      : value(result),
        error(result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0) {}
  int value, error;
};

bool permitted(uint32_t command, QuotaType type, uint32_t id) {
  if (command == GetFormat || command == Sync)
    return true;
  if (posix_effective_root())
    return true;
  if (command == GetQuota) {
    Process* process = Processor::information().getCurrentThread()->getParent();
    int64_t uid = process->getEffectiveUserId(), gid = process->getEffectiveGroupId();
    if (uid < 0)
      uid = process->getUserId();
    if (gid < 0)
      gid = process->getGroupId();
    if (type == QuotaType::User && uid >= 0 && static_cast<uint64_t>(uid) == id)
      return true;
    FilesystemCredentials credentials;
    if (type == QuotaType::Group && gid >= 0 &&
        Process::currentFilesystemCredentials(credentials)) {
      credentials.gid = static_cast<uint32_t>(gid);
      if (credentials.inGroup(id))
        return true;
    }
  }
  SYSCALL_ERROR(NotEnoughPermissions);
  return false;
}

int syncAll(const QuotaRequest& request) {
  Vector<VFS::MountIdentity> mounts;
  if (!VFS::instance().snapshotDiskMounts(mounts)) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  int error = 0;
  for (const auto& identity : mounts) {
    VFS::FilesystemPin pin;
    if (!identity.pin(pin)) {
      if (!error)
        error = Error::DeviceDoesNotExist;
      continue;
    }
    QuotaResponse response;
    const auto status = pin.filesystem()->quotaControl(request, response);
    if (status != QuotaStatus::Unsupported && status != QuotaStatus::NotEnabled &&
        status != QuotaStatus::Success && !error)
      error = quotaError(status);
  }
  syscallError(error);
  return error ? -1 : 0;
}

QuotaResult control(int encoded, const char* special, int signedId, void* address) {
  TerminationDeferral lifetime;
  const uint32_t type = static_cast<uint32_t>(encoded) & 0xff;
  const uint32_t command = static_cast<uint32_t>(encoded) >> 8;
  if (type > 1) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  QuotaRequest request;
  request.type = type ? QuotaType::Group : QuotaType::User;
  request.id = static_cast<uint32_t>(signedId);
  switch (command) {
    case Sync:
      request.operation = QuotaOperation::Sync;
      break;
    case Enable:
      request.operation = QuotaOperation::Enable;
      request.format = request.id;
      break;
    case Disable:
      request.operation = QuotaOperation::Disable;
      break;
    case GetFormat:
      request.operation = QuotaOperation::GetFormat;
      break;
    case GetQuota:
      request.operation = QuotaOperation::Get;
      break;
    case SetQuota:
      request.operation = QuotaOperation::Set;
      break;
    case GetInfo:
    case SetInfo:
      SYSCALL_ERROR(OperationNotSupported);
      return -1;
    default:
      SYSCALL_ERROR(InvalidArgument);
      return -1;
  }
  if (!permitted(command, request.type, request.id))
    return -1;
  if ((command == GetQuota || command == SetQuota) && request.id == 0xffffffffU) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (command == SetQuota &&
      !PosixSubsystem::copyFromUser(&request.record, address, sizeof(request.record))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (!special) {
    if (command == Sync)
      return syncAll(request);
    SYSCALL_ERROR(DeviceDoesNotExist);
    return -1;
  }

  // Path resolution precedes backing admission and quota locks. The pin is
  // declared first so it outlives both retained path owners during cleanup.
  VFS::FilesystemPin filesystem;
  PosixHandleTarget device, quota;
  if (command == Enable && !quota.resolve(AT_FDCWD, static_cast<const char*>(address), true, false))
    return -1;
  if (!device.resolve(AT_FDCWD, special, true, false))
    return -1;
  if (!device.file->isBlockDevice()) {
    SYSCALL_ERROR(NotABlockDevice);
    return -1;
  }
  const uint64_t number = device.file->deviceNumber();
  VFS::MountIdentity identity;
  if (!PosixBlock::valid(number, PosixBlock::MountedMajor) ||
      !VFS::instance().diskMount(PosixBlock::minor(number), identity) ||
      !identity.pin(filesystem)) {
    SYSCALL_ERROR(DeviceDoesNotExist);
    return -1;
  }
  if (command == Enable && quota.file->getFilesystem() != filesystem.filesystem()) {
    SYSCALL_ERROR(CrossDeviceLink);
    return -1;
  }
  QuotaResponse response;
  const auto status = filesystem.filesystem()->quotaControl(request, response, quota.file);
  if (status != QuotaStatus::Success) {
    syscallError(quotaError(status));
    return -1;
  }
  if ((command == GetQuota &&
       !PosixSubsystem::copyToUser(address, &response.record, sizeof(response.record))) ||
      (command == GetFormat &&
       !PosixSubsystem::copyToUser(address, &response.format, sizeof(response.format)))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}
}  // namespace

int posix_quotactl(int command, const char* special, int id, void* address) {
  const QuotaResult result = control(command, special, id, address);
  syscallError(result.error);
  return result.value;
}
