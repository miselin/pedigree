/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/wait.h>

#if !defined(__x86_64__)
#error This contract requires the x64 GS-base ABI.
#endif

#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004
#define WORKERS 4
#define ITERATIONS 4096

static _Alignas(64) uint64_t canaries[WORKERS + 2][8];
static struct worker {
  unsigned slot, stage, signals, error, stop;
  int pipe[2];
  uint64_t cpus, apics[4];
} workers[WORKERS];
static _Thread_local volatile uint64_t tls_token;
static _Thread_local uint64_t expected_magic;
static _Thread_local uintptr_t expected_gs;
static _Thread_local int* errno_address;
static _Thread_local int current_slot = -1;
static _Thread_local volatile sig_atomic_t gp_count, gp_error;
static long expected_uid;
static volatile sig_atomic_t child_pid;

static inline long raw6(long number, long a1, long a2, long a3, long a4, long a5, long a6) {
  register long r10 __asm__("r10") = a4;
  register long r8 __asm__("r8") = a5;
  register long r9 __asm__("r9") = a6;
  __asm__ volatile("syscall"
                   : "+a"(number)
                   : "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory", "cc");
  return number;
}

static uint64_t gs_value(void) {
  uint64_t value;
  __asm__ volatile("movq %%gs:0, %0" : "=r"(value) : : "memory");
  return value;
}

