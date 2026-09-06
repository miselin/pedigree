#ifndef REMAP_FILE_PAGES_CONTRACT_H
#define REMAP_FILE_PAGES_CONTRACT_H

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

enum { RP_MEMFD, RP_RAMFS, RP_EXT2 };
struct rp_file {
  int fd;
  int backend;
  char path[128];
};

extern size_t rp_page;
int rp_create(struct rp_file* file, int backend);
int rp_open_alias(const struct rp_file* file);
void rp_close(struct rp_file* file);
unsigned char rp_pattern(size_t offset);
int rp_matches(const volatile unsigned char* bytes, size_t offset, size_t length, int zero);
int rp_contents(int fd, size_t offset, size_t length, int zero);
int rp_size(int fd, size_t size);
int rp_resident(void* address, size_t pages, unsigned bits, const char* stage);
int rp_reap(pid_t child, int milliseconds);
int rp_send(int fd, char byte);
int rp_receive(int fd, char byte);
int rp_fault(void* address, int signal, int write_access);
int rp_limit(size_t bytes);
int rp_unprivileged(void);
int rp_offsets(void);
int rp_admission(void);
int rp_lifetime(void);
int rp_locks(void);
int rp_resize(void);

#endif
