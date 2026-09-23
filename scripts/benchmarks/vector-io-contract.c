#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>

static const char* current_case = "setup";
static unsigned char source[8192];
static unsigned char output[9][8194];
static int pedigree;

static void require(int condition, const char* check) {
  if (!condition) {
    printf("VECTORIO check=%s case=%s FAIL errno=%d\nVECTORIO FAIL END\n", check,
           current_case, errno);
    exit(1);
  }
}

static void passed(void) {
  printf("VECTORIO check=%s PASS\n", current_case);
}

static void reset_file(int fd, const void* bytes, size_t length) {
  require(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0, "reset-file");
  if (length)
    require(write(fd, bytes, length) == (ssize_t)length, "seed-file");
  require(lseek(fd, 0, SEEK_SET) == 0, "rewind-file");
}

static void verify_vectors(const struct iovec* vectors, int count, size_t total) {
  size_t offset = 0;
  for (int i = 0; i < count; ++i) {
    const size_t length = vectors[i].iov_len;
    require(!memcmp(output[i] + 1, source + offset, length), "scatter-bytes");
    require(output[i][0] == 0xa5 && output[i][length + 1] == 0xa5, "scatter-guards");
    offset += length;
  }
  require(offset == total, "scatter-total");
}

static void roundtrip(int fd, int count, size_t total) {
  char label[64];
  snprintf(label, sizeof(label), "vectors-%d-bytes-%zu", count, total);
  current_case = label;
  struct iovec writes[9], reads[9];
  size_t offset = 0;
  for (int i = 0; i < count; ++i) {
    size_t length = total / count + ((size_t)i < total % count);
    writes[i] = (struct iovec){source + offset, length};
    reads[i] = (struct iovec){output[i] + 1, length};
    offset += length;
  }
  reset_file(fd, NULL, 0);
  require(writev(fd, writes, count) == (ssize_t)total, "writev-count");
  require(lseek(fd, 0, SEEK_CUR) == (off_t)total, "writev-offset");
  require(lseek(fd, 0, SEEK_SET) == 0, "readv-rewind");
  memset(output, 0xa5, sizeof(output));
  require(readv(fd, reads, count) == (ssize_t)total, "readv-count");
  require(lseek(fd, 0, SEEK_CUR) == (off_t)total, "readv-offset");
  verify_vectors(reads, count, total);

  require(ftruncate(fd, 0) == 0 && lseek(fd, 7, SEEK_SET) == 7, "positional-setup");
  require(pwritev(fd, writes, count, 13) == (ssize_t)total, "pwritev-count");
  require(lseek(fd, 0, SEEK_CUR) == 7, "pwritev-preserves-offset");
  memset(output, 0xa5, sizeof(output));
  require(preadv(fd, reads, count, 13) == (ssize_t)total, "preadv-count");
  require(lseek(fd, 0, SEEK_CUR) == 7, "preadv-preserves-offset");
  verify_vectors(reads, count, total);
  struct stat status;
  require(fstat(fd, &status) == 0 && status.st_size == (off_t)(13 + total),
          "positional-file-size");
  passed();
}

static void empty_vectors(int fd, void* inaccessible) {
  current_case = "empty-and-zero-length-vectors";
  reset_file(fd, source, 16);
  require(readv(fd, inaccessible, 0) == 0 && writev(fd, inaccessible, 0) == 0,
          "zero-count-ignores-array");
  struct iovec vectors[] = {{inaccessible, 0}, {inaccessible, 0}};
  require(readv(fd, vectors, 2) == 0 && writev(fd, vectors, 2) == 0 &&
              lseek(fd, 0, SEEK_CUR) == 0,
          "zero-length-ignores-payload");
  struct iovec mixed[] = {{inaccessible, 0}, {source, 4}, {inaccessible, 0}};
  require(writev(fd, mixed, 3) == 4 && lseek(fd, 0, SEEK_CUR) == 4, "mixed-empty-write");
  require(lseek(fd, 0, SEEK_SET) == 0, "mixed-empty-rewind");
  mixed[1].iov_base = output[0];
  require(readv(fd, mixed, 3) == 4 && !memcmp(output[0], source, 4) &&
              lseek(fd, 0, SEEK_CUR) == 4,
          "mixed-empty-read");
  passed();
}

static void positional_snapshot(int fd, int count) {
  current_case = count == 2 ? "preadv-snapshot-2" : "preadv-snapshot-9";
  reset_file(fd, source, sizeof(struct iovec) + 4);
  require(lseek(fd, 7, SEEK_SET) == 7, "snapshot-start-offset");
  struct iovec vectors[9] = {{0}};
  vectors[0] = (struct iovec){&vectors[count - 1], sizeof(struct iovec)};
  vectors[count - 1] = (struct iovec){output[0], 4};
  // The first output overwrites a later user descriptor after its snapshot.
  require(preadv(fd, vectors, count, 0) == (ssize_t)(sizeof(struct iovec) + 4) &&
              !memcmp(&vectors[count - 1], source, sizeof(struct iovec)) &&
              !memcmp(output[0], source + sizeof(struct iovec), 4) &&
              lseek(fd, 0, SEEK_CUR) == 7,
          "snapshot-survives-descriptor-overwrite");
  passed();
}

