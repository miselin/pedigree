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
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/lib.h"

#include <fcntl.h>
#include <signal.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/UnixFilesystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/subsys/posix/net-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/VFS.h"
#include <sys/socket.h>

namespace {
constexpr size_t ReturnTimeout = 2 * Time::Multiplier::Second;
constexpr size_t SourceDescriptor = 90;

Atomic<size_t> g_SignalCalls(0);
Atomic<int> g_SignalCloseDescriptor(-1);
Atomic<int> g_SignalCloseResult(-2);
Atomic<int> g_SecondSignalCloseDescriptor(-1);
Atomic<int> g_SecondSignalCloseResult(-2);
Atomic<size_t> g_LateCloneRejected(0);
FileDescriptor* g_LateCloneSource = nullptr;

void streamSignalHandler(size_t) {
  g_SignalCalls += 1;
  const int descriptor = g_SignalCloseDescriptor;
  if (descriptor >= 0 && g_SignalCloseDescriptor.compareAndSwap(descriptor, -1)) {
    g_SignalCloseResult = posix_close(descriptor);
  }
}

void closeFromStreamControlLock() {
  streamSignalHandler(SIGUSR1);
}

void rejectLateCloneAfterHandlerClose() {
  if (!g_LateCloneSource) {
    return;
  }

  FileDescriptor* lateClone = new FileDescriptor(*g_LateCloneSource);
  if (lateClone->networkImpl && !lateClone->networkPublished()) {
    g_LateCloneRejected += 1;
  }
  delete lateClone;
}

void closeFromEndpointLockOrReadinessLease() {
  streamSignalHandler(SIGUSR1);
  rejectLateCloneAfterHandlerClose();
}

void closePairFromEndpointMutationLocks() {
  closeFromEndpointLockOrReadinessLease();
  const int descriptor = g_SecondSignalCloseDescriptor;
  if (descriptor >= 0 && g_SecondSignalCloseDescriptor.compareAndSwap(descriptor, -1)) {
    g_SecondSignalCloseResult = posix_close(descriptor);
  }
}

union ControlBuffer {
  struct cmsghdr alignment;
  uint8_t bytes[CMSG_SPACE(sizeof(int))];
};

struct StreamFixture {
  int sockets[2];
  uint8_t payload[1024];
  struct iovec vector;
  struct msghdr message;
  ControlBuffer control;
};

enum class IoOperation {
  Receive,
  Send,
};

struct IoContext {
  IoContext(int descriptor, uint8_t* payload, size_t length, IoOperation operation)
      : descriptor(descriptor),
        payload(payload),
        length(length),
        operation(operation),
        entered(0),
        returned(0),
        result(-2),
        error(0) {}

