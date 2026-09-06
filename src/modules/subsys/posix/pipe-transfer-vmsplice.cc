/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/assert.h"

#include <fcntl.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "modules/system/vfs/Pipe.h"
#include "pipe-transfer-syscalls.h"
#include <sys/uio.h>

namespace {
constexpr unsigned KnownFlags = 0xf;
constexpr unsigned Nonblock = 2;
constexpr unsigned Gift = 8;
constexpr size_t MaximumVectors = 1024;
constexpr size_t MaximumTransfer = 0x7ffff000;
using Status = PipeBuffer::Status;

bool importVectors(const struct iovec* user, size_t count, bool gift,
                   UniqueArray<struct iovec>& vectors, size_t& total) {
  total = 0;
  if (count > MaximumVectors) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (!count)
    return true;
  vectors = UniqueArray<struct iovec>::allocate(count);
  if (!vectors) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (!PosixSubsystem::copyFromUser(vectors.get(), user, count, sizeof(struct iovec))) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  for (size_t i = 0; i < count; ++i) {
    auto& vector = vectors.get()[i];
    const uintptr_t base = reinterpret_cast<uintptr_t>(vector.iov_base);
    if (vector.iov_len > (~size_t(0) >> 1)) {
      SYSCALL_ERROR(InvalidArgument);
      return false;
    }
    if (vector.iov_len) {
      const size_t checkedLength =
          count == 1 && vector.iov_len > MaximumTransfer ? MaximumTransfer : vector.iov_len;
      if (checkedLength - 1 > ~uintptr_t(0) - base || base < space.getUserStart() ||
          base >= space.getKernelStart() || base + checkedLength - 1 >= space.getKernelStart() ||
          !space.isAddressValid(reinterpret_cast<void*>(base)) ||
          !space.isAddressValid(reinterpret_cast<void*>(base + checkedLength - 1))) {
        SYSCALL_ERROR(BadAddress);
        return false;
      }
      if (gift && (base % pageSize || vector.iov_len % pageSize)) {
        SYSCALL_ERROR(InvalidArgument);
        return false;
      }
    }
    if (vector.iov_len > MaximumTransfer - total)
      vector.iov_len = MaximumTransfer - total;
    total += vector.iov_len;
  }
  return true;
}

ssize_t vmspliceCopy(Thread* thread, Pipe* pipe, bool writing, const struct iovec* vectors,
                     size_t vectorCount, size_t maximum, bool canBlock, bool& pipeSignal) {
  auto scratch = UniqueArray<uint8_t>::allocate(maximum);
  if (!scratch) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  size_t copied = 0;
  for (size_t i = 0; i < vectorCount && copied < maximum; ++i) {
    if (!vectors[i].iov_len)
      continue;
    if (thread->getInterruptionReason() == Thread::InterruptedBySignal ||
        thread->getUnwindState() != Thread::Continue) {
      SYSCALL_ERROR(Interrupted);
      break;
    }
    const size_t remaining = maximum - copied;
    const size_t requested = vectors[i].iov_len < remaining ? vectors[i].iov_len : remaining;
    Pipe::ReadReservation readReservation;
    Pipe::WriteReservation writeReservation;
    const auto reserved = writing
                              ? pipe->reserveWrite(requested, canBlock && !copied, writeReservation)
                              : pipe->reserveRead(requested, canBlock && !copied, readReservation);
    if (reserved.status != Status::Ready) {
      switch (reserved.status) {
        case Status::Closed:
          if (writing) {
            pipeSignal = true;
            SYSCALL_ERROR(BrokenPipe);
          }
          break;
        case Status::WouldBlock:
          SYSCALL_ERROR(NoMoreProcesses);
          break;
        case Status::Interrupted:
          SYSCALL_ERROR(Interrupted);
          break;
        case Status::Invalid:
          SYSCALL_ERROR(InvalidArgument);
          break;
        case Status::Eof:
        case Status::Ready:
          break;
      }
      break;
    }
    const size_t amount = writing ? writeReservation.size() : readReservation.size();
    assert(amount && amount <= requested);
    bool success;
    if (writing) {
      success = PosixSubsystem::copyFromUser(scratch.get(), vectors[i].iov_base, amount);
    } else {
      readReservation.copyTo(scratch.get(), amount);
      success = PosixSubsystem::copyToUser(vectors[i].iov_base, scratch.get(), amount);
    }
    if (!success) {
      SYSCALL_ERROR(BadAddress);
      break;
    }
    if (writing) {
      if (thread->getInterruptionReason() == Thread::InterruptedBySignal ||
          thread->getUnwindState() != Thread::Continue) {
        SYSCALL_ERROR(Interrupted);
        break;
      }
      const auto committed = writeReservation.commit(scratch.get(), amount);
      if (committed.status != Status::Ready) {
        if (committed.status == Status::Closed) {
          pipeSignal = true;
          SYSCALL_ERROR(BrokenPipe);
        } else {
          SYSCALL_ERROR(InvalidArgument);
        }
        break;
      }
      assert(committed.count == amount);
    } else {
      readReservation.consume(amount);
    }
    copied += amount;
    thread->setErrno(0);
    if (amount < requested)
      break;
  }
  if (copied) {
    thread->setErrno(0);
    return static_cast<ssize_t>(copied);
  }
  return thread->getErrno() ? -1 : 0;
}
}  // namespace

ssize_t posix_vmsplice(int fd, const struct iovec* userVectors, size_t vectorCount,
                       unsigned flags) {
  TerminationDeferral lifetime;
  Thread* thread = Processor::information().getCurrentThread();
  auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  thread->clearInterruption();
  thread->setErrno(0);
  if (flags & ~KnownFlags) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  bool pipeSignal = false;
  const ssize_t result = [&]() -> ssize_t {
    DescriptorLease descriptor;
    if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    const int status = descriptor->getStatusFlags();
    const int mode = status & O_ACCMODE;
    if ((status & O_PATH) || (mode != O_RDONLY && mode != O_WRONLY && mode != O_RDWR)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    const bool writing = mode != O_RDONLY;
    UniqueArray<struct iovec> vectors;
    size_t total;
    if (!importVectors(userVectors, vectorCount, writing && (flags & Gift), vectors, total))
      return -1;
    if (!total)
      return 0;
    if (!descriptor->file || !(descriptor->file->isPipe() || descriptor->file->isFifo())) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    const size_t maximum = total < PipeBuffer::Capacity ? total : PipeBuffer::Capacity;
    // Linux vmsplice uses its explicit NONBLOCK flag, not the OFD status bit.
    return vmspliceCopy(thread, Pipe::fromFile(descriptor->file), writing, vectors.get(),
                        vectorCount, maximum, !(flags & Nonblock), pipeSignal);
  }();
  const size_t error = result < 0 ? thread->getErrno() : 0;
  thread->clearInterruption();
  if (pipeSignal)
    subsystem->threadException(thread, Subsystem::Pipe);
  thread->setErrno(error);
  return result;
}
