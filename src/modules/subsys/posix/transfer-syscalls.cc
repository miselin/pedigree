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
#include "modules/system/vfs/File.h"
#include "net-syscalls.h"
#include "transfer-syscalls.h"

namespace {
constexpr uint64_t MaximumPosition = 0x7fffffffffffffffULL;
constexpr size_t MaximumTransfer = 0x7ffff000;
constexpr size_t BufferCapacity = 64 * 1024;
using PositionGuard = FileDescriptor::TransferPositionGuard;
using Endpoint = PositionGuard::Endpoint;

bool signedRange(uint64_t position, size_t count) {
  if (position > MaximumPosition || count > MaximumPosition - position) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  return true;
}

bool regularFile(File* file) {
  if (file && file->isDirectory()) {
    SYSCALL_ERROR(IsADirectory);
    return false;
  }
  if (!file || !file->supportsRegularFileOperations()) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  return true;
}

ssize_t moveBytes(Thread* thread, File* input, const DescriptorLease& output, File* outputFile,
                  uint64_t& inputPosition, uint64_t& outputPosition, size_t count,
                  bool& pipeSignal) {
  if (!count)
    return 0;
  const size_t capacity = count < BufferCapacity ? count : BufferCapacity;
  auto buffer = UniqueArray<uint8_t>::allocate(capacity);
  if (!buffer) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }

  auto copy = [&](File::WriteGuard* writer) -> ssize_t {
    size_t transferred = 0;
    while (transferred < count) {
      if (thread->getInterruptionReason() == Thread::InterruptedBySignal ||
          thread->getUnwindState() != Thread::Continue) {
        SYSCALL_ERROR(Interrupted);
        break;
      }
      const size_t remaining = count - transferred;
      const size_t requested = remaining < capacity ? remaining : capacity;
      thread->setErrno(0);
      const size_t read =
          input->read(inputPosition, requested, reinterpret_cast<uintptr_t>(buffer.get()), true);
      const size_t inputError = thread->getErrno();
      assert(read <= requested);
      if (!read)
        break;
      if (thread->getInterruptionReason() == Thread::InterruptedBySignal ||
          thread->getUnwindState() != Thread::Continue) {
        SYSCALL_ERROR(Interrupted);
        break;
      }

      size_t writable = read;
      if (writer) {
        // A genuine input EOF precedes the backend's output-size check.
        const uint64_t limit = outputFile->maximumFileSize();
        if (outputPosition >= limit) {
          SYSCALL_ERROR(FileTooLarge);
          break;
        }
        if (writable > limit - outputPosition)
          writable = static_cast<size_t>(limit - outputPosition);
      }
      thread->setErrno(0);
      const ssize_t written =
          writer ? static_cast<ssize_t>(writer->write(
                       outputPosition, writable, reinterpret_cast<uintptr_t>(buffer.get()), true))
                 : posix_send_descriptor(output, buffer.get(), read, 0, true);
      const size_t outputError = thread->getErrno();
      if (!writer && outputError == Error::BrokenPipe)
        pipeSignal = true;
      if (written <= 0) {
        if (!outputError) {
          syscallError(thread->getInterruptionReason() == Thread::InterruptedBySignal ||
                               thread->getUnwindState() != Thread::Continue
                           ? Error::Interrupted
                           : Error::IoError);
        }
        break;
      }
      assert(static_cast<size_t>(written) <= writable);
      // Reading the scratch suffix does not consume the input's position.
      inputPosition += written;
      outputPosition += written;
      transferred += written;
      thread->setErrno(0);
      if (static_cast<size_t>(written) < read || read < requested || inputError || outputError)
        break;
    }
    if (transferred) {
      thread->setErrno(0);
      return static_cast<ssize_t>(transferred);
    }
    if (thread->getErrno())
      return -1;
    if (thread->getInterruptionReason() == Thread::InterruptedBySignal ||
        thread->getUnwindState() != Thread::Continue) {
      SYSCALL_ERROR(Interrupted);
      return -1;
    }
    return 0;
  };

  if (outputFile) {
    auto writer = outputFile->lockWrites();
    return copy(&writer);
  }
  return copy(nullptr);
}

ssize_t finishTransfer(Thread* thread, PosixSubsystem* subsystem, ssize_t result, bool pipeSignal) {
  const size_t error = result < 0 ? thread->getErrno() : 0;
  thread->clearInterruption();
  if (pipeSignal)
    subsystem->threadException(thread, Subsystem::Pipe);
  thread->setErrno(error);
  return result;
}

