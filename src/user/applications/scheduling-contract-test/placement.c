#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/syscall.h>

static int self_placement(void) {
  int failed = 0;
  sc_tls = 0xb1759e2;
  for (int round = 0; round < 3 * sc_count; ++round) {
    int cpu = sc_cpus[round % sc_count];
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    long result = -1;
    errno = 271;
    CHECK(sc_register_syscall(SYS_sched_setaffinity, 0, sizeof(mask), (long)&mask, &result) == 0);
    CHECK(result == 0 && sc_tls == 0xb1759e2 && errno == 271);
    CHECK(sc_sample(cpu) == 0 && sc_mask(0, cpu) == 0);
    sched_yield();
    CHECK(sc_sample(cpu) == 0 && sc_tls == 0xb1759e2);
  }
out:
  if (sched_setaffinity(0, sizeof(sc_allowed), &sc_allowed))
    failed = 1;
  return failed;
}
struct running_peer {
  atomic_uint ready, command, ack, stop;
  pid_t tid;
  unsigned cpu, node, identity;
  int failed;
};
static void* running_entry(void* argument) {
  struct running_peer* peer = argument;
  peer->tid = (pid_t)syscall(SYS_gettid);
  sc_tls = peer->identity;
  errno = (int)peer->identity;
  atomic_store_explicit(&peer->ready, 1, memory_order_release);
  unsigned acknowledged = 0;
  int damaged = 0;
  while (!atomic_load_explicit(&peer->stop, memory_order_acquire)) {
    unsigned generation = atomic_load_explicit(&peer->command, memory_order_acquire);
    unsigned cpu = UINT32_MAX, node = UINT32_MAX;
    long result = -1;
    damaged |= sc_register_syscall(SYS_getcpu, (long)&cpu, (long)&node, 0, &result) != 0 ||
               result || node || sc_tls != peer->identity || errno != (int)peer->identity;
    if (generation != acknowledged) {
      peer->cpu = cpu;
      peer->node = node;
      peer->failed = damaged;
      acknowledged = generation;
      atomic_store_explicit(&peer->ack, generation, memory_order_release);
    }
  }
  return (void*)(intptr_t)damaged;
}
static int peer_placement(void) {
  int failed = 0, created = 0;
  struct running_peer peers[2] = {{.identity = 731}, {.identity = 829}};
  pthread_t threads[2];
  for (int i = 0; i < 2; ++i) {
    CHECK(pthread_create(&threads[i], NULL, running_entry, &peers[i]) == 0);
    ++created;
    CHECK(sc_wait(&peers[i].ready, 1) == 0);
    CHECK(peers[i].tid > 0 && peers[i].tid != (pid_t)syscall(SYS_gettid));
  }
  for (unsigned round = 1; round <= (unsigned)(6 * sc_count); ++round) {
    int expected[2] = {sc_cpus[round % (unsigned)sc_count],
                       sc_cpus[(unsigned)(sc_count - 1) - round % (unsigned)sc_count]};
    for (int i = 0; i < 2; ++i) {
      CHECK(sc_pin(peers[i].tid, expected[i]) == 0);
      CHECK(sc_mask(peers[i].tid, expected[i]) == 0);
      // This command follows kernel acknowledgement, so the sample cannot
      // accidentally describe the old placement before the setter completed.
      atomic_store_explicit(&peers[i].command, round, memory_order_release);
    }
    for (int i = 0; i < 2; ++i) {
      CHECK(sc_wait(&peers[i].ack, round) == 0);
      CHECK(!peers[i].failed && peers[i].cpu == (unsigned)expected[i] && !peers[i].node);
    }
  }
  for (int i = 0; i < 2; ++i) {
    cpu_set_t got;
    CHECK(pthread_setaffinity_np(threads[i], sizeof(sc_allowed), &sc_allowed) == 0);
    CHECK(pthread_getaffinity_np(threads[i], sizeof(got), &got) == 0 &&
          CPU_EQUAL(&got, &sc_allowed));
  }
out:
  for (int i = 0; i < created; ++i)
    atomic_store_explicit(&peers[i].stop, 1, memory_order_release);
  for (int i = 0; i < created; ++i) {
    void* result;
    if (pthread_join(threads[i], &result) || result)
      failed = 1;
  }
  return failed;
}
static int idle_balancing(void) {
  if (sc_count < 2) {
    return 0;
  }

  int failed = 0, created = 0, balanced = 0;
  struct running_peer peers[3] = {{.identity = 937}, {.identity = 1031}, {.identity = 1129}};
  pthread_t threads[3];
  const int source = sc_cpus[0];
  unsigned sampled_cpus[3] = {(unsigned)source, (unsigned)source, (unsigned)source};
  CHECK(sc_pin(0, source) == 0);
  for (int i = 0; i < 3; ++i) {
    CHECK(pthread_create(&threads[i], NULL, running_entry, &peers[i]) == 0);
    ++created;
    CHECK(sc_wait(&peers[i].ready, 1) == 0);
    CHECK(sc_pin(peers[i].tid, source) == 0 && sc_mask(peers[i].tid, source) == 0);
    atomic_store_explicit(&peers[i].command, 1, memory_order_release);
    CHECK(sc_wait(&peers[i].ack, 1) == 0);
    CHECK(!peers[i].failed && peers[i].cpu == (unsigned)source && !peers[i].node);
  }

  // Widening retains the current CPU. Only automatic balancing can move
  // these busy peers; the third peer must remain on its singleton mask.
  for (int i = 0; i < 2; ++i) {
    cpu_set_t got;
    CHECK(pthread_setaffinity_np(threads[i], sizeof(sc_allowed), &sc_allowed) == 0);
    CHECK(pthread_getaffinity_np(threads[i], sizeof(got), &got) == 0 &&
          CPU_EQUAL(&got, &sc_allowed));
  }
  int64_t now = sc_now();
  CHECK(now >= 0);
  const int64_t deadline = now + INT64_C(8000000000);
  unsigned generation = 2;
  for (int i = 0; i < 3; ++i) {
    atomic_store_explicit(&peers[i].command, generation, memory_order_release);
  }
  while (now >= 0 && now < deadline) {
    int sampled = 1;
    for (int i = 0; i < 3; ++i) {
      sampled &= atomic_load_explicit(&peers[i].ack, memory_order_acquire) == generation;
    }
    if (sampled) {
      int distinct = 0;
      for (int i = 0; i < 3; ++i) {
        CHECK(!peers[i].failed && !peers[i].node && peers[i].cpu < CPU_SETSIZE &&
              CPU_ISSET(peers[i].cpu, &sc_allowed));
        sampled_cpus[i] = peers[i].cpu;
        int previous = 0;
        while (previous < i && peers[previous].cpu != peers[i].cpu) {
          ++previous;
        }
        distinct += previous == i;
      }
      CHECK(peers[2].cpu == (unsigned)source);
      if (distinct == (sc_count < 3 ? sc_count : 3)) {
        balanced = 1;
        break;
      }
      ++generation;
      for (int i = 0; i < 3; ++i) {
        atomic_store_explicit(&peers[i].command, generation, memory_order_release);
      }
    }
    sched_yield();
    now = sc_now();
  }
  if (!balanced) {
    fprintf(stderr, "idle balancing timeout: peer CPUs=%u,%u,%u source=%d\n", sampled_cpus[0],
            sampled_cpus[1], sampled_cpus[2], source);
  }
  CHECK(balanced && sc_mask(peers[2].tid, source) == 0);
out:
  for (int i = 0; i < created; ++i) {
    atomic_store_explicit(&peers[i].stop, 1, memory_order_release);
  }
  for (int i = 0; i < created; ++i) {
    void* result;
    if (pthread_join(threads[i], &result) || result) {
      failed = 1;
    }
  }
  if (sched_setaffinity(0, sizeof(sc_allowed), &sc_allowed)) {
    failed = 1;
  }
  return failed;
}
int sc_placement(void) {
  return self_placement() || peer_placement() || idle_balancing();
}
