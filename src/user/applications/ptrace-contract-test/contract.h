#ifndef PTRACE_CONTRACT_H
#define PTRACE_CONTRACT_H

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "entry-layout.h"
#include "sentinel-layout.h"
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>

#if !defined(__x86_64__) || defined(__ILP32__)
#error This draft requires the musl amd64 ABI.
#endif

#define TC_ARCH_GET_FS 0x1003
#define TC_STOP_SIGNAL SIGUSR1
#define TC_EXECUTABLE "/applications/ptrace-contract-test"
#define TC_ENTRY_EXECUTABLE "/applications/ptrace-entry-test"
#define TC_EXIT_CODE 17
#define TC_IMAGE_TLS UINT64_C(0x1938574629183746)
#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

struct tc_setup {
  uint64_t pid, tid, fs_base, stack, label, cs, ss, ds, es, fs, gs;
};

struct tc_capture {
  struct tc_setup setup;
  uint64_t entry_flags;
  struct user_regs_struct resumed;
};

struct tc_entry_record {
  uint64_t magic;
  struct user_regs_struct registers;
  int64_t fs_result;
};

#define TC_OFFSET(type, field, offset) \
  _Static_assert(offsetof(type, field) == (offset), "assembly offset: " #field)
TC_OFFSET(struct tc_setup, pid, TC_PID);
TC_OFFSET(struct tc_setup, tid, TC_TID);
TC_OFFSET(struct tc_setup, fs_base, TC_FS_BASE);
TC_OFFSET(struct tc_setup, stack, TC_STACK);
TC_OFFSET(struct tc_setup, label, TC_LABEL);
TC_OFFSET(struct tc_setup, cs, TC_CS);
TC_OFFSET(struct tc_setup, ss, TC_SS);
TC_OFFSET(struct tc_setup, ds, TC_DS);
TC_OFFSET(struct tc_setup, es, TC_ES);
TC_OFFSET(struct tc_setup, fs, TC_FS);
TC_OFFSET(struct tc_setup, gs, TC_GS);
TC_OFFSET(struct tc_capture, entry_flags, TC_ENTRY_FLAGS);
TC_OFFSET(struct tc_capture, resumed, TC_RESUMED);
TC_OFFSET(struct tc_entry_record, magic, TC_ENTRY_MAGIC);
TC_OFFSET(struct tc_entry_record, registers, TC_ENTRY_REGISTERS);
TC_OFFSET(struct tc_entry_record, fs_result, TC_ENTRY_FS_RESULT);
TC_OFFSET(struct user_regs_struct, r15, TC_R15);
TC_OFFSET(struct user_regs_struct, r14, TC_R14);
TC_OFFSET(struct user_regs_struct, r13, TC_R13);
TC_OFFSET(struct user_regs_struct, r12, TC_R12);
TC_OFFSET(struct user_regs_struct, rbp, TC_RBP);
TC_OFFSET(struct user_regs_struct, rbx, TC_RBX);
TC_OFFSET(struct user_regs_struct, r11, TC_R11);
TC_OFFSET(struct user_regs_struct, r10, TC_R10);
TC_OFFSET(struct user_regs_struct, r9, TC_R9);
TC_OFFSET(struct user_regs_struct, r8, TC_R8);
TC_OFFSET(struct user_regs_struct, rax, TC_RAX);
TC_OFFSET(struct user_regs_struct, rcx, TC_RCX);
TC_OFFSET(struct user_regs_struct, rdx, TC_RDX);
TC_OFFSET(struct user_regs_struct, rsi, TC_RSI);
TC_OFFSET(struct user_regs_struct, rdi, TC_RDI);
TC_OFFSET(struct user_regs_struct, orig_rax, TC_ORIG_RAX);
TC_OFFSET(struct user_regs_struct, rip, TC_RIP);
TC_OFFSET(struct user_regs_struct, cs, TC_REG_CS);
TC_OFFSET(struct user_regs_struct, eflags, TC_EFLAGS);
TC_OFFSET(struct user_regs_struct, rsp, TC_RSP);
TC_OFFSET(struct user_regs_struct, ss, TC_REG_SS);
TC_OFFSET(struct user_regs_struct, fs_base, TC_REG_FS_BASE);
TC_OFFSET(struct user_regs_struct, gs_base, TC_REG_GS_BASE);
TC_OFFSET(struct user_regs_struct, ds, TC_REG_DS);
TC_OFFSET(struct user_regs_struct, es, TC_REG_ES);
TC_OFFSET(struct user_regs_struct, fs, TC_REG_FS);
TC_OFFSET(struct user_regs_struct, gs, TC_REG_GS);
#undef TC_OFFSET

_Static_assert(sizeof(struct tc_setup) == TC_SETUP_SIZE, "setup size");
_Static_assert(sizeof(struct user_regs_struct) == TC_REG_SIZE, "musl register ABI changed");
_Static_assert(sizeof(siginfo_t) == 128, "musl siginfo ABI changed");
_Static_assert(sizeof(struct tc_capture) == TC_CAPTURE_SIZE, "capture size");
_Static_assert(sizeof(struct tc_entry_record) == TC_ENTRY_SIZE, "entry record size");
_Static_assert(SYS_arch_prctl == 158 && SYS_exit == 60, "entry syscall ABI");
_Static_assert(SYS_write == 1 && SYS_getpid == 39 && SYS_tgkill == 234, "asm syscall ABI");
_Static_assert(TC_STOP_SIGNAL == 10, "asm signal ABI");

int64_t tc_now(void);
int tc_read(int fd, void* data, size_t size);
int tc_write(int fd, const void* data, size_t size);
int tc_wait(pid_t pid, int* status, int milliseconds);
void tc_cleanup(pid_t* pid);
int tc_probe(int report_fd, struct tc_capture* capture);
int tc_registers(void);

struct tc_child {
  pid_t pid;
  int command, report;
};
#define TC_CHILD_INIT {.pid = -1, .command = -1, .report = -1}
struct tc_command {
  int operation, signal;
};
struct tc_message {
  int kind, signal, code, error;
  pid_t pid;
  uid_t uid;
  int64_t result;
  uint64_t value;
};
extern size_t tc_page;
extern _Thread_local volatile uint64_t tc_image_tls;
int tc_atomic_wait(atomic_uint* value, unsigned expected);
int tc_spawn(struct tc_child* child, int traced);
void tc_tracee(int command, int report, int traced);
void tc_close(struct tc_child* child);
int tc_send(const struct tc_child* child, int operation, int signal);
int tc_receive(int fd, int kind, struct tc_message* message);
int tc_stop(struct tc_child* child, int signal);
int tc_wait_stop(struct tc_child* child, int signal);
int tc_finish(struct tc_child* child);
int tc_resume(pid_t child, int request, int signal);
int tc_signal_info(const siginfo_t* info, int signal, int code, pid_t pid, uid_t uid);
int tc_inspect(void);
int tc_signals(void);
int tc_ownership(void);
int tc_lifecycle(void);
int tc_errors(void);
int tc_exec_child(int report);
int tc_exec_tracer(pid_t child, int command, int report);

#endif
