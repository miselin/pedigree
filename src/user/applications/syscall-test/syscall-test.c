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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASS 1
#define FAIL 0

int test_open(void) {
  printf("open():\n");
  printf("\tOpen existing file - ");
  int fd = open("/usr/bin/bash", O_RDONLY);
  if (fd != -1) {
    close(fd);
    printf("PASS\n");
  } else {
    printf("FAIL - errno %d (%s)\n", errno, strerror(errno));
    return FAIL;
  }

  printf("\tOpen nonexistant file - ");
  int fd2 = open("/usr/bin/does-not-exist", O_RDONLY);
  if (fd2 != -1) {
    close(fd2);
    printf("FAIL - unexpectedly opened missing file\n");
    return FAIL;
  } else if (errno != ENOENT) {
    printf("FAIL - errno %d (%s)\n", errno, strerror(errno));
    return FAIL;
  }
  printf("PASS\n");

  printf("\tCreate file - ");
  int fd3 = open("/file-doesnt-exist", O_RDWR | O_CREAT, 0666);
  if (fd3 != -1) {
    close(fd3);
    printf("PASS\n");
  } else {
    printf("FAIL - errno %d (%s)\n", errno, strerror(errno));
    return FAIL;
  }

  printf("\tRecycle descriptors - ");
  int fd4 = open("/usr/bin/bash", O_RDWR);
  close(fd4);
  int fd5 = open("/usr/bin/bash", O_RDWR);
  close(fd5);
  if (fd4 == fd5) {
    printf("PASS\n");
  } else {
    printf("FAIL - %d, %d\n", fd4, fd5);
    return FAIL;
  }

  int hahaha = open("/usr/bin/bash", O_RDWR);
  pid_t pid = fork();

  if (pid == -1) {
    close(hahaha);
    printf("FAIL - fork failed\n");
    return FAIL;
  }

  if (pid == 0) {
    close(hahaha);
    int rofl = open("/usr/bin/bash", O_RDWR);

    printf("%d/%d\n", hahaha, rofl);

    close(rofl);
    exit(0);
  }
  close(hahaha);

  printf("Complete\n");

  while (1)
    ;

  return PASS;
}

int main(int argc, char** argv) {
  printf("argc: %d, argv[0]: %s, &optind: %p\n", argc, argv[0], (void*)&optind);
  while (getopt(argc, argv, "h?ABC:DEFHIKLNOQ:RST:UVWY:abcdefgijklmo:pr:s:tvwxz") != -1) {
    printf("bleh\n");
  }
  printf("optind: %d\n", optind);
  printf("Syscall test starting...\n");

  if (test_open() == PASS)
    return EXIT_SUCCESS;
  else
    return EXIT_FAILURE;
}