static void faults(int fd, void* inaccessible) {
  reset_file(fd, source, 16);
  const long operations[] = {SYS_readv, SYS_writev};
  for (int i = 0; i < 2; ++i) {
    current_case = i ? "writev-invalid-input" : "readv-invalid-input";
    errno = 0;
    require(syscall(operations[i], fd, inaccessible, 1025) == -1 && errno == EINVAL,
            "over-limit-count");
    errno = 0;
    require(syscall(operations[i], fd, inaccessible, 1) == -1 && errno == EFAULT,
            "inaccessible-vector-array");
    struct iovec bad = {inaccessible, 4};
    errno = 0;
    require(syscall(operations[i], fd, &bad, 1) == -1 && errno == EFAULT,
            "inaccessible-payload");
    require(lseek(fd, 0, SEEK_CUR) == 0, "fault-preserves-offset");
    require(pread(fd, output[0], 16, 0) == 16 && !memcmp(output[0], source, 16),
            "fault-preserves-file");
    passed();
  }

  current_case = "readv-prefix-before-fault";
  memset(output[0], 0xa5, sizeof(output[0]));
  struct iovec read_fault[] = {{output[0] + 1, 4}, {inaccessible, 4}};
  require(readv(fd, read_fault, 2) == 4 && !memcmp(output[0] + 1, source, 4) &&
              output[0][0] == 0xa5 && output[0][5] == 0xa5 &&
              lseek(fd, 0, SEEK_CUR) == 4,
          "partial-read-offset-and-bytes");
  require(read(fd, output[1], 4) == 4 && !memcmp(output[1], source + 4, 4),
          "partial-read-following-bytes");
  passed();

  const size_t prefixes[] = {4, 2048, 8191};
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
    size_t prefix = prefixes[i];
    char label[64];
    snprintf(label, sizeof(label), "writev-gather-fault-prefix-%zu", prefix);
    current_case = label;
    reset_file(fd, source, sizeof(source));
    memset(output[2], 0x7e, sizeof(output[2]));
    struct iovec write_fault[] = {{output[2], prefix}, {inaccessible, i ? 1 : 4}};
    errno = 0;
    ssize_t result = writev(fd, write_fault, 2);
    int error = errno;
    off_t position = lseek(fd, 0, SEEK_CUR);
    struct stat status;
    require(pread(fd, output[0], sizeof(source), 0) == (ssize_t)sizeof(source) &&
                fstat(fd, &status) == 0 && status.st_size == (off_t)sizeof(source),
            "fault-write-inspect");
    // Linux may commit a prefix; Pedigree gathers all of these requests first.
    if (!pedigree && result == (ssize_t)prefix) {
      require(position == (off_t)prefix && !memcmp(output[0], output[2], prefix) &&
                  !memcmp(output[0] + prefix, source + prefix, sizeof(source) - prefix),
              "linux-partial-write-state");
      printf("VECTORIO detail=linux-valid-prefix bytes=%zu\n", prefix);
    } else {
      require(result == -1 && error == EFAULT && position == 0 &&
                  !memcmp(output[0], source, sizeof(source)),
              "gather-fault-is-uncommitted");
    }
    passed();
  }
}

static void short_eof(int fd) {
  current_case = "short-eof-scatter";
  reset_file(fd, "abcde", 5);
  memset(output, 0xa5, sizeof(output));
  struct iovec vectors[] = {{output[0] + 1, 3}, {output[1] + 1, 8}};
  require(readv(fd, vectors, 2) == 5 && lseek(fd, 0, SEEK_CUR) == 5 &&
              !memcmp(output[0] + 1, "abc", 3) && !memcmp(output[1] + 1, "de", 2),
          "eof-count-bytes-offset");
  require(output[0][0] == 0xa5 && output[0][4] == 0xa5 && output[1][0] == 0xa5,
          "eof-leading-guards");
  for (int i = 3; i <= 9; ++i)
    require(output[1][i] == 0xa5, "eof-unused-destination");
  require(readv(fd, vectors, 2) == 0 && lseek(fd, 0, SEEK_CUR) == 5, "eof-repeat");
  passed();
}

static void shared_offset(int fd) {
  current_case = "dup-vector-shared-offset";
  reset_file(fd, source, 16);
  int alias = dup(fd);
  require(alias >= 0 && lseek(alias, 3, SEEK_SET) == 3 && lseek(fd, 0, SEEK_CUR) == 3,
          "alias-seek-shared");
  struct iovec reads[] = {{output[0], 2}, {output[1], 3}};
  require(readv(fd, reads, 2) == 5 && !memcmp(output[0], source + 3, 2) &&
              !memcmp(output[1], source + 5, 3) && lseek(alias, 0, SEEK_CUR) == 8,
          "alias-readv-shared");
  struct iovec writes[] = {{source + 20, 2}, {source + 22, 3}};
  require(writev(alias, writes, 2) == 5 && lseek(fd, 0, SEEK_CUR) == 13 &&
              lseek(alias, 0, SEEK_CUR) == 13 && pread(fd, output[2], 5, 8) == 5 &&
              !memcmp(output[2], source + 20, 5),
          "alias-writev-shared");
  require(close(alias) == 0, "alias-close");
  passed();
}

