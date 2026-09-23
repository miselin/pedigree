/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/lib.h"

#include <fcntl.h>

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
constexpr size_t SourceDescriptor = 90;
constexpr size_t SecondSourceDescriptor = 91;

union ControlBuffer {
  struct cmsghdr alignment;
  uint8_t bytes[CMSG_SPACE(2 * sizeof(int))];
};

struct StreamFixture {
  int sockets[2];
  uint8_t sendPayload[512];
  uint8_t receivePayload[512];
  struct iovec sendVector;
  struct iovec receiveVector;
  ControlBuffer sendControl;
  ControlBuffer receiveControl;
  struct msghdr sendMessage;
  struct msghdr receiveMessage;
};

struct StreamContext {
  explicit StreamContext(Process* process) : process(process), completed(false), result(false) {}

  Process* process;
  bool completed;
  bool result;
};

void prepareSend(StreamFixture& fixture, const void* payload, size_t payloadLength,
                 const int* descriptors, size_t descriptorCount) {
  ByteSet(&fixture.sendControl, 0, sizeof(fixture.sendControl));
  fixture.sendVector = {const_cast<void*>(payload), payloadLength};
  fixture.sendMessage = {};
  fixture.sendMessage.msg_iov = &fixture.sendVector;
  fixture.sendMessage.msg_iovlen = 1;
  fixture.sendMessage.msg_control = fixture.sendControl.bytes;
  fixture.sendMessage.msg_controllen = CMSG_SPACE(descriptorCount * sizeof(int));
  struct cmsghdr* header = CMSG_FIRSTHDR(&fixture.sendMessage);
  header->cmsg_len = CMSG_LEN(descriptorCount * sizeof(int));
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  MemoryCopy(CMSG_DATA(header), descriptors, descriptorCount * sizeof(int));
}

void prepareReceive(StreamFixture& fixture, size_t payloadCapacity, size_t controlCapacity) {
  ByteSet(fixture.receivePayload, 0, sizeof(fixture.receivePayload));
  ByteSet(&fixture.receiveControl, 0, sizeof(fixture.receiveControl));
  fixture.receiveVector = {fixture.receivePayload, payloadCapacity};
  fixture.receiveMessage = {};
  fixture.receiveMessage.msg_iov = &fixture.receiveVector;
  fixture.receiveMessage.msg_iovlen = 1;
  fixture.receiveMessage.msg_control = fixture.receiveControl.bytes;
  fixture.receiveMessage.msg_controllen = controlCapacity;
}

int receivedDescriptor(const StreamFixture& fixture, size_t expectedCount) {
  const struct cmsghdr* header = CMSG_FIRSTHDR(&fixture.receiveMessage);
  if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
      header->cmsg_len != CMSG_LEN(expectedCount * sizeof(int))) {
    return -1;
  }

  int descriptor = -1;
  MemoryCopy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
  return descriptor;
}

FileDescriptor::OpenFileDescriptionLease addSource(PosixSubsystem* subsystem, size_t descriptor) {
  FileDescriptor* source = new FileDescriptor;
  source->fd = descriptor;
  FileDescriptor::OpenFileDescriptionLease description = source->acquireOpenFileDescription();
  subsystem->addFileDescriptor(descriptor, source);
  return description;
}

bool closePair(StreamFixture& fixture) {
  bool passed = true;
  for (size_t i = 0; i < 2; ++i) {
    if (fixture.sockets[i] >= 0) {
      passed = posix_close(fixture.sockets[i]) == 0 && passed;
      fixture.sockets[i] = -1;
    }
  }
  return passed;
}

