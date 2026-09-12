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

#include <stdio.h>
#include <string.h>

#include <sys/reboot.h>

static int usage(const char* program) {
  fprintf(stderr, "Usage: %s [-r|-h|-P] [now]\n", program);
  return 1;
}

int main(int argc, char** argv) {
  const char* program = strrchr(argv[0], '/');
  program = program ? program + 1 : argv[0];
  int command = !strcmp(program, "reboot") ? RB_AUTOBOOT
                : !strcmp(program, "halt") ? RB_HALT_SYSTEM
                                           : RB_POWER_OFF;
  int selected = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "now") && i == argc - 1)
      continue;
    if (selected++)
      return usage(program);
    if (!strcmp(argv[i], "-r"))
      command = RB_AUTOBOOT;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "-P"))
      command = RB_POWER_OFF;
    else
      return usage(program);
  }
  // The kernel checks permission and flushes storage before terminal teardown.
  if (reboot(command) < 0)
    perror(program);
  return 1;
}