enum { RaceRounds = 128, GenerationBytes = 64 };

static void race_gate(pthread_barrier_t* gate) {
  int result = pthread_barrier_wait(gate);
  require(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD, "race-gate");
}

struct replacement_race {
  pthread_barrier_t gate;
  int sources[2];
  int target;
  int error;
};

static void* replace_descriptors(void* argument) {
  struct replacement_race* race = argument;
  for (int round = 0; round < RaceRounds; ++round) {
    race_gate(&race->gate);
    for (int change = 0; change < 8; ++change) {
      if (dup2(race->sources[(round + change) % 2], race->target) != race->target)
        race->error = errno ? errno : EIO;
      sched_yield();
    }
  }
  return NULL;
}

static int coherent_generation(const unsigned char* bytes) {
  if (bytes[0] != 0x36 && bytes[0] != 0xa9)
    return 0;
  for (size_t i = 1; i < GenerationBytes; ++i)
    if (bytes[i] != bytes[0])
      return 0;
  return 1;
}

static void concurrent_replacement(void) {
  current_case = "dup2-concurrent-vector-generation";
  struct replacement_race race = {.sources = {-1, -1}, .target = -1};
  const char* paths[] = {"generation-a", "generation-b"};
  unsigned char seed[RaceRounds * GenerationBytes];
  for (int i = 0; i < 2; ++i) {
    int writer = open(paths[i], O_CREAT | O_EXCL | O_WRONLY, 0600);
    memset(seed, i ? 0xa9 : 0x36, sizeof(seed));
    require(writer >= 0 && write(writer, seed, sizeof(seed)) == (ssize_t)sizeof(seed) &&
                close(writer) == 0,
            "generation-seed");
    race.sources[i] = open(paths[i], O_RDONLY);
    require(race.sources[i] >= 0 && unlink(paths[i]) == 0, "generation-open-immutable");
  }
  race.target = dup(race.sources[0]);
  require(race.target >= 0 && pthread_barrier_init(&race.gate, NULL, 2) == 0,
          "generation-setup");
  pthread_t writer;
  require(pthread_create(&writer, NULL, replace_descriptors, &race) == 0,
          "generation-thread");
  int error = 0;
  for (int round = 0; round < RaceRounds; ++round) {
    race_gate(&race.gate);
    unsigned char bytes[GenerationBytes];
    struct iovec vectors[] = {{bytes, 17}, {bytes + 17, sizeof(bytes) - 17}};
    memset(bytes, 0, sizeof(bytes));
    if (preadv(race.target, vectors, 2, 0) != (ssize_t)sizeof(bytes) ||
        !coherent_generation(bytes))
      error = 1;
    sched_yield();
    memset(bytes, 0, sizeof(bytes));
    // Either source OFD can advance, but no source can consume more than the
    // total number of reads. Its immutable seed therefore cannot reach EOF.
    if (readv(race.target, vectors, 2) != (ssize_t)sizeof(bytes) ||
        !coherent_generation(bytes))
      error = 1;
  }
  require(pthread_join(writer, NULL) == 0 && pthread_barrier_destroy(&race.gate) == 0,
          "generation-join");
  require(close(race.target) == 0 && close(race.sources[0]) == 0 &&
              close(race.sources[1]) == 0,
          "generation-close");
  require(!error && !race.error, "atomic-replacement-keeps-complete-generation");
  passed();
}

struct offset_record {
  uint32_t number;
  uint32_t check;
};

struct offset_race {
  pthread_barrier_t* gate;
  int fd;
  int error;
  struct offset_record records[RaceRounds];
};

static void* read_shared_offset(void* argument) {
  struct offset_race* race = argument;
  for (int round = 0; round < RaceRounds; ++round) {
    race_gate(race->gate);
    unsigned char* bytes = (unsigned char*)&race->records[round];
    struct iovec vectors[] = {{bytes, 3}, {bytes + 3, sizeof(struct offset_record) - 3}};
    if (readv(race->fd, vectors, 2) != (ssize_t)sizeof(struct offset_record))
      race->error = 1;
    sched_yield();
  }
  return NULL;
}