bool orderingAndSenderClose(PosixSubsystem* subsystem, StreamFixture& fixture) {
  fixture.sockets[0] = fixture.sockets[1] = -1;
  if (posix_socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fixture.sockets)) {
    return false;
  }

  prepareReceive(fixture, 1, sizeof(fixture.receiveControl));
  bool passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, 0) == -1;

  FileDescriptor::OpenFileDescriptionLease description = addSource(subsystem, SourceDescriptor);
  MemoryCopy(fixture.sendPayload, "ab", 2);
  passed = posix_send(fixture.sockets[0], fixture.sendPayload, 2, 0) == 2 && passed;
  MemoryCopy(fixture.sendPayload, "cd", 2);
  int source = SourceDescriptor;
  prepareSend(fixture, fixture.sendPayload, 2, &source, 1);
  passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 2 &&
           SocketRights::inFlightForTest() == 1 && posix_close(SourceDescriptor) == 0 &&
           description->descriptorOwnerCount() == 1 && passed;
  MemoryCopy(fixture.sendPayload, "ef", 2);
  passed = posix_send(fixture.sockets[0], fixture.sendPayload, 2, 0) == 2 &&
           posix_close(fixture.sockets[0]) == 0 && passed;
  fixture.sockets[0] = -1;

  passed = posix_recv(fixture.sockets[1], fixture.receivePayload, 1, 0) == 1 &&
           fixture.receivePayload[0] == 'a' && SocketRights::inFlightForTest() == 1 && passed;
  prepareReceive(fixture, sizeof(fixture.receivePayload), CMSG_SPACE(sizeof(int)));
  passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, MSG_CMSG_CLOEXEC) == 2 &&
           fixture.receivePayload[0] == 'b' && fixture.receivePayload[1] == 'c' &&
           !fixture.receiveMessage.msg_flags && SocketRights::inFlightForTest() == 0 && passed;
  const int received = receivedDescriptor(fixture, 1);
  passed = received >= 0 &&
           posix_fcntl(received, F_GETFD, reinterpret_cast<void*>(0)) == FD_CLOEXEC &&
           description->descriptorOwnerCount() == 1 && posix_close(received) == 0 &&
           description->descriptorOwnerCount() == 0 && passed;

  passed = posix_recv(fixture.sockets[1], fixture.receivePayload, 4, 0) == 3 &&
           fixture.receivePayload[0] == 'd' && fixture.receivePayload[1] == 'e' &&
           fixture.receivePayload[2] == 'f' &&
           posix_recv(fixture.sockets[1], fixture.receivePayload, 1, 0) == 0 && passed;
  return closePair(fixture) && passed;
}

bool splitIovecMarker(PosixSubsystem* subsystem, StreamFixture& fixture) {
  fixture.sockets[0] = fixture.sockets[1] = -1;
  if (posix_socketpair(AF_UNIX, SOCK_STREAM, 0, fixture.sockets)) {
    return false;
  }

  FileDescriptor::OpenFileDescriptionLease description = addSource(subsystem, SourceDescriptor);
  int source = SourceDescriptor;
  fixture.sendPayload[0] = 'a';
  MemoryCopy(fixture.sendPayload + 1, "bc", 2);
  ByteSet(&fixture.sendControl, 0, sizeof(fixture.sendControl));
  struct iovec sendVectors[2] = {
      {fixture.sendPayload, 0},
      {fixture.sendPayload + 1, 2},
  };
  fixture.sendMessage = {};
  fixture.sendMessage.msg_iov = sendVectors;
  fixture.sendMessage.msg_iovlen = 2;
  fixture.sendMessage.msg_control = fixture.sendControl.bytes;
  fixture.sendMessage.msg_controllen = CMSG_SPACE(sizeof(int));
  struct cmsghdr* header = CMSG_FIRSTHDR(&fixture.sendMessage);
  header->cmsg_len = CMSG_LEN(sizeof(int));
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  MemoryCopy(CMSG_DATA(header), &source, sizeof(source));

  bool passed = posix_send(fixture.sockets[0], fixture.sendPayload, 1, 0) == 1 &&
                posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 2 &&
                posix_close(SourceDescriptor) == 0 && description->descriptorOwnerCount() == 1 &&
                SocketRights::inFlightForTest() == 1;

  ByteSet(fixture.receivePayload, 0, sizeof(fixture.receivePayload));
  ByteSet(&fixture.receiveControl, 0, sizeof(fixture.receiveControl));
  struct iovec receiveVectors[2] = {
      {fixture.receivePayload, 1},
      {fixture.receivePayload + 1, sizeof(fixture.receivePayload) - 1},
  };
  fixture.receiveMessage = {};
  fixture.receiveMessage.msg_iov = receiveVectors;
  fixture.receiveMessage.msg_iovlen = 2;
  fixture.receiveMessage.msg_control = fixture.receiveControl.bytes;
  fixture.receiveMessage.msg_controllen = CMSG_SPACE(sizeof(int));
  passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, 0) == 2 &&
           fixture.receivePayload[0] == 'a' && fixture.receivePayload[1] == 'b' &&
           SocketRights::inFlightForTest() == 0 && passed;
  const int received = receivedDescriptor(fixture, 1);
  passed = received >= 0 && description->descriptorOwnerCount() == 1 &&
           posix_close(received) == 0 && description->descriptorOwnerCount() == 0 && passed;
  passed = posix_recv(fixture.sockets[1], fixture.receivePayload, 1, 0) == 1 &&
           fixture.receivePayload[0] == 'c' && closePair(fixture) && passed;
  return passed;
}

