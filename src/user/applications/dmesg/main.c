/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/syscall.h>

int main(int argc, char* argv[]) {
  if (argc != 1) {
    int help = argc == 2 && !strcmp(argv[1], "--help");
    fprintf(help ? stdout : stderr, "Usage: dmesg\nPrint the kernel log.\n");
    return help ? 0 : 1;
  }
  long capacity = syscall(SYS_syslog, 10, NULL, 0);
  if (capacity < 0) {
    perror("dmesg: log size");
    return 1;
  }
  char* buffer = malloc(capacity ? (size_t)capacity : 1);
  if (!buffer) {
    perror("dmesg");
    return 1;
  }
  long count = syscall(SYS_syslog, 3, buffer, capacity);
  if (count < 0) {
    perror("dmesg: read log");
    free(buffer);
    return 1;
  }
  int failed = fwrite(buffer, 1, (size_t)count, stdout) != (size_t)count;
  free(buffer);
  if (fflush(stdout) || ferror(stdout))
    failed = 1;
  if (failed)
    perror("dmesg");
  return failed;
}
