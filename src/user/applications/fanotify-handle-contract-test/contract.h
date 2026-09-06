#ifndef FANOTIFY_HANDLE_CONTRACT_H
#define FANOTIFY_HANDLE_CONTRACT_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/fanotify.h>
#include <sys/types.h>

#define FH_APP "/applications/fanotify-handle-contract-test"
#define FH_QUEUE_LIMIT 256
#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

struct fh_handle {
  unsigned handle_bytes;
  int handle_type;
  unsigned char bytes[128];
};
struct fh_file {
  int fd, mount_id;
  char path[192];
  struct fh_handle handle;
};
struct fh_record {
  uint64_t mask;
  int pid;
  unsigned char fsid[8];
  struct fh_handle handle;
};

extern size_t fh_page;
int64_t fh_now(void);
void fh_pause(int milliseconds);
int fh_wait(volatile int* flag, int milliseconds);
int fh_reap(pid_t child, int milliseconds);
int fh_send(int fd, char byte);
int fh_receive(int fd, char byte);
int fh_read_all(int fd, void* buffer, size_t length);
int fh_write_all(int fd, const void* buffer, size_t length);
int fh_send_fd(int socket, int fd);
int fh_receive_fd(int socket);
int fh_readable(int fd, int milliseconds);
int fh_export(int fd, struct fh_handle* handle, int* mount_id);
int fh_equal(const struct fh_handle* first, const struct fh_handle* second);
int fh_create(struct fh_file* file);
void fh_close(struct fh_file* file);
int fh_group(int nonblock);
int fh_mark(int group, const struct fh_file* file, unsigned flags, uint64_t mask);
int fh_modify(const struct fh_file* file);
size_t fh_record_size(const struct fh_handle* handle);
int fh_parse(const void* bytes, size_t length, struct fh_record* record);
int fh_take(int group, const struct fh_handle* shape, struct fh_record* record);
int fh_event(int group, const struct fh_file* file, uint64_t mask, pid_t pid);
int fh_drain(int group);
int fh_handles(void);
int fh_admission(void);
int fh_events(void);
int fh_queue(void);
int fh_lifetime(void);
int fh_waits(void);
int fh_exec(int argc, char** argv);
int fh_persistence(const char* base, const char* token, int write_stage);

#endif
