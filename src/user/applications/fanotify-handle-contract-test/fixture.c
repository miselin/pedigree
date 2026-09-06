#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/socket.h>
#include <sys/wait.h>

int64_t fh_now(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
void fh_pause(int milliseconds) {
  struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
  while (nanosleep(&pause, &pause) && errno == EINTR) {
  }
}
int fh_wait(volatile int* flag, int milliseconds) {
  int64_t start = fh_now(), deadline = start + (int64_t)milliseconds * 1000000;
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    int64_t now = fh_now();
    if (start < 0 || now < 0 || now >= deadline)
      return -1;
    fh_pause(2);
  }
  return 0;
}
int fh_reap(pid_t child, int milliseconds) {
  int64_t start = fh_now(), deadline = start + (int64_t)milliseconds * 1000000;
  int64_t now;
  while (start >= 0 && (now = fh_now()) >= 0 && now < deadline) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    fh_pause(5);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}
int fh_read_all(int fd, void* buffer, size_t length) {
  unsigned char* bytes = buffer;
  while (length) {
    ssize_t amount = read(fd, bytes, length);
    if (amount < 0 && errno == EINTR)
      continue;
    if (amount <= 0)
      return -1;
    bytes += amount;
    length -= amount;
  }
  return 0;
}
int fh_write_all(int fd, const void* buffer, size_t length) {
  const unsigned char* bytes = buffer;
  while (length) {
    ssize_t amount = write(fd, bytes, length);
    if (amount < 0 && errno == EINTR)
      continue;
    if (amount <= 0)
      return -1;
    bytes += amount;
    length -= amount;
  }
  return 0;
}
int fh_send(int fd, char byte) {
  return fh_write_all(fd, &byte, 1);
}
int fh_readable(int fd, int milliseconds) {
  struct pollfd entry = {.fd = fd, .events = POLLIN};
  int result = poll(&entry, 1, milliseconds);
  return result > 0 ? !!(entry.revents & POLLIN) : result;
}
int fh_receive(int fd, char byte) {
  if (fh_readable(fd, 5000) != 1)
    return -1;
  char received;
  return !fh_read_all(fd, &received, 1) && received == byte ? 0 : -1;
}
int fh_send_fd(int socket, int fd) {
  char byte = 'f';
  struct iovec iov = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &iov,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control.bytes)};
  struct cmsghdr* item = CMSG_FIRSTHDR(&message);
  item->cmsg_level = SOL_SOCKET;
  item->cmsg_type = SCM_RIGHTS;
  item->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(item), &fd, sizeof(fd));
  return sendmsg(socket, &message, 0) == 1 ? 0 : -1;
}
int fh_receive_fd(int socket) {
  char byte;
  struct iovec iov = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &iov,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control.bytes)};
  if (recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1 || message.msg_flags & MSG_CTRUNC)
    return -1;
  struct cmsghdr* item = CMSG_FIRSTHDR(&message);
  if (!item || item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS ||
      item->cmsg_len != CMSG_LEN(sizeof(int)))
    return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(item), sizeof(fd));
  return fd;
}
int fh_export(int fd, struct fh_handle* handle, int* mount_id) {
  memset(handle, 0, sizeof(*handle));
  handle->handle_bytes = sizeof(handle->bytes);
  return name_to_handle_at(fd, "", (struct file_handle*)handle, mount_id, AT_EMPTY_PATH);
}
int fh_equal(const struct fh_handle* first, const struct fh_handle* second) {
  return first->handle_bytes <= sizeof(first->bytes) &&
         first->handle_bytes == second->handle_bytes && first->handle_type == second->handle_type &&
         !memcmp(first->bytes, second->bytes, first->handle_bytes);
}
int fh_create(struct fh_file* file) {
  static unsigned sequence;
  memset(file, 0, sizeof(*file));
  file->fd = -1;
  snprintf(file->path, sizeof(file->path), "/fanotify-handle-%ld-%u", (long)getpid(), ++sequence);
  file->fd = open(file->path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (file->fd < 0 || fh_write_all(file->fd, "0123456789abcdef", 16) ||
      fh_export(file->fd, &file->handle, &file->mount_id)) {
    int saved = errno;
    fh_close(file);
    errno = saved;
    return -1;
  }
  return 0;
}
void fh_close(struct fh_file* file) {
  if (file->fd >= 0)
    close(file->fd);
  if (file->path[0])
    unlink(file->path);
  file->fd = -1;
  file->path[0] = 0;
}
int fh_group(int nonblock) {
  return fanotify_init(FAN_REPORT_FID | FAN_CLOEXEC | (nonblock ? FAN_NONBLOCK : 0), O_RDONLY);
}
int fh_mark(int group, const struct fh_file* file, unsigned flags, uint64_t mask) {
  return fanotify_mark(group, flags, mask, file->fd, NULL);
}
int fh_modify(const struct fh_file* file) {
  return pwrite(file->fd, "!", 1, 0) == 1 ? 0 : -1;
}
size_t fh_record_size(const struct fh_handle* handle) {
  return sizeof(struct fanotify_event_metadata) +
         ((sizeof(struct fanotify_event_info_fid) + 8 + handle->handle_bytes + 3) & ~(size_t)3);
}
int fh_parse(const void* bytes, size_t length, struct fh_record* record) {
  struct fanotify_event_metadata metadata;
  if (length < sizeof(metadata))
    return -1;
  memcpy(&metadata, bytes, sizeof(metadata));
  if (metadata.vers != FANOTIFY_METADATA_VERSION || metadata.event_len != length ||
      metadata.metadata_len < sizeof(metadata) || metadata.metadata_len > length ||
      metadata.fd != FAN_NOFD)
    return -1;
  memset(record, 0, sizeof(*record));
  record->mask = metadata.mask;
  record->pid = metadata.pid;
  if (metadata.mask & FAN_Q_OVERFLOW)
    return length == metadata.metadata_len ? 0 : -1;
  const unsigned char* cursor = (const unsigned char*)bytes + metadata.metadata_len;
  size_t remaining = length - metadata.metadata_len;
  if (remaining < sizeof(struct fanotify_event_info_fid) + 8)
    return -1;
  struct fanotify_event_info_header info;
  memcpy(&info, cursor, sizeof(info));
  if (info.info_type != FAN_EVENT_INFO_TYPE_FID || info.len != remaining)
    return -1;
  memcpy(record->fsid, cursor + sizeof(info), sizeof(record->fsid));
  memcpy(&record->handle, cursor + sizeof(struct fanotify_event_info_fid), 8);
  size_t count = record->handle.handle_bytes;
  if (!count || count > sizeof(record->handle.bytes) ||
      sizeof(struct fanotify_event_info_fid) + 8 + count > remaining)
    return -1;
  memcpy(record->handle.bytes, cursor + sizeof(struct fanotify_event_info_fid) + 8, count);
  unsigned present = 0;
  for (size_t n = 0; n < sizeof(record->fsid); ++n)
    present |= record->fsid[n];
  return present ? 0 : -1;
}
int fh_take(int group, const struct fh_handle* shape, struct fh_record* record) {
  unsigned char bytes[256];
  size_t length = fh_record_size(shape);
  if (length > sizeof(bytes))
    return -1;
  ssize_t result = read(group, bytes, length);
  return result > 0 ? fh_parse(bytes, (size_t)result, record) : -1;
}
int fh_event(int group, const struct fh_file* file, uint64_t mask, pid_t pid) {
  struct fh_record record = {0};
  int result = fh_take(group, &file->handle, &record);
  if (result || !fh_equal(&record.handle, &file->handle) || (record.mask & mask) != mask ||
      record.pid != pid) {
    fprintf(stderr,
            "FANOTIFY-HANDLE-CONTRACT: event result=%d expected=%llx/%ld actual=%llx/%d errno=%d\n",
            result, (unsigned long long)mask, (long)pid, (unsigned long long)record.mask,
            record.pid, errno);
    return -1;
  }
  return 0;
}
int fh_drain(int group) {
  unsigned char bytes[4096];
  for (unsigned attempt = 0; attempt < FH_QUEUE_LIMIT + 2; ++attempt) {
    ssize_t count = read(group, bytes, sizeof(bytes));
    if (count < 0 && errno == EAGAIN)
      return 0;
    if (count <= 0)
      return -1;
  }
  return -1;
}