static void concurrent_shared_offset(int fd) {
  current_case = "dup-concurrent-vector-shared-offset";
  struct offset_record seed[2 * RaceRounds];
  for (uint32_t i = 0; i < 2 * RaceRounds; ++i)
    seed[i] = (struct offset_record){i, i ^ UINT32_C(0x9e3779b9)};
  reset_file(fd, seed, sizeof(seed));
  int alias = dup(fd);
  pthread_barrier_t gate;
  require(alias >= 0 && pthread_barrier_init(&gate, NULL, 2) == 0, "offset-race-setup");
  struct offset_race readers[] = {{.gate = &gate, .fd = fd}, {.gate = &gate, .fd = alias}};
  pthread_t worker;
  require(pthread_create(&worker, NULL, read_shared_offset, &readers[1]) == 0,
          "offset-race-thread");
  read_shared_offset(&readers[0]);
  require(pthread_join(worker, NULL) == 0 && pthread_barrier_destroy(&gate) == 0,
          "offset-race-join");
  unsigned char seen[2 * RaceRounds] = {0};
  int error = readers[0].error || readers[1].error;
  for (int reader = 0; reader < 2; ++reader) {
    for (int round = 0; round < RaceRounds; ++round) {
      struct offset_record record = readers[reader].records[round];
      if (record.number >= 2 * RaceRounds ||
          record.check != (record.number ^ UINT32_C(0x9e3779b9))) {
        error = 1;
      } else if (seen[record.number]++) {
        error = 1;
      }
    }
  }
  for (int i = 0; i < 2 * RaceRounds; ++i)
    if (seen[i] != 1)
      error = 1;
  struct offset_record last;
  struct iovec eof = {&last, sizeof(last)};
  require(lseek(fd, 0, SEEK_CUR) == (off_t)sizeof(seed) &&
              lseek(alias, 0, SEEK_CUR) == (off_t)sizeof(seed) && readv(alias, &eof, 1) == 0 &&
              close(alias) == 0,
          "offset-race-final-position");
  require(!error, "shared-offset-consumes-each-record-once");
  passed();
}

static void access_flags(int fixture, void* inaccessible) {
  const int modes[] = {O_RDONLY, O_WRONLY, O_RDWR, O_PATH};
  for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    char label[64];
    snprintf(label, sizeof(label), "immutable-access-flags-%d", modes[i]);
    current_case = label;
    reset_file(fixture, source, 16);
    int fd = open("vectors", modes[i]);
    require(fd >= 0, "access-open");
    int alias = dup(fd);
    require(alias >= 0, "access-dup");
    const int changedMode = modes[i] == O_RDWR ? O_RDONLY : O_RDWR;
    errno = 0;
    const int changed = fcntl(alias, F_SETFL, changedMode | O_APPEND | O_NONBLOCK);
    if (modes[i] == O_PATH) {
      require(changed == -1 && errno == EBADF, "path-rejects-setfl");
    } else {
      require(changed == 0, "access-setfl");
      require((fcntl(fd, F_GETFL) & (O_ACCMODE | O_APPEND | O_NONBLOCK)) ==
                  (modes[i] | O_APPEND | O_NONBLOCK),
              "access-preserved-mutable-flags-shared");
      require(lseek(fd, 0, SEEK_SET) == 0, "access-rewind");
    }

    const int readable = modes[i] == O_RDONLY || modes[i] == O_RDWR;
    const int writable = modes[i] == O_WRONLY || modes[i] == O_RDWR;
    struct iovec vector = {output[0], 1};
    if (readable) {
      require(read(alias, output[0], 1) == 1 && output[0][0] == source[0] &&
                  readv(fd, &vector, 1) == 1 && output[0][0] == source[1],
              "access-read-permitted");
    } else {
      errno = 0;
      require(read(alias, inaccessible, 1) == -1 && errno == EBADF,
              "access-read-denied-before-payload");
      errno = 0;
      require(readv(fd, inaccessible, 1) == -1 && errno == EBADF,
              "access-readv-denied-before-vector");
    }
    vector.iov_base = source + 17;
    if (writable) {
      require(write(alias, source + 16, 1) == 1 && writev(fd, &vector, 1) == 1 &&
                  lseek(alias, 0, SEEK_CUR) == 18 &&
                  pread(fixture, output[0], 2, 16) == 2 &&
                  !memcmp(output[0], source + 16, 2),
              "access-write-permitted-shared-append");
    } else {
      errno = 0;
      require(write(alias, inaccessible, 1) == -1 && errno == EBADF,
              "access-write-denied-before-payload");
      errno = 0;
      require(writev(fd, inaccessible, 1) == -1 && errno == EBADF,
              "access-writev-denied-before-vector");
    }
    if (modes[i] != O_PATH) {
      require(fcntl(fd, F_SETFL, changedMode) == 0 &&
                  (fcntl(alias, F_GETFL) & (O_ACCMODE | O_APPEND | O_NONBLOCK)) == modes[i],
              "access-preserved-cleared-flags-shared");
    }
    require(close(alias) == 0 && close(fd) == 0, "access-close");
    passed();
  }
}