static unsigned load(unsigned* value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void require(int valid, const char* operation) {
  if (valid)
    return;
  dprintf(2, "KERNEL-GS-CONTRACT FAIL worker=%d operation=%s errno=%d\n", current_slot, operation,
          errno);
  if (child_pid > 0)
    (void)kill(child_pid, SIGKILL);
  _exit(1);
}

static void timeout_handler(int signal_number) {
  (void)signal_number;
  static const char message[] = "KERNEL-GS-CONTRACT FAIL operation=timeout\n";
  (void)raw6(SYS_write, 2, (long)message, sizeof(message) - 1, 0, 0, 0);
  if (child_pid > 0)
    (void)raw6(SYS_kill, child_pid, SIGKILL, 0, 0, 0, 0);
  _exit(124);
}

static int state_valid(uint64_t token, int error) {
  return gs_value() == expected_magic && tls_token == token && &errno == errno_address &&
         errno == error;
}

static uintptr_t get_gs(void) {
  uintptr_t base = UINTPTR_MAX;
  require(raw6(SYS_arch_prctl, ARCH_GET_GS, (long)&base, 0, 0, 0, 0) == 0, "get-gs");
  return base;
}

static void install_gs(unsigned slot, uint64_t token) {
  expected_magic = UINT64_C(0x35be6809ac142700) + slot;
  canaries[slot][0] = expected_magic;
  expected_gs = (uintptr_t)canaries[slot];
  errno_address = &errno;
  tls_token = token;
  errno = EDOM;
  require(raw6(SYS_arch_prctl, ARCH_SET_GS, (long)expected_gs, 0, 0, 0, 0) == 0,
          "nonzero-gs-required");
  require(state_valid(token, EDOM), "direct-gs-after-set");
  require(get_gs() == expected_gs && state_valid(token, EDOM), "get-gs-roundtrip");
}

static void user_signal(int signal_number) {
  const int saved_errno = errno;
  const uint64_t saved_token = tls_token;
  const uint64_t token = saved_token ^ UINT64_C(0xb438501d3e76fa29);
  unsigned error = signal_number != SIGUSR1 || !state_valid(saved_token, saved_errno);
  tls_token = token;
  errno = EOVERFLOW;
  for (unsigned i = 0; i < 4; ++i) {
    if (raw6(SYS_getuid, 0, 0, 0, 0, 0, 0) != expected_uid || !state_valid(token, EOVERFLOW))
      error = 1;
  }
  if (raw6(SYS_sched_yield, 0, 0, 0, 0, 0, 0) != 0 || !state_valid(token, EOVERFLOW))
    error = 1;
  tls_token = saved_token;
  errno = saved_errno;
  if (current_slot < 0 || current_slot >= WORKERS)
    _exit(2);
  if (error)
    __atomic_store_n(&workers[current_slot].error, error, __ATOMIC_RELEASE);
  __atomic_add_fetch(&workers[current_slot].signals, 1, __ATOMIC_RELEASE);
}

extern const unsigned char kernel_gs_user_gp_instruction[], kernel_gs_user_gp_resume[];

__attribute__((noinline)) static void trigger_user_gp(void) {
  __asm__ volatile(
      ".global kernel_gs_user_gp_instruction\n"
      "kernel_gs_user_gp_instruction:\n"
      "cli\n"
      ".global kernel_gs_user_gp_resume\n"
      "kernel_gs_user_gp_resume:\n"
      :
      :
      : "memory", "cc");
}

static void user_gp_signal(int signal_number, siginfo_t* info, void* saved_context) {
  ucontext_t* context = saved_context;
  if (signal_number != SIGBUS || !info || info->si_signo != SIGBUS || !context ||
      (uintptr_t)context->uc_mcontext.gregs[REG_RIP] != (uintptr_t)kernel_gs_user_gp_instruction) {
    static const char message[] = "KERNEL-GS-CONTRACT FAIL operation=unexpected-gp-signal\n";
    (void)raw6(SYS_write, 2, (long)message, sizeof(message) - 1, 0, 0, 0);
    _exit(3);
  }

  const int saved_errno = errno;
  const uint64_t saved_token = tls_token;
  const uint64_t token = saved_token ^ UINT64_C(0x163a8fe4297b50cd);
  int error = !state_valid(saved_token, saved_errno) || gp_count != 0;
  tls_token = token;
  errno = EOVERFLOW;
  if (raw6(SYS_getuid, 0, 0, 0, 0, 0, 0) != expected_uid || !state_valid(token, EOVERFLOW))
    error = 1;
  if (raw6(SYS_sched_yield, 0, 0, 0, 0, 0, 0) != 0 || !state_valid(token, EOVERFLOW))
    error = 1;

  // Pedigree maps user #GP to SIGBUS. CLI is one byte; resume after the fault.
  context->uc_mcontext.gregs[REG_RIP] += 1;
  if ((uintptr_t)context->uc_mcontext.gregs[REG_RIP] != (uintptr_t)kernel_gs_user_gp_resume)
    error = 1;
  tls_token = saved_token;
  errno = saved_errno;
  gp_error = error;
  gp_count = gp_count + 1;
}

static void user_gp_contract(uint64_t token) {
  errno = EDOM;
  require(state_valid(token, EDOM), "user-gp-before-fault");
  trigger_user_gp();
  require(gp_count == 1 && !gp_error, "user-gp-handler");
  require(state_valid(token, EDOM), "user-gp-direct-return-state");
  require(get_gs() == expected_gs && state_valid(token, EDOM), "user-gp-return-base");
  printf("KERNEL-GS-CONTRACT PASS phase=user-gp worker=%d\n", current_slot);
}

static void user_gs_selector_contract(uint64_t token) {
  errno = EDOM;
  require(state_valid(token, EDOM), "selector-before-reset");
  // Pedigree's flat user data descriptor has base zero; avoid null-selector quirks.
  __asm__ volatile("mov $0x23, %%eax\n\tmov %%ax, %%gs" : : : "rax", "memory");
  // Do not dereference GS while its base is zero.
  require(get_gs() == 0 && tls_token == token && &errno == errno_address && errno == EDOM,
          "selector-resets-base");
  require(raw6(SYS_sched_yield, 0, 0, 0, 0, 0, 0) == 0, "selector-yield");
  require(get_gs() == 0 && tls_token == token && &errno == errno_address && errno == EDOM,
          "selector-reset-survives-yield");
  struct timespec delay = {0, 1000000};
  while (nanosleep(&delay, &delay))
    require(errno == EINTR, "selector-nanosleep");
  errno = EDOM;
  require(get_gs() == 0 && tls_token == token && &errno == errno_address && errno == EDOM,
          "selector-reset-survives-blocking");
  install_gs(WORKERS, token);
  puts("KERNEL-GS-CONTRACT PASS phase=user-gs-selector");
}

// No syscalls: progress on one CPU requires IRQ-driven preemption of these loops.
__attribute__((noinline)) void kernel_gs_user_loop(unsigned slot, uint64_t token) {
  while (!load(&workers[slot].stop)) {
    if (!state_valid(token, EDOM)) {
      __atomic_store_n(&workers[slot].error, 1, __ATOMIC_RELEASE);
      return;
    }
  }
}

static void record_cpu(struct worker* worker, uint64_t token) {
  unsigned cpu = UINT32_MAX, node = UINT32_MAX;
  require(raw6(SYS_getcpu, (long)&cpu, (long)&node, 0, 0, 0, 0) == 0, "getcpu");
  require(state_valid(token, EDOM) && cpu < 64, "getcpu-state");
  worker->cpus |= UINT64_C(1) << cpu;
  unsigned eax = 1, ebx, ecx = 0, edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx) : : "memory");
  const unsigned apic = ebx >> 24;
  worker->apics[apic / 64] |= UINT64_C(1) << (apic % 64);
}

