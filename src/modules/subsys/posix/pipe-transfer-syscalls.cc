/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/assert.h"

#include <fcntl.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "modules/system/vfs/Pipe.h"
#include "net-syscalls.h"
#include "pipe-transfer-syscalls.h"

namespace {
constexpr unsigned KnownFlags = 0xf;
constexpr unsigned Nonblock = 2;
constexpr uint64_t MaximumPosition = 0x7fffffffffffffffULL;
using Status = PipeBuffer::Status;
using PositionGuard = FileDescriptor::TransferPositionGuard;
using PositionSide = PositionGuard::Endpoint;

struct Endpoint {
  DescriptorLease descriptor;
  FileDescriptor::OpenFileDescriptionLease description;
  SharedPointer<NetworkSyscalls> socket;
  File* file = nullptr;
  Pipe* pipe = nullptr;
  int flags = 0;
};

bool acquireEndpoint(PosixSubsystem* subsystem, int fd, Endpoint& endpoint) {
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, endpoint.descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }
  endpoint.description = endpoint.descriptor->acquireOpenFileDescription();
  if (!endpoint.description) {
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }
  endpoint.flags = endpoint.descriptor->getStatusFlags();
  if (endpoint.flags & O_PATH) {
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }
  endpoint.file = endpoint.description->getFile();
  endpoint.socket = endpoint.description->getNetworkImpl();
  if (endpoint.file && (endpoint.file->isPipe() || endpoint.file->isFifo()))
    endpoint.pipe = Pipe::fromFile(endpoint.file);
  return true;
}

bool accessAllowed(const Endpoint& endpoint, bool writing) {
  const int mode = endpoint.flags & O_ACCMODE;
  if (endpoint.socket || mode == O_RDWR || mode == (writing ? O_WRONLY : O_RDONLY))
    return true;
  SYSCALL_ERROR(BadFileDescriptor);
  return false;
}

bool validRange(uint64_t offset, size_t count) {
  if (offset > MaximumPosition || count > MaximumPosition - offset) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  return true;
}

bool interrupted(Thread* thread) {
  if (thread->getInterruptionReason() == Thread::InterruptedBySignal ||
      thread->getUnwindState() != Thread::Continue) {
    SYSCALL_ERROR(Interrupted);
    return true;
  }
  return false;
}

ssize_t pipeResult(PipeBuffer::Result result, bool writing, bool& pipeSignal) {
  if (result.count)
    return static_cast<ssize_t>(result.count);
  switch (result.status) {
    case Status::Ready:
    case Status::Eof:
      return 0;
    case Status::Closed:
      if (!writing)
        return 0;
      pipeSignal = true;
      SYSCALL_ERROR(BrokenPipe);
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
  }
  return -1;
}

ssize_t finish(Thread* thread, PosixSubsystem* subsystem, ssize_t result, bool pipeSignal) {
  const size_t error = result < 0 ? thread->getErrno() : 0;
  thread->clearInterruption();
  if (pipeSignal)
    subsystem->threadException(thread, Subsystem::Pipe);
  thread->setErrno(error);
  return result;
}

