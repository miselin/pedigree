/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "mqueue-state.h"

Mutex g_MqueueRegistryLock;
List<PosixMessageQueue*> g_Mqueues;
namespace {
// Pooled nodes retain erased references and can destroy them under this lock
// when reused; queue destruction acquires the registry lock itself.
List<SharedPointer<PosixMessageQueue>, 0> namedQueues;
constexpr size_t MaximumQueues = 64;
constexpr int AllowedFlags = O_ACCMODE | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC;

bool copyName(const char* userName, String& name) {
  const auto result = PosixSubsystem::copyUserString(userName, name, 256);
  if (result != PosixSubsystem::UserStringSuccess) {
    syscallError(result == PosixSubsystem::UserStringTooLong ? Error::NameTooLong
                                                             : Error::BadAddress);
    return false;
  }
  if (!name.length()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  for (size_t n = 0; n < name.length(); ++n) {
    if (name[n] == '/') {
      SYSCALL_ERROR(PermissionDenied);
      return false;
    }
  }
  if (name == "." || name == "..") {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  return true;
}

bool queueDescriptor(int fd, DescriptorLease& descriptor, SharedPointer<PosixMessageQueue>& queue,
                     int forbiddenAccess = -1) {
  if (!acquireDescriptor(fd, descriptor) || !(queue = descriptor->getMqueueImpl()) ||
      (descriptor->getStatusFlags() & O_ACCMODE) == forbiddenAccess) {
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }
  return true;
}
}  // namespace

int posix_mq_open(const char* userName, int flags, unsigned mode, const LinuxMqAttr* userAttr) {
  TerminationDeferral lifetime;
  if ((flags & ~AllowedFlags) || (flags & O_ACCMODE) == O_ACCMODE) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  String name;
  if (!copyName(userName, name)) {
    return -1;
  }
  Process* process = Processor::information().getCurrentThread()->getParent();
  SharedPointer<PosixMessageQueue> queue;
  {
    LockGuard<Mutex> guard(g_MqueueRegistryLock);
    for (auto it = namedQueues.begin(); it != namedQueues.end(); ++it) {
      if ((*it)->name() == name) {
        queue = *it;
        break;
      }
    }
    if (queue) {
      if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        SYSCALL_ERROR(FileExists);
        return -1;
      }
      if (!queue->mayOpen(process, flags)) {
        SYSCALL_ERROR(PermissionDenied);
        return -1;
      }
    } else {
      if (!(flags & O_CREAT)) {
        SYSCALL_ERROR(DoesNotExist);
        return -1;
      }
      LinuxMqAttr attr = {0, 10, 8192, 0, {}};
      if (userAttr && !PosixSubsystem::copyFromUser(&attr, userAttr, sizeof(attr))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      if (attr.maxMessages <= 0 || attr.maxMessages > 128 || attr.messageSize <= 0 ||
          attr.messageSize > 8192) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      if (g_Mqueues.count() >= MaximumQueues) {
        SYSCALL_ERROR(NoSpaceLeftOnDevice);
        return -1;
      }
      mode &= 0777 & ~static_cast<PosixProcess*>(process)->getMask();
      queue = SharedPointer<PosixMessageQueue>(new PosixMessageQueue(
          name, attr.maxMessages, attr.messageSize, process->getEffectiveUserId(),
          process->getEffectiveGroupId(), mode));
      g_Mqueues.pushBack(queue.get());
      namedQueues.pushBack(queue);
    }
  }
  // Linux applies close-on-exec to every mq_open descriptor, including opens
  // which omit O_CLOEXEC. The status flags belong to the shared description.
  FileDescriptor* descriptor =
      new FileDescriptor(nullptr, 0, 0xFFFFFFFF, FD_CLOEXEC, flags & (O_ACCMODE | O_NONBLOCK));
  descriptor->setMqueueImpl(queue);
  DescriptorLease installed;
  return static_cast<int>(installDescriptor(descriptor, installed));
}

int posix_mq_unlink(const char* userName) {
  TerminationDeferral lifetime;
  String name;
  if (!copyName(userName, name)) {
    return -1;
  }
  SharedPointer<PosixMessageQueue> removed;
  {
    LockGuard<Mutex> guard(g_MqueueRegistryLock);
    for (auto it = namedQueues.begin(); it != namedQueues.end(); ++it) {
      if ((*it)->name() == name) {
        if (!(*it)->mayUnlink(Processor::information().getCurrentThread()->getParent())) {
          SYSCALL_ERROR(PermissionDenied);
          return -1;
        }
        removed = *it;
        namedQueues.erase(it);
        return 0;
      }
    }
  }
  SYSCALL_ERROR(DoesNotExist);
  return -1;
}

int posix_mq_timedsend(int fd, const char* data, size_t length, unsigned priority,
                       const LinuxMqTimespec* timeout) {
  TerminationDeferral lifetime;
  DescriptorLease descriptor;
  SharedPointer<PosixMessageQueue> queue;
  if (!queueDescriptor(fd, descriptor, queue, O_RDONLY)) {
    return -1;
  }
  return queue->send(data, length, priority, descriptor->getStatusFlags() & O_NONBLOCK, timeout);
}

int posix_mq_timedreceive(int fd, char* data, size_t length, unsigned* priority,
                          const LinuxMqTimespec* timeout) {
  TerminationDeferral lifetime;
  DescriptorLease descriptor;
  SharedPointer<PosixMessageQueue> queue;
  if (!queueDescriptor(fd, descriptor, queue, O_WRONLY)) {
    return -1;
  }
  return queue->receive(data, length, priority, descriptor->getStatusFlags() & O_NONBLOCK, timeout);
}

int posix_mq_notify(int fd, const LinuxMqSigevent* event) {
  TerminationDeferral lifetime;
  DescriptorLease descriptor;
  SharedPointer<PosixMessageQueue> queue;
  if (!queueDescriptor(fd, descriptor, queue)) {
    return -1;
  }
  return queue->notify(event);
}

int posix_mq_getsetattr(int fd, const LinuxMqAttr* requested, LinuxMqAttr* previous) {
  TerminationDeferral lifetime;
  DescriptorLease descriptor;
  SharedPointer<PosixMessageQueue> queue;
  if (!queueDescriptor(fd, descriptor, queue)) {
    return -1;
  }
  return queue->attributes(*descriptor, requested, previous);
}

void posix_mqueue_close(PosixMessageQueue* queue, size_t pid) {
  if (queue) {
    queue->cancelNotification(pid);
  }
}

void posix_mqueue_process_exit(size_t pid) {
  LockGuard<Mutex> guard(g_MqueueRegistryLock);
  for (auto it = g_Mqueues.begin(); it != g_Mqueues.end(); ++it) {
    (*it)->cancelNotification(pid);
  }
}

void posix_mqueue_clock_changed() {
  LockGuard<Mutex> guard(g_MqueueRegistryLock);
  for (auto it = g_Mqueues.begin(); it != g_Mqueues.end(); ++it) {
    (*it)->clockChanged();
  }
}
