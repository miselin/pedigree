/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_MQUEUE_SYSCALLS_H
#define POSIX_MQUEUE_SYSCALLS_H

#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/String.h"

class FileDescriptor;
class Process;
struct MqueueState;

struct LinuxMqAttr {
  int64_t flags, maxMessages, messageSize, currentMessages, reserved[4];
};
struct LinuxMqTimespec {
  int64_t seconds, nanoseconds;
};
struct LinuxMqSigevent {
  uint64_t value;
  int32_t signal, notify;
  uint8_t reserved[48];
};
static_assert(sizeof(LinuxMqAttr) == 64 && sizeof(LinuxMqSigevent) == 64,
              "Linux amd64 message queue ABI");

class EXPORTED_PUBLIC PosixMessageQueue final : public ReadinessSource {
 public:
  PosixMessageQueue(const String& name, size_t maxMessages, size_t messageSize, int64_t uid,
                    int64_t gid, unsigned mode);
  ~PosixMessageQueue() override;

  int send(const char* data, size_t length, unsigned priority, bool nonblock,
           const LinuxMqTimespec* timeout);
  int receive(char* data, size_t length, unsigned* priority, bool nonblock,
              const LinuxMqTimespec* timeout);
  int attributes(FileDescriptor& descriptor, const LinuxMqAttr* requested, LinuxMqAttr* previous);
  int notify(const LinuxMqSigevent* event);
  void cancelNotification(size_t pid);
  void clockChanged();
  bool mayOpen(Process* process, int flags) const;
  bool mayUnlink(Process* process) const;
  const String& name() const;
  ReadyMask queryReady();
  ReadinessGenerations readinessGenerations() override;

 private:
  MqueueState* m_State;
};

int posix_mq_open(const char* name, int flags, unsigned mode, const LinuxMqAttr* attr);
int posix_mq_unlink(const char* name);
int posix_mq_timedsend(int fd, const char* data, size_t length, unsigned priority,
                       const LinuxMqTimespec* timeout);
int posix_mq_timedreceive(int fd, char* data, size_t length, unsigned* priority,
                          const LinuxMqTimespec* timeout);
int posix_mq_notify(int fd, const LinuxMqSigevent* event);
int posix_mq_getsetattr(int fd, const LinuxMqAttr* requested, LinuxMqAttr* previous);
void posix_mqueue_close(PosixMessageQueue* queue, size_t pid);
void posix_mqueue_process_exit(size_t pid);
void posix_mqueue_clock_changed();

#endif
