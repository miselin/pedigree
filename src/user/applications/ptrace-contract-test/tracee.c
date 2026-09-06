#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/prctl.h>

_Thread_local volatile uint64_t tc_image_tls = TC_IMAGE_TLS;
static volatile sig_atomic_t handler_report = -1;

static void caught(int signal, siginfo_t* info, void* context) {
  (void)context;
  int saved = errno;
  struct tc_message message = {.kind = 'H',
                               .signal = signal,
                               .code = info->si_code,
                               .pid = info->si_pid,
                               .uid = info->si_uid};
  ssize_t ignored = write(handler_report, &message, sizeof(message));
  (void)ignored;
  errno = saved;
}

struct alive {
  atomic_uint ready, finish;
};

static void* remain_alive(void* argument) {
  struct alive* state = argument;
  atomic_store(&state->ready, 1);
  tc_atomic_wait(&state->finish, 1);
  return NULL;
}

static long multithreaded_enrollment(int* error) {
  struct alive state = {0};
  pthread_t thread;
  int result = pthread_create(&thread, NULL, remain_alive, &state);
  if (result) {
    *error = result;
    return -2;
  }
  long enrolled = -2;
  if (!tc_atomic_wait(&state.ready, 1)) {
    errno = 0;
    enrolled = ptrace(PTRACE_TRACEME, (pid_t)0, (void*)0, (void*)0);
    *error = errno;
  }
  atomic_store(&state.finish, 1);
  if (pthread_join(thread, NULL))
    enrolled = -2;
  return enrolled;
}

void tc_tracee(int command, int report, int traced) {
  alarm(40);
  sigset_t empty;
  sigemptyset(&empty);
  struct sigaction action = {.sa_sigaction = caught, .sa_flags = SA_SIGINFO};
  sigemptyset(&action.sa_mask);
  handler_report = report;
  if (sigprocmask(SIG_SETMASK, &empty, NULL) || sigaction(SIGUSR1, &action, NULL) ||
      sigaction(SIGUSR2, &action, NULL))
    _exit(122);
  errno = 0;
  struct tc_message ready = {.kind = 'R'};
  if (traced) {
    ready.result = ptrace(PTRACE_TRACEME, (pid_t)0, (void*)0, (void*)0);
    ready.error = errno;
  }
  if (tc_write(report, &ready, sizeof(ready)) || ready.result)
    _exit(123);
  for (;;) {
    struct tc_command request;
    if (tc_read(command, &request, sizeof(request)))
      _exit(124);
    struct tc_message reply = {.kind = request.operation, .signal = request.signal};
    errno = 0;
    switch (request.operation) {
      case 'S':
        reply.kind = 'A';
        reply.result = syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), request.signal);
        break;
      case 'I':
      case 'F':
        action.sa_handler = request.operation == 'I' ? SIG_IGN : SIG_DFL;
        action.sa_flags = 0;
        reply.result = sigaction(request.signal, &action, NULL);
        break;
      case 'B':
      case 'U': {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, request.signal);
        reply.result = sigprocmask(request.operation == 'B' ? SIG_BLOCK : SIG_UNBLOCK, &mask, NULL);
        break;
      }
      case 'D':
        reply.result = prctl(PR_SET_DUMPABLE, 0UL, 0UL, 0UL, 0UL);
        break;
      case 'T':
        reply.result =
            ptrace(PTRACE_TRACEME, (pid_t)424242, (void*)UINTPTR_MAX, (void*)UINTPTR_MAX);
        break;
      case 'M':
        reply.result = multithreaded_enrollment(&reply.error);
        break;
      case 'Q': {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, request.signal);
        struct timespec immediate = {0, 0};
        for (unsigned i = 0; i < 16; ++i) {
          siginfo_t info;
          int signal = sigtimedwait(&mask, &info, &immediate);
          if (signal != request.signal || info.si_code != SI_QUEUE ||
              info.si_value.sival_int != (int)i + 100) {
            reply.result = -1;
            break;
          }
          ++reply.result;
        }
        break;
      }
      case 'X':
      case 'J': {
        tc_image_tls ^= UINT64_C(0xffffffffffffffff);
        char descriptor[24];
        snprintf(descriptor, sizeof(descriptor), "%d", report);
        if (fcntl(report, F_SETFD, 0))
          _exit(125);
        if (request.operation == 'X')
          execl(TC_EXECUTABLE, TC_EXECUTABLE, "--exec-child", descriptor, (char*)NULL);
        else
          execl(TC_ENTRY_EXECUTABLE, TC_ENTRY_EXECUTABLE, descriptor, (char*)NULL);
        reply.result = -1;
        break;
      }
      case 'P':
      case 'E':
        break;
      default:
        _exit(126);
    }
    if (request.operation != 'M')
      reply.error = errno;
    if (tc_write(report, &reply, sizeof(reply)))
      _exit(127);
    if (request.operation == 'E')
      _exit(TC_EXIT_CODE);
  }
}

int tc_exec_child(int report) {
  alarm(10);
  struct tc_message message = {.kind = 'X', .pid = getpid(), .value = tc_image_tls};
  if (tc_image_tls != TC_IMAGE_TLS || tc_write(report, &message, sizeof(message)))
    return 1;
  return 0;
}
