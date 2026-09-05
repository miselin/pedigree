/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_MQUEUE_STATE_H
#define POSIX_MQUEUE_STATE_H

#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include "mqueue-syscalls.h"
#include "net-syscalls.h"

struct MqueueNotification {
  MqueueNotification() : process(nullptr), pid(0), event(), socket(), cookie() {}
  Process* process;
  size_t pid;
  LinuxMqSigevent event;
  SharedPointer<NetworkSyscalls> socket;
  uint8_t cookie[32];
  void complete(bool removed);
};

struct MqueueState {
  struct Message {
    Message() : next(-1), length(0), priority(0) {}
    int next;
    size_t length;
    unsigned priority;
  };
  MqueueState(const String& name, size_t capacity, size_t size, int64_t uid, int64_t gid,
              unsigned mode);
  Mutex lock;
  ConditionVariable readers, writers;
  String name;
  size_t capacity, size, count, receiverCount;
  int64_t uid, gid;
  unsigned mode;
  int head, free;
  UniqueArray<Message> messages;
  UniqueArray<uint8_t> storage;
  MqueueNotification notification;
  ReadinessGenerations generations;
};

extern Mutex g_MqueueRegistryLock;
extern List<PosixMessageQueue*> g_Mqueues;

#endif
