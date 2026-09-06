#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/utsname.h>

_Static_assert(sizeof(struct utsname) == 390, "Linux utsname ABI");

static int setter_contract(int domain) {
  int failed = 0;
  int (*setter)(const char*, size_t) = domain ? setdomainname : sethostname;
  unsigned char expected[65] = {0};
  char full[65];
  memset(full, domain ? 'd' : 'h', sizeof(full));
  void* mapping =
      mmap(NULL, ns_page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  struct utsname value;
  CHECK(mapping != MAP_FAILED);
  CHECK(mprotect((char*)mapping + ns_page, ns_page, PROT_NONE) == 0);
  CHECK(setter(full, 64) == 0);
  CHECK(uname(&value) == 0);
  memcpy(expected, full, 64);
  CHECK(!memcmp(domain ? value.domainname : value.nodename, expected, 65));
  errno = 0;
  CHECK(setter(full, 65) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(setter(full, SIZE_MAX) == -1 && errno == EINVAL);
  char* edge = (char*)mapping + ns_page - 2;
  edge[0] = 'x';
  edge[1] = 'y';
  errno = 0;
  CHECK(setter(edge, 4) == -1 && errno == EFAULT);
  CHECK(uname(&value) == 0);
  CHECK(!memcmp(domain ? value.domainname : value.nodename, expected, 65));

  const unsigned char binary[] = {'a', 0, 'b', 0xff};
  memset(expected, 0, sizeof(expected));
  memcpy(expected, binary, sizeof(binary));
  CHECK(setter((const char*)binary, sizeof(binary)) == 0);
  CHECK(uname(&value) == 0);
  CHECK(!memcmp(domain ? value.domainname : value.nodename, expected, 65));
  /* musl's size_t argument reaches a Linux signed 32-bit length parameter. */
  CHECK(setter(NULL, (size_t)UINT64_C(0x100000000)) == 0);
  CHECK(setter(full, (size_t)UINT64_C(0x100000002)) == 0);
  memset(expected, 0, sizeof(expected));
  memcpy(expected, full, 2);
  CHECK(uname(&value) == 0);
  CHECK(!memcmp(domain ? value.domainname : value.nodename, expected, 65));
  CHECK(setter(NULL, 0) == 0);
  memset(expected, 0, sizeof(expected));
  CHECK(uname(&value) == 0);
  CHECK(!memcmp(domain ? value.domainname : value.nodename, expected, 65));
out:
  if (mapping != MAP_FAILED)
    munmap(mapping, ns_page * 2);
  return failed;
}

struct name_writer {
  pthread_barrier_t start;
  char first[64], second[64];
  int result;
};
static void* replace_names(void* argument) {
  struct name_writer* state = argument;
  pthread_barrier_wait(&state->start);
  for (int i = 0; i < 128; ++i) {
    if (sethostname(i & 1 ? state->first : state->second, 64)) {
      state->result = 1;
      break;
    }
  }
  return NULL;
}

static int snapshots(void) {
  int failed = 0, initialized = 0, started = 0;
  pthread_t worker;
  struct name_writer state = {0};
  memset(state.first, 'a', sizeof(state.first));
  memset(state.second, 'b', sizeof(state.second));
  CHECK(ns_set("snapshot", "fixed-domain") == 0);
  CHECK(sethostname(state.first, 64) == 0);
  CHECK(pthread_barrier_init(&state.start, NULL, 2) == 0);
  initialized = 1;
  CHECK(pthread_create(&worker, NULL, replace_names, &state) == 0);
  started = 1;
  pthread_barrier_wait(&state.start);
  for (int i = 0; i < 128; ++i) {
    struct utsname value;
    CHECK(uname(&value) == 0);
    CHECK((!memcmp(value.nodename, state.first, 64) || !memcmp(value.nodename, state.second, 64)) &&
          value.nodename[64] == 0);
    CHECK(!strcmp(value.domainname, "fixed-domain"));
  }
out:
  if (started && (pthread_join(worker, NULL) || state.result))
    failed = 1;
  if (initialized)
    pthread_barrier_destroy(&state.start);
  return failed;
}

int ns_names(void) {
  int failed = 0;
  struct utsname before, after;
  unsigned char output[sizeof(struct utsname) + 2];
  void* readonly = MAP_FAILED;
  struct ns_identity first, second;
  CHECK(uname(&before) == 0);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &first) == 0);
  CHECK(unshare(0) == 0);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &second) == 0 && ns_same(first, second));
  CHECK(setter_contract(0) == 0 && setter_contract(1) == 0);
  CHECK(ns_set("alpha", "domain") == 0);
  char host[3] = {'?', '?', '?'}, domain[7] = {0};
  CHECK(gethostname(host, 2) == 0 && host[0] == 'a' && host[1] == 0 && host[2] == '?');
  CHECK(gethostname(host + 2, 0) == 0 && host[2] == '?');
  errno = 0;
  CHECK(getdomainname(domain, 6) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(getdomainname(domain, 0) == -1 && errno == EINVAL);
  CHECK(getdomainname(domain, sizeof(domain)) == 0 && !strcmp(domain, "domain"));
  memset(output, 0xcc, sizeof(output));
  CHECK(uname((struct utsname*)(output + 1)) == 0);
  CHECK(output[0] == 0xcc && output[sizeof(output) - 1] == 0xcc);
  memcpy(&after, output + 1, sizeof(after));
  CHECK(!memcmp(before.sysname, after.sysname, 65) && !memcmp(before.release, after.release, 65) &&
        !memcmp(before.version, after.version, 65) && !memcmp(before.machine, after.machine, 65));
  CHECK(ns_expect("alpha", "domain") == 0);
  readonly = mmap(NULL, ns_page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(readonly != MAP_FAILED);
  errno = 0;
  CHECK(uname(readonly) == -1 && errno == EFAULT);
  CHECK(ns_expect("alpha", "domain") == 0);
  CHECK(snapshots() == 0);
out:
  if (readonly != MAP_FAILED)
    munmap(readonly, ns_page);
  return failed;
}