static void cache_mapping_lifetime(int fd, size_t page) {
  current_case = "cache-mapping-lifetime";
  const size_t length = 2 * page;
  unsigned char* seed = malloc(length);
  unsigned char* readback = malloc(length);
  require(seed && readback, "mapping-buffers");
  for (size_t i = 0; i < length; ++i)
    seed[i] = (unsigned char)((i * 37 + 11) ^ ((i / page) * 0x5d));
  reset_file(fd, seed, length);
  require(pread(fd, readback, length, 0) == (ssize_t)length && !memcmp(readback, seed, length),
          "mapping-warm-pages");

  const unsigned char* shared = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
  const unsigned char* alias = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
  unsigned char* private = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  require(shared != MAP_FAILED && alias != MAP_FAILED && private != MAP_FAILED, "mapping-create");
  require(!memcmp(shared, seed, length) && !memcmp(alias, seed, length) &&
              !memcmp(private, seed, length),
          "mapping-warmed-bytes");
  for (size_t offset = 0; offset < length; offset += page) {
    private[offset] ^= 0xff;
    private[offset + page - 1] ^= 0x55;
    require(private[offset] == (unsigned char)(seed[offset] ^ 0xff) &&
                private[offset + page - 1] == (unsigned char)(seed[offset + page - 1] ^ 0x55),
            "mapping-private-write");
  }
  require(!memcmp(shared, seed, length) && !memcmp(alias, seed, length) &&
              pread(fd, readback, length, 0) == (ssize_t)length && !memcmp(readback, seed, length),
          "mapping-private-preserves-file");
  require(munmap(private, length) == 0 && munmap((void*)shared, length) == 0 &&
              munmap((void*)alias, length) == 0,
          "mapping-release-pins");

  struct stat status;
  require(ftruncate(fd, 0) == 0 && fstat(fd, &status) == 0 && status.st_size == 0,
          "mapping-shrink-after-unmap");
  for (size_t i = 0; i < length; ++i)
    seed[i] ^= 0xa7;
  require(pwrite(fd, seed, length, 0) == (ssize_t)length, "mapping-rewrite");
  shared = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
  require(shared != MAP_FAILED && !memcmp(shared, seed, length), "mapping-fresh-bytes");
  require(munmap((void*)shared, length) == 0, "mapping-final-unmap");
  free(readback);
  free(seed);
  passed();
}

static void eventfd_vectors(void) {
  current_case = "eventfd-vector-dispatch";
  int fd = eventfd(0, EFD_NONBLOCK);
  require(fd >= 0, "eventfd-create");
  uint64_t values[] = {5, 9};
  struct iovec writes[] = {{values, 8}, {values + 1, 8}};
  require(writev(fd, writes, 2) == 16, "eventfd-writev-records");
  uint64_t counts[] = {0, UINT64_C(0xa5a5a5a5a5a5a5a5)};
  struct iovec reads[] = {{counts, 3}, {(unsigned char*)counts + 3, 5}};
  require(readv(fd, reads, 2) == 8 && counts[0] == 14 && counts[1] == UINT64_C(0xa5a5a5a5a5a5a5a5),
          "eventfd-readv-split-record");
  errno = 0;
  require(readv(fd, reads, 2) == -1 && errno == EAGAIN, "eventfd-drained");
  // Pedigree eventfd writes consume one complete 8-byte value per nonempty vector.
  struct iovec split[] = {{values, 3}, {(unsigned char*)values + 3, 5}};
  errno = 0;
  require(writev(fd, split, 2) == -1 && errno == EINVAL, "eventfd-writev-short-record");
  errno = 0;
  require(readv(fd, reads, 2) == -1 && errno == EAGAIN, "eventfd-short-write-uncommitted");
  require(close(fd) == 0, "eventfd-close");
  passed();
}

static void timerfd_vectors(void) {
  current_case = "timerfd-vector-dispatch";
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  require(fd >= 0, "timerfd-create");
  struct itimerspec setting = {.it_value = {0, 1000000}};
  require(timerfd_settime(fd, 0, &setting, NULL) == 0, "timerfd-arm");
  struct pollfd ready = {.fd = fd, .events = POLLIN};
  require(poll(&ready, 1, 2000) == 1 && (ready.revents & POLLIN), "timerfd-expired");
  uint64_t counts[] = {0, UINT64_C(0xa5a5a5a5a5a5a5a5)};
  struct iovec vectors[] = {{counts, 3}, {(unsigned char*)counts + 3, 5}};
  require(readv(fd, vectors, 2) == 8 && counts[0] == 1 && counts[1] == UINT64_C(0xa5a5a5a5a5a5a5a5),
          "timerfd-readv-split-record");
  errno = 0;
  require(readv(fd, vectors, 2) == -1 && errno == EAGAIN, "timerfd-drained");
  errno = 0;
  require(writev(fd, vectors, 2) == -1 && errno == EINVAL, "timerfd-writev-rejected");
  require(close(fd) == 0, "timerfd-close");
  passed();
}

