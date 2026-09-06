#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/xattr.h>

enum { CREATE, REPLACE, REMOVE, WRITE_MANY, READ_MANY };
struct contender {
  int fd, ready, gate, operation;
  const unsigned char* value;
  size_t length;
  int result, error;
};

static unsigned char short_value[191], long_value[257];

static void* compete(void* opaque) {
  struct contender* item = opaque;
  item->result = -1;
  item->error = EIO;
  if (xa_send(item->ready, 'r') || xa_receive(item->gate, 'g'))
    return NULL;
  if (item->operation == REMOVE) {
    item->result = fremovexattr(item->fd, "user.race");
    item->error = errno;
  } else if (item->operation == CREATE || item->operation == REPLACE) {
    item->result = fsetxattr(item->fd, "user.race", item->value, item->length,
                             item->operation == CREATE ? XATTR_CREATE : XATTR_REPLACE);
    item->error = errno;
  } else {
    item->result = 0;
    item->error = 0;
    for (int n = 0; n < 64; ++n) {
      if (item->operation == WRITE_MANY) {
        const void* value = (n & 1) ? (const void*)short_value : long_value;
        size_t length = (n & 1) ? sizeof(short_value) : sizeof(long_value);
        if (fsetxattr(item->fd, "user.race", value, length, XATTR_REPLACE)) {
          item->result = -1;
          item->error = errno;
          break;
        }
      } else {
        unsigned char buffer[sizeof(long_value) + 1];
        memset(buffer, 0xa5, sizeof(buffer));
        ssize_t length = fgetxattr(item->fd, "user.race", buffer, sizeof(buffer));
        int valid = (length == sizeof(short_value) && !memcmp(buffer, short_value, length)) ||
                    (length == sizeof(long_value) && !memcmp(buffer, long_value, length));
        if (!valid || buffer[length] != 0xa5) {
          item->result = -1;
          item->error = length < 0 ? errno : EIO;
          break;
        }
      }
    }
  }
  return NULL;
}

static int pair(struct contender* first, struct contender* second) {
  int failed = 0, ready[2] = {-1, -1}, gate[2] = {-1, -1}, created = 0;
  pthread_t threads[2];
  struct contender* items[] = {first, second};
  CHECK(!pipe(ready) && !pipe(gate));
  for (int n = 0; n < 2; ++n) {
    items[n]->ready = ready[1];
    items[n]->gate = gate[0];
    CHECK(!pthread_create(&threads[n], NULL, compete, items[n]));
    ++created;
  }
  CHECK(!xa_receive(ready[0], 'r') && !xa_receive(ready[0], 'r'));
  CHECK(!xa_send(gate[1], 'g') && !xa_send(gate[1], 'g'));
out:
  if (gate[1] >= 0) {
    close(gate[1]);
    gate[1] = -1;
  }
  for (int n = 0; n < created; ++n) {
    int result = pthread_join(threads[n], NULL);
    if (result) {
      fprintf(stderr, "XATTR-CONTRACT: contender join=%d\n", result);
      failed = 1;
    }
  }
  for (int n = 0; n < 2; ++n) {
    if (gate[n] >= 0)
      close(gate[n]);
    if (ready[n] >= 0)
      close(ready[n]);
  }
  return failed;
}

static int atomic_updates(int backend) {
  int failed = 0, alias = -1;
  struct xa_file file = {.fd = -1};
  CHECK(!xa_create(&file, backend, 0));
  CHECK((alias = xa_open_alias(&file)) >= 0);
  for (int round = 0; round < 4; ++round) {
    struct contender first = {
        .fd = file.fd, .operation = CREATE, .value = short_value, .length = sizeof(short_value)};
    struct contender second = {
        .fd = alias, .operation = CREATE, .value = long_value, .length = sizeof(long_value)};
    CHECK(!pair(&first, &second));
    CHECK((!first.result && second.result == -1 && second.error == EEXIST) ||
          (!second.result && first.result == -1 && first.error == EEXIST));
    struct contender* winner = !first.result ? &first : &second;
    CHECK(!xa_value(&file, XA_FD, "user.race", winner->value, winner->length));
    first.operation = REPLACE;
    second.operation = REPLACE;
    CHECK(!pair(&first, &second));
    CHECK(!first.result && !second.result);
    unsigned char buffer[sizeof(long_value)];
    ssize_t length = fgetxattr(alias, "user.race", buffer, sizeof(buffer));
    CHECK((length == sizeof(short_value) && !memcmp(buffer, short_value, length)) ||
          (length == sizeof(long_value) && !memcmp(buffer, long_value, length)));
    second.operation = REMOVE;
    CHECK(!pair(&first, &second));
    CHECK(!second.result);
    CHECK(!first.result || (first.result == -1 && first.error == ENODATA));
    CHECK(fgetxattr(file.fd, "user.race", NULL, 0) == -1 && errno == ENODATA);
  }
  CHECK(!fsetxattr(file.fd, "user.race", short_value, sizeof(short_value), XATTR_CREATE));
  struct contender writer = {.fd = file.fd, .operation = WRITE_MANY};
  struct contender reader = {.fd = alias, .operation = READ_MANY};
  CHECK(!pair(&writer, &reader));
  if (writer.result || reader.result)
    fprintf(stderr, "XATTR-CONTRACT: snapshots backend=%d writer=%d/%d reader=%d/%d\n", backend,
            writer.result, writer.error, reader.result, reader.error);
  CHECK(!writer.result && !reader.result);
out:
  if (alias >= 0)
    close(alias);
  xa_close(&file);
  return failed;
}

int xa_concurrency(void) {
  for (size_t n = 0; n < sizeof(short_value); ++n)
    short_value[n] = (unsigned char)(n * 17 + 3);
  for (size_t n = 0; n < sizeof(long_value); ++n)
    long_value[n] = (unsigned char)(n * 31 + 11);
  for (int backend = XA_MEMFD; backend <= XA_EXT2; ++backend)
    if (atomic_updates(backend))
      return 1;
  return 0;
}
