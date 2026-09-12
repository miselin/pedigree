/* Copyright (c) 2026, Pedigree Developers. */
#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
  unsigned long long divisor = 1;
  if (argc == 2 && !strcmp(argv[1], "-m")) {
    divisor = 1024;
  } else if (argc != 1 && !(argc == 2 && !strcmp(argv[1], "-k"))) {
    int help = argc == 2 && !strcmp(argv[1], "--help");
    fprintf(help ? stdout : stderr, "Usage: free [-k|-m]\nShow memory in KiB (default) or MiB.\n");
    return help ? 0 : 1;
  }

  FILE* file = fopen("/proc/meminfo", "r");
  if (!file) {
    perror("/proc/meminfo");
    return 1;
  }
  char line[256], key[64], unit[16];
  unsigned long long total = 0, freeKb = 0, value;
  int fields = 0;
  while (fgets(line, sizeof(line), file)) {
    if (sscanf(line, "%63s %llu %15s", key, &value, unit) != 3 || strcmp(unit, "kB"))
      continue;
    if (!strcmp(key, "MemTotal:")) {
      total = value;
      fields |= 1;
    } else if (!strcmp(key, "MemFree:")) {
      freeKb = value;
      fields |= 2;
    }
  }
  int failed = ferror(file);
  fclose(file);
  if (failed || fields != 3 || freeKb > total) {
    fputs("free: invalid memory information\n", stderr);
    return 1;
  }
  printf("%12s %12s %12s %12s\n", "", "total", "used", "free");
  printf("%12s %12llu %12llu %12llu\n", "Mem:", total / divisor, (total - freeKb) / divisor,
         freeKb / divisor);
  if (fflush(stdout) || ferror(stdout)) {
    perror("free");
    return 1;
  }
  return 0;
}
