#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

struct reader {
  int fd;
  size_t size;
  volatile int started, done, handled;
  ssize_t result;
  int error;
  unsigned char bytes[256];
};
static struct reader* active_reader;
static void handler(int signal) {
  (void)signal;
  if (active_reader)
    __atomic_add_fetch(&active_reader->handled, 1, __ATOMIC_RELEASE);
}
static void* read_thread(void* argument) {
  struct reader* reader = argument;
  __atomic_store_n(&reader->started, 1, __ATOMIC_RELEASE);
  errno = 0;
  reader->result = read(reader->fd, reader->bytes, reader->size);
  reader->error = errno;
  __atomic_store_n(&reader->done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static int interrupted(int restart) {
  int failed = 0, group = -1, running = 0, installed = 0;
  pthread_t thread;
  struct reader reader = {0};
  struct sigaction old, action = {.sa_handler = handler, .sa_flags = restart ? SA_RESTART : 0};
  struct fh_file file = {.fd = -1};
  struct fh_record record;
  CHECK(!fh_create(&file));
  group = fh_group(0);
  CHECK(group >= 0 && !fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY));
  reader.fd = group;
  reader.size = fh_record_size(&file.handle);
  sigemptyset(&action.sa_mask);
  CHECK(!sigaction(SIGUSR1, &action, &old));
  installed = 1;
  active_reader = &reader;
  CHECK(!pthread_create(&thread, NULL, read_thread, &reader));
  running = 1;
  CHECK(!fh_wait(&reader.started, 2000));
  int64_t deadline = fh_now() + 5000000000;
  while (!__atomic_load_n(&reader.done, __ATOMIC_ACQUIRE) && fh_now() < deadline) {
    CHECK(!pthread_kill(thread, SIGUSR1));
    if (restart && __atomic_load_n(&reader.handled, __ATOMIC_ACQUIRE) >= 3)
      break;
    fh_pause(2);
  }
  CHECK(__atomic_load_n(&reader.handled, __ATOMIC_ACQUIRE) > 0);
  if (restart) {
    CHECK(!__atomic_load_n(&reader.done, __ATOMIC_ACQUIRE));
    CHECK(!fh_modify(&file));
    CHECK(!fh_wait(&reader.done, 5000));
    CHECK(reader.result == (ssize_t)reader.size &&
          !fh_parse(reader.bytes, (size_t)reader.result, &record));
    CHECK(record.mask == FAN_MODIFY && record.pid == getpid() &&
          fh_equal(&record.handle, &file.handle));
  } else {
    CHECK(!fh_wait(&reader.done, 100));
    CHECK(reader.result == -1 && reader.error == EINTR);
    CHECK(!fh_modify(&file));
    CHECK(!fh_event(group, &file, FAN_MODIFY, getpid()));
  }
  CHECK(!pthread_join(thread, NULL));
  running = 0;
out:
  if (group >= 0)
    close(group);
  if (running) {
    if (fh_wait(&reader.done, 5000)) {
      fprintf(stderr, "FANOTIFY-HANDLE-CONTRACT: reader did not retire result=%ld errno=%d\n",
              (long)reader.result, reader.error);
      _exit(90);
    }
    pthread_join(thread, NULL);
  }
  active_reader = NULL;
  if (installed)
    sigaction(SIGUSR1, &old, NULL);
  fh_close(&file);
  return failed;
}

static int last_close(void) {
  int failed = 0, group = -1, running = 0;
  pthread_t thread;
  struct reader reader = {0};
  struct fh_file file = {.fd = -1};
  CHECK(!fh_create(&file));
  group = fh_group(0);
  CHECK(group >= 0);
  reader.fd = group;
  reader.size = fh_record_size(&file.handle);
  CHECK(!pthread_create(&thread, NULL, read_thread, &reader));
  running = 1;
  CHECK(!fh_wait(&reader.started, 2000));
  CHECK(!close(group));
  group = -1;
  CHECK(!fh_wait(&reader.done, 5000));
  CHECK(reader.result == -1 && reader.error == EBADF);
  CHECK(!pthread_join(thread, NULL));
  running = 0;
out:
  if (group >= 0)
    close(group);
  if (running) {
    if (fh_wait(&reader.done, 5000))
      _exit(91);
    pthread_join(thread, NULL);
  }
  fh_close(&file);
  return failed;
}
int fh_waits(void) {
  return interrupted(0) || interrupted(1) || last_close();
}
