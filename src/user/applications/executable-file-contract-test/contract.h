#ifndef EXECUTABLE_FILE_CONTRACT_H
#define EXECUTABLE_FILE_CONTRACT_H

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/types.h>

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      fprintf(stderr, "EXECUTABLE-FILE-CONTRACT: FAIL %s:%d: %s errno=%d\n", __FILE__, __LINE__, \
              #condition, errno);                                                                \
      failed = 1;                                                                                \
      goto out;                                                                                  \
    }                                                                                            \
  } while (0)

extern size_t ef_page;
extern char ef_directory[PATH_MAX];
extern char ef_self[PATH_MAX];
extern int ef_ext2;

int ef_create(const char* name, char path[PATH_MAX]);
int ef_copy(const char* source, const char* destination);
int ef_receive(int fd, char expected);
int ef_send(int fd, char value);
int ef_reap(pid_t child);
int ef_mutations(void);
int ef_lifetime(void);
int ef_conflicts(void);
int ef_ordinary(void);
int ef_library(void);
int ef_load(const char* path, int version);

#endif
