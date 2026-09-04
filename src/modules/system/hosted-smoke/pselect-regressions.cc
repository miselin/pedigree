/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"

#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/eventfd-syscalls.h"
#include "modules/subsys/posix/linux-wait-abi.h"
#include "modules/subsys/posix/select-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Pipe.h"

static_assert(sizeof(LinuxPselectSigsetArgument) == 16);
static_assert(offsetof(LinuxPselectSigsetArgument, signalMask) == 0);
static_assert(offsetof(LinuxPselectSigsetArgument, signalMaskSize) == 8);

namespace {
constexpr size_t BitsPerWord = sizeof(uint64_t) * 8;
constexpr size_t TestSignal = 10;
constexpr int PreservedErrno = 123;
constexpr uint64_t TestSignalBit = static_cast<uint64_t>(1) << (TestSignal - 1);
constexpr uint64_t PreservedSignalBit = static_cast<uint64_t>(1) << (12 - 1);
constexpr uint64_t UnblockableSignalBits =
    (static_cast<uint64_t>(1) << (SIGKILL - 1)) | (static_cast<uint64_t>(1) << (SIGSTOP - 1));
constexpr uint64_t OriginalSignalMask = TestSignalBit | PreservedSignalBit;
constexpr uint64_t RequestedSignalMask = PreservedSignalBit | UnblockableSignalBits;
constexpr uint64_t ActiveSignalMask = PreservedSignalBit;

Atomic<size_t> g_PselectSignalHandlerCalls(0);
Atomic<size_t> g_PselectSignalHandlerWrites(0);
FileDescriptor* g_PselectSignalWriter = nullptr;

size_t bitmapExtent(int nfds) {
  return ((static_cast<size_t>(nfds) + BitsPerWord - 1) / BitsPerWord) * sizeof(uint64_t);
}

void setBit(uint64_t* words, size_t fd) {
  words[fd / BitsPerWord] |= static_cast<uint64_t>(1) << (fd % BitsPerWord);
}

bool isSet(const uint64_t* words, size_t fd) {
  return words[fd / BitsPerWord] & (static_cast<uint64_t>(1) << (fd % BitsPerWord));
}

void pselectSignalHandler(size_t) {
  g_PselectSignalHandlerCalls += 1;
  if (g_PselectSignalWriter) {
    char value = 'r';
    if (g_PselectSignalWriter->write(1, reinterpret_cast<uintptr_t>(&value), true) == 1) {
      g_PselectSignalHandlerWrites += 1;
    }
  }
}

bool closeDescriptor(PosixSubsystem* subsystem, size_t fd) {
  DescriptorLease descriptor;
  if (!subsystem->acquireFileDescriptor(fd, descriptor)) {
    return false;
  }
  const bool closed = subsystem->closeFileDescriptor(fd, descriptor);
  descriptor.reset();
  return closed;
}

bool waitForPselectBlock(Thread* thread) {
  const Time::Timestamp deadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(debugAddress) == Thread::SemWait) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

struct PselectValidationContext {
  explicit PselectValidationContext(size_t highReadFd)
      : highReadFd(highReadFd), passed(false), returned(0) {}

