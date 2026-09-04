#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>

_Static_assert(sizeof(struct inotify_event) == 16,
               "musl's Linux amd64 inotify_event header must be 16 bytes");
_Static_assert(SYS_inotify_init == 253, "unexpected musl inotify_init syscall number");
_Static_assert(SYS_inotify_add_watch == 254, "unexpected musl inotify_add_watch syscall number");
_Static_assert(SYS_inotify_rm_watch == 255, "unexpected musl inotify_rm_watch syscall number");
_Static_assert(SYS_inotify_init1 == 294, "unexpected musl inotify_init1 syscall number");

static int find_event(int fd, int wd, uint32_t required_mask, const char* name) {
  unsigned char buffer[2048];
  ssize_t amount = read(fd, buffer, sizeof(buffer));
  if (amount < 0) {
    return 0;
  }

  size_t offset = 0;
  while (offset + sizeof(struct inotify_event) <= (size_t)amount) {
    const struct inotify_event* event = (const struct inotify_event*)(buffer + offset);
    const size_t record_size = sizeof(*event) + event->len;
    if (record_size > (size_t)amount - offset ||
        (event->len && event->len % sizeof(struct inotify_event))) {
      return 0;
    }
    if (event->wd == wd && (event->mask & required_mask) == required_mask) {
      if (!name && !event->len) {
        return 1;
      }
      if (name) {
        const size_t name_length = strlen(name);
        const size_t expected_length = (name_length + 1 + sizeof(struct inotify_event) - 1) &
                                       ~(sizeof(struct inotify_event) - 1);
        if (event->len != expected_length || memcmp(event->name, name, name_length) ||
            event->name[name_length]) {
          offset += record_size;
          continue;
        }
        int padding_zeroed = 1;
        for (size_t i = name_length + 1; i < event->len; ++i) {
          if (event->name[i]) {
            padding_zeroed = 0;
            break;
          }
        }
        if (padding_zeroed) {
          return 1;
        }
      }
    }
    offset += record_size;
  }
  return 0;
}

static int find_single_named_event(int fd, int wd, uint32_t mask, const char* name) {
  unsigned char buffer[256];
  const size_t name_length = strlen(name);
  const size_t padded_length =
      (name_length + 1 + sizeof(struct inotify_event) - 1) & ~(sizeof(struct inotify_event) - 1);
  const ssize_t amount = read(fd, buffer, sizeof(buffer));
  const struct inotify_event* event = (const struct inotify_event*)buffer;
  if (amount != (ssize_t)(sizeof(*event) + padded_length) || event->wd != wd ||
      event->mask != mask || event->cookie != 0 || event->len != padded_length ||
      memcmp(event->name, name, name_length) || event->name[name_length]) {
    return 0;
  }
  for (size_t i = name_length + 1; i < padded_length; ++i) {
    if (event->name[i]) {
      return 0;
    }
  }
  return 1;
}

static int find_one_shot_pair(int fd, int wd) {
  unsigned char buffer[2048];
  ssize_t amount = read(fd, buffer, sizeof(buffer));
  if (amount < 0) {
    return 0;
  }

  int modifies = 0;
  int ignored = 0;
  size_t offset = 0;
  while (offset + sizeof(struct inotify_event) <= (size_t)amount) {
    const struct inotify_event* event = (const struct inotify_event*)(buffer + offset);
    const size_t record_size = sizeof(*event) + event->len;
    if (record_size > (size_t)amount - offset) {
      return 0;
    }
    if (event->wd == wd && event->mask == IN_MODIFY) {
      ++modifies;
    } else if (event->wd == wd && event->mask == IN_IGNORED) {
      ++ignored;
    }
    offset += record_size;
  }
  return modifies == 1 && ignored == 1;
}

static int find_exact_pair(int fd, int wd, uint32_t first_mask, uint32_t second_mask) {
  uint64_t storage[4] = {0};
  ssize_t amount = read(fd, storage, sizeof(storage));
  const struct inotify_event* first = (const struct inotify_event*)storage;
  const struct inotify_event* second =
      (const struct inotify_event*)((const unsigned char*)storage + sizeof(*first));
  return amount == (ssize_t)sizeof(storage) && first->wd == wd && first->mask == first_mask &&
         first->cookie == 0 && first->len == 0 && second->wd == wd && second->mask == second_mask &&
         second->cookie == 0 && second->len == 0;
}

static int find_single_event(int fd, int wd, uint32_t mask) {
  struct inotify_event event;
  ssize_t amount = read(fd, &event, sizeof(event));
  return amount == (ssize_t)sizeof(event) && event.wd == wd && event.mask == mask &&
         event.cookie == 0 && event.len == 0;
}

static int empty_nonblocking(int fd) {
  unsigned char buffer[sizeof(struct inotify_event)];
  errno = 0;
  return read(fd, buffer, sizeof(buffer)) == -1 && errno == EAGAIN;
}

