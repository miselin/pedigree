#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "contract.h"
#include <sys/auxv.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/stat.h>

struct fs_thread {
  int command, report;
  const char* owned;
  const char* created;
};
static int owner(int fd, uid_t uid, gid_t gid) {
  struct stat st;
  return fstat(fd, &st) || st.st_uid != uid || st.st_gid != gid || (st.st_mode & 0777) != 0600;
}
static void* isolated_fs(void* opaque) {
  struct fs_thread* args = opaque;
  int failed = 0, fd = -1;
  CHECK(setfsuid(-1) == 70001 && setfsgid(-1) == 70002 && geteuid() == 0 && getegid() == 0);
  CHECK(setfsuid(70003) == 70001 && setfsgid(70004) == 70002);
  CHECK(!pm_send(args->report, 'r') && !pm_receive(args->command, 'p'));
  CHECK(setfsuid(-1) == 70003 && setfsgid(-1) == 70004 && geteuid() == 0);
  errno = 0;
  fd = open(args->owned, O_RDONLY | O_CLOEXEC);
  CHECK(fd == -1 && errno == EACCES);
  fd = open(args->created, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  CHECK(fd >= 0 && !owner(fd, 70003, 70004));
  CHECK(!pm_send(args->report, 's') && !pm_receive(args->command, 'v'));
  CHECK(setfsuid(-1) == 0 && setfsgid(-1) == 0);
out:
  if (fd >= 0)
    close(fd);
  return (void*)(uintptr_t)failed;
}
int pm_fs_exec(int argc, char** argv) {
  if (argc != 7)
    return 2;
  int failed = 0;
  uid_t r, e, s, wanted_r = strtoul(argv[2], NULL, 10), wanted_e = strtoul(argv[3], NULL, 10);
  gid_t gr, ge, gs, wanted_gr = strtoul(argv[4], NULL, 10), wanted_ge = strtoul(argv[5], NULL, 10);
  CHECK(!getresuid(&r, &e, &s) && r == wanted_r && e == wanted_e && s == wanted_e);
  CHECK(!getresgid(&gr, &ge, &gs) && gr == wanted_gr && ge == wanted_ge && gs == wanted_ge);
  CHECK((uid_t)setfsuid(-1) == e && (gid_t)setfsgid(-1) == ge);
  CHECK(prctl(PR_GET_DUMPABLE, 0UL, 0UL, 0UL, 0UL) == atoi(argv[6]));
  CHECK(getauxval(AT_UID) == r && getauxval(AT_EUID) == e && getauxval(AT_GID) == gr &&
        getauxval(AT_EGID) == ge);
out:
  return failed;
}
static int backend(int disk) {
  int failed = 0, root_fd = -1, owned_fd = -1, probe = -1, live = 0;
  int command[2] = {-1, -1}, report[2] = {-1, -1};
  pid_t child = -1;
  pthread_t thread;
  void* thread_result;
  char directory[128], root_path[160], owned_path[160], worker_path[160], private_dir[160],
      nested[192], own_dir[160];
  snprintf(directory, sizeof(directory), "%s/process-memory-fs-%d", disk ? "" : "/tmp", getpid());
  snprintf(root_path, sizeof(root_path), "%s/root", directory);
  snprintf(owned_path, sizeof(owned_path), "%s/owned", directory);
  snprintf(worker_path, sizeof(worker_path), "%s/thread", directory);
  snprintf(private_dir, sizeof(private_dir), "%s/private", directory);
  snprintf(nested, sizeof(nested), "%s/file", private_dir);
  snprintf(own_dir, sizeof(own_dir), "%s/owned-dir", directory);
  CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0);
  CHECK(!mkdir(directory, 0777) && !chmod(directory, 0777));
  root_fd = open(root_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  CHECK(root_fd >= 0 && !owner(root_fd, 0, 0));
  CHECK(!mkdir(private_dir, 0700));
  probe = open(nested, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
  CHECK(probe >= 0 && !close(probe));
  probe = -1;
  CHECK(!pm_dumpable(1));
  CHECK(setfsuid(70001) == 0 && setfsgid(70002) == 0);
  CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0);
  CHECK(prctl(PR_GET_DUMPABLE, 0UL, 0UL, 0UL, 0UL) == 0);
  errno = 0;
  probe = open(root_path, O_RDONLY | O_CLOEXEC);
  CHECK(probe == -1 && errno == EACCES);
  CHECK(!access(nested, R_OK));
  CHECK(setfsuid(-1) == 70001 && setfsgid(-1) == 70002);
  errno = 0;
  CHECK(faccessat(AT_FDCWD, nested, R_OK, AT_EACCESS) == -1 && errno == EACCES);
  errno = 0;
  probe = open(nested, O_RDONLY | O_CLOEXEC);
  CHECK(probe == -1 && errno == EACCES);
  owned_fd = open(owned_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  CHECK(owned_fd >= 0 && !owner(owned_fd, 70001, 70002));
  CHECK(!mkdir(own_dir, 0700));
  struct stat directory_stat;
  CHECK(!stat(own_dir, &directory_stat) && S_ISDIR(directory_stat.st_mode) &&
        directory_stat.st_uid == 70001 && directory_stat.st_gid == 70002 &&
        (directory_stat.st_mode & 0777) == 0700);
  CHECK(!pm_write(owned_fd, "f", 1));
  probe = open(owned_path, O_RDONLY | O_CLOEXEC);
  CHECK(probe >= 0 && !close(probe));
  probe = -1;
  CHECK(!fchmod(owned_fd, 0040) && setfsuid(70003) == 70001);
  probe = open(owned_path, O_RDONLY | O_CLOEXEC);
  CHECK(probe >= 0 && !close(probe));
  probe = -1;
  CHECK(setfsgid(70004) == 70002);
  errno = 0;
  probe = open(owned_path, O_RDONLY | O_CLOEXEC);
  CHECK(probe == -1 && errno == EACCES);
  CHECK(setfsuid(70001) == 70003 && setfsgid(70002) == 70004 && !fchmod(owned_fd, 0600));
  CHECK(!pipe(command) && !pipe(report));
  struct fs_thread arguments = {command[0], report[1], owned_path, worker_path};
  CHECK(!pthread_create(&thread, NULL, isolated_fs, &arguments));
  live = 1;
  CHECK(!pm_receive(report[0], 'r'));
  CHECK(setfsuid(-1) == 70001 && setfsgid(-1) == 70002);
  probe = open(owned_path, O_RDONLY | O_CLOEXEC);
  CHECK(probe >= 0 && !close(probe));
  probe = -1;
  CHECK(setfsuid(70005) == 70001 && setfsgid(70006) == 70002);
  CHECK(!pm_send(command[1], 'p') && !pm_receive(report[0], 's'));
  /* Musl synchronizes ordinary setters; their callbacks must repair each FS pair. */
  CHECK(!seteuid(0) && !setegid(0) && setfsuid(-1) == 0 && setfsgid(-1) == 0);
  CHECK(!pm_send(command[1], 'v') && !pthread_join(thread, &thread_result));
  live = 0;
  CHECK(thread_result == NULL && setfsuid(70005) == 0 && setfsgid(70006) == 0);
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    if (setfsuid(-1) != 70005 || setfsgid(-1) != 70006 || geteuid() != 0 || getegid() != 0)
      _exit(10);
    execl(PM_APP, PM_APP, "fs-exec", "0", "0", "0", "0", "1", (char*)NULL);
    _exit(11);
  }
  int status = pm_reap(child, 10000);
  child = -1;
  CHECK(status == 0 && setfsuid(-1) == 70005 && setfsgid(-1) == 70006);
  CHECK(setfsuid(0) == 70005 && setfsgid(0) == 70006);
  probe = open(worker_path, O_RDONLY | O_CLOEXEC);
  CHECK(probe >= 0 && !owner(probe, 70003, 70004));
  CHECK(!fsync(owned_fd));
out:
  if (live) {
    pm_send(command[1], 'p');
    pthread_join(thread, NULL);
  }
  if (child > 0)
    pm_reap(child, 100);
  setfsuid(0);
  setfsgid(0);
  for (int n = 0; n < 2; ++n) {
    if (command[n] >= 0)
      close(command[n]);
    if (report[n] >= 0)
      close(report[n]);
  }
  if (probe >= 0)
    close(probe);
  if (owned_fd >= 0)
    close(owned_fd);
  if (root_fd >= 0)
    close(root_fd);
  unlink(worker_path);
  unlink(owned_path);
  unlink(root_path);
  unlink(nested);
  rmdir(own_dir);
  rmdir(private_dir);
  rmdir(directory);
  return failed;
}
int pm_filesystem(void) {
  return backend(0) || backend(1);
}
