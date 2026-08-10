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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/klog.h>
#include <sys/stat.h>

const char* g_FilesToPreload[] = {
    "/usr/bin/bash",
    "/usr/bin/ls",
    "/usr/bin/cat",
    "/usr/bin/make",
    "/usr/bin/gcc",
    "/usr/bin/ld",
    "/usr/bin/as",
    "/usr/bin/cpp",
    "/usr/lib/pedigree/gcc/libexec/gcc/x86_64-pedigree/4.8.2/cc1",
    "/usr/lib/pedigree/gcc/libexec/gcc/x86_64-pedigree/4.8.2/cc1plus",
    "/usr/lib/pedigree/gcc/libexec/gcc/x86_64-pedigree/4.8.2/collect2",
    "/usr/lib/libc.so",
    "/usr/lib/libm.so",
    "/usr/lib/libpthread.so",
    "/usr/lib/libpedigree-c.so",
    "/usr/lib/libpedigree.so",
    "/usr/lib/libui.so",
    "/usr/lib/libgmp.so",
    "/usr/lib/libmpfr.so",
    "/usr/lib/libmpc.so",
    0};

#define BLOCK_READ_SIZE 0x100000

int main(int argc, char** argv) {
  pid_t f = fork();
  if (f != 0) {
    klog(LOG_INFO, "preloadd: forked, daemon is pid %d...", f);
    return 0;
  }

  klog(LOG_INFO, "preloadd: daemon starting...");

  size_t n = 0;
  const char* s = g_FilesToPreload[n++];
  char* buf = (char*)malloc(BLOCK_READ_SIZE);
  do {
    struct stat st;
    int e = stat(s, &st);
    if (e == 0) {
      klog(LOG_INFO, "preloadd: preloading %s...", s);
      FILE* fp = fopen(s, "rb");
      for (off_t off = 0; off < st.st_size; off += BLOCK_READ_SIZE)
        fread(buf, BLOCK_READ_SIZE, 1, fp);
      fclose(fp);
      klog(LOG_INFO, "preloadd: preloading %s complete!", s);
    } else {
      klog(LOG_INFO, "preloadd: %s probably does not exist", s);
    }
    s = g_FilesToPreload[n++];
  } while (s);

  free(buf);

  return 0;
}
