#ifndef FILE_RESIZE_CONTRACT_H
#define FILE_RESIZE_CONTRACT_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/types.h>

#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

enum { FR_MEMFD, FR_RAMFS, FR_EXT2 };
struct fr_file {
  int fd;
  int backend;
  char path[128];
};

extern size_t fr_page;
int fr_create(struct fr_file* file, int backend);
int fr_open_alias(const struct fr_file* file);
void fr_close(struct fr_file* file);
unsigned char fr_pattern(size_t offset);
int fr_matches(const volatile unsigned char* bytes, size_t offset, size_t length, int zero);
int fr_contents(int fd, size_t offset, size_t length, int zero);
int fr_size(int fd, size_t size);
int fr_resident(void* address, size_t pages, unsigned bits, const char* stage);
int fr_send(int fd, char byte);
int fr_receive(int fd, char byte);
int fr_reap(pid_t child, int milliseconds);
int fr_fault(void* address);
int fr_shared(int backend);
int fr_private(int backend);
int fr_seals(void);

#endif