bool plainReadAndTruncation(PosixSubsystem* subsystem, StreamFixture& fixture) {
  fixture.sockets[0] = fixture.sockets[1] = -1;
  if (posix_socketpair(AF_UNIX, SOCK_STREAM, 0, fixture.sockets)) {
    return false;
  }

  FileDescriptor::OpenFileDescriptionLease discarded = addSource(subsystem, SourceDescriptor);
  int source = SourceDescriptor;
  fixture.sendPayload[0] = 'p';
  prepareSend(fixture, fixture.sendPayload, 1, &source, 1);
  bool passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 1 &&
                posix_close(SourceDescriptor) == 0 && discarded->descriptorOwnerCount() == 1 &&
                SocketRights::inFlightForTest() == 1;
  passed = posix_recv(fixture.sockets[1], fixture.receivePayload, 1, 0) == 1 &&
           fixture.receivePayload[0] == 'p' && discarded->descriptorOwnerCount() == 0 &&
           SocketRights::inFlightForTest() == 0 && passed;

  FileDescriptor::OpenFileDescriptionLease first = addSource(subsystem, SourceDescriptor);
  FileDescriptor::OpenFileDescriptionLease second = addSource(subsystem, SecondSourceDescriptor);
  int sources[2] = {static_cast<int>(SourceDescriptor), static_cast<int>(SecondSourceDescriptor)};
  fixture.sendPayload[0] = 't';
  prepareSend(fixture, fixture.sendPayload, 1, sources, 2);
  passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 1 &&
           posix_close(SourceDescriptor) == 0 && posix_close(SecondSourceDescriptor) == 0 &&
           first->descriptorOwnerCount() == 1 && second->descriptorOwnerCount() == 1 && passed;

  prepareReceive(fixture, 1, CMSG_LEN(sizeof(int)));
  passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, 0) == 1 &&
           fixture.receivePayload[0] == 't' && (fixture.receiveMessage.msg_flags & MSG_CTRUNC) &&
           SocketRights::inFlightForTest() == 0 && second->descriptorOwnerCount() == 0 && passed;
  const int received = receivedDescriptor(fixture, 1);
  passed = received >= 0 && first->descriptorOwnerCount() == 1 && posix_close(received) == 0 &&
           first->descriptorOwnerCount() == 0 && passed;
  return closePair(fixture) && passed;
}