static void signalfd_vectors(void) {
  current_case = "signalfd-vector-dispatch";
  sigset_t signals, previous;
  require(sigemptyset(&signals) == 0 && sigaddset(&signals, SIGUSR1) == 0 &&
              sigprocmask(SIG_BLOCK, &signals, &previous) == 0,
          "signalfd-block-signal");
  int fd = signalfd(-1, &signals, SFD_NONBLOCK);
  require(fd >= 0 && sigqueue(getpid(), SIGUSR1, (union sigval){.sival_int = 0x2345}) == 0,
          "signalfd-queue-signal");
  struct signalfd_siginfo record;
  unsigned char bytes[sizeof(record) + 17];
  memset(bytes, 0xa5, sizeof(bytes));
  struct iovec short_record = {bytes, sizeof(record) - 1};
  errno = 0;
  require(readv(fd, &short_record, 1) == -1 && errno == EINVAL, "signalfd-short-record");
  struct iovec vectors[] = {{bytes, 31}, {bytes + 31, sizeof(bytes) - 31}};
  require(readv(fd, vectors, 2) == (ssize_t)sizeof(record), "signalfd-readv-split-record");
  memcpy(&record, bytes, sizeof(record));
  require(record.ssi_signo == SIGUSR1 && record.ssi_code == SI_QUEUE &&
              record.ssi_pid == (unsigned)getpid() && record.ssi_int == 0x2345,
          "signalfd-record-content");
  for (size_t i = sizeof(record); i < sizeof(bytes); ++i)
    require(bytes[i] == 0xa5, "signalfd-partial-record-untouched");
  errno = 0;
  require(readv(fd, vectors, 2) == -1 && errno == EAGAIN, "signalfd-drained");
  errno = 0;
  require(writev(fd, vectors, 2) == -1 && errno == EINVAL, "signalfd-writev-rejected");
  require(close(fd) == 0 && sigprocmask(SIG_SETMASK, &previous, NULL) == 0, "signalfd-cleanup");
  passed();
}

static void pipe_vectors(void) {
  current_case = "pipe-vector-atomic-records";
  int descriptors[2];
  require(pipe(descriptors) == 0, "pipe-create");
  pid_t children[2];
  for (int writer = 0; writer < 2; ++writer) {
    children[writer] = fork();
    require(children[writer] >= 0, "pipe-fork");
    if (!children[writer]) {
      close(descriptors[0]);
      unsigned char first[2048], second[2048];
      memset(first, 'A' + writer, sizeof(first));
      memset(second, 'a' + writer, sizeof(second));
      struct iovec vectors[] = {{first, sizeof(first)}, {second, sizeof(second)}};
      for (int record = 0; record < 8; ++record)
        if (writev(descriptors[1], vectors, 2) != 4096)
          _exit(1);
      close(descriptors[1]);
      _exit(0);
    }
  }
  require(close(descriptors[1]) == 0, "pipe-close-parent-writer");
  unsigned counts[2] = {0};
  unsigned char first[2048], second[2048];
  struct iovec vectors[] = {{first, sizeof(first)}, {second, sizeof(second)}};
  for (int record = 0; record < 16; ++record) {
    require(readv(descriptors[0], vectors, 2) == 4096, "pipe-read-record");
    require(first[0] == 'A' || first[0] == 'B', "pipe-record-writer");
    int writer = first[0] - 'A';
    for (size_t i = 0; i < sizeof(first); ++i)
      require(first[i] == 'A' + writer && second[i] == 'a' + writer,
              "pipe-record-not-interleaved");
    ++counts[writer];
  }
  require(counts[0] == 8 && counts[1] == 8 && readv(descriptors[0], vectors, 2) == 0,
          "pipe-counts-and-eof");
  require(close(descriptors[0]) == 0, "pipe-close-reader");
  for (int writer = 0; writer < 2; ++writer) {
    int status;
    require(waitpid(children[writer], &status, 0) == children[writer] &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "pipe-child-status");
  }
  passed();
}

struct read_protection_race {
  void* buffer;
  size_t length;
  pthread_barrier_t gate;
};

static void* change_read_protection(void* opaque) {
  struct read_protection_race* race = opaque;
  race_gate(&race->gate);
  for (size_t i = 0; i < 32; ++i) {
    require(mprotect(race->buffer, race->length, PROT_NONE) == 0, "read-race-protect");
    sched_yield();
    require(mprotect(race->buffer, race->length, PROT_READ | PROT_WRITE) == 0, "read-race-restore");
  }
  return NULL;
}

