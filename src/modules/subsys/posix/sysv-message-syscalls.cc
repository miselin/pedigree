/* Copyright (c) 2026, Pedigree Developers. See LICENSE for licensing details. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/utility.h"

#include "PosixSubsystem.h"
#include "ipc-common.h"
#include "sysv-message-syscalls.h"

namespace {
constexpr size_t MaximumQueues = 128;
constexpr size_t MaximumMessage = 8192;
constexpr size_t DefaultQueueBytes = 16384;
constexpr size_t MaximumQueueBytes = 65536;
constexpr int Create = 01000, Exclusive = 02000, Nowait = 04000;
constexpr int Noerror = 010000, Except = 020000, Copy = 040000;
constexpr int Remove = 0, Set = 1, Stat = 2, Info = 3;
constexpr int MessageStat = 11, MessageInfo = 12, MessageStatAny = 13;

struct QueueStatus {
  PosixIpc::Permission permission;
  int64_t sendTime, receiveTime, changeTime;
  uint64_t bytes, count, capacity;
  int32_t sender, receiver;
  uint64_t unused[2];
};
static_assert(sizeof(QueueStatus) == 120, "Linux amd64 msqid_ds layout");
static_assert(__builtin_offsetof(QueueStatus, capacity) == 88, "Linux msg_qbytes offset");

struct QueueInfo {
  int32_t pool, map, maximum, defaultBytes, queues, segmentSize, messages;
  uint16_t segments, padding;
};
static_assert(sizeof(QueueInfo) == 32, "Linux msginfo layout");

struct Message {
  explicit Message(size_t length) : size(length), data(new uint8_t[length + sizeof(int64_t)]) {}
  ~Message() {
    delete[] data;
  }
  int64_t type() const {
    int64_t value;
    MemoryCopy(&value, data, sizeof(value));
    return value;
  }
  size_t size;
  uint8_t* data;
};

struct Queue {
  Queue(int identifier, int key, int mode) : id(identifier), removed(false), status{} {
    PosixIpc::initialize(status.permission, key, mode, identifier / MaximumQueues);
    status.capacity = DefaultQueueBytes;
    status.changeTime = Time::getTime();
  }
  int id;
  bool removed;
  QueueStatus status;
  Mutex lock;
  ConditionVariable changed;
  // Retire each payload as soon as a receive releases its queue capacity.
  List<SharedPointer<Message>, 0> messages;
};

Mutex registryLock;
SharedPointer<Queue> queues[MaximumQueues];
uint32_t nextIdentifier = 0;

SharedPointer<Queue> findQueue(int id, bool index = false) {
  LockGuard<Mutex> guard(registryLock);
  if (id >= 0) {
    if (index) {
      if (static_cast<size_t>(id) < MaximumQueues && queues[id]) {
        return queues[id];
      }
    } else {
      for (size_t i = 0; i < MaximumQueues; ++i) {
        if (queues[i] && queues[i]->id == id) {
          return queues[i];
        }
      }
    }
  }
  SYSCALL_ERROR(InvalidArgument);
  return SharedPointer<Queue>();
}

bool usable(Queue& queue, unsigned access) {
  if (queue.removed) {
    SYSCALL_ERROR(IdentifierRemoved);
    return false;
  }
  if (!PosixIpc::allowed(queue.status.permission, access)) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  return true;
}

bool wait(Queue& queue, LockGuard<Mutex>& guard) {
  ConditionVariable::Error error = ConditionVariable::NoError;
  if (queue.changed.wait(queue.lock, error)) {
    return true;
  }
  if (!ConditionVariable::mutexAcquired(error)) {
    guard.disown();
  }
  if (ConditionVariable::mutexAcquired(error) && queue.removed) {
    SYSCALL_ERROR(IdentifierRemoved);
  } else {
    SYSCALL_ERROR(Interrupted);
  }
  return false;
}

int queueInfo(int command, void* buffer) {
  QueueInfo result = {};
  result.maximum = MaximumMessage;
  result.defaultBytes = DefaultQueueBytes;
  result.queues = MaximumQueues;
  result.segmentSize = 16;
  result.segments = 65535;
  int highest = 0;
  LockGuard<Mutex> guard(registryLock);
  for (size_t i = 0; i < MaximumQueues; ++i) {
    if (!queues[i]) {
      continue;
    }
    highest = i;
    if (command == MessageInfo) {
      LockGuard<Mutex> queueGuard(queues[i]->lock);
      ++result.pool;
      result.map += queues[i]->status.count;
      result.messages += queues[i]->status.bytes;
    }
  }
  if (command == Info) {
    result.pool = MaximumQueues * DefaultQueueBytes / 1024;
    result.map = MaximumQueues;
    result.messages = MaximumQueues * MaximumQueueBytes;
  }
  if (!PosixSubsystem::copyToUser(buffer, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return highest;
}
}  // namespace

int posix_msgget(int32_t key, int flags) {
  TerminationDeferral lifetime;
  if (flags & ~(0777 | Create | Exclusive)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  LockGuard<Mutex> guard(registryLock);
  size_t freeSlot = MaximumQueues;
  for (size_t i = 0; i < MaximumQueues; ++i) {
    if (!queues[i]) {
      if (freeSlot == MaximumQueues) {
        freeSlot = i;
      }
    } else if (key && queues[i]->status.permission.key == key) {
      if ((flags & (Create | Exclusive)) == (Create | Exclusive)) {
        SYSCALL_ERROR(FileExists);
        return -1;
      }
      LockGuard<Mutex> queueGuard(queues[i]->lock);
      const unsigned access = ((flags >> 6) | (flags >> 3) | flags) & 7;
      return usable(*queues[i], access) ? queues[i]->id : -1;
    }
  }
  if (key && !(flags & Create)) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  if (freeSlot == MaximumQueues || nextIdentifier > 0x7fffffffU) {
    SYSCALL_ERROR(NoSpaceLeftOnDevice);
    return -1;
  }
  const int id = nextIdentifier++;
  queues[freeSlot].reset(new Queue(id, key, flags));
  return id;
}

int posix_msgsnd(int id, const void* message, size_t size, int flags) {
  TerminationDeferral lifetime;
  if (size > MaximumMessage || (flags & ~Nowait)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  SharedPointer<Message> snapshot(new Message(size));
  if (!PosixSubsystem::copyFromUser(snapshot->data, message, size + sizeof(int64_t))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (snapshot->type() <= 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  SharedPointer<Queue> queue = findQueue(id);
  if (!queue) {
    return -1;
  }
  LockGuard<Mutex> guard(queue->lock);
  while (usable(*queue, 2)) {
    // Counting empty records as well as bytes keeps zero-length sends bounded.
    if (queue->status.bytes <= queue->status.capacity &&
        size <= queue->status.capacity - queue->status.bytes &&
        queue->status.count < queue->status.capacity) {
      queue->messages.pushBack(snapshot);
      queue->status.bytes += size;
      ++queue->status.count;
      queue->status.sender = PosixIpc::process()->getId();
      queue->status.sendTime = Time::getTime();
      queue->changed.broadcast();
      return 0;
    }
    if (flags & Nowait) {
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }
    if (!wait(*queue, guard)) {
      return -1;
    }
  }
  return -1;
}

ssize_t posix_msgrcv(int id, void* message, size_t size, int64_t type, int flags) {
  TerminationDeferral lifetime;
  if (size > static_cast<size_t>(0x7fffffffffffffffULL) ||
      (flags & ~(Nowait | Noerror | Except | Copy)) ||
      ((flags & Copy) && (!(flags & Nowait) || (flags & Except)))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  SharedPointer<Queue> queue = findQueue(id);
  if (!queue) {
    return -1;
  }
  LockGuard<Mutex> guard(queue->lock);
  while (usable(*queue, 4)) {
    auto selected = queue->messages.end();
    uint64_t position = 0;
    // Unsigned negation also handles the minimum signed message selector.
    const uint64_t ceiling = 0 - static_cast<uint64_t>(type);
    for (auto it = queue->messages.begin(); it != queue->messages.end(); ++it, ++position) {
      const int64_t candidate = (*it)->type();
      if (flags & Copy) {
        if (position == static_cast<uint64_t>(type)) {
          selected = it;
          break;
        }
      } else if (!type ||
                 (type > 0 && ((flags & Except) ? candidate != type : candidate == type))) {
        selected = it;
        break;
      } else if (type < 0 && static_cast<uint64_t>(candidate) <= ceiling &&
                 (selected == queue->messages.end() || candidate < (*selected)->type())) {
        selected = it;
      }
    }
    if (selected != queue->messages.end()) {
      Message& record = **selected;
      if (record.size > size && !(flags & Noerror)) {
        SYSCALL_ERROR(TooBig);
        return -1;
      }
      if ((flags & Copy) && record.size > size) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      const size_t copied = size < record.size ? size : record.size;
      // Hold the queue transaction through the copy so EFAULT cannot consume a
      // message and another receiver cannot observe an uncommitted dequeue.
      if (!PosixSubsystem::copyToUser(message, record.data, copied + sizeof(int64_t))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      if (!(flags & Copy)) {
        queue->status.bytes -= record.size;
        --queue->status.count;
        queue->status.receiver = PosixIpc::process()->getId();
        queue->status.receiveTime = Time::getTime();
        queue->messages.erase(selected);
        queue->changed.broadcast();
      }
      return copied;
    }
    if (flags & Nowait) {
      SYSCALL_ERROR(NoMessage);
      return -1;
    }
    if (!wait(*queue, guard)) {
      return -1;
    }
  }
  return -1;
}

int posix_msgctl(int id, int command, void* buffer) {
  TerminationDeferral lifetime;
  if (command == Info || command == MessageInfo) {
    return queueInfo(command, buffer);
  }
  if (command != Remove && command != Set && command != Stat && command != MessageStat &&
      command != MessageStatAny) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  QueueStatus input = {};
  if (command == Set && !PosixSubsystem::copyFromUser(&input, buffer, sizeof(input))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  SharedPointer<Queue> queue = findQueue(id, command == MessageStat || command == MessageStatAny);
  if (!queue) {
    return -1;
  }
  // Registry-before-queue is the only nested lock order, including removal.
  LockGuard<Mutex> registryGuard(registryLock, command == Remove);
  LockGuard<Mutex> guard(queue->lock);
  const unsigned access = command == Stat || command == MessageStat ? 4 : 0;
  if (!usable(*queue, access)) {
    return -1;
  }
  if (command == Stat || command == MessageStat || command == MessageStatAny) {
    if (!PosixSubsystem::copyToUser(buffer, &queue->status, sizeof(queue->status))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return command == Stat ? 0 : queue->id;
  }
  if (!PosixIpc::owner(queue->status.permission)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  if (command == Remove) {
    queue->removed = true;
    for (size_t i = 0; i < MaximumQueues; ++i) {
      if (queues[i] && queues[i]->id == id) {
        queues[i].reset();
        break;
      }
    }
    queue->messages.clear();
  } else {
    if (input.permission.uid == 0xffffffffU || input.permission.gid == 0xffffffffU) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (input.capacity > DefaultQueueBytes && PosixIpc::process()->getEffectiveUserId() != 0) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return -1;
    }
    if (input.capacity > MaximumQueueBytes) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    queue->status.permission.uid = input.permission.uid;
    queue->status.permission.gid = input.permission.gid;
    queue->status.permission.mode = input.permission.mode & 0777;
    queue->status.capacity = input.capacity;
    queue->status.changeTime = Time::getTime();
  }
  queue->changed.broadcast();
  return 0;
}
