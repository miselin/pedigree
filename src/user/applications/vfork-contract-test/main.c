/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#if !defined(__x86_64__)
int main(void) {
  puts("VFORK-CONTRACT: SKIP x86-64 syscall fixture");
  return 77;
}
#else

#define CHECK(expression)                                                          \
  do {                                                                             \
    if (!(expression)) {                                                           \
      fprintf(stderr, "VFORK-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      return 1;                                                                    \
    }                                                                              \
  } while (0)

extern char** environ;
static char self[PATH_MAX];
static volatile int shared_value;
static int child_ready, parent_returned, observer_failure;
static char* child_mapping;
static int observer_status;
static int gate[2], parent_tid, child_tid, seen_tid;
static unsigned long clone_flags;
static int clear_sighand;
static __thread int tls_value;

struct clone3_args {
  uint64_t flags, pidfd, child_tid, parent_tid, exit_signal;
  uint64_t stack, stack_size, tls, set_tid, set_tid_size, cgroup;
};
_Static_assert(sizeof(struct clone3_args) == 88, "Linux clone3 argument layout");

#define CLEAR_SIGHAND (1ULL << 32)
#define VFORK_FLAGS (CLONE_VM | CLONE_VFORK)
#define TID_FLAGS (CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)
#define CLONE3_SYSCALL 435

/* The child cannot return through a C syscall wrapper after replacing its stack. */
extern long clone3_start(int (*fn)(void*), void* arg, struct clone3_args* args, size_t size);
extern long raw_clone_start(int (*fn)(void*), void* arg, struct clone3_args* args);
extern int tls_child(void* expected);
__asm__(
    ".text\n"
    ".global raw_clone_start\n"
    ".type raw_clone_start,@function\n"
    "raw_clone_start:\n"
    "push %r12\n"
    "push %r13\n"
    "mov %rdi,%r12\n"
    "mov %rsi,%r13\n"
    "mov %rdx,%rax\n"
    "mov 0(%rax),%rdi\n"
    "or 32(%rax),%rdi\n"
    "mov 40(%rax),%rsi\n"
    "add 48(%rax),%rsi\n"
    "and $-16,%rsi\n"
    "mov 24(%rax),%rdx\n"
    "mov 16(%rax),%r10\n"
    "mov 56(%rax),%r8\n"
    "mov $56,%eax\n"
    "syscall\n"
    "test %rax,%rax\n"
    "jnz 1f\n"
    "xor %ebp,%ebp\n"
    "mov %r13,%rdi\n"
    "call *%r12\n"
    "mov %eax,%edi\n"
    "mov $60,%eax\n"
    "syscall\n"
    "ud2\n"
    "1: pop %r13\n"
    "pop %r12\n"
    "ret\n"
    ".size raw_clone_start,.-raw_clone_start\n"
    ".global clone3_start\n"
    ".type clone3_start,@function\n"
    "clone3_start:\n"
    "push %r12\n"
    "push %r13\n"
    "mov %rdi,%r12\n"
    "mov %rsi,%r13\n"
    "mov %rdx,%rdi\n"
    "mov %rcx,%rsi\n"
    "mov $435,%eax\n"
    "syscall\n"
    "test %rax,%rax\n"
    "jnz 1f\n"
    "xor %ebp,%ebp\n"
    "mov %r13,%rdi\n"
    "call *%r12\n"
    "mov %eax,%edi\n"
    "mov $60,%eax\n"
    "syscall\n"
    "ud2\n"
    "1: pop %r13\n"
    "pop %r12\n"
    "ret\n"
    ".size clone3_start,.-clone3_start\n"
    /* Read FS without touching libc or assuming the supplied TLS is a pthread. */
    ".global tls_child\n"
    ".type tls_child,@function\n"
    "tls_child:\n"
    "mov %rdi,%r8\n"
    "sub $8,%rsp\n"
    "mov %rsp,%rsi\n"
    "mov $0x1003,%edi\n"
    "mov $158,%eax\n"
    "syscall\n"
    "test %rax,%rax\n"
    "jnz 2f\n"
    "cmp %r8,(%rsp)\n"
    "jne 2f\n"
    "mov $27,%eax\n"
    "jmp 3f\n"
    "2: mov $85,%eax\n"
    "3: add $8,%rsp\n"
    "ret\n"
    ".size tls_child,.-tls_child\n");

static long start_raw_clone(int (*fn)(void*), void* arg, struct clone3_args* args, size_t size) {
  long child = size ? clone3_start(fn, arg, args, size) : raw_clone_start(fn, arg, args);
  if (child < 0) {
    errno = -child;
    fprintf(stderr, "VFORK-CONTRACT: %s flags=%#llx size=%zu errno=%d\n",
            size ? "clone3" : "raw clone", (unsigned long long)args->flags, size, errno);
    return -1;
  }
  return child;
}

static void pause_briefly(void) {
  const struct timespec delay = {0, 30000000};
  syscall(SYS_nanosleep, &delay, NULL);
}

static int exited(pid_t child, int code) {
  int status;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  CHECK(result == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == code);
  return 0;
}

static int repeated_exit(void) {
  for (int iteration = 0; iteration < 12; ++iteration) {
    shared_value = 0;
    pid_t child = vfork();
    CHECK(child >= 0);
    if (!child) {
      shared_value = 1;
      pause_briefly();
      shared_value = 2;
      _exit(17);
    }
    CHECK(shared_value == 2);
    CHECK(exited(child, 17) == 0);
  }
  return 0;
}

static int private_fork(void) {
  shared_value = 11;
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    shared_value = 12;
    _exit(20);
  }
  CHECK(exited(child, 20) == 0 && shared_value == 11);
  return 0;
}

static int private_clone_child(void* ignored) {
  (void)ignored;
  if (child_tid != syscall(SYS_getpid))
    return 86;
  shared_value = 12;
  return 29;
}

static int private_clone(void) {
  char* stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(stack != MAP_FAILED);
  for (int api = 0; api < 2; ++api) {
    parent_tid = child_tid = -37;
    shared_value = 11;
    struct clone3_args args = {.flags = TID_FLAGS,
                               .child_tid = (uintptr_t)&child_tid,
                               .parent_tid = (uintptr_t)&parent_tid,
                               .exit_signal = SIGCHLD,
                               .stack = (uintptr_t)stack,
                               .stack_size = 65536};
    long child = start_raw_clone(private_clone_child, NULL, &args, api ? sizeof(args) : 0);
    CHECK(child > 0 && exited(child, 29) == 0);
    if (parent_tid != child || child_tid != -37 || shared_value != 11)
      fprintf(stderr, "VFORK-CONTRACT: private clone api=%s ptid=%d ctid=%d shared=%d\n",
              api ? "clone3" : "raw-clone", parent_tid, child_tid, shared_value);
    CHECK(parent_tid == child && child_tid == -37 && shared_value == 11);
  }
  CHECK(munmap(stack, 65536) == 0);
  puts("VFORK-CONTRACT: ordinary clone/clone3 modifier isolation PASS");
  return 0;
}

static int failed_exec(void) {
  char* arguments[] = {"missing", NULL};
  shared_value = 0;
  pid_t child = vfork();
  CHECK(child >= 0);
  if (!child) {
    if (syscall(SYS_execve, "/does-not-exist/vfork-contract", arguments, environ) != -1)
      _exit(81);
    shared_value = 3;
    pause_briefly();
    shared_value = 4;
    _exit(18);
  }
  CHECK(shared_value == 4);
  CHECK(exited(child, 18) == 0);
  return 0;
}

static int successful_exec(void) {
  int gate[2];
  CHECK(pipe(gate) == 0);
  char descriptor[24];
  snprintf(descriptor, sizeof(descriptor), "%d", gate[0]);
  char* arguments[] = {self, "--exec-child", descriptor, NULL};
  shared_value = 0;
  pid_t child = vfork();
  CHECK(child >= 0);
  if (!child) {
    shared_value = 5;
    syscall(SYS_execve, self, arguments, environ);
    _exit(82);
  }
  CHECK(shared_value == 5);
  // The replacement image cannot exit until vfork has returned to this write.
  CHECK(write(gate[1], "x", 1) == 1);
  CHECK(exited(child, 0) == 0);
  CHECK(close(gate[0]) == 0 && close(gate[1]) == 0);
  return 0;
}

static int clone_child(void* ignored) {
  (void)ignored;
  int result = 19;
  seen_tid = (int)syscall(SYS_getpid);
  if ((clone_flags & CLONE_CHILD_SETTID) && child_tid != seen_tid)
    result = 86;
  if (clone_flags & CLONE_NEWUTS) {
    if (sethostname("vfork-child", 11))
      result = 87;
  }
  if (clear_sighand) {
    struct sigaction caught, ignored_action;
    if (sigaction(SIGUSR1, NULL, &caught) || sigaction(SIGUSR2, NULL, &ignored_action) ||
        caught.sa_handler != SIG_DFL || ignored_action.sa_handler != SIG_IGN)
      result = 88;
  }
  child_mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (child_mapping == MAP_FAILED)
    result = 83;
  else
    child_mapping[0] = 42;
  shared_value = 6;
  __atomic_store_n(&child_ready, seen_tid, __ATOMIC_RELEASE);
  char byte;
  if (read(gate[0], &byte, 1) != 1 || byte != 'x')
    result = 89;
  shared_value = 7;
  return result;
}

static void* gate_observer(void* ignored) {
  (void)ignored;
  int child;
  while (!(child = __atomic_load_n(&child_ready, __ATOMIC_ACQUIRE)))
    sched_yield();
  if (child < 0)
    return NULL;
  // Give an incorrectly unblocked creator a chance to publish parent_returned.
  pause_briefly();
  if (__atomic_load_n(&parent_returned, __ATOMIC_ACQUIRE))
    observer_failure = 1;
  if (write(gate[1], "x", 1) != 1)
    observer_failure = 2;
  return NULL;
}

static int spawn_clone_case(unsigned long modifiers, size_t clone3_size, int reset_handlers) {
  const size_t stack_size = 65536;
  char* stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(stack != MAP_FAILED);
  if (clone3_size == 96)
    CHECK(mprotect(stack, 4096, PROT_NONE) == 0);
  char hostname[256], after[256];
  CHECK(gethostname(hostname, sizeof(hostname)) == 0);
  CHECK(pipe(gate) == 0);
  shared_value = 0;
  child_mapping = NULL;
  parent_tid = child_tid = -37;
  child_ready = parent_returned = observer_failure = seen_tid = 0;
  clone_flags = VFORK_FLAGS | modifiers;
  clear_sighand = reset_handlers;
  pthread_t observer;
  CHECK(pthread_create(&observer, NULL, gate_observer, NULL) == 0);
  struct {
    struct clone3_args args;
    uint64_t extension;
  } record = {.args = {.flags = clone_flags | (reset_handlers ? CLEAR_SIGHAND : 0),
                       .child_tid = (uintptr_t)&child_tid,
                       .parent_tid = (uintptr_t)&parent_tid,
                       .exit_signal = SIGCHLD,
                       .stack = (uintptr_t)stack,
                       .stack_size = stack_size}};
  long child;
  // Musl's public wrapper rejects TLS/clear-TID modifiers before issuing clone.
  if (clone3_size || (modifiers & (CLONE_CHILD_CLEARTID | CLONE_SETTLS))) {
    child = start_raw_clone(clone_child, NULL, &record.args, clone3_size);
  } else {
    child = clone(clone_child, stack + stack_size, clone_flags | SIGCHLD, NULL, &parent_tid, NULL,
                  &child_tid);
  }
  __atomic_store_n(&parent_returned, 1, __ATOMIC_RELEASE);
  if (child < 0)
    __atomic_store_n(&child_ready, -1, __ATOMIC_RELEASE);
  CHECK(pthread_join(observer, NULL) == 0);
  CHECK(child > 0 && shared_value == 7 && !observer_failure && seen_tid == child);
  CHECK(exited(child, 19) == 0);
  CHECK(parent_tid == ((modifiers & CLONE_PARENT_SETTID) ? child : -37));
  CHECK(
      child_tid ==
      ((modifiers & CLONE_CHILD_CLEARTID) ? 0 : ((modifiers & CLONE_CHILD_SETTID) ? child : -37)));
  CHECK(gethostname(after, sizeof(after)) == 0 && !strcmp(hostname, after));
  CHECK(child_mapping && child_mapping != MAP_FAILED && child_mapping[0] == 42);
  child_mapping[4095] = 43;
  CHECK(munmap(child_mapping, 4096) == 0);
  CHECK(munmap(stack, stack_size) == 0);
  CHECK(close(gate[0]) == 0 && close(gate[1]) == 0);
  return 0;
}

static int spawn_clone(unsigned long modifiers, size_t clone3_size, int reset_handlers) {
  int result = spawn_clone_case(modifiers, clone3_size, reset_handlers);
  const char* api = "libc-clone";
  if (clone3_size)
    api = "clone3";
  else if (modifiers & (CLONE_CHILD_CLEARTID | CLONE_SETTLS))
    api = "raw-clone";
  if (result)
    fprintf(stderr,
            "VFORK-CONTRACT: clone matrix api=%s modifiers=%#lx size=%zu clear_sighand=%d\n", api,
            modifiers, clone3_size, reset_handlers);
  return result;
}

static int clone_tls(void) {
  char* stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(stack != MAP_FAILED);
  uintptr_t tls[16] = {0}, before, after;
  CHECK(syscall(SYS_arch_prctl, 0x1003, &before) == 0);
  struct clone3_args args = {.flags = VFORK_FLAGS | CLONE_SETTLS,
                             .exit_signal = SIGCHLD,
                             .stack = (uintptr_t)stack,
                             .stack_size = 65536,
                             .tls = (uintptr_t)tls};
  long child = start_raw_clone(tls_child, tls, &args, 0);
  CHECK(child > 0 && exited(child, 27) == 0);
  child = start_raw_clone(tls_child, tls, &args, sizeof(args));
  CHECK(child > 0 && exited(child, 27) == 0);
  CHECK(syscall(SYS_arch_prctl, 0x1003, &after) == 0 && before == after);
  CHECK(munmap(stack, 65536) == 0);
  return 0;
}

static void caught_signal(int signal) {
  (void)signal;
}

static int clone_matrix(void) {
  const unsigned long modifiers[] = {
      0,         CLONE_PARENT_SETTID,     CLONE_CHILD_SETTID, CLONE_CHILD_CLEARTID,
      TID_FLAGS, TID_FLAGS | CLONE_NEWUTS};
  for (size_t i = 0; i < sizeof(modifiers) / sizeof(modifiers[0]); ++i)
    CHECK(spawn_clone(modifiers[i], 0, 0) == 0);
  CHECK(spawn_clone(0, 64, 0) == 0);
  CHECK(spawn_clone(TID_FLAGS, 80, 0) == 0);
  CHECK(spawn_clone(TID_FLAGS | CLONE_NEWUTS, 88, 0) == 0);
  CHECK(spawn_clone(0, 96, 0) == 0);
  struct sigaction caught = {.sa_handler = caught_signal}, ignored = {.sa_handler = SIG_IGN};
  struct sigaction old1, old2, after;
  sigemptyset(&caught.sa_mask);
  sigemptyset(&ignored.sa_mask);
  CHECK(sigaction(SIGUSR1, &caught, &old1) == 0 && sigaction(SIGUSR2, &ignored, &old2) == 0);
  CHECK(spawn_clone(0, 88, 1) == 0);
  CHECK(sigaction(SIGUSR1, NULL, &after) == 0 && after.sa_handler == caught_signal);
  CHECK(sigaction(SIGUSR2, NULL, &after) == 0 && after.sa_handler == SIG_IGN);
  CHECK(sigaction(SIGUSR1, &old1, NULL) == 0 && sigaction(SIGUSR2, &old2, NULL) == 0);
  CHECK(clone_tls() == 0);
  puts("VFORK-CONTRACT: clone/clone3 sharing, blocking, TID, TLS and signals PASS");
  return 0;
}

static int rejected_clone3(struct clone3_args* args, size_t size, int expected) {
  errno = 0;
  long result = syscall(CLONE3_SYSCALL, args, size);
  if (!result)
    _exit(90);
  CHECK(result == -1 && errno == expected);
  return 0;
}

static int rejected_clones(void) {
  const unsigned long invalid[] = {CLONE_VM | SIGCHLD,
                                   CLONE_VFORK | SIGCHLD,
                                   VFORK_FLAGS,
                                   VFORK_FLAGS | CLONE_FILES | SIGCHLD,
                                   VFORK_FLAGS | CLONE_FS | SIGCHLD,
                                   VFORK_FLAGS | CLONE_SIGHAND | SIGCHLD};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    errno = 0;
    long result = syscall(SYS_clone, invalid[i], NULL, NULL, NULL, 0);
    if (!result)
      _exit(90);
    CHECK(result == -1 && errno == EINVAL);
  }
  const unsigned long bad_tid_flags[] = {CLONE_PARENT_SETTID, CLONE_CHILD_SETTID};
  for (size_t i = 0; i < 2; ++i) {
    errno = 0;
    long result =
        syscall(SYS_clone, VFORK_FLAGS | SIGCHLD | bad_tid_flags[i], NULL, (void*)1, (void*)1, 0);
    if (!result)
      _exit(90);
    CHECK(result == -1 && errno == EFAULT);
  }
  struct clone3_args valid = {.flags = VFORK_FLAGS, .exit_signal = SIGCHLD};
  struct clone3_args args = valid;
  CHECK(rejected_clone3(&args, 63, EINVAL) == 0);
  CHECK(rejected_clone3(&args, 4097, E2BIG) == 0);
  CHECK(rejected_clone3((void*)1, 88, EFAULT) == 0);
  struct {
    struct clone3_args args;
    uint64_t extra;
  } extended = {valid, 1};
  CHECK(rejected_clone3(&extended.args, sizeof(extended), E2BIG) == 0);
  const uint64_t flags[] = {VFORK_FLAGS | SIGCHLD,
                            VFORK_FLAGS | CLONE_FILES,
                            VFORK_FLAGS | CLONE_FS,
                            VFORK_FLAGS | CLONE_SIGHAND,
                            VFORK_FLAGS | CLEAR_SIGHAND | CLONE_SIGHAND,
                            VFORK_FLAGS | (1ULL << 63),
                            CLONE_VM,
                            CLONE_VFORK};
  for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
    args = valid;
    args.flags = flags[i];
    CHECK(rejected_clone3(&args, 88, EINVAL) == 0);
  }
  args = valid;
  args.exit_signal = 0;
  CHECK(rejected_clone3(&args, 88, EINVAL) == 0);
  args.exit_signal = SIGUSR1;
  CHECK(rejected_clone3(&args, 88, EINVAL) == 0);
  for (int field = 0; field < 4; ++field) {
    args = valid;
    if (field == 0)
      args.pidfd = 1;
    if (field == 1)
      args.set_tid = 1;
    if (field == 2)
      args.set_tid_size = 1;
    if (field == 3)
      args.cgroup = 1;
    CHECK(rejected_clone3(&args, 88, EINVAL) == 0);
  }
  args = valid;
  args.stack_size = 4096;
  CHECK(rejected_clone3(&args, 88, EINVAL) == 0);
  char* pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(pages != MAP_FAILED);
  memcpy(pages + 4096 - 88, &valid, 88);
  CHECK(mprotect(pages + 4096, 4096, PROT_NONE) == 0);
  CHECK(rejected_clone3((void*)(pages + 4096 - 88), 96, EFAULT) == 0);
  CHECK(munmap(pages, 8192) == 0);
  args.stack = UINT64_MAX - 2047;
  CHECK(rejected_clone3(&args, 88, EINVAL) == 0);
  args.stack = 1;
  args.stack_size = 0;
  CHECK(rejected_clone3(&args, 88, EINVAL) == 0);
  args.stack = 0xffff800000000000ULL;
  args.stack_size = 4096;
  CHECK(rejected_clone3(&args, 88, EINVAL) == 0);
  for (size_t i = 0; i < 2; ++i) {
    args = valid;
    args.flags |= bad_tid_flags[i];
    args.parent_tid = args.child_tid = 1;
    CHECK(rejected_clone3(&args, 88, EFAULT) == 0);
  }
  int status;
  CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
  puts("VFORK-CONTRACT: rejected clone/clone3 arguments PASS");
  return 0;
}