int main(void) {
  int result = 1;
  int notify_fd = -1;
  int one_shot_fd = -1;
  int delete_fd = -1;
  int epoll_fd = -1;
  int child_fd = -1;
  int other_fd = -1;
  char directory[128];
  char child[160];
  char other[160];
  char subdirectory[160];

  snprintf(directory, sizeof(directory), "/tmp/inotify-test-%ld", (long)getpid());
  snprintf(child, sizeof(child), "%s/child", directory);
  snprintf(other, sizeof(other), "%s/other", directory);
  snprintf(subdirectory, sizeof(subdirectory), "%s/subdir", directory);

  errno = 0;
  if (inotify_init1(0x40000000) != -1 || errno != EINVAL) {
    goto cleanup;
  }
  int public_fd = inotify_init();
  if (public_fd < 0 || close(public_fd)) {
    goto cleanup;
  }
  int legacy_fd = (int)syscall(SYS_inotify_init);
  if (legacy_fd < 0 || close(legacy_fd)) {
    goto cleanup;
  }

  notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (notify_fd < 0 || !(fcntl(notify_fd, F_GETFL) & O_NONBLOCK) ||
      !(fcntl(notify_fd, F_GETFD) & FD_CLOEXEC)) {
    goto cleanup;
  }
  errno = 0;
  unsigned char short_buffer[sizeof(struct inotify_event) - 1];
  if (read(notify_fd, short_buffer, sizeof(short_buffer)) != -1 || errno != EAGAIN) {
    goto cleanup;
  }
  errno = 0;
  void* bad_read_buffer = (void*)(uintptr_t)1;
  if (read(notify_fd, bad_read_buffer, sizeof(struct inotify_event)) != -1 || errno != EAGAIN) {
    goto cleanup;
  }

  if (mkdir(directory, 0700)) {
    goto cleanup;
  }
  errno = 0;
  if (inotify_add_watch(-1, (const char*)1, 0) != -1 || errno != EINVAL) {
    goto cleanup;
  }
  errno = 0;
  if (inotify_add_watch(-1, (const char*)1, IN_CREATE | 0x00800000U) != -1 || errno != EINVAL) {
    goto cleanup;
  }
  errno = 0;
  if (inotify_add_watch(-1, directory, IN_CREATE) != -1 || errno != EBADF) {
    goto cleanup;
  }
  const int directory_wd = inotify_add_watch(notify_fd, directory, IN_CREATE | IN_DELETE);
  if (directory_wd < 0) {
    goto cleanup;
  }
  errno = 0;
  if (inotify_add_watch(notify_fd, directory, IN_CREATE | IN_MASK_CREATE) != -1 ||
      errno != EEXIST) {
    goto cleanup;
  }
  errno = 0;
  if (inotify_add_watch(notify_fd, directory, IN_CREATE | IN_MASK_CREATE | IN_MASK_ADD) != -1 ||
      errno != EINVAL) {
    goto cleanup;
  }
  errno = 0;
  if (inotify_add_watch(notify_fd, (const char*)1, IN_CREATE | IN_MASK_CREATE | IN_MASK_ADD) !=
          -1 ||
      errno != EINVAL) {
    goto cleanup;
  }

  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 0x494e4f5449465901ULL};
  struct pollfd poll_state = {.fd = notify_fd, .events = POLLIN, .revents = 0};
  if (epoll_fd < 0 || epoll_ctl(epoll_fd, EPOLL_CTL_ADD, notify_fd, &interest) ||
      poll(&poll_state, 1, 0) != 0) {
    goto cleanup;
  }
  child_fd = open(child, O_CREAT | O_RDWR, 0600);
  struct epoll_event ready = {0};
  if (child_fd < 0 || poll(&poll_state, 1, 0) != 1 || !(poll_state.revents & POLLIN) ||
      epoll_wait(epoll_fd, &ready, 1, 1000) != 1 || !(ready.events & EPOLLIN) ||
      ready.data.u64 != interest.data.u64) {
    goto cleanup;
  }
  errno = 0;
  if (read(notify_fd, short_buffer, sizeof(short_buffer)) != -1 || errno != EINVAL ||
      !find_event(notify_fd, directory_wd, IN_CREATE, "child")) {
    goto cleanup;
  }

  if (inotify_add_watch(notify_fd, directory, IN_MODIFY | IN_ATTRIB | IN_MASK_ADD) !=
          directory_wd ||
      write(child_fd, "a", 1) != 1 || write(child_fd, "a", 1) != 1 ||
      !find_single_named_event(notify_fd, directory_wd, IN_MODIFY, "child")) {
    goto cleanup;
  }
  if (chmod(directory, 0755) || write(child_fd, "f", 1) != 1) {
    goto cleanup;
  }
  errno = 0;
  if (read(notify_fd, bad_read_buffer, sizeof(struct inotify_event)) != -1 || errno != EFAULT ||
      !find_single_named_event(notify_fd, directory_wd, IN_MODIFY, "child") ||
      !empty_nonblocking(notify_fd) || chmod(directory, 0700) ||
      !find_event(notify_fd, directory_wd, IN_ATTRIB | IN_ISDIR, NULL)) {
    goto cleanup;
  }

  int reopened = open(child, O_RDONLY);
  if (reopened < 0 || close(reopened) || !empty_nonblocking(notify_fd)) {
    goto cleanup;
  }
  if (inotify_add_watch(notify_fd, directory, IN_ATTRIB) != directory_wd ||
      write(child_fd, "r", 1) != 1 || !empty_nonblocking(notify_fd) || chmod(directory, 0755) ||
      !find_event(notify_fd, directory_wd, IN_ATTRIB | IN_ISDIR, NULL) ||
      inotify_add_watch(notify_fd, directory, IN_CREATE | IN_DELETE | IN_MODIFY | IN_MASK_ADD) !=
          directory_wd) {
    goto cleanup;
  }
  other_fd = open(other, O_CREAT | O_RDWR, 0600);
  if (other_fd < 0 || !find_event(notify_fd, directory_wd, IN_CREATE, "other")) {
    goto cleanup;
  }
  errno = 0;
  if (inotify_add_watch(notify_fd, child, IN_MODIFY | IN_ONLYDIR) != -1 || errno != ENOTDIR) {
    goto cleanup;
  }

  one_shot_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  const int output_only_wd = inotify_add_watch(one_shot_fd, child, IN_Q_OVERFLOW);
  if (one_shot_fd < 0 || output_only_wd < 0 || inotify_rm_watch(one_shot_fd, output_only_wd) ||
      !find_event(one_shot_fd, output_only_wd, IN_IGNORED, NULL)) {
    goto cleanup;
  }
  const int one_shot_wd = inotify_add_watch(one_shot_fd, child, IN_MODIFY | IN_ONESHOT);
  if (one_shot_fd < 0 || one_shot_wd < 0 || write(child_fd, "b", 1) != 1 ||
      write(child_fd, "c", 1) != 1 || !find_one_shot_pair(one_shot_fd, one_shot_wd) ||
      !empty_nonblocking(one_shot_fd)) {
    goto cleanup;
  }
  errno = 0;
  if (inotify_rm_watch(one_shot_fd, one_shot_wd) != -1 || errno != EINVAL) {
    goto cleanup;
  }
  const int replacement_wd = inotify_add_watch(one_shot_fd, child, IN_ATTRIB);
  if (replacement_wd < 0 || inotify_rm_watch(one_shot_fd, replacement_wd) ||
      !find_event(one_shot_fd, replacement_wd, IN_IGNORED, NULL)) {
    goto cleanup;
  }
  const int ignored_only_wd = inotify_add_watch(one_shot_fd, child, IN_MODIFY);
  if (ignored_only_wd < 0) {
    goto cleanup;
  }

  delete_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  const int delete_wd = inotify_add_watch(delete_fd, child, IN_DELETE_SELF);
  if (delete_fd < 0 || delete_wd < 0 || close(child_fd)) {
    child_fd = -1;
    goto cleanup;
  }
  child_fd = -1;
  if (unlink(child) || !find_exact_pair(delete_fd, delete_wd, IN_DELETE_SELF, IN_IGNORED) ||
      !find_single_event(one_shot_fd, ignored_only_wd, IN_IGNORED) ||
      !find_event(notify_fd, directory_wd, IN_DELETE, "child")) {
    goto cleanup;
  }
  errno = 0;
  if (inotify_rm_watch(delete_fd, delete_wd) != -1 || errno != EINVAL) {
    goto cleanup;
  }
  close(delete_fd);
  delete_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (mkdir(subdirectory, 0700) ||
      !find_event(notify_fd, directory_wd, IN_CREATE | IN_ISDIR, "subdir")) {
    goto cleanup;
  }
  const int directory_delete_wd = inotify_add_watch(delete_fd, subdirectory, IN_DELETE_SELF);
  if (delete_fd < 0 || directory_delete_wd < 0 || rmdir(subdirectory) ||
      !find_exact_pair(delete_fd, directory_delete_wd, IN_DELETE_SELF, IN_IGNORED) ||
      !find_event(notify_fd, directory_wd, IN_DELETE | IN_ISDIR, "subdir")) {
    goto cleanup;
  }

  if (inotify_rm_watch(notify_fd, directory_wd) ||
      !find_event(notify_fd, directory_wd, IN_IGNORED, NULL)) {
    goto cleanup;
  }

  result = 0;

cleanup:
  if (child_fd >= 0)
    close(child_fd);
  if (other_fd >= 0)
    close(other_fd);
  if (epoll_fd >= 0)
    close(epoll_fd);
  if (delete_fd >= 0)
    close(delete_fd);
  if (one_shot_fd >= 0)
    close(one_shot_fd);
  if (notify_fd >= 0)
    close(notify_fd);
  unlink(child);
  unlink(other);
  rmdir(subdirectory);
  rmdir(directory);

  if (result) {
    printf("INOTIFY-TEST: FAIL errno=%d\n", errno);
    return 1;
  }
  printf("INOTIFY-TEST: PASS\n");
  return 0;
}
