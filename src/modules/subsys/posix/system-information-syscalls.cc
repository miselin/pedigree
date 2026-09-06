/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include "system-information-syscalls.h"

#include "PosixSubsystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "system-information-abi.h"

int posix_sysinfo(void* information) {
  PosixSystemInformation::Record snapshot;
  const auto memory = PhysicalMemoryManager::instance().memorySnapshot();
  if (!memory.available) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  const auto swap = MemoryMapManager::instance().swapSnapshot();
  if (!PosixSystemInformation::setMemory(
          snapshot, memory.totalPages, memory.freePages, swap.active ? swap.totalPages : 0,
          swap.active ? swap.usedPages : 0, PhysicalMemoryManager::getPageSize())) {
    SYSCALL_ERROR(ValueTooLarge);
    return -1;
  }
  const uint64_t ticks = Time::getTicks();
  snapshot.uptime = ticks / Time::Multiplier::Second + (ticks % Time::Multiplier::Second != 0);
  const auto activity = Scheduler::instance().systemActivity();
  snapshot.procs = activity.tasks > 65535 ? 65535 : activity.tasks;
  for (size_t i = 0; i < 3; ++i)
    snapshot.loads[i] = activity.loads[i];
  // Separate shared-memory and buffer accounting is unavailable. amd64 has
  // no high-memory zone. The explicit zero fields do not imply reclaimability.
  if (!PosixSubsystem::copyToUser(information, &snapshot, sizeof(snapshot))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int posix_personality(unsigned long requested) {
  Thread& thread = *Processor::information().getCurrentThread();
  uint32_t previous;
  // Linux's argument is unsigned int even though musl exposes unsigned long.
  const uint32_t value = static_cast<uint32_t>(requested);
#if !X64
  if (value == ExecutionPersonality::Linux32) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
#endif
  if (!thread.executionPersonality().select(value, previous)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return previous;
}
