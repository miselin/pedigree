#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

static _Thread_local volatile uint64_t tls_value;

static void tracee(int report) {
  alarm(25);
  struct sigaction action = {.sa_handler = SIG_DFL};
  sigemptyset(&action.sa_mask);
  sigset_t unblocked;
  sigemptyset(&unblocked);
  sigaddset(&unblocked, TC_STOP_SIGNAL);
  sigaddset(&unblocked, SIGALRM);
  if (sigaction(TC_STOP_SIGNAL, &action, NULL) || sigaction(SIGALRM, &action, NULL) ||
      sigprocmask(SIG_UNBLOCK, &unblocked, NULL))
    _exit(110);

  struct tc_capture capture = {0};
  // These two slots have no selected userspace readback; they are not observations.
  capture.resumed.orig_rax = UINT64_MAX;
  capture.resumed.gs_base = UINT64_MAX;
  capture.setup.pid = (uint64_t)getpid();
  capture.setup.tid = (uint64_t)syscall(SYS_gettid);
  tls_value = UINT64_C(0x1726354453627180);
  if (syscall(SYS_arch_prctl, TC_ARCH_GET_FS, &capture.setup.fs_base))
    _exit(111);
  // This exact task's creator performs every subsequent inspection and resume.
  if (ptrace(PTRACE_TRACEME, (pid_t)0, (void*)0, (void*)0))
    _exit(112);
  if (tc_probe(report, &capture))
    _exit(113);
  if (syscall(SYS_arch_prctl, TC_ARCH_GET_FS, &capture.resumed.fs_base) ||
      capture.resumed.fs_base != capture.setup.fs_base || tls_value != UINT64_C(0x1726354453627180))
    _exit(114);
  if (tc_write(report, &capture, sizeof(capture)))
    _exit(115);
  alarm(0);
  _exit(0);
}

static int matches(const struct user_regs_struct* regs, const struct tc_setup* setup,
                   uint64_t entry_flags, int traced) {
#define FIELD(name, expected)                                                                      \
  do {                                                                                             \
    if (regs->name != (unsigned long)(expected)) {                                                 \
      fprintf(stderr, "%s %s actual=%#lx expected=%#lx\n", traced ? "snapshot" : "resumed", #name, \
              regs->name, (unsigned long)(expected));                                              \
      return -1;                                                                                   \
    }                                                                                              \
  } while (0)
  FIELD(r15, TC_SENT_R15);
  FIELD(r14, TC_SENT_R14);
  FIELD(r13, TC_SENT_R13);
  FIELD(r12, TC_SENT_R12);
  FIELD(rbp, TC_SENT_RBP);
  FIELD(rbx, TC_SENT_RBX);
  FIELD(r10, TC_SENT_R10);
  FIELD(r9, TC_SENT_R9);
  FIELD(r8, TC_SENT_R8);
  FIELD(rax, 0);
  FIELD(rcx, setup->label);
  FIELD(r11, entry_flags);
  FIELD(rdi, setup->pid);
  FIELD(rsi, setup->tid);
  FIELD(rdx, TC_STOP_SIGNAL);
  FIELD(rip, setup->label);
  FIELD(rsp, setup->stack);
  FIELD(eflags, entry_flags);
  FIELD(cs, setup->cs);
  FIELD(ss, setup->ss);
  FIELD(ds, setup->ds);
  FIELD(es, setup->es);
  FIELD(fs, setup->fs);
  FIELD(gs, setup->gs);
  FIELD(fs_base, setup->fs_base);
  if (traced)
    FIELD(orig_rax, SYS_tgkill);
#undef FIELD
  return 0;
}

int tc_registers(void) {
  int failed = 0, status = 0;
  int report[2] = {-1, -1};
  pid_t child = -1;
  struct tc_setup setup;
  struct tc_capture capture;
  struct user_regs_struct saved;
  struct {
    struct user_regs_struct regs;
    uint64_t tail[2];
  } first, second;
  CHECK(pipe2(report, O_CLOEXEC | O_NONBLOCK) == 0);
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(report[0]);
    tracee(report[1]);
    _exit(116);
  }
  close(report[1]);
  report[1] = -1;
  CHECK(tc_read(report[0], &setup, sizeof(setup)) == 0);
  CHECK(setup.pid == (uint64_t)child && setup.tid == (uint64_t)child);
  CHECK(setup.stack && setup.label && setup.fs_base);
  CHECK(tc_wait(child, &status, 10000) == 0);
  if (WIFEXITED(status) || WIFSIGNALED(status))
    child = -1;
  CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == TC_STOP_SIGNAL);

  // Distinct initial bytes expose an omitted field, including an unasserted GS base.
  memset(&first, 0xa5, sizeof(first));
  memset(&second, 0x5a, sizeof(second));
  CHECK(ptrace(PTRACE_GETREGS, child, (void*)0, (void*)&first.regs) == 0);
  CHECK(ptrace(PTRACE_GETREGS, child, (void*)0, (void*)&second.regs) == 0);
  CHECK(memcmp(&first.regs, &second.regs, TC_REG_SIZE) == 0);
  CHECK(first.tail[0] == UINT64_C(0xa5a5a5a5a5a5a5a5) &&
        first.tail[1] == UINT64_C(0xa5a5a5a5a5a5a5a5));
  CHECK(second.tail[0] == UINT64_C(0x5a5a5a5a5a5a5a5a) &&
        second.tail[1] == UINT64_C(0x5a5a5a5a5a5a5a5a));
  saved = first.regs;
  CHECK(ptrace(PTRACE_CONT, child, (void*)0, (void*)0) == 0);
  CHECK(tc_read(report[0], &capture, sizeof(capture)) == 0);
  CHECK(memcmp(&capture.setup, &setup, sizeof(setup)) == 0);
  CHECK(matches(&first.regs, &setup, capture.entry_flags, 1) == 0);
  CHECK(matches(&capture.resumed, &setup, capture.entry_flags, 0) == 0);
  CHECK(memcmp(&first.regs, &saved, TC_REG_SIZE) == 0);
  CHECK(memcmp(&second.regs, &saved, TC_REG_SIZE) == 0);
  CHECK(tc_wait(child, &status, 10000) == 0);
  if (WIFEXITED(status) || WIFSIGNALED(status))
    child = -1;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
out:
  if (failed)
    fprintf(stderr, "register family status=%#x child=%d\n", status, child);
  tc_cleanup(&child);
  if (report[0] >= 0)
    close(report[0]);
  if (report[1] >= 0)
    close(report[1]);
  return failed;
}