bool multipleControlsAndFaultRetry(PosixSubsystem* subsystem, StreamFixture& fixture) {
  fixture.sockets[0] = fixture.sockets[1] = -1;
  if (posix_socketpair(AF_UNIX, SOCK_STREAM, 0, fixture.sockets)) {
    return false;
  }

  FileDescriptor::OpenFileDescriptionLease first = addSource(subsystem, SourceDescriptor);
  int source = SourceDescriptor;
  MemoryCopy(fixture.sendPayload, "12", 2);
  prepareSend(fixture, fixture.sendPayload, 2, &source, 1);
  bool passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 2;

  FileDescriptor::OpenFileDescriptionLease second = addSource(subsystem, SecondSourceDescriptor);
  source = SecondSourceDescriptor;
  MemoryCopy(fixture.sendPayload, "34", 2);
  prepareSend(fixture, fixture.sendPayload, 2, &source, 1);
  passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 2 &&
           posix_close(SourceDescriptor) == 0 && posix_close(SecondSourceDescriptor) == 0 &&
           SocketRights::inFlightForTest() == 2 && first->descriptorOwnerCount() == 1 &&
           second->descriptorOwnerCount() == 1 && passed;

  prepareReceive(fixture, sizeof(fixture.receivePayload), CMSG_SPACE(sizeof(int)));
  passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, 0) == 1 &&
           fixture.receivePayload[0] == '1' && SocketRights::inFlightForTest() == 1 && passed;
  int received = receivedDescriptor(fixture, 1);
  passed = received >= 0 && first->descriptorOwnerCount() == 1 && posix_close(received) == 0 &&
           first->descriptorOwnerCount() == 0 && passed;

  prepareReceive(fixture, sizeof(fixture.receivePayload), CMSG_SPACE(sizeof(int)));
  passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, 0) == 2 &&
           fixture.receivePayload[0] == '2' && fixture.receivePayload[1] == '3' &&
           SocketRights::inFlightForTest() == 0 && passed;
  received = receivedDescriptor(fixture, 1);
  passed = received >= 0 && second->descriptorOwnerCount() == 1 && posix_close(received) == 0 &&
           second->descriptorOwnerCount() == 0 && passed;
  passed = posix_recv(fixture.sockets[1], fixture.receivePayload, 1, 0) == 1 &&
           fixture.receivePayload[0] == '4' && closePair(fixture) && passed;

  if (posix_socketpair(AF_UNIX, SOCK_STREAM, 0, fixture.sockets)) {
    return false;
  }
  FileDescriptor::OpenFileDescriptionLease faultOwner = addSource(subsystem, SourceDescriptor);
  source = SourceDescriptor;
  fixture.sendPayload[0] = 'f';
  prepareSend(fixture, fixture.sendPayload, 1, &source, 1);
  passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 1 &&
           posix_close(SourceDescriptor) == 0 && faultOwner->descriptorOwnerCount() == 1 &&
           SocketRights::inFlightForTest() == 1 && passed;

  prepareReceive(fixture, 1, CMSG_SPACE(sizeof(int)));
  fixture.receiveMessage.msg_control = reinterpret_cast<void*>(~static_cast<uintptr_t>(0));
  passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, 0) == -1 &&
           faultOwner->descriptorOwnerCount() == 1 && SocketRights::inFlightForTest() == 1 &&
           passed;
  prepareReceive(fixture, 1, CMSG_SPACE(sizeof(int)));
  passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, 0) == 1 &&
           fixture.receivePayload[0] == 'f' && SocketRights::inFlightForTest() == 0 && passed;
  received = receivedDescriptor(fixture, 1);
  passed = received >= 0 && faultOwner->descriptorOwnerCount() == 1 && posix_close(received) == 0 &&
           faultOwner->descriptorOwnerCount() == 0 && closePair(fixture) && passed;
  return passed;
}