static void scalar_writes(size_t page) {
  current_case = "scalar-write-batches";
  const size_t length = 128 * 1024 + 17, capacity = length + page;
  const char* path = pedigree ? "/scalar-write-contract" : "scalar-write-contract";
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  unsigned char* buffer =
      mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char* actual = malloc(capacity);
  require(fd >= 0 && buffer != MAP_FAILED && actual, "write-fixture");
  for (size_t i = 0; i < capacity; ++i)
    buffer[i] = (unsigned char)(i * 37 + (i >> 8));
  require(write(fd, buffer + 1, length) == (ssize_t)length &&
              lseek(fd, 0, SEEK_CUR) == (off_t)length &&
              pread(fd, actual, length, 0) == (ssize_t)length &&
              !memcmp(actual, buffer + 1, length),
          "write-unaligned-multiple-batches");
  require(pwrite(fd, buffer, length, 13) == (ssize_t)length &&
              lseek(fd, 0, SEEK_CUR) == (off_t)length &&
              pread(fd, actual, length, 13) == (ssize_t)length &&
              !memcmp(actual, buffer, length),
          "pwrite-batches-preserve-offset");
  int append = open(path, O_WRONLY | O_APPEND);
  require(append >= 0 && write(append, buffer, length) == (ssize_t)length &&
              lseek(append, 0, SEEK_CUR) == (off_t)(2 * length + 13) &&
              pread(fd, actual, length, length + 13) == (ssize_t)length &&
              !memcmp(actual, buffer, length) && close(append) == 0,
          "append-multiple-batches");
  passed();

  current_case = "scalar-write-fault-progress";
  int notify = inotify_init1(IN_NONBLOCK);
  require(notify >= 0 && inotify_add_watch(notify, path, IN_MODIFY) >= 0, "write-watch");
  char events[4096];
  for (int positional = 0; positional < 2; ++positional) {
    const size_t valid = (positional ? 64 * 1024 : 0) + 2 * page;
    require(ftruncate(fd, 0) == 0 && lseek(fd, 13, SEEK_SET) == 13 &&
                mprotect(buffer + valid, page, PROT_NONE) == 0,
            "write-fault-setup");
    while (read(notify, events, sizeof(events)) > 0) {}
    ssize_t written = positional ? pwrite(fd, buffer, length, 13) : write(fd, buffer, length);
    require(written >= (ssize_t)(valid - page + 1) && written <= (ssize_t)valid &&
                lseek(fd, 0, SEEK_CUR) == (positional ? 13 : 13 + written),
            "write-fault-retains-valid-prefix");
    struct stat status;
    require(fstat(fd, &status) == 0 && status.st_size == 13 + written &&
                pread(fd, actual, written, 13) == written && !memcmp(actual, buffer, written),
            "write-fault-bytes-and-size");
    ssize_t n = read(notify, events, sizeof(events));
    require(n >= (ssize_t)sizeof(struct inotify_event) &&
                (((struct inotify_event*)events)->mask & IN_MODIFY),
            "partial-write-publishes-modification");
    const off_t saved_size = status.st_size;
    const off_t saved_offset = lseek(fd, 0, SEEK_CUR);
    errno = 0;
    written = positional ? pwrite(fd, buffer + valid, page, 13)
                         : write(fd, buffer + valid, page);
    require(written == -1 && errno == EFAULT && fstat(fd, &status) == 0 &&
                status.st_size == saved_size && lseek(fd, 0, SEEK_CUR) == saved_offset,
            "initial-write-fault-uncommitted");
    require(mprotect(buffer + valid, page, PROT_READ | PROT_WRITE) == 0, "write-unprotect");
  }
  require(fsync(fd) == 0 && close(notify) == 0 && close(fd) == 0 && unlink(path) == 0 &&
              munmap(buffer, capacity) == 0,
          "write-cleanup");
  free(actual);
  passed();
}

static void cached_scalar_reads(size_t page) {
  current_case = "cached-scalar-read";
  const size_t length = 256 * 1024 + 17, capacity = length + 2 * page;
  const char* path = pedigree ? "/scalar-read-contract" : "scalar-read-contract";
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  unsigned char* expected = malloc(length);
  unsigned char* buffer =
      mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(fd >= 0 && expected && buffer != MAP_FAILED, "scalar-fixture");
  for (size_t i = 0; i < length; ++i)
    expected[i] = (unsigned char)(i / page * 29 + i % 251);
  reset_file(fd, expected, length);
  require(pread(fd, buffer, length, 0) == (ssize_t)length && !memcmp(buffer, expected, length),
          "scalar-demand-paged-destination");
  memset(buffer, 0xA5, capacity);
  pid_t child = fork();
  require(child >= 0, "scalar-cow-fork");
  if (!child) {
    _exit(pread(fd, buffer, page, 0) == (ssize_t)page && !memcmp(buffer, expected, page) ? 0 : 1);
  }
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
              buffer[0] == 0xA5 && buffer[page - 1] == 0xA5,
          "scalar-cow-preserves-parent");
  require(pread(fd, buffer + 7, length, 0) == (ssize_t)length &&
              !memcmp(buffer + 7, expected, length) && buffer[6] == 0xA5 &&
              buffer[length + 7] == 0xA5 && lseek(fd, 0, SEEK_CUR) == 0,
          "scalar-unaligned-multiple-batches");
  require(lseek(fd, 3, SEEK_SET) == 3 && read(fd, buffer, length) == (ssize_t)length - 3 &&
              !memcmp(buffer, expected + 3, length - 3) && lseek(fd, 0, SEEK_CUR) == (off_t)length,
          "scalar-read-eof-and-offset");
  memset(buffer, 0xA5, capacity);
  require(pread(fd, buffer, 128 * 1024, (off_t)length - 13) == 13 &&
              !memcmp(buffer, expected + length - 13, 13) && buffer[13] == 0xA5,
          "scalar-short-eof-guard");
  require(mprotect(buffer, page, PROT_NONE) == 0 && lseek(fd, 0, SEEK_SET) == 0, "scalar-protect");
  errno = 0;
  require(read(fd, buffer, page) == -1 && errno == EFAULT && lseek(fd, 0, SEEK_CUR) == 0,
          "scalar-fault-preserves-offset");
  require(mprotect(buffer, page, PROT_READ | PROT_WRITE) == 0 &&
              mprotect(buffer + 2 * page, page, PROT_NONE) == 0,
          "scalar-protect-suffix");
  ssize_t prefix = read(fd, buffer, 3 * page);
  require(prefix > 0 && prefix <= (ssize_t)(2 * page) &&
              !memcmp(buffer, expected, (size_t)prefix) && lseek(fd, 0, SEEK_CUR) == prefix,
          "scalar-fault-delivers-prefix");
  require(mprotect(buffer + 2 * page, page, PROT_READ | PROT_WRITE) == 0, "scalar-restore-suffix");
  passed();

  current_case = "cached-scalar-protection-race";
  struct read_protection_race race = {.buffer = buffer, .length = 128 * 1024};
  require(pthread_barrier_init(&race.gate, NULL, 2) == 0, "read-race-gate");
  pthread_t worker;
  require(pthread_create(&worker, NULL, change_read_protection, &race) == 0, "read-race-thread");
  race_gate(&race.gate);
  for (size_t i = 0; i < 64; ++i) {
    errno = 0;
    ssize_t result = pread(fd, buffer, race.length, 0);
    require(result == (ssize_t)race.length || (result == -1 && errno == EFAULT),
            "read-race-result");
  }
  require(pthread_join(worker, NULL) == 0 && pthread_barrier_destroy(&race.gate) == 0,
          "read-race-join");
  passed();

  if (pedigree) {
    current_case = "cached-scalar-file-mapping-alias";
    unsigned char* alias = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    require(alias != MAP_FAILED, "scalar-map-source");
    // Preserve Pedigree's existing request snapshot when the output aliases its source.
    require(pread(fd, alias + page, 128 * 1024, 0) == 128 * 1024 &&
                !memcmp(alias + page, expected, 128 * 1024),
            "scalar-overlapping-source-snapshot");
    require(munmap(alias, length) == 0, "scalar-unmap-source");
    passed();
  }
  require(munmap(buffer, capacity) == 0 && close(fd) == 0 && unlink(path) == 0, "scalar-cleanup");
  free(expected);
}