struct exec_arguments {
  char* path;
  char** argv;
};

static int clone_exec_child(void* opaque) {
  struct exec_arguments* args = opaque;
  syscall(SYS_execve, args->path, args->argv, environ);
  seen_tid = child_tid;
  return errno == ENOENT && child_tid > 0 ? 18 : 91;
}

static int clone_exec(void) {
  char* stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(stack != MAP_FAILED);
  for (int api = 0; api < 2; ++api) {
    for (int fail = 0; fail < 2; ++fail) {
      CHECK(pipe(gate) == 0);
      char descriptor[24];
      snprintf(descriptor, sizeof(descriptor), "%d", gate[0]);
      char* argv[] = {self, "--exec-child", descriptor, NULL};
      struct exec_arguments exec = {fail ? "/does-not-exist/vfork-contract" : self, argv};
      child_tid = -37;
      seen_tid = 0;
      unsigned long flags = VFORK_FLAGS | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
      struct clone3_args args = {.flags = flags,
                                 .child_tid = (uintptr_t)&child_tid,
                                 .exit_signal = SIGCHLD,
                                 .stack = (uintptr_t)stack,
                                 .stack_size = 65536};
      long child = start_raw_clone(clone_exec_child, &exec, &args, api ? sizeof(args) : 0);
      if (child <= 0 || child_tid != 0)
        fprintf(stderr, "VFORK-CONTRACT: clear-TID exec api=%s failed_exec=%d child=%ld ctid=%d\n",
                api ? "clone3" : "raw-clone", fail, child, child_tid);
      CHECK(child > 0 && child_tid == 0);
      if (fail)
        CHECK(seen_tid == child);
      else
        CHECK(write(gate[1], "x", 1) == 1);
      CHECK(exited(child, fail ? 18 : 0) == 0);
      CHECK(close(gate[0]) == 0 && close(gate[1]) == 0);
    }
  }
  CHECK(munmap(stack, 65536) == 0);
  puts("VFORK-CONTRACT: clone/clone3 clear-TID exec and failed exec PASS");
  return 0;
}

