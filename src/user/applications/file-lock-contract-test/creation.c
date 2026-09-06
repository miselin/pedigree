#define _GNU_SOURCE
#include <grp.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/stat.h>

static int metadata_matches(const char* name, const struct stat* metadata, mode_t mode, uid_t uid,
                            gid_t gid) {
  if ((metadata->st_mode & 0777) == mode && metadata->st_uid == uid && metadata->st_gid == gid)
    return 1;
  fprintf(stderr,
          "FILE-LOCK-CONTRACT: %s mode=%04o uid=%lu gid=%lu expected mode=%04o uid=%lu gid=%lu\n",
          name, (unsigned)(metadata->st_mode & 07777), (unsigned long)metadata->st_uid,
          (unsigned long)metadata->st_gid, (unsigned)mode, (unsigned long)uid, (unsigned long)gid);
  return 0;
}

int file_lock_creation(void) {
  int failed = 0, fd = -1;
  pid_t child = -1;
  char file[128], directory[128], owned_file[128], owned_directory[128];
  struct stat metadata;
  const mode_t saved = umask(0027);
  snprintf(file, sizeof(file), "/tmp/file-lock-mode-%ld", (long)getpid());
  snprintf(directory, sizeof(directory), "/tmp/file-lock-dir-%ld", (long)getpid());
  snprintf(owned_file, sizeof(owned_file), "/tmp/file-lock-owned-%ld", (long)getpid());
  snprintf(owned_directory, sizeof(owned_directory), "/tmp/file-lock-own-dir-%ld", (long)getpid());
  CHECK((fd = open(file, O_CREAT | O_EXCL | O_RDWR, 0666)) >= 0);
  CHECK(fstat(fd, &metadata) == 0 && metadata_matches(file, &metadata, 0640, geteuid(), getegid()));
  CHECK(mkdir(directory, 0777) == 0);
  CHECK(stat(directory, &metadata) == 0 &&
        metadata_matches(directory, &metadata, 0750, geteuid(), getegid()));
  if (!geteuid()) {
    CHECK((child = fork()) >= 0);
    if (!child) {
      alarm(6);
      if (setgroups(0, NULL) || setgid(65534) || setuid(65534))
        _exit(10);
      if (open(file, O_RDONLY) != -1 || errno != EACCES ||
          open(directory, O_RDONLY | O_DIRECTORY) != -1 || errno != EACCES)
        _exit(11);
      int own = open(owned_file, O_CREAT | O_EXCL | O_RDWR, 0666);
      if (own < 0 || fstat(own, &metadata) ||
          !metadata_matches(owned_file, &metadata, 0640, 65534, 65534))
        _exit(12);
      close(own);
      own = open(owned_file, O_RDWR);
      if (own < 0 || write(own, "x", 1) != 1 || mkdir(owned_directory, 0777) ||
          stat(owned_directory, &metadata) ||
          !metadata_matches(owned_directory, &metadata, 0750, 65534, 65534))
        _exit(13);
      close(own);
      own = open(owned_directory, O_RDONLY | O_DIRECTORY);
      if (own < 0)
        _exit(14);
      close(own);
      if (unlink(owned_file) || rmdir(owned_directory))
        _exit(15);
      _exit(0);
    }
    CHECK(fl_reap(child, 7000) == 0);
    child = -1;
  } else {
    puts("FILE-LOCK-CONTRACT: SKIP nonowner creation checks require root");
  }
out:
  if (child > 0) {
    kill(child, SIGKILL);
    fl_reap(child, 1000);
  }
  umask(saved);
  if (fd >= 0)
    close(fd);
  unlink(file);
  rmdir(directory);
  unlink(owned_file);
  rmdir(owned_directory);
  return failed;
}
