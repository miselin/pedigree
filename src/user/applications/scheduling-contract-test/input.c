#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "contract.h"

/* The SDK library exports these C wrappers without an installed input header. */
extern void pedigree_input_install_callback(void*, uint32_t, uintptr_t);
extern void pedigree_input_remove_callback(void*);
extern int pedigree_event_return(void);

static void input_callback(size_t first, size_t second, uintptr_t* buffer, size_t fourth) {
  (void)first;
  (void)second;
  (void)buffer;
  (void)fourth;
  pedigree_event_return();
}

static int retained_pin(int source, int destination) {
  int failed = 0;
  cpu_set_t actual;
  if (source != destination) {
    errno = 0;
    CHECK(sc_pin(0, destination) == -1 && errno == EOPNOTSUPP);
  }
  CHECK(sched_getaffinity(0, sizeof(actual), &actual) == 0);
  CHECK(CPU_EQUAL(&actual, &sc_allowed));
  CHECK(sc_sample(source) == 0);
out:
  return failed;
}

int sc_input_exec(int argc, char** argv) {
  int failed = 0;
  alarm(15);
  CHECK(argc == 5);
  int source = atoi(argv[2]), destination = atoi(argv[3]), expected = atoi(argv[4]);
  CHECK(source >= 0 && source < CPU_SETSIZE && destination >= 0 && destination < CPU_SETSIZE);
  CHECK(expected > 0 && expected <= CPU_SETSIZE);
  CHECK(sc_init(expected) == 0);
  CHECK(CPU_ISSET(source, &sc_allowed) && CPU_ISSET(destination, &sc_allowed));
  CHECK(sc_tls == 0);
  sc_tls = 0x5176a92b;
  CHECK(sc_pin(0, destination) == 0);
  CHECK(sc_mask(0, destination) == 0 && sc_sample(destination) == 0);
  CHECK(sc_tls == 0x5176a92b);
out:
  return failed;
}

int sc_input(void) {
  int failed = 0;
  const int source = sc_cpus[0], destination = sc_cpus[sc_count - 1];
  CHECK(sc_pin(0, source) == 0);
  CHECK(sc_mask(0, source) == 0 && sc_sample(source) == 0);
  if (source != destination) {
    CHECK(sc_pin(0, destination) == 0 && sc_sample(destination) == 0);
    CHECK(sc_pin(0, source) == 0 && sc_sample(source) == 0);
  }

  pedigree_input_install_callback((void*)input_callback, 1, 0);
  CHECK(sc_pin(0, source) == 0);
  CHECK(sc_mask(0, source) == 0 && sc_sample(source) == 0);
  CHECK(sched_setaffinity(0, sizeof(sc_allowed), &sc_allowed) == 0);
  if (sc_count == 1)
    puts("SCHEDULING-CONTRACT: SKIP input pin exclusion (one allowed CPU)");
  CHECK(retained_pin(source, destination) == 0);

  /* Removal stops producers; this image retains its callback-domain pin. */
  pedigree_input_remove_callback((void*)input_callback);
  CHECK(retained_pin(source, destination) == 0);
  execl("/applications/scheduling-no-such-image", "absent", (char*)NULL);
  CHECK(errno == ENOENT);
  CHECK(retained_pin(source, destination) == 0);

  char from[16], to[16], count[16];
  snprintf(from, sizeof(from), "%d", source);
  snprintf(to, sizeof(to), "%d", destination);
  snprintf(count, sizeof(count), "%d", sc_count);
  execl(SC_APP, SC_APP, "schedule-input-exec", from, to, count, (char*)NULL);
  CHECK(0);
out:
  return failed;
}