static int* futex_alias;
static int futex_waiting, futex_result, futex_errno;

static void* futex_observer(void* ignored) {
  (void)ignored;
  int tid;
  while (!(tid = __atomic_load_n(&child_ready, __ATOMIC_ACQUIRE)))
    sched_yield();
  if (tid < 0)
    return NULL;
  struct timespec timeout = {2, 0};
  __atomic_store_n(&futex_waiting, 1, __ATOMIC_RELEASE);
  futex_result = syscall(SYS_futex, futex_alias, 0, tid, &timeout, NULL, 0);
  futex_errno = errno;
  return NULL;
}

static int futex_child(void* ignored) {
  (void)ignored;
  __atomic_store_n(&child_ready, (int)syscall(SYS_getpid), __ATOMIC_RELEASE);
  while (!__atomic_load_n(&futex_waiting, __ATOMIC_ACQUIRE))
    sched_yield();
  pause_briefly();
  return 28;
}

static int shared_clear_tid(void) {
  int fd = syscall(SYS_memfd_create, "vfork-ctid", 0);
  CHECK(fd >= 0 && ftruncate(fd, 4096) == 0);
  int* ctid = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  futex_alias = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  char* stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(ctid != MAP_FAILED && futex_alias != MAP_FAILED && stack != MAP_FAILED);
  for (int api = 0; api < 2; ++api) {
    child_ready = futex_waiting = futex_result = futex_errno = 0;
    *ctid = -37;
    pthread_t observer;
    CHECK(pthread_create(&observer, NULL, futex_observer, NULL) == 0);
    unsigned long flags = VFORK_FLAGS | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
    struct clone3_args args = {.flags = flags,
                               .child_tid = (uintptr_t)ctid,
                               .exit_signal = SIGCHLD,
                               .stack = (uintptr_t)stack,
                               .stack_size = 65536};
    long child = start_raw_clone(futex_child, NULL, &args, api ? sizeof(args) : 0);
    if (child < 0)
      __atomic_store_n(&child_ready, -1, __ATOMIC_RELEASE);
    CHECK(pthread_join(observer, NULL) == 0 && child > 0);
    CHECK(exited(child, 28) == 0);
    if (futex_result)
      fprintf(stderr, "VFORK-CONTRACT: shared clear-TID futex api=%s errno=%d\n",
              api ? "clone3" : "raw-clone", futex_errno);
    CHECK(futex_result == 0 && *ctid == 0 && *futex_alias == 0);
  }
  CHECK(munmap(ctid, 4096) == 0 && munmap(futex_alias, 4096) == 0);
  CHECK(munmap(stack, 65536) == 0 && close(fd) == 0);
  puts("VFORK-CONTRACT: shared-file clear-TID wake PASS");
  return 0;
}

