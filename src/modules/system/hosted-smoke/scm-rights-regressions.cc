/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/lib.h"

#include <fcntl.h>
#include <stddef.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/UnixFilesystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/subsys/posix/net-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/VFS.h"
#include <sys/socket.h>
#include <sys/un.h>

namespace {
constexpr size_t SourceDescriptor = 90;
constexpr size_t SecondSourceDescriptor = 91;

union ControlBuffer {
  struct cmsghdr alignment;
  uint8_t bytes[CMSG_SPACE(2 * sizeof(int))];
};

struct ScmRightsUserFixture {
  struct sockaddr_un address;
  char sendPayload;
  char receivedPayload;
  struct iovec sendVector;
  struct iovec receiveVector;
  ControlBuffer sendControl;
  ControlBuffer receiveControl;
  struct msghdr sendMessage;
  struct msghdr receiveMessage;
};

struct ScmRightsContext {
  explicit ScmRightsContext(Process* process)
      : process(process),
        completed(false),
        result(false),
        receivedDescriptor(-1),
        receivedFlags(-1) {}

  Process* process;
  bool completed;
  bool result;
  int receivedDescriptor;
  int receivedFlags;
};

void prepareControl(struct msghdr& message, ControlBuffer& control, const int* descriptors,
                    size_t count) {
  ByteSet(&control, 0, sizeof(control));
  message.msg_control = control.bytes;
  message.msg_controllen = CMSG_SPACE(count * sizeof(int));
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_len = CMSG_LEN(count * sizeof(int));
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  MemoryCopy(CMSG_DATA(header), descriptors, count * sizeof(int));
}

int runScmRightsWorker(void* parameter) {
  ScmRightsContext* context = reinterpret_cast<ScmRightsContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t mappingAddress = 0;
  if (!context->process->allocateUserRange(Process::UserRegion::Normal, pageSize, mappingAddress)) {
    context->completed = true;
    return 1;
  }

  uintptr_t mappedAddress = mappingAddress;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapAnon(
      mappedAddress, pageSize, MemoryMappedObject::Read | MemoryMappedObject::Write);
  if (!mapping || mappedAddress != mappingAddress || sizeof(ScmRightsUserFixture) > pageSize) {
    if (mapping) {
      MemoryMapManager::instance().remove(mappedAddress, pageSize);
    }
    context->process->freeUserRange(Process::UserRegion::Normal, mappingAddress, pageSize);
    context->completed = true;
    return 1;
  }

  ScmRightsUserFixture* fixture = reinterpret_cast<ScmRightsUserFixture*>(mappingAddress);
  ByteSet(fixture, 0, sizeof(*fixture));
  struct sockaddr_un& address = fixture->address;
  char& payload = fixture->sendPayload;
  char& receivedPayload = fixture->receivedPayload;
  struct iovec& sendVector = fixture->sendVector;
  struct iovec& receiveVector = fixture->receiveVector;
  ControlBuffer& sendControl = fixture->sendControl;
  ControlBuffer& receiveControl = fixture->receiveControl;
  struct msghdr& sendMessage = fixture->sendMessage;
  struct msghdr& receiveMessage = fixture->receiveMessage;

  const char path[] = "/hosted-scm-rights";
  address.sun_family = AF_UNIX;
  StringCopy(address.sun_path, path);
  const socklen_t addressLength = offsetof(struct sockaddr_un, sun_path) + sizeof(path);

  const int receiver = posix_socket(AF_UNIX, SOCK_DGRAM, 0);
  const int sender = posix_socket(AF_UNIX, SOCK_DGRAM, 0);
  bool passed = receiver >= 0 && sender >= 0 &&
                posix_bind(receiver, reinterpret_cast<struct sockaddr_storage*>(&address),
                           addressLength) == 0;

  payload = 'r';
  sendVector = {&payload, sizeof(payload)};
  sendMessage.msg_name = &address;
  sendMessage.msg_namelen = addressLength;
  sendMessage.msg_iov = &sendVector;
  sendMessage.msg_iovlen = 1;
  int source = SourceDescriptor;
  prepareControl(sendMessage, sendControl, &source, 1);

  FileDescriptor* sourceDescriptor = new FileDescriptor;
  sourceDescriptor->fd = SourceDescriptor;
  sourceDescriptor->setFlags(FD_CLOEXEC);
  subsystem->addFileDescriptor(SourceDescriptor, sourceDescriptor);
  passed = passed && posix_sendmsg(sender, &sendMessage, 0) == 1 &&
           SocketRights::inFlightForTest() == 1 && posix_close(SourceDescriptor) == 0 &&
           SocketRights::inFlightForTest() == 1;

  receivedPayload = 0;
  receiveVector = {&receivedPayload, sizeof(receivedPayload)};
  receiveMessage.msg_iov = &receiveVector;
  receiveMessage.msg_iovlen = 1;
  receiveMessage.msg_control = receiveControl.bytes;
  receiveMessage.msg_controllen = sizeof(receiveControl.bytes);
  passed = passed && posix_recvmsg(receiver, &receiveMessage, MSG_CMSG_CLOEXEC) == 1 &&
           receivedPayload == payload && !(receiveMessage.msg_flags & MSG_CTRUNC) &&
           SocketRights::inFlightForTest() == 0;

  struct cmsghdr* receivedHeader = CMSG_FIRSTHDR(&receiveMessage);
  if (receivedHeader && receivedHeader->cmsg_level == SOL_SOCKET &&
      receivedHeader->cmsg_type == SCM_RIGHTS &&
      receivedHeader->cmsg_len == CMSG_LEN(sizeof(int))) {
    MemoryCopy(&context->receivedDescriptor, CMSG_DATA(receivedHeader), sizeof(int));
    context->receivedFlags =
        posix_fcntl(context->receivedDescriptor, F_GETFD, reinterpret_cast<void*>(0));
  } else {
    passed = false;
  }
  passed = passed && context->receivedDescriptor >= 0 && context->receivedFlags == FD_CLOEXEC &&
           posix_close(context->receivedDescriptor) == 0;

  FileDescriptor* truncatedFirst = new FileDescriptor;
  truncatedFirst->fd = SourceDescriptor;
  FileDescriptor::OpenFileDescriptionLease truncatedFirstDescription =
      truncatedFirst->acquireOpenFileDescription();
  subsystem->addFileDescriptor(SourceDescriptor, truncatedFirst);
  FileDescriptor* truncatedSecond = new FileDescriptor;
  truncatedSecond->fd = SecondSourceDescriptor;
  FileDescriptor::OpenFileDescriptionLease truncatedSecondDescription =
      truncatedSecond->acquireOpenFileDescription();
  subsystem->addFileDescriptor(SecondSourceDescriptor, truncatedSecond);
  int truncatedSources[2] = {static_cast<int>(SourceDescriptor),
                             static_cast<int>(SecondSourceDescriptor)};
  prepareControl(sendMessage, sendControl, truncatedSources, 2);
  payload = 't';
  passed = passed && posix_sendmsg(sender, &sendMessage, 0) == 1 &&
           posix_close(SourceDescriptor) == 0 && posix_close(SecondSourceDescriptor) == 0 &&
           truncatedFirstDescription->descriptorOwnerCount() == 1 &&
           truncatedSecondDescription->descriptorOwnerCount() == 1;

  ByteSet(&receiveControl, 0, sizeof(receiveControl));
  receivedPayload = 0;
  receiveMessage.msg_controllen = CMSG_LEN(sizeof(int));
  receiveMessage.msg_flags = 0;
  passed = passed && posix_recvmsg(receiver, &receiveMessage, 0) == 1 &&
           receivedPayload == payload && (receiveMessage.msg_flags & MSG_CTRUNC) &&
           SocketRights::inFlightForTest() == 0 &&
           truncatedFirstDescription->descriptorOwnerCount() == 1 &&
           truncatedSecondDescription->descriptorOwnerCount() == 0;
  receivedHeader = CMSG_FIRSTHDR(&receiveMessage);
  int truncatedReceived = -1;
  if (receivedHeader && receivedHeader->cmsg_len == CMSG_LEN(sizeof(int))) {
    MemoryCopy(&truncatedReceived, CMSG_DATA(receivedHeader), sizeof(truncatedReceived));
  } else {
    passed = false;
  }
  passed = passed && truncatedReceived >= 0 && posix_close(truncatedReceived) == 0 &&
           truncatedFirstDescription->descriptorOwnerCount() == 0;

  FileDescriptor* queuedDescriptor = new FileDescriptor;
  queuedDescriptor->fd = SecondSourceDescriptor;
  subsystem->addFileDescriptor(SecondSourceDescriptor, queuedDescriptor);
  source = SecondSourceDescriptor;
  prepareControl(sendMessage, sendControl, &source, 1);
  payload = 'q';
  passed = passed && posix_sendmsg(sender, &sendMessage, 0) == 1 &&
           posix_close(SecondSourceDescriptor) == 0 && SocketRights::inFlightForTest() == 1 &&
           posix_close(receiver) == 0 && SocketRights::inFlightForTest() == 0;

  passed = passed && posix_close(sender) == 0;
  MemoryMapManager::instance().remove(mappingAddress, pageSize);
  context->process->freeUserRange(Process::UserRegion::Normal, mappingAddress, pageSize);
  context->result = passed;
  context->completed = true;
  return 0;
}

bool inFlightCeiling() {
  constexpr size_t FullRecords = SocketRights::MaximumInFlight / SocketRights::MaximumDescriptors;
  constexpr size_t Remainder = SocketRights::MaximumInFlight % SocketRights::MaximumDescriptors;
  UniqueArray<SharedPointer<SocketRights>> owners =
      UniqueArray<SharedPointer<SocketRights>>::allocate(FullRecords + (Remainder ? 1 : 0));

  bool passed = SocketRights::inFlightForTest() == 0;
  for (size_t i = 0; i < FullRecords; ++i) {
    passed = SocketRights::create(SocketRights::MaximumDescriptors, owners.get()[i]) && passed;
  }
  if (Remainder) {
    passed = SocketRights::create(Remainder, owners.get()[FullRecords]) && passed;
  }
  SharedPointer<SocketRights> excess;
  passed = !SocketRights::create(1, excess) &&
           SocketRights::inFlightForTest() == SocketRights::MaximumInFlight && passed;
  owners = UniqueArray<SharedPointer<SocketRights>>();
  return passed && SocketRights::inFlightForTest() == 0;
}
}  // namespace