  size_t highReadFd;
  bool passed;
  Atomic<size_t> returned;
};

int pselectValidationWorker(void* parameter) {
  PselectValidationContext* context = reinterpret_cast<PselectValidationContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  bool passed = true;

  LinuxKernelTimespec zero = {0, 0};
  thread->setErrno(PreservedErrno);
  const int zeroResult = posix_pselect6(0, nullptr, nullptr, nullptr, &zero, nullptr);
  passed &= zeroResult == 0 && thread->getErrno() == PreservedErrno;

  LinuxKernelTimespec oneNanosecond = {0, 1};
  thread->setErrno(PreservedErrno);
  const int oneNanosecondResult =
      posix_pselect6(0, nullptr, nullptr, nullptr, &oneNanosecond, nullptr);
  passed &= oneNanosecondResult == 0 && !oneNanosecond.tv_sec && !oneNanosecond.tv_nsec &&
            thread->getErrno() == PreservedErrno;

  LinuxKernelTimespec negativeSeconds = {-1, 0};
  thread->setErrno(0);
  passed &= posix_pselect6(0, nullptr, nullptr, nullptr, &negativeSeconds, nullptr) == -1 &&
            thread->getErrno() == Error::InvalidArgument;

  LinuxKernelTimespec negativeNanoseconds = {0, -1};
  thread->setErrno(0);
  passed &= posix_pselect6(0, nullptr, nullptr, nullptr, &negativeNanoseconds, nullptr) == -1 &&
            thread->getErrno() == Error::InvalidArgument;

  LinuxKernelTimespec excessiveNanoseconds = {0, 1000000000};
  thread->setErrno(0);
  passed &= posix_pselect6(0, nullptr, nullptr, nullptr, &excessiveNanoseconds, nullptr) == -1 &&
            thread->getErrno() == Error::InvalidArgument;

  uint64_t signalMask = 0;
  LinuxKernelTimespec unchangedWrongSize = {1, 123};
  LinuxPselectSigsetArgument wrongMaskSize = {reinterpret_cast<uintptr_t>(&signalMask),
                                              sizeof(signalMask) - 1};
  thread->setErrno(0);
  passed &=
      posix_pselect6(0, nullptr, nullptr, nullptr, &unchangedWrongSize, &wrongMaskSize) == -1 &&
      thread->getErrno() == Error::InvalidArgument && unchangedWrongSize.tv_sec == 1 &&
      unchangedWrongSize.tv_nsec == 123;

  LinuxPselectSigsetArgument nullMask = {0, 1};
  zero = {0, 0};
  thread->setErrno(PreservedErrno);
  passed &= posix_pselect6(0, nullptr, nullptr, nullptr, &zero, &nullMask) == 0 &&
            thread->getErrno() == PreservedErrno;

  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  LinuxKernelTimespec invalidWithBadArgument = {-1, 0};
  thread->setErrno(0);
  passed &=
      posix_pselect6(0, nullptr, nullptr, nullptr, &invalidWithBadArgument,
                     reinterpret_cast<const LinuxPselectSigsetArgument*>(kernelStart)) == -1 &&
      thread->getErrno() == Error::BadAddress;

  LinuxKernelTimespec unchangedBadMask = {1, 456};
  LinuxPselectSigsetArgument badMask = {kernelStart, sizeof(signalMask)};
  thread->setErrno(0);
  passed &= posix_pselect6(0, nullptr, nullptr, nullptr, &unchangedBadMask, &badMask) == -1 &&
            thread->getErrno() == Error::BadAddress && unchangedBadMask.tv_sec == 1 &&
            unchangedBadMask.tv_nsec == 456;

  LinuxKernelTimespec invalidBeforeBadMask = {-1, 0};
  thread->setErrno(0);
  passed &= posix_pselect6(0, nullptr, nullptr, nullptr, &invalidBeforeBadMask, &badMask) == -1 &&
            thread->getErrno() == Error::InvalidArgument;

  thread->setErrno(0);
  passed &= posix_pselect6(0, nullptr, nullptr, nullptr,
                           reinterpret_cast<LinuxKernelTimespec*>(kernelStart), nullptr) == -1 &&
            thread->getErrno() == Error::BadAddress;

  LinuxPselectSigsetArgument validSignalArgument = {
      reinterpret_cast<uintptr_t>(&RequestedSignalMask), sizeof(RequestedSignalMask)};
  thread->setSignalMask(OriginalSignalMask);
  thread->clearInterruption();
  LinuxKernelTimespec invalidNfdsTimeout = {0, 1};
  thread->setErrno(0);
  const int invalidNfdsResult =
      posix_pselect6(-1, nullptr, nullptr, nullptr, &invalidNfdsTimeout, &validSignalArgument);
  const bool invalidNfdsMaskRestored = thread->getSignalMask() == OriginalSignalMask;
  passed &= invalidNfdsResult == -1 && thread->getErrno() == Error::InvalidArgument &&
            invalidNfdsMaskRestored && !invalidNfdsTimeout.tv_sec && !invalidNfdsTimeout.tv_nsec;

  LinuxKernelTimespec badFdsetTimeout = {0, 1};
  thread->setErrno(0);
  const int badFdsetResult = posix_pselect6(1, reinterpret_cast<fd_set*>(kernelStart), nullptr,
                                            nullptr, &badFdsetTimeout, &validSignalArgument);
  const bool badFdsetMaskRestored = thread->getSignalMask() == OriginalSignalMask;
  passed &= badFdsetResult == -1 && thread->getErrno() == Error::BadAddress &&
            badFdsetMaskRestored && !badFdsetTimeout.tv_sec && !badFdsetTimeout.tv_nsec;
  thread->setSignalMask(0);
  thread->clearInterruption();

  const int eventFd = posix_eventfd(1);
  uint64_t readyReads[2] = {};
  uint64_t readyWrites[2] = {};
  const int readyNfds = eventFd + 1;
  if (eventFd >= 0 && readyNfds <= static_cast<int>(BitsPerWord * 2)) {
    setBit(readyReads, eventFd);
    setBit(readyWrites, eventFd);
  } else {
    passed = false;
  }
  LinuxKernelTimespec saturatedTimeout = {INT64_MAX, 999999999};
  thread->setErrno(PreservedErrno);
  const int saturatedResult =
      posix_pselect6(readyNfds, reinterpret_cast<fd_set*>(readyReads),
                     reinterpret_cast<fd_set*>(readyWrites), nullptr, &saturatedTimeout, nullptr);
  passed &= saturatedResult == 2 && isSet(readyReads, eventFd) && isSet(readyWrites, eventFd) &&
            saturatedTimeout.tv_sec >= 0 && saturatedTimeout.tv_nsec >= 0 &&
            saturatedTimeout.tv_nsec < 1000000000 && thread->getErrno() == PreservedErrno;

  uint64_t invalidDescriptor[1] = {};
  setBit(invalidDescriptor, BitsPerWord - 1);
  zero = {0, 0};
  thread->setErrno(0);
  passed &= posix_pselect6(BitsPerWord, reinterpret_cast<fd_set*>(invalidDescriptor), nullptr,
                           nullptr, &zero, nullptr) == -1 &&
            thread->getErrno() == Error::BadFileDescriptor;

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t address = 0;
  const bool allocated = process->getSpaceAllocator().allocate(pageSize, address);
  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping =
      allocated ? MemoryMapManager::instance().mapAnon(
                      mappedAddress, pageSize, MemoryMappedObject::Read | MemoryMappedObject::Write)
                : nullptr;
  bool dynamicBitmaps = mapping && mappedAddress == address;
  if (dynamicBitmaps) {
    uint64_t emptyWord = 0;
    fd_set* oneWord = reinterpret_cast<fd_set*>(address + pageSize - sizeof(emptyWord));
    zero = {0, 0};
    dynamicBitmaps &= PosixSubsystem::copyToUser(oneWord, &emptyWord, sizeof(emptyWord)) &&
                      posix_pselect6(1, oneWord, nullptr, nullptr, &zero, nullptr) == 0;

    constexpr int HighNfds = 1058;
    constexpr size_t HighWords = (HighNfds + BitsPerWord - 1) / BitsPerWord;
    uint64_t highBits[HighWords] = {};
    setBit(highBits, context->highReadFd);
    const size_t highExtent = bitmapExtent(HighNfds);
    fd_set* highSet = reinterpret_cast<fd_set*>(address + pageSize - highExtent);
    zero = {0, 0};
    dynamicBitmaps &= context->highReadFd < static_cast<size_t>(HighNfds) &&
                      PosixSubsystem::copyToUser(highSet, highBits, highExtent) &&
                      posix_pselect6(HighNfds, highSet, nullptr, nullptr, &zero, nullptr) == 1 &&
                      PosixSubsystem::copyFromUser(highBits, highSet, highExtent) &&
                      isSet(highBits, context->highReadFd);

    dynamicBitmaps &= MemoryMapManager::instance().remove(address, pageSize) == 1;
    process->getSpaceAllocator().free(address, pageSize);
  } else if (allocated) {
    if (mapping) {
      MemoryMapManager::instance().remove(mappedAddress, pageSize);
    }
    process->getSpaceAllocator().free(address, pageSize);
  }
  passed &= dynamicBitmaps;
  passed &= eventFd >= 0 && posix_close(eventFd) == 0;

  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}

bool pselectValidationAndDynamicBitmaps(Process* kernelProcess) {
  constexpr size_t HighReadDescriptor = 1057;
  constexpr size_t HighWriteDescriptor = 1058;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  Pipe* pipe = new Pipe;
  FileDescriptor* reader = new FileDescriptor(pipe, 0, HighReadDescriptor, 0, O_RDONLY);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, HighWriteDescriptor, 0, O_WRONLY);
  subsystem->addFileDescriptor(HighReadDescriptor, reader);
  subsystem->addFileDescriptor(HighWriteDescriptor, writer);