typedef int (*spawn_function)(pid_t*, const char*, const posix_spawn_file_actions_t*,
                              const posix_spawnattr_t*, char* const[], char* const[]);

static int spawn_worker(int closed_fd) {
  char byte;
  sigset_t mask;
  struct sigaction action;
  CHECK(read(STDIN_FILENO, &byte, 1) == 1 && byte == 'x');
  CHECK(fcntl(closed_fd, F_GETFD) == -1 && errno == EBADF);
  CHECK((fcntl(100, F_GETFL) & O_ACCMODE) == O_RDONLY && read(100, &byte, 1) == 0);
  CHECK(sigprocmask(SIG_SETMASK, NULL, &mask) == 0 && sigismember(&mask, SIGUSR1) == 1);
  CHECK(sigaction(SIGUSR2, NULL, &action) == 0 && action.sa_handler == SIG_DFL);
  CHECK(getpgrp() == getpid());
  const char* token = getenv("VFORK_SPAWN_TOKEN");
  CHECK(token && !strcmp(token, "child"));
  CHECK(write(STDOUT_FILENO, "p", 1) == 1);
  return 0;
}

static int spawn_success(spawn_function start, const char* path, int use_vfork) {
  int input[2], output[2];
  CHECK(pipe(input) == 0 && pipe(output) == 0);
  int sentinel = fcntl(STDOUT_FILENO, F_DUPFD, 64);
  CHECK(sentinel >= 0 && sentinel != 100);
  char descriptor[24];
  snprintf(descriptor, sizeof(descriptor), "%d", sentinel);
  char* argv[] = {self, "--spawn-child", descriptor, NULL};
  char* env[] = {"VFORK_SPAWN_TOKEN=child", NULL};
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  CHECK(posix_spawn_file_actions_init(&actions) == 0 && posix_spawnattr_init(&attr) == 0);
  CHECK(posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO) == 0);
  CHECK(posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0);
  CHECK(posix_spawn_file_actions_addopen(&actions, 100, "/dev/null", O_RDONLY, 0) == 0);
  const int closing[] = {input[0], input[1], output[0], output[1], sentinel};
  for (size_t i = 0; i < sizeof(closing) / sizeof(closing[0]); ++i)
    CHECK(posix_spawn_file_actions_addclose(&actions, closing[i]) == 0);
  sigset_t mask, defaults, parent_mask, after_mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGUSR2);
  CHECK(sigprocmask(SIG_UNBLOCK, &mask, &parent_mask) == 0);
  struct sigaction ignored = {.sa_handler = SIG_IGN}, parent_action, after_action;
  sigemptyset(&ignored.sa_mask);
  CHECK(sigaction(SIGUSR2, &ignored, &parent_action) == 0);
  pid_t parent_group = getpgrp();
  CHECK(posix_spawnattr_setsigmask(&attr, &mask) == 0);
  CHECK(posix_spawnattr_setsigdefault(&attr, &defaults) == 0);
  CHECK(posix_spawnattr_setpgroup(&attr, 0) == 0);
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_USEVFORK
  if (use_vfork)
    flags |= POSIX_SPAWN_USEVFORK;