  int descriptor;
  uint8_t* payload;
  size_t length;
  IoOperation operation;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  ssize_t result;
  int error;
};

int ioWorker(void* parameter) {
  IoContext* context = reinterpret_cast<IoContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(0);
  thread->clearInterruption();
  context->entered += 1;
  if (context->operation == IoOperation::Receive) {
    context->result = posix_recv(context->descriptor, context->payload, context->length, 0);
  } else {
    context->result = posix_send(context->descriptor, context->payload, context->length, 0);
  }
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

bool waitUntilBlocked(Thread* thread, IoContext& context,
                      Thread::DebugState expectedState = Thread::CondWait) {
  const Time::Timestamp deadline = Time::getTicks() + ReturnTimeout;
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo wait = {};
    uintptr_t address = 0;
    if (context.entered == 1 && !context.returned && thread->getWaitDebugInfo(wait) && wait.queue &&
        wait.queued && thread->getDebugState(address) == expectedState) {
      return true;
    }
    if (thread->getStatus() == Thread::AwaitingJoin) {
      return false;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitUntilReturned(IoContext& context) {
  const Time::Timestamp deadline = Time::getTicks() + ReturnTimeout;
  while (!context.returned && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  return context.returned == 1;
}

bool sendCaughtSignal(Thread* thread) {
  SignalEvent* signal = new SignalEvent(reinterpret_cast<uintptr_t>(&streamSignalHandler), SIGUSR1,
                                        ~0UL, 0, true, true);
  if (thread->sendEvent(signal)) {
    return true;
  }
  delete signal;
  return false;
}

bool closeSocket(int& descriptor) {
  if (descriptor < 0) {
    return true;
  }
  const bool closed = posix_close(descriptor) == 0;
  descriptor = -1;
  return closed;
}

bool closePair(StreamFixture& fixture) {
  const bool first = closeSocket(fixture.sockets[0]);
  const bool second = closeSocket(fixture.sockets[1]);
  return first && second;
}

bool createPair(StreamFixture& fixture) {
  fixture.sockets[0] = fixture.sockets[1] = -1;
  return posix_socketpair(AF_UNIX, SOCK_STREAM, 0, fixture.sockets) == 0;
}

FileDescriptor::OpenFileDescriptionLease addSourceDescriptor(PosixSubsystem* subsystem) {
  FileDescriptor* source = new FileDescriptor;
  source->fd = SourceDescriptor;
  FileDescriptor::OpenFileDescriptionLease description = source->acquireOpenFileDescription();
  subsystem->addFileDescriptor(SourceDescriptor, source);
  return description;
}

void prepareControlMessage(StreamFixture& fixture, bool sending) {
  ByteSet(&fixture.control, 0, sizeof(fixture.control));
  fixture.vector = {fixture.payload, 1};
  fixture.message = {};
  fixture.message.msg_iov = &fixture.vector;
  fixture.message.msg_iovlen = 1;
  fixture.message.msg_control = fixture.control.bytes;
  fixture.message.msg_controllen = sizeof(fixture.control.bytes);
  if (sending) {
    struct cmsghdr* header = CMSG_FIRSTHDR(&fixture.message);
    header->cmsg_len = CMSG_LEN(sizeof(int));
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    const int source = SourceDescriptor;
    MemoryCopy(CMSG_DATA(header), &source, sizeof(source));
  }
}

bool signalHandlerCloseWhileControlLocked(PosixSubsystem* subsystem, StreamFixture& fixture,
                                          IoOperation operation) {
  if (!createPair(fixture)) {
    return false;
  }

  FileDescriptor::OpenFileDescriptionLease source = addSourceDescriptor(subsystem);
  fixture.payload[0] = 'c';
  prepareControlMessage(fixture, true);
  bool passed = true;
  if (operation == IoOperation::Receive) {
    if (posix_sendmsg(fixture.sockets[0], &fixture.message, 0) != 1 ||
        posix_close(SourceDescriptor) != 0 || source->descriptorOwnerCount() != 1 ||
        SocketRights::inFlightForTest() != 1) {
      posix_close(SourceDescriptor);
      closePair(fixture);
      return false;
    }
    prepareControlMessage(fixture, false);
  }

  const size_t operationSide = operation == IoOperation::Receive ? 1 : 0;
  const size_t closeSide = 1;
  g_SignalCalls = 0;
  g_SignalCloseResult = -2;
  g_SignalCloseDescriptor = fixture.sockets[closeSide];
  setUnixStreamControlLockHookForTest(closeFromStreamControlLock);
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(0);
  const ssize_t result = operation == IoOperation::Receive
                             ? posix_recvmsg(fixture.sockets[operationSide], &fixture.message, 0)
                             : posix_sendmsg(fixture.sockets[operationSide], &fixture.message, 0);
  const int error = thread->getErrno();
  setUnixStreamControlLockHookForTest(nullptr);

  const bool handlerClosed = g_SignalCloseResult == 0;
  if (handlerClosed) {
    fixture.sockets[closeSide] = -1;
  }
  g_SignalCloseDescriptor = -1;

  if (operation == IoOperation::Send) {
    passed = result == -1 && error == Error::BrokenPipe && posix_close(SourceDescriptor) == 0 &&
             source->descriptorOwnerCount() == 0 && passed;
  } else {
    passed = result == 0 && source->descriptorOwnerCount() == 0 && passed;
  }
  passed = handlerClosed && g_SignalCalls == 1 && SocketRights::inFlightForTest() == 0 && passed;
  return closePair(fixture) && passed;
}

bool lateCloneIsRejected(StreamFixture& fixture) {
  if (!createPair(fixture)) {
    return false;
  }

  DescriptorLease retained;
  if (!acquireDescriptor(fixture.sockets[0], retained)) {
    closePair(fixture);
    return false;
  }

  const bool closed = closeSocket(fixture.sockets[0]);
  FileDescriptor* lateClone = closed ? new FileDescriptor(*retained) : nullptr;
  const bool rejected = lateClone && retained->networkImpl && !lateClone->networkPublished();
  delete lateClone;
  retained.reset();
  return rejected && closePair(fixture);
}

bool handlerCloseWhileEndpointMutationLocked(StreamFixture& fixture) {
  if (!createPair(fixture)) {
    return false;
  }

  DescriptorLease retained;
  if (!acquireDescriptor(fixture.sockets[0], retained)) {
    closePair(fixture);
    return false;
  }
  SharedPointer<NetworkSyscalls> network = retained->networkImpl;

  g_SignalCalls = 0;
  g_SignalCloseResult = -2;
  g_LateCloneRejected = 0;
  g_LateCloneSource = &*retained;
  g_SignalCloseDescriptor = fixture.sockets[0];
  setUnixEndpointMutationLockHookForTest(closeFromEndpointLockOrReadinessLease);
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(0);
  const bool created = network->create();
  const int error = thread->getErrno();
  setUnixEndpointMutationLockHookForTest(nullptr);

  const bool handlerClosed = g_SignalCloseResult == 0;
  if (handlerClosed) {
    fixture.sockets[0] = -1;
  }
  g_SignalCloseDescriptor = -1;
  g_LateCloneSource = nullptr;

  const ReadyMask ready = network->queryReady(true, true);
  const ssize_t peerResult = posix_recv(fixture.sockets[1], fixture.payload, 1, 0);
  const bool passed = !created && error == Error::BadFileDescriptor && handlerClosed &&
                      g_SignalCalls == 1 && g_LateCloneRejected == 1 &&
                      (ready & (ReadyInvalid | ReadyHangup)) == (ReadyInvalid | ReadyHangup) &&
                      peerResult == 0;
  return closePair(fixture) && passed;
}

bool handlerCloseWhileEndpointReadinessActive(StreamFixture& fixture) {
  if (!createPair(fixture)) {
    return false;
  }

  DescriptorLease retained;
  if (!acquireDescriptor(fixture.sockets[0], retained)) {
    closePair(fixture);
    return false;
  }
  SharedPointer<NetworkSyscalls> network = retained->networkImpl;

  g_SignalCalls = 0;
  g_SignalCloseResult = -2;
  g_LateCloneRejected = 0;
  g_LateCloneSource = &*retained;
  g_SignalCloseDescriptor = fixture.sockets[0];
  setUnixEndpointReadinessLeaseHookForTest(closeFromEndpointLockOrReadinessLease);
  const ReadyMask ready = network->queryReady(true, true);
  setUnixEndpointReadinessLeaseHookForTest(nullptr);

  const bool handlerClosed = g_SignalCloseResult == 0;
  if (handlerClosed) {
    fixture.sockets[0] = -1;
  }
  g_SignalCloseDescriptor = -1;
  g_LateCloneSource = nullptr;

  const ssize_t peerResult = posix_recv(fixture.sockets[1], fixture.payload, 1, 0);
  const bool passed = handlerClosed && g_SignalCalls == 1 && g_LateCloneRejected == 1 &&
                      (ready & (ReadyInvalid | ReadyHangup)) == (ReadyInvalid | ReadyHangup) &&
                      peerResult == 0;
  return closePair(fixture) && passed;
}

bool handlerClosesBothEndpointsWhilePairLocksOwned(StreamFixture& fixture) {
  fixture.sockets[0] = posix_socket(AF_UNIX, SOCK_STREAM, 0);
  fixture.sockets[1] = posix_socket(AF_UNIX, SOCK_STREAM, 0);
  if (fixture.sockets[0] < 0 || fixture.sockets[1] < 0) {
    closePair(fixture);
    return false;
  }

  DescriptorLease firstDescriptor;
  DescriptorLease secondDescriptor;
  if (!acquireDescriptor(fixture.sockets[0], firstDescriptor) ||
      !acquireDescriptor(fixture.sockets[1], secondDescriptor)) {
    closePair(fixture);
    return false;
  }
  UnixSocketSyscalls* first = static_cast<UnixSocketSyscalls*>(firstDescriptor->networkImpl.get());
  UnixSocketSyscalls* second =
      static_cast<UnixSocketSyscalls*>(secondDescriptor->networkImpl.get());

  g_SignalCalls = 0;
  g_SignalCloseResult = -2;
  g_SecondSignalCloseResult = -2;
  g_LateCloneRejected = 0;
  g_LateCloneSource = &*firstDescriptor;
  g_SignalCloseDescriptor = fixture.sockets[0];
  g_SecondSignalCloseDescriptor = fixture.sockets[1];
  setUnixEndpointMutationLockHookForTest(closePairFromEndpointMutationLocks);
  const bool paired = first->pairWith(second);
  setUnixEndpointMutationLockHookForTest(nullptr);

  const bool firstClosed = g_SignalCloseResult == 0;
  const bool secondClosed = g_SecondSignalCloseResult == 0;
  if (firstClosed) {
    fixture.sockets[0] = -1;
  }
  if (secondClosed) {
    fixture.sockets[1] = -1;
  }
  g_SignalCloseDescriptor = -1;
  g_SecondSignalCloseDescriptor = -1;
  g_LateCloneSource = nullptr;

  const ReadyMask firstReady = first->queryReady(true, true);
  const ReadyMask secondReady = second->queryReady(true, true);
  const bool passed = !paired && firstClosed && secondClosed && g_SignalCalls == 1 &&
                      g_LateCloneRejected == 1 &&
                      (firstReady & (ReadyInvalid | ReadyHangup)) == (ReadyInvalid | ReadyHangup) &&
                      (secondReady & (ReadyInvalid | ReadyHangup)) == (ReadyInvalid | ReadyHangup);
  return closePair(fixture) && passed;
}

bool setNonblocking(int descriptor, bool nonblocking) {
  const int flags = posix_fcntl(descriptor, F_GETFL, nullptr);
  if (flags < 0) {
    return false;
  }
  const int replacement = nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
  return posix_fcntl(descriptor, F_SETFL, reinterpret_cast<void*>(replacement)) == 0;
}

bool fillSendQueue(StreamFixture& fixture) {
  if (!setNonblocking(fixture.sockets[0], true)) {
    return false;
  }

  ByteSet(fixture.payload, 0x51, sizeof(fixture.payload));
  size_t written = 0;
  while (true) {
    Thread* thread = Processor::information().getCurrentThread();
    thread->setErrno(0);
    const ssize_t result =
        posix_send(fixture.sockets[0], fixture.payload, sizeof(fixture.payload), 0);
    if (result > 0) {
      written += static_cast<size_t>(result);
      continue;
    }
    if (result != -1 || thread->getErrno() != Error::NoMoreProcesses) {
      return false;
    }
    break;
  }

  return written == MAX_UNIX_STREAM_QUEUE && setNonblocking(fixture.sockets[0], false);
}

bool signalInterruptsEmptyReceive(Process* process, StreamFixture& fixture) {
  if (!createPair(fixture)) {
    return false;
  }

  g_SignalCalls = 0;
  IoContext context(fixture.sockets[1], fixture.payload, 1, IoOperation::Receive);
  Thread* worker = new Thread(process, ioWorker, &context, nullptr, false, true, true);
  worker->setName("hosted AF_UNIX interrupted receiver");
  const bool started = worker->start();
  const bool blocked = started && waitUntilBlocked(worker, context);
  const bool signalled = blocked && sendCaughtSignal(worker);
  const bool returned = signalled && waitUntilReturned(context);

  if (started && !returned) {
    closePair(fixture);
    if (!context.returned) {
      sendCaughtSignal(worker);
      waitUntilReturned(context);
    }
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool closed = closePair(fixture);
  return started && blocked && signalled && returned && joined && context.result == -1 &&
         context.error == Error::Interrupted && g_SignalCalls == 1 && closed;
}

bool signalInterruptsFullSend(Process* process, StreamFixture& fixture) {
  if (!createPair(fixture) || !fillSendQueue(fixture)) {
    closePair(fixture);
    return false;
  }

  g_SignalCalls = 0;
  IoContext context(fixture.sockets[0], fixture.payload, 1, IoOperation::Send);
  Thread* worker = new Thread(process, ioWorker, &context, nullptr, false, true, true);
  worker->setName("hosted AF_UNIX interrupted sender");
  const bool started = worker->start();
  const bool blocked = started && waitUntilBlocked(worker, context);
  const bool signalled = blocked && sendCaughtSignal(worker);
  const bool returned = signalled && waitUntilReturned(context);

  if (started && !returned) {
    closePair(fixture);
    if (!context.returned) {
      sendCaughtSignal(worker);
      waitUntilReturned(context);
    }
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool closed = closePair(fixture);
  return started && blocked && signalled && returned && joined && context.result == -1 &&
         context.error == Error::Interrupted && g_SignalCalls == 1 && closed;
}

bool signalInterruptsSerializationWait(Process* process, StreamFixture& fixture,
                                       IoOperation operation) {
  if (!createPair(fixture)) {
    return false;
  }
  if (operation == IoOperation::Send && !fillSendQueue(fixture)) {
    closePair(fixture);
    return false;
  }

  const size_t operationSide = operation == IoOperation::Receive ? 1 : 0;
  g_SignalCalls = 0;
  IoContext holderContext(fixture.sockets[operationSide], fixture.payload, 1, operation);
  IoContext waiterContext(fixture.sockets[operationSide], fixture.payload, 1, operation);
  Thread* holder = new Thread(process, ioWorker, &holderContext, nullptr, false, true, true);
  Thread* waiter = new Thread(process, ioWorker, &waiterContext, nullptr, false, true, true);
  holder->setName("hosted AF_UNIX serialization holder");
  waiter->setName("hosted AF_UNIX serialization waiter");

  const bool holderStarted = holder->start();
  const bool holderBlocked = holderStarted && waitUntilBlocked(holder, holderContext);
  const bool waiterStarted = holderBlocked && waiter->start();
  const bool waiterBlocked =
      waiterStarted && waitUntilBlocked(waiter, waiterContext, Thread::SemWait);
  const bool signalled = waiterBlocked && sendCaughtSignal(waiter);
  const bool waiterReturned = signalled && waitUntilReturned(waiterContext);
  const bool holderStayedBlocked = !holderContext.returned;

  const bool closed = closePair(fixture);
  const bool holderReturned = holderStarted && waitUntilReturned(holderContext);
  if (waiterStarted && !waiterContext.returned) {
    sendCaughtSignal(waiter);
    waitUntilReturned(waiterContext);
  }
  if (holderStarted && !holderContext.returned) {
    sendCaughtSignal(holder);
    waitUntilReturned(holderContext);
  }

  const bool waiterJoined = waiterStarted && waiter->joinForCompletion();
  const bool holderJoined = holderStarted && holder->joinForCompletion();
  if (!waiterStarted) {
    delete waiter;
  }
  if (!holderStarted) {
    delete holder;
  }

  const bool holderResult =
      operation == IoOperation::Receive
          ? holderContext.result == 0
          : holderContext.result == -1 && holderContext.error == Error::BrokenPipe;
  return holderStarted && holderBlocked && waiterStarted && waiterBlocked && signalled &&
         waiterReturned && holderStayedBlocked && closed && holderReturned && waiterJoined &&
         holderJoined && waiterContext.result == -1 && waiterContext.error == Error::Interrupted &&
         holderResult && g_SignalCalls == 1;
}

bool partialSendWinsSignal(Process* process, StreamFixture& fixture) {
  if (!createPair(fixture) || !fillSendQueue(fixture)) {
    closePair(fixture);
    return false;
  }

  if (posix_recv(fixture.sockets[1], fixture.payload, 1, 0) != 1) {
    closePair(fixture);
    return false;
  }
  fixture.payload[0] = 'x';
  fixture.payload[1] = 'y';

  g_SignalCalls = 0;
  IoContext context(fixture.sockets[0], fixture.payload, 2, IoOperation::Send);
  Thread* worker = new Thread(process, ioWorker, &context, nullptr, false, true, true);
  worker->setName("hosted AF_UNIX partial interrupted sender");
  const bool started = worker->start();
  const bool blocked = started && waitUntilBlocked(worker, context);
  const bool signalled = blocked && sendCaughtSignal(worker);
  const bool returned = signalled && waitUntilReturned(context);

  if (started && !returned) {
    closePair(fixture);
    if (!context.returned) {
      sendCaughtSignal(worker);
      waitUntilReturned(context);
    }
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool closed = closePair(fixture);
  return started && blocked && signalled && returned && joined && context.result == 1 &&
         context.error == 0 && g_SignalCalls == 1 && closed;
}

bool closeWakesBlockedIo(Process* process, StreamFixture& fixture, IoOperation operation,
                         bool closeLocal) {
  if (!createPair(fixture)) {
    return false;
  }
  if (operation == IoOperation::Send && !fillSendQueue(fixture)) {
    closePair(fixture);
    return false;
  }

  const size_t operationSide = operation == IoOperation::Receive ? 1 : 0;
  const size_t closeSide = closeLocal ? operationSide : 1 - operationSide;
  IoContext context(fixture.sockets[operationSide], fixture.payload, 1, operation);
  Thread* worker = new Thread(process, ioWorker, &context, nullptr, false, true, true);
  if (operation == IoOperation::Receive) {
    worker->setName("hosted AF_UNIX close-woken receiver");
  } else {
    worker->setName("hosted AF_UNIX close-woken sender");
  }
  const bool started = worker->start();
  const bool blocked = started && waitUntilBlocked(worker, context);
  const bool triggeringClose = blocked && closeSocket(fixture.sockets[closeSide]);
  const bool returned = triggeringClose && waitUntilReturned(context);

  if (started && !returned) {
    closePair(fixture);
    if (!context.returned) {
      sendCaughtSignal(worker);
      waitUntilReturned(context);
    }
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool closed = closePair(fixture);
  const bool result = operation == IoOperation::Receive
                          ? context.result == 0
                          : context.result == -1 && context.error == Error::BrokenPipe;
  return started && blocked && triggeringClose && returned && joined && result && closed;
}

struct SuiteContext {
  explicit SuiteContext(Process* process) : process(process), completed(0), passed(false) {}

  Process* process;
  Atomic<size_t> completed;
  bool passed;
};

int suiteWorker(void* parameter) {
  SuiteContext* context = reinterpret_cast<SuiteContext*>(parameter);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t mappingAddress = 0;
  if (!context->process->allocateUserRange(Process::UserRegion::Normal, pageSize, mappingAddress)) {
    context->completed += 1;
    return 1;
  }

  uintptr_t mappedAddress = mappingAddress;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapAnon(
      mappedAddress, pageSize, MemoryMappedObject::Read | MemoryMappedObject::Write);
  if (!mapping || mappedAddress != mappingAddress || sizeof(StreamFixture) > pageSize) {
    if (mapping) {
      MemoryMapManager::instance().remove(mappedAddress, pageSize);
    }
    context->process->freeUserRange(Process::UserRegion::Normal, mappingAddress, pageSize);
    context->completed += 1;
    return 1;
  }

  StreamFixture* fixture = reinterpret_cast<StreamFixture*>(mappedAddress);
  ByteSet(fixture, 0, sizeof(*fixture));
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(context->process->getSubsystem());
  bool passed = lateCloneIsRejected(*fixture);
  passed = handlerCloseWhileEndpointMutationLocked(*fixture) && passed;
  passed = handlerCloseWhileEndpointReadinessActive(*fixture) && passed;
  passed = handlerClosesBothEndpointsWhilePairLocksOwned(*fixture) && passed;
  passed =
      signalHandlerCloseWhileControlLocked(subsystem, *fixture, IoOperation::Receive) && passed;
  passed = signalHandlerCloseWhileControlLocked(subsystem, *fixture, IoOperation::Send) && passed;
  passed = signalInterruptsEmptyReceive(context->process, *fixture) && passed;
  passed = signalInterruptsFullSend(context->process, *fixture) && passed;
  passed =
      signalInterruptsSerializationWait(context->process, *fixture, IoOperation::Receive) && passed;
  passed =
      signalInterruptsSerializationWait(context->process, *fixture, IoOperation::Send) && passed;
  passed = partialSendWinsSignal(context->process, *fixture) && passed;
  passed = closeWakesBlockedIo(context->process, *fixture, IoOperation::Receive, false) && passed;
  passed = closeWakesBlockedIo(context->process, *fixture, IoOperation::Receive, true) && passed;
  passed = closeWakesBlockedIo(context->process, *fixture, IoOperation::Send, false) && passed;
  passed = closeWakesBlockedIo(context->process, *fixture, IoOperation::Send, true) && passed;

  MemoryMapManager::instance().remove(mappedAddress, pageSize);
  context->process->freeUserRange(Process::UserRegion::Normal, mappingAddress, pageSize);
  context->passed = passed;
  context->completed += 1;
  return passed ? 0 : 1;
}
}  // namespace

bool runHostedUnixStreamInterruptionRegressions(Process* kernelProcess) {
  Filesystem* priorRoot = VFS::instance().getRootFilesystem();
  auto* priorView = VFS::instance().mountView();
  VFS::HostedRootViewScope fixture;
  UnixFilesystem* filesystem = new UnixFilesystem;
  if (!fixture.open(filesystem)) {
    delete filesystem;
    return false;
  }
  Process* process =
      new PosixProcess(kernelProcess, true, Process::FilesystemContextMode::Deferred);
  process->setSubsystem(new PosixSubsystem);
  const bool contextInstalled = fixture.installContext(*process);
  SuiteContext context(process);
  Thread* worker = new Thread(process, suiteWorker, &context, nullptr, false, true, true);
  worker->setName("hosted AF_UNIX stream interruption suite");
  const bool started = contextInstalled && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  bool passed = started && joined && context.completed == 1 && context.passed;
  delete process;

  const bool rootRestored = fixture.close();
  if (!rootRestored)
    FATAL("Hosted filesystem fixture retained owners after teardown");
  passed = rootRestored && VFS::instance().getRootFilesystem() == priorRoot &&
           VFS::instance().mountView() == priorView && passed;
  delete filesystem;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL unix-stream-interruption: "
        "EINTR, partial-progress, or close wake semantics regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS unix-stream-interruption");
  return true;
}
