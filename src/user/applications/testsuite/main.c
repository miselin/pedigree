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

#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void test_mprotect();
extern void test_mmap();
extern void test_prctl(void);
extern void test_resource_accounting(void);
extern void test_fs();
extern void test_dup3(void);
extern void test_epoll_pty(void);
extern int exec_shebang_child(int argc, char* argv[]);
extern void test_exec_shebang(const char* program);
extern int process_exec_signal_child(void);
extern void test_linux_signal_frame(void);
extern void test_process(const char* program);
extern void test_posix_spawn(const char* program);
extern void test_scm_rights(void);
extern void test_scm_rights_stream(void);
extern void test_unix_stream_interruption(void);

static jmp_buf buf;

void fail() __attribute__((noreturn));

void fail() {
  longjmp(buf, 1);
}

int main(int argc, char* argv[]) {
  if (argc == 2 && !strcmp(argv[1], "--exec-signal-child"))
    return process_exec_signal_child();
  if (argc > 1 && !strncmp(argv[1], "--exec-shebang-", sizeof("--exec-shebang-") - 1))
    return exec_shebang_child(argc, argv);

  if (setjmp(buf) == 1) {
    printf("FAILED\n");
    return 1;
  }

  if (argc == 2 && !strcmp(argv[1], "--scm-rights")) {
    test_scm_rights();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--scm-rights-stream")) {
    test_scm_rights_stream();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--unix-stream-interruption")) {
    test_unix_stream_interruption();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--posix-spawn")) {
    test_posix_spawn(argv[0]);
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--dup3")) {
    test_dup3();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--epoll-pty")) {
    test_epoll_pty();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--mmap")) {
    test_mmap();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--prctl")) {
    test_prctl();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--resource")) {
    test_resource_accounting();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--signal-frame")) {
    test_linux_signal_frame();
    return 0;
  }

  printf("Running tests...\n");

  // Add calls to test functions here...
  test_mmap();
  test_mprotect();
  test_prctl();
  test_resource_accounting();
  test_exec_shebang(argv[0]);
  test_process(argv[0]);
  test_dup3();
  test_epoll_pty();
  test_scm_rights();
  test_scm_rights_stream();
  test_unix_stream_interruption();
  test_fs();

  printf("Tests complete!\n");
  return 0;
}