#else
  (void)use_vfork;
#endif
  CHECK(posix_spawnattr_setflags(&attr, flags) == 0);
  pid_t child = -1;
  int error = start(&child, path, &actions, &attr, argv, env);
  errno = error;
  CHECK(error == 0 && child > 0);
  CHECK(fcntl(sentinel, F_GETFD) >= 0 && getpgrp() == parent_group);
  CHECK(sigprocmask(SIG_SETMASK, NULL, &after_mask) == 0 && sigismember(&after_mask, SIGUSR1) == 0);
  CHECK(sigaction(SIGUSR2, NULL, &after_action) == 0 && after_action.sa_handler == SIG_IGN);
  // This worker cannot finish until the successful spawn has returned after exec.
  CHECK(write(input[1], "x", 1) == 1 && close(output[1]) == 0);
  char byte;
  CHECK(read(output[0], &byte, 1) == 1 && byte == 'p');
  CHECK(exited(child, 0) == 0);
  CHECK(close(input[0]) == 0 && close(input[1]) == 0 && close(output[0]) == 0);
  CHECK(close(sentinel) == 0);
  CHECK(posix_spawn_file_actions_destroy(&actions) == 0 && posix_spawnattr_destroy(&attr) == 0);
  CHECK(sigaction(SIGUSR2, &parent_action, NULL) == 0);
  CHECK(sigprocmask(SIG_SETMASK, &parent_mask, NULL) == 0);
  return 0;
}

