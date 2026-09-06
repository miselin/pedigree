#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/stat.h>

static int aliases_and_marks(void) {
  int failed = 0, group = -1, alias_fd = -1, opened = -1;
  struct fh_file file = {.fd = -1};
  struct fh_record record;
  struct fh_handle alias_handle;
  char alias[224] = {0};
  int mount_id;
  CHECK(!fh_create(&file));
  snprintf(alias, sizeof(alias), "%s.alias", file.path);
  CHECK(!link(file.path, alias));
  alias_fd = open(alias, O_RDONLY | O_CLOEXEC);
  CHECK(alias_fd >= 0 && !fh_export(alias_fd, &alias_handle, &mount_id));
  CHECK(fh_equal(&alias_handle, &file.handle));
  group = fh_group(1);
  CHECK(group >= 0 && !fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY));
  CHECK(!fanotify_mark(group, FAN_MARK_ADD, FAN_ATTRIB, alias_fd, NULL));
  CHECK(!fchmod(alias_fd, 0644) && !fh_modify(&file));
  CHECK(!fh_take(group, &file.handle, &record));
  CHECK(record.mask == (FAN_ATTRIB | FAN_MODIFY) && record.pid == getpid());
  CHECK(fh_equal(&record.handle, &file.handle) && fh_readable(group, 0) == 0);
  opened = open_by_handle_at(file.fd, (struct file_handle*)&record.handle, O_RDONLY);
  char byte;
  CHECK(opened >= 0 && pread(opened, &byte, 1, 0) == 1 && byte == '!');
  CHECK(!close(opened));
  opened = -1;
  CHECK(!fanotify_mark(group, FAN_MARK_REMOVE, FAN_MODIFY, alias_fd, NULL));
  CHECK(!fh_modify(&file) && fh_readable(group, 0) == 0);
  CHECK(!fchmod(file.fd, 0600));
  CHECK(!fh_event(group, &file, FAN_ATTRIB, getpid()));
  CHECK(!fh_mark(group, &file, FAN_MARK_REMOVE, FAN_ATTRIB));
  errno = 0;
  CHECK(fh_mark(group, &file, FAN_MARK_REMOVE, FAN_ATTRIB) == -1 && errno == ENOENT);
  CHECK(!fh_mark(group, &file, FAN_MARK_ADD, FAN_OPEN | FAN_CLOSE));
  opened = open(alias, O_RDONLY | O_CLOEXEC);
  CHECK(opened >= 0 && !fh_event(group, &file, FAN_OPEN, getpid()));
  CHECK(!close(opened));
  opened = -1;
  CHECK(!fh_event(group, &file, FAN_CLOSE_NOWRITE, getpid()));
  opened = open(file.path, O_RDWR | O_CLOEXEC);
  CHECK(opened >= 0 && !fh_event(group, &file, FAN_OPEN, getpid()));
  CHECK(!close(opened));
  opened = -1;
  CHECK(!fh_event(group, &file, FAN_CLOSE_WRITE, getpid()));
  CHECK(!fanotify_mark(group, FAN_MARK_FLUSH, UINT64_MAX, -1, (const char*)1));
  CHECK(!fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY));
  CHECK(!fh_modify(&file));
  CHECK(!fh_mark(group, &file, FAN_MARK_REMOVE, FAN_MODIFY));
  CHECK(!fh_event(group, &file, FAN_MODIFY, getpid()));
  CHECK(!fh_modify(&file) && fh_readable(group, 0) == 0);
out:
  if (opened >= 0)
    close(opened);
  if (group >= 0)
    close(group);
  if (alias_fd >= 0)
    close(alias_fd);
  if (alias[0])
    unlink(alias);
  fh_close(&file);
  return failed;
}

static int producer_exit(void) {
  int failed = 0, group = -1, gate[2] = {-1, -1}, completed[2] = {-1, -1};
  pid_t child = -1;
  struct fh_file file = {.fd = -1};
  CHECK(!fh_create(&file));
  group = fh_group(1);
  CHECK(group >= 0 && !fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY));
  CHECK(!pipe(gate) && !pipe(completed));
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    alarm(10);
    close(gate[1]);
    close(completed[0]);
    if (fh_receive(gate[0], 'g') || fh_modify(&file) || fh_send(completed[1], 'd'))
      _exit(10);
    _exit(0);
  }
  CHECK(!fh_send(gate[1], 'g') && !fh_receive(completed[0], 'd'));
  pid_t producer = child;
  CHECK(!fh_reap(child, 12000));
  child = -1;
  CHECK(!fh_event(group, &file, FAN_MODIFY, producer));
  CHECK(fh_readable(group, 0) == 0);
out:
  if (child > 0)
    fh_reap(child, 100);
  for (int n = 0; n < 2; ++n) {
    if (gate[n] >= 0)
      close(gate[n]);
    if (completed[n] >= 0)
      close(completed[n]);
  }
  if (group >= 0)
    close(group);
  fh_close(&file);
  return failed;
}
int fh_events(void) {
  return aliases_and_marks() || producer_exit();
}
