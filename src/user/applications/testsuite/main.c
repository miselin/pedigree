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
extern void test_fs();
extern int exec_shebang_child(int argc, char* argv[]);
extern void test_exec_shebang(const char* program);
extern int process_exec_signal_child(void);
extern void test_process(const char* program);

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

  printf("Running tests...\n");

  // Add calls to test functions here...
  test_mprotect();
  test_exec_shebang(argv[0]);
  test_process(argv[0]);
  test_fs();

  printf("Tests complete!\n");
  return 0;
}
