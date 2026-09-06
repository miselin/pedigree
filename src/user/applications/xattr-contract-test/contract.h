#ifndef XATTR_CONTRACT_H
#define XATTR_CONTRACT_H

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

enum { XA_MEMFD, XA_RAMFS, XA_EXT2 };
enum { XA_PATH, XA_LINK, XA_FD };
struct xa_file {
  int fd;
  int backend;
  int directory;
  char path[192];
};

extern size_t xa_page;
extern size_t xa_block;
int64_t xa_now(void);
int xa_reap(pid_t child, int milliseconds);
int xa_send(int fd, char byte);
int xa_receive(int fd, char expected);
int xa_read_all(int fd, void* buffer, size_t length);
int xa_write_all(int fd, const void* buffer, size_t length);
int xa_create(struct xa_file* file, int backend, int directory);
int xa_open_alias(const struct xa_file* file);
void xa_close(struct xa_file* file);
int xa_set(const struct xa_file* file, int how, const char* name, const void* value, size_t size,
           int flags);
ssize_t xa_get(const struct xa_file* file, int how, const char* name, void* value, size_t size);
ssize_t xa_list(const struct xa_file* file, int how, char* names, size_t size);
int xa_remove(const struct xa_file* file, int how, const char* name);
int xa_value(const struct xa_file* file, int how, const char* name, const void* value, size_t size);
int xa_names(const struct xa_file* file, int how, const char* const* names, size_t count);
int xa_unprivileged(uid_t uid, gid_t gid, const gid_t* groups, size_t count);
int xa_values(void);
int xa_admission(void);
int xa_permissions(void);
int xa_lifetime(void);
int xa_concurrency(void);
int xa_events(void);
int xa_ext2(void);
int xa_persistence(const char* base, const char* token, int write_stage);

#endif