static void* run_worker(void* argument) {
  struct worker* worker = argument;
  current_slot = (int)worker->slot;
  const uint64_t token = UINT64_C(0xa793046dbc12e500) + worker->slot;
  install_gs(worker->slot, token);
  user_gp_contract(token);
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGUSR1);
  require(pthread_sigmask(SIG_UNBLOCK, &signals, NULL) == 0, "worker-unblock");
  errno = EDOM;
  __atomic_store_n(&worker->stage, 1, __ATOMIC_RELEASE);
  unsigned char byte = 0;
  long result;
  do {
    result = raw6(SYS_read, worker->pipe[0], (long)&byte, 1, 0, 0, 0);
    require(state_valid(token, EDOM), "blocking-read-state");
  } while (result == -EINTR);
  require(result == 1 && byte == worker->slot + 1, "blocking-read-result");
  for (unsigned i = 0; i < ITERATIONS; ++i) {
    require(raw6(SYS_getuid, 0, 0, 0, 0, 0, 0) == expected_uid, "raw-getuid");
    require(state_valid(token, EDOM), "direct-gs-after-query");
    if (!(i % 64)) {
      require(raw6(SYS_sched_yield, 0, 0, 0, 0, 0, 0) == 0, "yield");
      require(state_valid(token, EDOM), "yield-state");
      record_cpu(worker, token);
    }
  }
  __atomic_store_n(&worker->stage, 2, __ATOMIC_RELEASE);
  kernel_gs_user_loop(worker->slot, token);
  require(!load(&worker->error) && load(&worker->signals) == 2, "signal-and-user-loop");
  require(state_valid(token, EDOM), "state-after-user-loop");
  require(get_gs() == expected_gs && state_valid(token, EDOM), "worker-final-gs");
  return NULL;
}

static void wait_workers(int signals, unsigned value) {
  for (unsigned attempt = 0; attempt < 10000; ++attempt) {
    unsigned ready = 0;
    for (unsigned i = 0; i < WORKERS; ++i) {
      require(!load(&workers[i].error), "worker-state-error");
      ready += load(signals ? &workers[i].signals : &workers[i].stage) >= value;
    }
    if (ready == WORKERS)
      return;
    struct timespec delay = {0, 1000000};
    while (nanosleep(&delay, &delay))
      require(errno == EINTR, "wait-nanosleep");
  }
  require(0, "worker-progress");
}

static void thread_contract(void) {
  pthread_t threads[WORKERS];
  for (unsigned i = 0; i < WORKERS; ++i) {
    workers[i].slot = i;
    require(pipe(workers[i].pipe) == 0, "pipe-create");
    require(pthread_create(&threads[i], NULL, run_worker, &workers[i]) == 0, "thread-create");
  }
  wait_workers(0, 1);
  for (unsigned i = 0; i < WORKERS; ++i)
    require(pthread_kill(threads[i], SIGUSR1) == 0, "blocking-signal-send");
  wait_workers(1, 1);
  for (unsigned i = 0; i < WORKERS; ++i) {
    unsigned char byte = i + 1;
    require(write(workers[i].pipe[1], &byte, 1) == 1, "pipe-release");
  }
  wait_workers(0, 2);
  for (unsigned i = 0; i < WORKERS; ++i)
    require(pthread_kill(threads[i], SIGUSR1) == 0, "user-loop-signal-send");
  wait_workers(1, 2);
  for (unsigned i = 0; i < WORKERS; ++i)
    __atomic_store_n(&workers[i].stop, 1, __ATOMIC_RELEASE);
  for (unsigned i = 0; i < WORKERS; ++i) {
    require(pthread_join(threads[i], NULL) == 0, "thread-join");
    require(close(workers[i].pipe[0]) == 0 && close(workers[i].pipe[1]) == 0, "pipe-close");
    printf(
        "KERNEL-GS-CONTRACT THREAD PASS worker=%u signals=%u cpu_mask=%llx "
        "apic_mask=%llx:%llx:%llx:%llx\n",
        i, load(&workers[i].signals), (unsigned long long)workers[i].cpus,
        (unsigned long long)workers[i].apics[3], (unsigned long long)workers[i].apics[2],
        (unsigned long long)workers[i].apics[1], (unsigned long long)workers[i].apics[0]);
  }
}