ssize_t mixedSplice(Thread* thread, Endpoint& input, Endpoint& output, int64_t* explicitPosition,
                    size_t count, unsigned flags, bool& pipeSignal) {
  const bool writingPipe = output.pipe != nullptr;
  Pipe* pipe = writingPipe ? output.pipe : input.pipe;
  Endpoint& regular = writingPipe ? input : output;
  const bool socket = !writingPipe && bool(output.socket);
  if (socket) {
    if (explicitPosition || output.socket->getType() != SOCK_STREAM ||
        (output.socket->getDomain() != AF_UNIX && output.socket->getDomain() != AF_INET &&
         output.socket->getDomain() != AF_INET6)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
  } else if (!regular.file || !regular.file->supportsRegularFileOperations()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!writingPipe && (output.flags & O_APPEND)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  const uint64_t initial = explicitPosition ? static_cast<uint64_t>(*explicitPosition)
                           : socket         ? 0
                                            : regular.descriptor->getOffset();
  if (!validRange(initial, count))
    return -1;
  if (socket) {
    struct sockaddr_storage peer = {};
    socklen_t length = sizeof(peer);
    thread->setErrno(0);
    if (output.socket->getpeername(&peer, &length) < 0)
      return -1;
  }
  const size_t maximum = count < PipeBuffer::Capacity ? count : PipeBuffer::Capacity;
  auto scratch = UniqueArray<uint8_t>::allocate(maximum);
  if (!scratch) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  const bool canBlock =
      !(flags & Nonblock) && !((writingPipe ? output.flags : input.flags) & O_NONBLOCK);

  for (;;) {
    if (interrupted(thread))
      return -1;
    // No position or writer lock may keep the opposite transfer from draining.
    const auto ready = pipe->waitTransfer(writingPipe, canBlock);
    if (ready.status != Status::Ready)
      return pipeResult(ready, writingPipe, pipeSignal);
    bool retry = false;
    const ssize_t result = [&]() -> ssize_t {
      PositionGuard positions(input.description, output.description,
                              writingPipe && !explicitPosition, !writingPipe && !socket);
      if (!writingPipe && (positions.statusFlags(PositionSide::Output) & O_APPEND)) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      uint64_t position =
          explicitPosition ? static_cast<uint64_t>(*explicitPosition)
          : socket         ? 0
                   : positions.offset(writingPipe ? PositionSide::Input : PositionSide::Output);
      if (!validRange(position, count) || interrupted(thread))
        return -1;
      ssize_t moved;
      if (writingPipe) {
        Pipe::WriteReservation reservation;
        const auto reserved = pipe->reserveWrite(maximum, false, reservation);
        if (reserved.status == Status::WouldBlock) {
          retry = true;
          return -1;
        }
        if (reserved.status != Status::Ready)
          return pipeResult(reserved, true, pipeSignal);
        thread->setErrno(0);
        const size_t read = regular.file->read(position, reservation.size(),
                                               reinterpret_cast<uintptr_t>(scratch.get()), true);
        assert(read <= reservation.size());
        if (!read)
          return thread->getErrno() || interrupted(thread) ? -1 : 0;
        if (interrupted(thread))
          return -1;
        moved = pipeResult(reservation.commit(scratch.get(), read), true, pipeSignal);
      } else {
        auto fromPipe = [&](File::WriteGuard* writer) -> ssize_t {
          Pipe::ReadReservation reservation;
          const auto reserved = pipe->reserveRead(maximum, false, reservation);
          if (reserved.status == Status::WouldBlock) {
            retry = true;
            return -1;
          }
          if (reserved.status != Status::Ready)
            return pipeResult(reserved, false, pipeSignal);
          size_t amount = reservation.size();
          if (writer) {
            const uint64_t limit = regular.file->maximumFileSize();
            if (position >= limit) {
              SYSCALL_ERROR(FileTooLarge);
              return -1;
            }
            if (amount > limit - position)
              amount = static_cast<size_t>(limit - position);
          }
          reservation.copyTo(scratch.get(), amount);
          if (interrupted(thread))
            return -1;
          thread->setErrno(0);
          const ssize_t written =
              writer ? static_cast<ssize_t>(writer->write(
                           position, amount, reinterpret_cast<uintptr_t>(scratch.get()), true))
                     : posix_send_descriptor(output.descriptor, scratch.get(), amount, 0, true);
          if (!writer && thread->getErrno() == Error::BrokenPipe)
            pipeSignal = true;
          if (written <= 0) {
            if (!thread->getErrno() && !interrupted(thread))
              SYSCALL_ERROR(IoError);
            return -1;
          }
          assert(static_cast<size_t>(written) <= amount);
          reservation.consume(static_cast<size_t>(written));
          return written;
        };
        if (socket) {
          moved = fromPipe(nullptr);
        } else {
          auto writer = regular.file->lockWrites();
          moved = fromPipe(&writer);
        }
      }
      if (moved > 0) {
        position += static_cast<size_t>(moved);
        if (explicitPosition)
          *explicitPosition = static_cast<int64_t>(position);
        else if (!socket)
          positions.commitOffset(writingPipe ? PositionSide::Input : PositionSide::Output,
                                 position);
        thread->setErrno(0);
      }
      return moved;
    }();
    if (!retry)
      return result;
  }
}
}  // namespace

ssize_t posix_splice(int inputFd, int64_t* inputOffset, int outputFd, int64_t* outputOffset,
                     size_t count, unsigned flags) {
  TerminationDeferral lifetime;
  Thread* thread = Processor::information().getCurrentThread();
  auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  thread->clearInterruption();
  thread->setErrno(0);
  if (!count)
    return 0;
  if (flags & ~KnownFlags) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  bool pipeSignal = false;
  const ssize_t result = [&]() -> ssize_t {
    Endpoint input, output;
    if (!acquireEndpoint(subsystem, inputFd, input) ||
        !acquireEndpoint(subsystem, outputFd, output))
      return -1;
    if ((input.pipe && inputOffset) || (output.pipe && outputOffset)) {
      SYSCALL_ERROR(IllegalSeek);
      return -1;
    }
    int64_t inputPosition = 0, outputPosition = 0;
    if ((outputOffset &&
         !PosixSubsystem::copyFromUser(&outputPosition, outputOffset, sizeof(outputPosition))) ||
        (inputOffset &&
         !PosixSubsystem::copyFromUser(&inputPosition, inputOffset, sizeof(inputPosition)))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (!accessAllowed(input, false) || !accessAllowed(output, true))
      return -1;
    ssize_t moved;
    if (input.pipe && output.pipe) {
      const bool canBlock = !(flags & Nonblock) && !((input.flags | output.flags) & O_NONBLOCK);
      moved =
          pipeResult(input.pipe->transferTo(*output.pipe, count, true, canBlock), true, pipeSignal);
    } else if (input.pipe || output.pipe) {
      int64_t* position = input.pipe ? (outputOffset ? &outputPosition : nullptr)
                                     : (inputOffset ? &inputPosition : nullptr);
      moved = mixedSplice(thread, input, output, position, count, flags, pipeSignal);
    } else {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (moved < 0)
      return moved;
    // Unlike sendfile, splice exports offsets after EOF but never after an error.
    if ((outputOffset &&
         !PosixSubsystem::copyToUser(outputOffset, &outputPosition, sizeof(outputPosition))) ||
        (inputOffset &&
         !PosixSubsystem::copyToUser(inputOffset, &inputPosition, sizeof(inputPosition)))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return moved;
  }();
  return finish(thread, subsystem, result, pipeSignal);
}

ssize_t posix_tee(int inputFd, int outputFd, size_t count, unsigned flags) {
  TerminationDeferral lifetime;
  Thread* thread = Processor::information().getCurrentThread();
  auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  thread->clearInterruption();
  thread->setErrno(0);
  if (flags & ~KnownFlags) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!count)
    return 0;
  bool pipeSignal = false;
  const ssize_t result = [&]() -> ssize_t {
    Endpoint input, output;
    if (!acquireEndpoint(subsystem, inputFd, input) ||
        !acquireEndpoint(subsystem, outputFd, output) || !accessAllowed(input, false) ||
        !accessAllowed(output, true))
      return -1;
    if (!input.pipe || !output.pipe) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    const bool canBlock = !(flags & Nonblock) && !((input.flags | output.flags) & O_NONBLOCK);
    return pipeResult(input.pipe->transferTo(*output.pipe, count, false, canBlock), true,
                      pipeSignal);
  }();
  return finish(thread, subsystem, result, pipeSignal);
}