ssize_t sendFile(PosixSubsystem* subsystem, Thread* thread, int outputFd, int inputFd,
                 int64_t* explicitPosition, size_t count, bool& pipeSignal) {
  DescriptorLease input;
  if (!subsystem || !subsystem->acquireFileDescriptor(inputFd, input)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  auto inputDescription = input->acquireOpenFileDescription();
  if (!inputDescription) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  const int inputFlags = input->getStatusFlags();
  const int inputAccess = inputFlags & O_ACCMODE;
  if ((inputFlags & O_PATH) || (inputAccess != O_RDONLY && inputAccess != O_RDWR)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  File* inputFile = inputDescription->getFile();
  if (explicitPosition && inputFile && !inputFile->isSeekable()) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }
  if (!regularFile(inputFile))
    return -1;
  const uint64_t initialInput =
      explicitPosition ? static_cast<uint64_t>(*explicitPosition) : input->getOffset();
  if (!signedRange(initialInput, count))
    return -1;

  DescriptorLease output;
  if (!subsystem->acquireFileDescriptor(outputFd, output)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  auto outputDescription = output->acquireOpenFileDescription();
  if (!outputDescription) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  auto socket = outputDescription->getNetworkImpl();
  File* outputFile = socket ? nullptr : outputDescription->getFile();
  if (socket) {
    if (socket->getType() != SOCK_STREAM ||
        (socket->getDomain() != AF_UNIX && socket->getDomain() != AF_INET &&
         socket->getDomain() != AF_INET6)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
  } else if (!regularFile(outputFile)) {
    return -1;
  }

  PositionGuard positions(inputDescription, outputDescription, !explicitPosition, !socket);
  const int outputFlags = positions.statusFlags(Endpoint::Output);
  const int outputAccess = outputFlags & O_ACCMODE;
  // Socket OFDs currently carry no access-mode bits; their endpoint supplies
  // the write capability independently of regular-file open modes.
  if ((outputFlags & O_PATH) || (!socket && outputAccess != O_WRONLY && outputAccess != O_RDWR)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (outputFlags & O_APPEND) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  uint64_t inputPosition = explicitPosition ? static_cast<uint64_t>(*explicitPosition)
                                            : positions.offset(Endpoint::Input);
  uint64_t outputPosition = socket ? 0 : positions.offset(Endpoint::Output);
  if (!signedRange(inputPosition, count))
    return -1;
  if (count > MaximumTransfer)
    count = MaximumTransfer;
  uint64_t limit = inputFile->maximumFileSize();
  if (outputFile && outputFile->maximumFileSize() < limit)
    limit = outputFile->maximumFileSize();
  if (inputPosition > limit || count > limit - inputPosition) {
    if (inputPosition >= limit) {
      SYSCALL_ERROR(ValueTooLarge);
      return -1;
    }
    count = static_cast<size_t>(limit - inputPosition);
  }
  if (!signedRange(outputPosition, count))
    return -1;
  if (socket) {
    struct sockaddr_storage peer = {};
    socklen_t length = sizeof(peer);
    thread->setErrno(0);
    if (socket->getpeername(&peer, &length) < 0)
      return -1;
  }
  const ssize_t result = moveBytes(thread, inputFile, output, outputFile, inputPosition,
                                   outputPosition, count, pipeSignal);
  if (result > 0) {
    if (!socket)
      positions.commitOffset(Endpoint::Output, outputPosition);
    if (explicitPosition)
      *explicitPosition = static_cast<int64_t>(inputPosition);
    else if (socket || !positions.sameDescription())
      positions.commitOffset(Endpoint::Input, inputPosition);
  }
  return result;
}
}  // namespace

ssize_t posix_sendfile(int outputFd, int inputFd, int64_t* offset, size_t count) {
  TerminationDeferral lifetime;
  Thread* thread = Processor::information().getCurrentThread();
  auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  int64_t position = 0;
  thread->clearInterruption();
  thread->setErrno(0);
  if (offset && !PosixSubsystem::copyFromUser(&position, offset, sizeof(position))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  bool pipeSignal = false;
  ssize_t result = sendFile(subsystem, thread, outputFd, inputFd, offset ? &position : nullptr,
                            count, pipeSignal);
  // The amd64 sendfile wrapper exports its offset even after EOF or error.
  if (offset && !PosixSubsystem::copyToUser(offset, &position, sizeof(position))) {
    SYSCALL_ERROR(BadAddress);
    result = -1;
  }
  return finishTransfer(thread, subsystem, result, pipeSignal);
}

ssize_t posix_copy_file_range(int inputFd, int64_t* inputOffset, int outputFd,
                              int64_t* outputOffset, size_t count, unsigned flags) {
  TerminationDeferral lifetime;
  Thread* thread = Processor::information().getCurrentThread();
  auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  thread->clearInterruption();
  thread->setErrno(0);
  bool pipeSignal = false;
  const ssize_t result = [&]() -> ssize_t {
    DescriptorLease input, output;
    if (!subsystem || !subsystem->acquireFileDescriptor(inputFd, input) ||
        !subsystem->acquireFileDescriptor(outputFd, output)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    int64_t importedInput = 0, importedOutput = 0;
    if ((inputOffset &&
         !PosixSubsystem::copyFromUser(&importedInput, inputOffset, sizeof(importedInput))) ||
        (outputOffset &&
         !PosixSubsystem::copyFromUser(&importedOutput, outputOffset, sizeof(importedOutput)))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (flags) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    auto inputDescription = input->acquireOpenFileDescription();
    auto outputDescription = output->acquireOpenFileDescription();
    if (!inputDescription || !outputDescription) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    File* inputFile = inputDescription->getFile();
    File* outputFile = outputDescription->getFile();
    PositionGuard positions(inputDescription, outputDescription, !inputOffset, true);
    const int inputFlags = positions.statusFlags(Endpoint::Input);
    const int outputFlags = positions.statusFlags(Endpoint::Output);
    if ((inputFlags & O_PATH) || (outputFlags & O_PATH)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    if ((inputFile && inputFile->isDirectory()) || (outputFile && outputFile->isDirectory())) {
      SYSCALL_ERROR(IsADirectory);
      return -1;
    }
    if (!regularFile(inputFile) || !regularFile(outputFile))
      return -1;
    const int inputAccess = inputFlags & O_ACCMODE;
    const int outputAccess = outputFlags & O_ACCMODE;
    if ((inputAccess != O_RDONLY && inputAccess != O_RDWR) ||
        (outputAccess != O_WRONLY && outputAccess != O_RDWR) || (outputFlags & O_APPEND)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    if (inputFile->getFilesystem() != outputFile->getFilesystem()) {
      SYSCALL_ERROR(CrossDeviceLink);
      return -1;
    }
    uint64_t inputPosition =
        inputOffset ? static_cast<uint64_t>(importedInput) : positions.offset(Endpoint::Input);
    uint64_t outputPosition =
        outputOffset ? static_cast<uint64_t>(importedOutput) : positions.offset(Endpoint::Output);
    if (count > ~uint64_t(0) - inputPosition || count > ~uint64_t(0) - outputPosition) {
      SYSCALL_ERROR(ValueTooLarge);
      return -1;
    }
    const uint64_t inputSize = inputFile->getSize();
    if (inputPosition <= MaximumPosition && inputPosition >= inputSize)
      count = 0;
    else if (count > inputSize - inputPosition)
      count = static_cast<size_t>(inputSize - inputPosition);
    const uint64_t outputLimit = outputFile->maximumFileSize();
    if (outputPosition <= MaximumPosition) {
      if (outputPosition >= outputLimit) {
        SYSCALL_ERROR(FileTooLarge);
        return -1;
      }
      if (count > outputLimit - outputPosition)
        count = static_cast<size_t>(outputLimit - outputPosition);
    }
    if (inputFile->futexIdentity() == outputFile->futexIdentity() &&
        outputPosition + count > inputPosition && outputPosition < inputPosition + count) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (!signedRange(inputPosition, count) || !signedRange(outputPosition, count))
      return -1;
    if (count > MaximumTransfer)
      count = MaximumTransfer;
    ssize_t moved = moveBytes(thread, inputFile, output, outputFile, inputPosition, outputPosition,
                              count, pipeSignal);
    if (moved <= 0)
      return moved;

    // Both completions run, even when the first user store faults.
    bool copied = true;
    if (inputOffset) {
      importedInput = static_cast<int64_t>(inputPosition);
      copied = PosixSubsystem::copyToUser(inputOffset, &importedInput, sizeof(importedInput));
    } else {
      positions.commitOffset(Endpoint::Input, inputPosition);
    }
    if (outputOffset) {
      importedOutput = static_cast<int64_t>(outputPosition);
      if (!PosixSubsystem::copyToUser(outputOffset, &importedOutput, sizeof(importedOutput)))
        copied = false;
    } else {
      positions.commitOffset(Endpoint::Output, outputPosition);
    }
    if (!copied) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return moved;
  }();
  return finishTransfer(thread, subsystem, result, pipeSignal);
}