static int spawn_failures(spawn_function start, const char* path, const char* missing) {
  char* argv[] = {self, "--unused", NULL};
  for (int iteration = 0; iteration < 2; ++iteration) {
    for (int failure = 0; failure < 3; ++failure) {
      posix_spawn_file_actions_t actions;
      CHECK(posix_spawn_file_actions_init(&actions) == 0);
      int invalid = fcntl(STDOUT_FILENO, F_DUPFD, 64);
      CHECK(invalid >= 0 && close(invalid) == 0);
      if (failure == 1)
        CHECK(posix_spawn_file_actions_addopen(&actions, 100, "/does-not-exist/spawn-input",
                                               O_RDONLY, 0) == 0);
      if (failure == 2)
        CHECK(posix_spawn_file_actions_adddup2(&actions, invalid, STDIN_FILENO) == 0);
      pid_t child = -1;
      int error = start(&child, failure ? path : missing, &actions, NULL, argv, environ);
      errno = error;
      CHECK(error == (failure == 2 ? EBADF : ENOENT));
      CHECK(posix_spawn_file_actions_destroy(&actions) == 0);
      int status;
      CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    }
  }
  return 0;
}

static int spawn_contracts(void) {
  CHECK(spawn_success(posix_spawn, self, 1) == 0);
  CHECK(spawn_failures(posix_spawn, self, "/does-not-exist/spawn-image") == 0);
  puts("VFORK-CONTRACT: posix_spawn file actions, attributes and failures PASS");
  char directory[PATH_MAX + 32];
  const char* name = strrchr(self, '/');
  CHECK(name);
  snprintf(directory, sizeof(directory), "/does-not-exist:%.*s", (int)(name - self), self);
  const char* path = getenv("PATH");
  char* saved_path = path ? strdup(path) : NULL;
  CHECK(!path || saved_path);
  CHECK(setenv("PATH", directory, 1) == 0);
  CHECK(spawn_success(posix_spawnp, name + 1, 0) == 0);
  CHECK(spawn_failures(posix_spawnp, name + 1, "vfork-contract-missing-image") == 0);
  CHECK(saved_path ? setenv("PATH", saved_path, 1) == 0 : unsetenv("PATH") == 0);
  free(saved_path);
  puts("VFORK-CONTRACT: posix_spawnp PATH lookup, actions and failures PASS");
  int status = system("exit 7");
  CHECK(status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 7);
  FILE* pipe = popen("printf 'vfork-wrapper\\n'", "r");
  CHECK(pipe);
  char output[64];
  CHECK(fgets(output, sizeof(output), pipe) && !strcmp(output, "vfork-wrapper\n"));
  status = pclose(pipe);
  CHECK(status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  puts("VFORK-CONTRACT: system and popen PASS");
  return 0;
}

static void* pthread_worker(void* ignored) {
  (void)ignored;
  int initial = tls_value;
  tls_value = 20;
  errno = EDOM;
  shared_value = 99;
  return (void*)(uintptr_t)(initial != 0);
}

static int pthread_control(void) {
  pthread_t thread;
  tls_value = 10;
  shared_value = 0;
  errno = EINTR;
  CHECK(pthread_create(&thread, NULL, pthread_worker, NULL) == 0);
  void* result;
  CHECK(pthread_join(thread, &result) == 0);
  CHECK(!result && tls_value == 10 && errno == EINTR && shared_value == 99);
  return 0;
}

static void* kill_observer(void* ignored) {
  (void)ignored;
  int child;
  while (!(child = __atomic_load_n(&child_ready, __ATOMIC_ACQUIRE)))
    sched_yield();
  pause_briefly();
  if (__atomic_load_n(&parent_returned, __ATOMIC_ACQUIRE))
    observer_failure = 1;
  if (kill(child, SIGKILL))
    observer_failure = 2;
  if (waitpid(child, &observer_status, 0) != child)
    observer_failure = 3;
  return NULL;
}

static int signal_exit(void) {
  pthread_t observer;
  child_ready = parent_returned = observer_failure = 0;
  CHECK(pthread_create(&observer, NULL, kill_observer, NULL) == 0);
  pid_t child = vfork();
  CHECK(child >= 0);
  if (!child) {
    __atomic_store_n(&child_ready, (int)syscall(SYS_getpid), __ATOMIC_RELEASE);
    for (;;)
      syscall(SYS_pause);
  }
  __atomic_store_n(&parent_returned, 1, __ATOMIC_RELEASE);
  CHECK(pthread_join(observer, NULL) == 0 && !observer_failure);
  CHECK(WIFSIGNALED(observer_status) && WTERMSIG(observer_status) == SIGKILL);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && !strcmp(argv[1], "--exec-child")) {
    char byte;
    return read(atoi(argv[2]), &byte, 1) == 1 && byte == 'x' ? 0 : 84;
  }
  if (argc == 3 && !strcmp(argv[1], "--spawn-child"))
    return spawn_worker(atoi(argv[2]));
  alarm(90);
  ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
  CHECK(length > 0 && length < (ssize_t)sizeof(self));
  self[length] = 0;
  CHECK(private_fork() == 0);
  CHECK(private_clone() == 0);
  CHECK(pthread_control() == 0);
  puts("VFORK-CONTRACT: ordinary fork and pthread controls PASS");
  CHECK(repeated_exit() == 0);
  CHECK(failed_exec() == 0);
  CHECK(successful_exec() == 0);
  CHECK(signal_exit() == 0);
  puts("VFORK-CONTRACT: vfork exit, exec, failed exec and kill/reap PASS");
  CHECK(clone_matrix() == 0);
  CHECK(clone_exec() == 0);
  CHECK(shared_clear_tid() == 0);
  CHECK(rejected_clones() == 0);
  CHECK(spawn_contracts() == 0);
  puts("VFORK-CONTRACT: PASS");
  return 0;
}
#endif