static void fork_exec_contract(const char* executable, uint64_t token) {
  errno = EDOM;
  pid_t child = fork();
  require(child >= 0, "fork");
  require(state_valid(token, EDOM), "fork-inherited-state");
  require(get_gs() == expected_gs && state_valid(token, EDOM), "fork-inherited-gs");
  if (!child) {
    child_pid = 0;
    alarm(30);
    install_gs(WORKERS + 1, token);
    puts("KERNEL-GS-CONTRACT PASS phase=fork-inheritance");
    execl(executable, executable, "--after-exec", NULL);
    execlp(executable, executable, "--after-exec", NULL);
    require(0, "exec");
  }
  child_pid = child;
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child-exit");
  child_pid = 0;
  errno = EDOM;
  require(state_valid(token, EDOM) && get_gs() == expected_gs, "parent-gs-isolation");
  puts("KERNEL-GS-CONTRACT PASS phase=fork-exec");
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  require(signal(SIGALRM, timeout_handler) != SIG_ERR, "timeout-setup");
  alarm(120);
  expected_uid = getuid();
  if (argc == 2 && !strcmp(argv[1], "--after-exec")) {
    require(get_gs() == 0 && tls_token == 0, "exec-resets-gs-and-tls");
    puts("KERNEL-GS-CONTRACT PASS phase=exec-zero");
    return 0;
  }
  require(argc == 1, "arguments");
  puts("KERNEL-GS-CONTRACT BEGIN workers=4");
  const uintptr_t original_gs = get_gs();
  const uint64_t token = UINT64_C(0xfe3a7941620dc58b);
  install_gs(WORKERS, token);
  const uintptr_t invalid[] = {UINT64_C(0x0000800000000000), UINT64_C(0xffff800000000000),
                               UINTPTR_MAX};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    long result = raw6(SYS_arch_prctl, ARCH_SET_GS, (long)invalid[i], 0, 0, 0, 0);
    require(result < 0 && result >= -4095, "reject-high-gs");
    require(state_valid(token, EDOM) && get_gs() == expected_gs, "failed-set-retains-gs");
  }
  require(raw6(SYS_arch_prctl, ARCH_GET_GS, -1, 0, 0, 0, 0) == -EFAULT, "bad-get-pointer");
  require(state_valid(token, EDOM), "bad-get-retains-state");
  puts("KERNEL-GS-CONTRACT PASS phase=arch-prctl");
  user_gs_selector_contract(token);
  struct sigaction gp_action = {0};
  gp_action.sa_sigaction = user_gp_signal;
  gp_action.sa_flags = SA_SIGINFO;
  require(sigemptyset(&gp_action.sa_mask) == 0 && sigaction(SIGBUS, &gp_action, NULL) == 0,
          "user-gp-signal-setup");
  user_gp_contract(token);
  // Vector 1 enters the kernel debugger instead of a userspace signal handler.
  puts("KERNEL-GS-CONTRACT SKIP phase=user-db reason=kernel-debugger-vector");
  struct sigaction action = {0};
  action.sa_handler = user_signal;
  require(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGUSR1, &action, NULL) == 0,
          "signal-setup");
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGUSR1);
  require(pthread_sigmask(SIG_BLOCK, &signals, NULL) == 0, "parent-block-signals");
  thread_contract();
  errno = EDOM;
  require(state_valid(token, EDOM) && get_gs() == expected_gs, "parent-thread-isolation");
  fork_exec_contract(argv[0], token);
  require(raw6(SYS_arch_prctl, ARCH_SET_GS, (long)original_gs, 0, 0, 0, 0) == 0, "restore-gs");
  require(get_gs() == original_gs && tls_token == token && &errno == errno_address,
          "restored-gs-and-tls");
  alarm(0);
  puts("KERNEL-GS-CONTRACT PASS END workers=4");
  return 0;
}
