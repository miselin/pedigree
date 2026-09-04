/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/syscall.h>

extern void fail(void) __attribute__((noreturn));

static void status(const char* s) {
  puts(s);
  fflush(stdout);
}

#define OK status("OK\n")

static void test_positional_io(void) {
  static const char initial[] = "abcdefghij";
  static const char replacement[] = "XYZ";
  static const char expected[] = "aXYZefghij";
  char buffer[sizeof(initial)] = {0};

  status("Testing positional file I/O... ");
  int fd = open("/testing/positional-io", O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0666);
  if (fd < 0 || write(fd, initial, sizeof(initial) - 1) != (ssize_t)(sizeof(initial) - 1) ||
      lseek(fd, 3, SEEK_SET) != 3)
    fail();

  if (syscall(SYS_pread64, fd, buffer, 3, 6) != 3 || memcmp(buffer, "ghi", 3) ||
      lseek(fd, 0, SEEK_CUR) != 3)
    fail();
  if (syscall(SYS_pwrite64, fd, replacement, sizeof(replacement) - 1, 1) !=
          (ssize_t)(sizeof(replacement) - 1) ||
      lseek(fd, 0, SEEK_CUR) != 3)
    fail();

  memset(buffer, 0, sizeof(buffer));
  if (syscall(SYS_pread64, fd, buffer, sizeof(expected) - 1, 0) !=
          (ssize_t)(sizeof(expected) - 1) ||
      memcmp(buffer, expected, sizeof(expected) - 1))
    fail();

  errno = 0;
  if (syscall(SYS_pread64, fd, buffer, 1, (off_t)-1) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_pwrite64, fd, buffer, 2, INT64_MAX) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_pread64, fd, buffer, (size_t)INT64_MAX + 1, 0) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_pwrite64, fd, (const void*)UINTPTR_MAX, 2, 0) != -1 || errno != EFAULT)
    fail();

  int pipefd[2];
  if (pipe(pipefd))
    fail();
  errno = 0;
  if (syscall(SYS_pread64, pipefd[0], buffer, 1, 0) != -1 || errno != ESPIPE)
    fail();
  errno = 0;
  if (syscall(SYS_pwrite64, pipefd[1], buffer, 1, 0) != -1 || errno != ESPIPE)
    fail();
  if (close(pipefd[0]) || close(pipefd[1]))
    fail();

  int readOnly = open("/testing/positional-io", O_RDONLY);
  int writeOnly = open("/testing/positional-io", O_WRONLY);
  if (readOnly < 0 || writeOnly < 0)
    fail();
  errno = 0;
  if (syscall(SYS_pwrite64, readOnly, buffer, 1, 0) != -1 || errno != EBADF)
    fail();
  errno = 0;
  if (syscall(SYS_pread64, writeOnly, buffer, 1, 0) != -1 || errno != EBADF)
    fail();

  if (close(readOnly) || close(writeOnly) || close(fd) || unlink("/testing/positional-io"))
    fail();
  OK;
}

void test_fs() {
  int fd = -1;
  int rc = 0;

  srand(0);

  printf("Testing filesystem...\n");

  int urandom_fd = open("/dev/urandom", O_RDONLY);
  if (urandom_fd < 0)
    fail();

  // fsck test directory - deleting a directory like we do below will result
  // in us possibly missing "bad directory count" errors.
  status("Creating directory for fsck test... ");
  rc = mkdir("/fscktest", 0777);
  if (rc)
    fail();
  OK;

  // directory to be deleted later - shouldn't leave any cruft lying around
  status("Creating directory for main test... ");
  rc = mkdir("/testing", 0777);
  if (rc)
    fail();
  OK;

  test_positional_io();

  // Create some files of varying sizes and destroy them.
  status("Testing file creation... ");
  for (size_t i = 0; i < 10; ++i) {
    size_t sz = rand() % 8192;
    if (!sz)
      ++sz;
    void* p = malloc(sz);
    if (!p)
      fail();
    size_t bytesRead = 0;
    while (bytesRead < sz) {
      ssize_t n = read(urandom_fd, (char*)p + bytesRead, sz - bytesRead);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        free(p);
        fail();
      }
      bytesRead += (size_t)n;
    }

    // Files that are expected to be deleted (might miss issues here in
    // fsck after their deletion).
    char fn[256];
    sprintf(fn, "/testing/f%zu", i);
    fd = open(fn, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
      free(p);
      fail();
    }
    if (write(fd, p, sz) != (ssize_t)sz) {
      close(fd);
      free(p);
      fail();
    }
    close(fd);

    // Same deal for the fscktest directory. fsck will pick these up.
    sprintf(fn, "/fscktest/f%zu", i);
    fd = open(fn, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
      free(p);
      fail();
    }
    if (write(fd, p, sz) != (ssize_t)sz) {
      close(fd);
      free(p);
      fail();
    }
    close(fd);
    free(p);
  }
  OK;

  status("Testing file deletion... ");
  for (size_t i = 0; i < 5; ++i) {
    char fn[256];
    sprintf(fn, "/testing/f%zu", i);
    unlink(fn);
  }
  OK;

  status("Testing a failed rmdir... ");
  rc = rmdir("/testing");
  if (rc == 0)
    fail();
  OK;

  status("Testing further file deletion... ");
  for (size_t i = 5; i < 10; ++i) {
    char fn[256];
    sprintf(fn, "/testing/f%zu", i);
    unlink(fn);
  }
  OK;

  status("Testing rmdir... ");
  rc = rmdir("/testing");
  if (rc)
    fail();
  close(urandom_fd);
  OK;
}