int main(int argc, char** argv) {
  const int stdio_mode = argc == 2 && !strcmp(argv[1], "--stdio");
  require(argc == 1 || stdio_mode, "arguments");
  if (!stdio_mode) {
    int serial = open("/dev/ttyS0", O_RDWR);
    if (serial < 0 || dup2(serial, 0) < 0 || dup2(serial, 1) < 0 || dup2(serial, 2) < 0)
      return 2;
    if (serial > 2)
      close(serial);
  }
  setvbuf(stdout, NULL, _IONBF, 0);
  struct utsname system;
  require(uname(&system) == 0, "uname");
  pedigree = !strcmp(system.sysname, "Pedigree");
  char directory[80] = "/tmp/vector-io-contract-XXXXXX";
  if (stdio_mode) {
    require(mkdtemp(directory) != NULL, "temporary-directory");
  } else {
    strcpy(directory, "/tmp/vector-io-contract");
    require((mkdir(directory, 0700) == 0 || errno == EEXIST) &&
                mount("none", directory, "ramfs", 0, NULL) == 0,
            "fresh-ramfs");
  }
  require(chdir(directory) == 0, "fixture-directory");
  int fd = open("vectors", O_CREAT | O_EXCL | O_RDWR, 0600);
  require(fd >= 0, "fixture-open");
  for (size_t i = 0; i < sizeof(source); ++i)
    source[i] = (unsigned char)(i * 37 + 11);
  const int counts[] = {1, 2, 8, 9};
  const size_t totals[] = {16, 2048, 2049, 8192};
  for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i)
    for (size_t j = 0; j < sizeof(totals) / sizeof(totals[0]); ++j)
      roundtrip(fd, counts[i], totals[j]);
  positional_snapshot(fd, 2);
  positional_snapshot(fd, 9);
  current_case = "fault-mapping";
  long page = sysconf(_SC_PAGESIZE);
  require(page > 0, "page-size");
  void* inaccessible = mmap(NULL, (size_t)page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(inaccessible != MAP_FAILED, "protected-page");
  empty_vectors(fd, inaccessible);
  faults(fd, inaccessible);
  short_eof(fd);
  shared_offset(fd);
  concurrent_replacement();
  concurrent_shared_offset(fd);
  access_flags(fd, inaccessible);
  cache_mapping_lifetime(fd, (size_t)page);
  scalar_writes((size_t)page);
  cached_scalar_reads((size_t)page);
  // Special descriptor vector writes and errors are Pedigree-specific contracts.
  if (pedigree) {
    eventfd_vectors();
    timerfd_vectors();
    signalfd_vectors();
  } else {
    printf(
        "VECTORIO check=pedigree-special-descriptor-dispatch SKIP platform=%s "
        "reason=pedigree-specific-contracts\n",
        system.sysname);
  }
  pipe_vectors();
  current_case = "cleanup";
  require(munmap(inaccessible, (size_t)page) == 0 && close(fd) == 0 && unlink("vectors") == 0,
          "fixture-cleanup");
  require(chdir("/") == 0 && (!stdio_mode || rmdir(directory) == 0), "directory-cleanup");
  puts("VECTORIO PASS END");
  return 0;
}