  char value = 'v';
  const bool madeReady = writer->write(1, reinterpret_cast<uintptr_t>(&value), true) == 1;
  PselectValidationContext context(HighReadDescriptor);
  Thread* worker =
      new Thread(process, pselectValidationWorker, &context, nullptr, false, true, true);
  worker->setName("hosted pselect validation worker");
  const bool started = madeReady && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool writerClosed = closeDescriptor(subsystem, HighWriteDescriptor);
  const bool readerClosed = closeDescriptor(subsystem, HighReadDescriptor);
  const bool passed =
      started && joined && context.returned == 1 && context.passed && writerClosed && readerClosed;
  delete process;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL pselect-validation-bitmap: "
        "Linux ABI validation, ready-bit counting, or bitmap extent regressed");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS pselect-validation-bitmap");
  return true;
}

struct PselectSignalContext {
  explicit PselectSignalContext(size_t readFd)
      : readFd(readFd),
        entered(0),
        returned(0),
        result(-2),
        error(0),
        readReady(false),
        restoredMask(0),
        timeout({5, 0}) {}

  size_t readFd;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  int result;
  int error;
  bool readReady;
  uint64_t restoredMask;
  LinuxKernelTimespec timeout;
};

int pselectSignalWorker(void* parameter) {
  PselectSignalContext* context = reinterpret_cast<PselectSignalContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  uint64_t readBits[2] = {};
  setBit(readBits, context->readFd);
  LinuxPselectSigsetArgument argument = {reinterpret_cast<uintptr_t>(&RequestedSignalMask),
                                         sizeof(RequestedSignalMask)};
  thread->setSignalMask(OriginalSignalMask);
  thread->clearInterruption();
  context->entered += 1;
  thread->setErrno(PreservedErrno);
  context->result =
      posix_pselect6(static_cast<int>(context->readFd + 1), reinterpret_cast<fd_set*>(readBits),
                     nullptr, nullptr, &context->timeout, &argument);
  context->error = thread->getErrno();
  context->readReady = isSet(readBits, context->readFd);
  context->restoredMask = thread->getSignalMask();
  thread->setSignalMask(0);
  thread->clearInterruption();
  context->returned += 1;
  return 0;
}