bool runHostedScmRightsRegressions(Process* kernelProcess) {
  bool passed = inFlightCeiling();

  Filesystem* priorRoot = VFS::instance().getRootFilesystem();
  if (priorRoot) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL scm-rights-datagram: "
        "the isolated pathname fixture requires an empty hosted root namespace");
    return false;
  }

  UnixFilesystem* filesystem = new UnixFilesystem;
  Filesystem* displacedRoot = VFS::instance().swapRootFilesystemForHostedTest(filesystem);

  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  process->setCwd(filesystem->getRoot());
  ScmRightsContext context(process);
  Thread* worker = new Thread(process, runScmRightsWorker, &context, nullptr, false, true, true);
  worker->setName("hosted AF_UNIX SCM_RIGHTS");
  const bool started = displacedRoot == priorRoot && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  passed = started && joined && context.completed && context.result && passed;
  process->setCwd(nullptr);
  delete process;

  Filesystem* removedRoot = VFS::instance().swapRootFilesystemForHostedTest(priorRoot);
  passed = removedRoot == filesystem && passed;
  delete filesystem;
  passed = SocketRights::inFlightForTest() == 0 && passed;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL scm-rights-datagram: descriptor ownership, cloexec, "
        "queue drain, or in-flight limit regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS scm-rights-datagram");
  return true;
}