bool sendRetryDoesNotDuplicate(PosixSubsystem* subsystem, StreamFixture& fixture) {
  fixture.sockets[0] = fixture.sockets[1] = -1;
  if (posix_socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fixture.sockets)) {
    return false;
  }

  ByteSet(fixture.sendPayload, 'q', sizeof(fixture.sendPayload));
  size_t queued = 0;
  while (queued < MAX_UNIX_STREAM_QUEUE) {
    const size_t remaining = MAX_UNIX_STREAM_QUEUE - queued;
    const size_t amount =
        remaining < sizeof(fixture.sendPayload) ? remaining : sizeof(fixture.sendPayload);
    const ssize_t written = posix_send(fixture.sockets[0], fixture.sendPayload, amount, 0);
    if (written <= 0) {
      closePair(fixture);
      return false;
    }
    queued += static_cast<size_t>(written);
  }
  bool passed = posix_send(fixture.sockets[0], fixture.sendPayload, 1, 0) == -1;

  FileDescriptor::OpenFileDescriptionLease retryOwner = addSource(subsystem, SourceDescriptor);
  int source = SourceDescriptor;
  fixture.sendPayload[0] = 'r';
  prepareSend(fixture, fixture.sendPayload, 1, &source, 1);
  passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == -1 &&
           SocketRights::inFlightForTest() == 0 && retryOwner->descriptorOwnerCount() == 1 &&
           passed;

  passed = posix_recv(fixture.sockets[1], fixture.receivePayload, 1, 0) == 1 &&
           posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 1 &&
           SocketRights::inFlightForTest() == 1 && posix_close(SourceDescriptor) == 0 &&
           retryOwner->descriptorOwnerCount() == 1 && passed;

  size_t toDrain = queued - 1;
  while (toDrain) {
    const size_t amount =
        toDrain < sizeof(fixture.receivePayload) ? toDrain : sizeof(fixture.receivePayload);
    const ssize_t received = posix_recv(fixture.sockets[1], fixture.receivePayload, amount, 0);
    if (received <= 0) {
      closePair(fixture);
      return false;
    }
    toDrain -= static_cast<size_t>(received);
  }
  passed = SocketRights::inFlightForTest() == 1 && passed;

  prepareReceive(fixture, 1, CMSG_SPACE(sizeof(int)));
  passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, 0) == 1 &&
           fixture.receivePayload[0] == 'r' && SocketRights::inFlightForTest() == 0 && passed;
  const int received = receivedDescriptor(fixture, 1);
  passed = received >= 0 && retryOwner->descriptorOwnerCount() == 1 && posix_close(received) == 0 &&
           retryOwner->descriptorOwnerCount() == 0 && closePair(fixture) && passed;
  return passed;
}

bool emptyPayloadAndCloseDrain(PosixSubsystem* subsystem, StreamFixture& fixture) {
  fixture.sockets[0] = fixture.sockets[1] = -1;
  if (posix_socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fixture.sockets)) {
    return false;
  }

  FileDescriptor::OpenFileDescriptionLease empty = addSource(subsystem, SourceDescriptor);
  int source = SourceDescriptor;
  prepareSend(fixture, fixture.sendPayload, 0, &source, 1);
  bool passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == -1 &&
                SocketRights::inFlightForTest() == 0 && empty->descriptorOwnerCount() == 1 &&
                posix_close(SourceDescriptor) == 0 && empty->descriptorOwnerCount() == 0;
  fixture.sendMessage = {};
  fixture.sendVector = {fixture.sendPayload, 0};
  fixture.sendMessage.msg_iov = &fixture.sendVector;
  fixture.sendMessage.msg_iovlen = 1;
  passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 0 && passed;
  prepareReceive(fixture, 1, sizeof(fixture.receiveControl));
  passed = posix_recvmsg(fixture.sockets[1], &fixture.receiveMessage, 0) == -1 && passed;
  passed = closePair(fixture) && passed;

  if (posix_socketpair(AF_UNIX, SOCK_STREAM, 0, fixture.sockets)) {
    return false;
  }
  FileDescriptor::OpenFileDescriptionLease closeOwner = addSource(subsystem, SourceDescriptor);
  fixture.sendPayload[0] = 'c';
  prepareSend(fixture, fixture.sendPayload, 1, &source, 1);
  passed = posix_sendmsg(fixture.sockets[0], &fixture.sendMessage, 0) == 1 &&
           posix_close(SourceDescriptor) == 0 && closeOwner->descriptorOwnerCount() == 1 &&
           posix_close(fixture.sockets[1]) == 0 && passed;
  fixture.sockets[1] = -1;
  passed = closeOwner->descriptorOwnerCount() == 0 && SocketRights::inFlightForTest() == 0 &&
           closePair(fixture) && passed;
  return passed;
}