bool pselectSignalRace(Process* kernelProcess, bool readyWins) {
  constexpr size_t ReadDescriptor = 91;
  constexpr size_t WriteDescriptor = 92;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  Pipe* pipe = new Pipe;
  FileDescriptor* reader = new FileDescriptor(pipe, 0, ReadDescriptor, 0, O_RDONLY);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  subsystem->addFileDescriptor(WriteDescriptor, writer);

  g_PselectSignalHandlerCalls = 0;
  g_PselectSignalHandlerWrites = 0;
  g_PselectSignalWriter = readyWins ? writer : nullptr;
  PselectSignalContext context(ReadDescriptor);
  Thread* worker = new Thread(process, pselectSignalWorker, &context, nullptr, false, true, true);
  if (readyWins) {
    worker->setName("hosted pselect ready-signal worker");
  } else {
    worker->setName("hosted pselect EINTR worker");
  }
  const bool started = worker->start();
  while (started && !context.entered) {
    Scheduler::instance().yield();
  }
  const bool blocked = started && waitForPselectBlock(worker);
  const bool activeMaskObserved = blocked && worker->getSignalMask() == ActiveSignalMask;
  SignalEvent* signal = new SignalEvent(reinterpret_cast<uintptr_t>(&pselectSignalHandler),
                                        TestSignal, ~0UL, 0, true, true);
  const bool signalQueued = blocked && worker->sendEvent(signal);
  if (!signalQueued) {
    delete signal;
  }

  bool rescueWrite = false;
  const Time::Timestamp returnDeadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (started && !context.returned && Time::getTicks() < returnDeadline) {
    Scheduler::instance().yield();
  }
  if (started && !context.returned) {
    char rescue = 'x';
    rescueWrite = writer->write(1, reinterpret_cast<uintptr_t>(&rescue), true) == 1;
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  g_PselectSignalWriter = nullptr;

  const bool resultPassed =
      readyWins ? context.result == 1 && context.readReady && g_PselectSignalHandlerWrites == 1
                : context.result == -1 && context.error == Error::Interrupted &&
                      !g_PselectSignalHandlerWrites;
  bool passed = started && blocked && activeMaskObserved && signalQueued && !rescueWrite &&
                joined && context.returned == 1 && resultPassed &&
                context.restoredMask == OriginalSignalMask && g_PselectSignalHandlerCalls == 1 &&
                context.timeout.tv_sec >= 0 && context.timeout.tv_sec <= 5 &&
                context.timeout.tv_nsec >= 0 && context.timeout.tv_nsec < 1000000000;
  const bool writerClosed = closeDescriptor(subsystem, WriteDescriptor);
  const bool readerClosed = closeDescriptor(subsystem, ReadDescriptor);
  passed &= writerClosed && readerClosed;
  delete process;
  if (!passed) {
    ERROR("HOSTED-SYSCALL-TEST: FAIL "
          << (readyWins ? "pselect-ready-beats-signal: " : "pselect-eintr-mask: ")
          << "temporary mask, restoration, or final readiness precedence regressed");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS "
         << (readyWins ? "pselect-ready-beats-signal" : "pselect-eintr-mask"));
  return true;
}

struct PselectOutputFaultContext {
  explicit PselectOutputFaultContext(size_t readFd)
      : readFd(readFd), entered(0), returned(0), result(-2), error(0), passed(false) {}

  size_t readFd;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  int result;
  int error;
  bool passed;
};

int pselectOutputFaultWorker(void* parameter) {
  PselectOutputFaultContext* context = reinterpret_cast<PselectOutputFaultContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const int nfds = static_cast<int>(context->readFd + 1);
  const size_t extent = bitmapExtent(nfds);
  uintptr_t address = 0;
  const bool allocated = process->getSpaceAllocator().allocate(pageSize, address);
  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping =
      allocated ? MemoryMapManager::instance().mapAnon(
                      mappedAddress, pageSize, MemoryMappedObject::Read | MemoryMappedObject::Write)
                : nullptr;
  fd_set* readSet = reinterpret_cast<fd_set*>(address + pageSize - extent);
  uint64_t readBits[2] = {};
  uint64_t writeBits[2] = {};
  setBit(readBits, context->readFd);
  setBit(writeBits, context->readFd);
  const bool prepared =
      mapping && mappedAddress == address &&
      PosixSubsystem::copyToUser(readSet, readBits, extent) &&
      MemoryMapManager::instance().setPermissions(address, pageSize, MemoryMappedObject::Read) == 1;

  LinuxKernelTimespec timeout = {5, 0};
  LinuxPselectSigsetArgument argument = {reinterpret_cast<uintptr_t>(&RequestedSignalMask),
                                         sizeof(RequestedSignalMask)};
  thread->setSignalMask(OriginalSignalMask);
  thread->clearInterruption();
  if (prepared) {
    context->entered += 1;
    thread->setErrno(0);
    context->result = posix_pselect6(nfds, readSet, reinterpret_cast<fd_set*>(writeBits), nullptr,
                                     &timeout, &argument);
    context->error = thread->getErrno();
  }
  const bool maskRestored = thread->getSignalMask() == OriginalSignalMask;
  thread->setSignalMask(0);
  thread->clearInterruption();
  const bool timeoutWritten = timeout.tv_sec >= 0 && timeout.tv_sec < 5 && timeout.tv_nsec >= 0 &&
                              timeout.tv_nsec < 1000000000;
  const bool laterOutputUntouched = isSet(writeBits, context->readFd);
  context->passed = prepared && context->result == -1 && context->error == Error::BadAddress &&
                    maskRestored && timeoutWritten && laterOutputUntouched;

  if (mapping) {
    MemoryMapManager::instance().remove(address, pageSize);
  }
  if (allocated) {
    process->getSpaceAllocator().free(address, pageSize);
  }
  context->returned += 1;
  return context->passed ? 0 : 1;
}

bool pselectOutputFaultCleanup(Process* kernelProcess) {
  constexpr size_t ReadDescriptor = 93;
  constexpr size_t WriteDescriptor = 94;
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  Pipe* pipe = new Pipe;
  FileDescriptor* reader = new FileDescriptor(pipe, 0, ReadDescriptor, 0, O_RDONLY);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  subsystem->addFileDescriptor(WriteDescriptor, writer);

  PselectOutputFaultContext context(ReadDescriptor);
  Thread* worker =
      new Thread(process, pselectOutputFaultWorker, &context, nullptr, false, true, true);
  worker->setName("hosted pselect output fault worker");
  const bool started = worker->start();
  while (started && !context.entered && !context.returned) {
    Scheduler::instance().yield();
  }
  const bool blocked = started && waitForPselectBlock(worker);
  const bool activeMaskObserved = blocked && worker->getSignalMask() == ActiveSignalMask;
  char value = 'o';
  const bool madeReady =
      blocked && writer->write(1, reinterpret_cast<uintptr_t>(&value), true) == 1;
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  bool passed = started && blocked && activeMaskObserved && madeReady && joined &&
                context.returned == 1 && context.passed;
  const bool writerClosed = closeDescriptor(subsystem, WriteDescriptor);
  const bool readerClosed = closeDescriptor(subsystem, ReadDescriptor);
  passed &= writerClosed && readerClosed;
  delete process;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL pselect-output-fault: "
        "mask cleanup, timeout writeback, or fdset copyout order regressed");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS pselect-output-fault");
  return true;
}
}  // namespace

bool runHostedPselectRegressions(Process* process) {
  return pselectValidationAndDynamicBitmaps(process) && pselectSignalRace(process, false) &&
         pselectSignalRace(process, true) && pselectOutputFaultCleanup(process);
}
