#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

struct race {
  int fd;
  size_t page;
  volatile int gate;
  volatile int ready[2];
  volatile int done[2];
  int mapping_race;
  int seal_result, seal_error;
  ssize_t write_result;
  int operation_error;
  void* mapping;
};
static void* operation(void* argument) {
  struct race* race = argument;
  __atomic_store_n(&race->ready[0], 1, __ATOMIC_RELEASE);
  if (!mf_wait(&race->gate, 5000)) {
    errno = 0;
    if (race->mapping_race)
      race->mapping = mmap(NULL, race->page, PROT_READ | PROT_WRITE, MAP_SHARED, race->fd, 0);
    else
      race->write_result = pwrite(race->fd, "b", 1, 0);
    race->operation_error = errno;
  } else
    race->operation_error = ETIMEDOUT;
  __atomic_store_n(&race->done[0], 1, __ATOMIC_RELEASE);
  return NULL;
}
static void* sealer(void* argument) {
  struct race* race = argument;
  __atomic_store_n(&race->ready[1], 1, __ATOMIC_RELEASE);
  if (!mf_wait(&race->gate, 5000)) {
    errno = 0;
    race->seal_result = fcntl(race->fd, F_ADD_SEALS, F_SEAL_WRITE);
    race->seal_error = errno;
  } else
    race->seal_error = ETIMEDOUT;
  __atomic_store_n(&race->done[1], 1, __ATOMIC_RELEASE);
  return NULL;
}
static int run_race(int mapping_race, unsigned iteration) {
  int failed = 0, started = 0;
  pthread_t threads[2];
  struct race race = {.fd = -1,
                      .page = sysconf(_SC_PAGESIZE),
                      .mapping_race = mapping_race,
                      .seal_result = -2,
                      .write_result = -2,
                      .mapping = MAP_FAILED};
  CHECK((race.fd = mf_make(race.page)) >= 0 && pwrite(race.fd, "a", 1, 0) == 1);
  CHECK(!pthread_create(&threads[0], NULL, operation, &race));
  started = 1;
  CHECK(!pthread_create(&threads[1], NULL, sealer, &race));
  started = 2;
  CHECK(!mf_wait(&race.ready[0], 5000) && !mf_wait(&race.ready[1], 5000));
  __atomic_store_n(&race.gate, 1, __ATOMIC_RELEASE);
  CHECK(!mf_wait(&race.done[0], 5000) && !mf_wait(&race.done[1], 5000));
  if (mapping_race) {
    if (race.mapping == MAP_FAILED) {
      CHECK(race.operation_error == EPERM && race.seal_result == 0);
    } else {
      /* The mapping stays published until both results have been observed. */
      CHECK(race.seal_result == -1 && race.seal_error == EBUSY);
      CHECK(fcntl(race.fd, F_GET_SEALS) == 0);
      CHECK(!munmap(race.mapping, race.page));
      race.mapping = MAP_FAILED;
      CHECK(!fcntl(race.fd, F_ADD_SEALS, F_SEAL_WRITE));
    }
  } else {
    CHECK(race.seal_result == 0);
    CHECK(race.write_result == 1 || (race.write_result == -1 && race.operation_error == EPERM));
    CHECK(!mf_contents(race.fd, 0, race.write_result == 1 ? "b" : "a", 1));
  }
  CHECK(fcntl(race.fd, F_GET_SEALS) == F_SEAL_WRITE);
  CHECK(pwrite(race.fd, "c", 1, 0) == -1 && errno == EPERM);
  CHECK(mmap(NULL, race.page, PROT_READ | PROT_WRITE, MAP_SHARED, race.fd, 0) == MAP_FAILED &&
        errno == EPERM);
  CHECK(!mf_contents(race.fd, 0, !mapping_race && race.write_result == 1 ? "b" : "a", 1));
out:
  __atomic_store_n(&race.gate, 1, __ATOMIC_RELEASE);
  for (int n = 0; n < started; ++n) {
    if (mf_wait(&race.done[n], 5000)) {
      fprintf(stderr, "MEMFD-CONTRACT: race worker=%d failed to finish\n", n);
      _exit(60);
    }
    pthread_join(threads[n], NULL);
  }
  if (failed)
    fprintf(stderr, "MEMFD-CONTRACT: race=%s iteration=%u seal=%d/%d mapping=%p write=%ld/%d\n",
            mapping_race ? "mmap" : "write", iteration, race.seal_result, race.seal_error,
            race.mapping, (long)race.write_result, race.operation_error);
  if (race.mapping != MAP_FAILED)
    munmap(race.mapping, race.page);
  if (race.fd >= 0)
    close(race.fd);
  return failed;
}
int memfd_races(void) {
  for (unsigned n = 0; n < 8; ++n)
    if (run_race(1, n) || run_race(0, n))
      return 1;
  return 0;
}