bool streamShutdownSemantics(StreamFixture& fixture) {
  fixture.sockets[0] = fixture.sockets[1] = -1;
  if (posix_socketpair(AF_UNIX, SOCK_STREAM, 0, fixture.sockets)) {
    return false;
  }

  fixture.sendPayload[0] = 'q';
  bool passed = posix_shutdown(fixture.sockets[0], SHUT_WR) == 0 &&
                posix_send(fixture.sockets[0], fixture.sendPayload, 1, 0) == -1 &&
                posix_recv(fixture.sockets[1], fixture.receivePayload, 1, 0) == 0;
  passed = posix_send(fixture.sockets[1], fixture.sendPayload, 1, 0) == 1 &&
           posix_recv(fixture.sockets[0], fixture.receivePayload, 1, 0) == 1 && passed;
  passed = posix_shutdown(fixture.sockets[0], SHUT_RD) == 0 &&
           posix_send(fixture.sockets[1], fixture.sendPayload, 1, 0) == -1 && passed;
  passed = posix_shutdown(fixture.sockets[0], 17) == -1 && closePair(fixture) && passed;
  return passed;
}

int runStreamWorker(void* parameter) {
  StreamContext* context = reinterpret_cast<StreamContext*>(parameter);
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(
      Processor::information().getCurrentThread()->getParent()->getSubsystem());
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t mappingAddress = 0;
  if (!context->process->allocateUserRange(Process::UserRegion::Normal, pageSize, mappingAddress)) {
    context->completed = true;
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
    context->completed = true;
    return 1;
  }

  StreamFixture* fixture = reinterpret_cast<StreamFixture*>(mappingAddress);
  ByteSet(fixture, 0, sizeof(*fixture));
  bool passed = orderingAndSenderClose(subsystem, *fixture);
  passed = splitIovecMarker(subsystem, *fixture) && passed;
  passed = plainReadAndTruncation(subsystem, *fixture) && passed;
  passed = multipleControlsAndFaultRetry(subsystem, *fixture) && passed;
  passed = sendRetryDoesNotDuplicate(subsystem, *fixture) && passed;
  passed = emptyPayloadAndCloseDrain(subsystem, *fixture) && passed;
  passed = streamShutdownSemantics(*fixture) && passed;

  MemoryMapManager::instance().remove(mappingAddress, pageSize);
  context->process->freeUserRange(Process::UserRegion::Normal, mappingAddress, pageSize);
  context->result = passed;
  context->completed = true;
  return 0;
}
}  // namespace

bool runHostedScmStreamRegressions(Process* kernelProcess) {
  bool passed = SocketRights::inFlightForTest() == 0;

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
  StreamContext context(process);
  Thread* worker = new Thread(process, runStreamWorker, &context, nullptr, false, true, true);
  worker->setName("hosted AF_UNIX stream SCM_RIGHTS");
  const bool started = contextInstalled && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  passed = started && joined && context.completed && context.result && passed;
  delete process;

  const bool rootRestored = fixture.close();
  if (!rootRestored)
    FATAL("Hosted filesystem fixture retained owners after teardown");
  passed = rootRestored && VFS::instance().getRootFilesystem() == priorRoot &&
           VFS::instance().mountView() == priorView && passed;
  delete filesystem;
  passed = SocketRights::inFlightForTest() == 0 && passed;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL scm-rights-stream: ordering, one-shot delivery, "
        "truncation, or lifecycle drain regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS scm-rights-stream");
  return true;
}
