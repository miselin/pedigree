/* Copyright (c) 2026, Pedigree Developers. */
#include <stdio.h>
#include <string.h>

static int printFile(const char* path) {
  FILE* file = fopen(path, "r");
  if (!file) {
    perror(path);
    return 1;
  }
  char buffer[4096];
  size_t count;
  int failed = 0;
  while ((count = fread(buffer, 1, sizeof(buffer), file))) {
    if (fwrite(buffer, 1, count, stdout) != count) {
      failed = 1;
      break;
    }
  }
  if (ferror(file)) {
    perror(path);
    failed = 1;
  }
  fclose(file);
  return failed;
}

int main(int argc, char* argv[]) {
  if (argc != 1) {
    const bool help = argc == 2 && !strcmp(argv[1], "--help");
    fprintf(help ? stdout : stderr,
            "Usage: netconfig\nShow network devices, addresses, and DNS.\n");
    return help ? 0 : 1;
  }
  if (printFile("/proc/net/interfaces"))
    return 1;
  puts("DNS:");
  if (printFile("/proc/resolv.conf"))
    return 1;
  if (fflush(stdout) || ferror(stdout)) {
    perror("netconfig");
    return 1;
  }
  return 0;
}
