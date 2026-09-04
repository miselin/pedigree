/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));

enum { wait_attempts = 100000 };

enum io_operation {
  receive_operation,
  send_operation,
};

struct io_context {
  int descriptor;
  char* payload;
  size_t length;
  enum io_operation operation;
  int use_message;
  int rights_descriptor;
  volatile int entered;
  volatile int returned;
  long tid;
  ssize_t result;
  int error;
};

union control_buffer {
  struct cmsghdr alignment;
  unsigned char bytes[CMSG_SPACE(sizeof(int))];
};

static volatile sig_atomic_t signal_calls;
static volatile sig_atomic_t close_from_signal = -1;
static volatile sig_atomic_t signal_close_result = -2;

static void signal_handler(int signal_number) {
  if (signal_number == SIGUSR1) {
    ++signal_calls;
    const int descriptor = __atomic_exchange_n(&close_from_signal, -1, __ATOMIC_RELAXED);
    if (descriptor >= 0) {
      __atomic_store_n(&signal_close_result, close(descriptor), __ATOMIC_RELEASE);
    }
  }
}

static void* run_io(void* parameter) {
  struct io_context* context = parameter;
  context->tid = syscall(SYS_gettid);
  errno = 0;
  __atomic_store_n(&context->entered, 1, __ATOMIC_RELEASE);
  if (context->use_message) {
    struct iovec vector = {
        .iov_base = context->payload,
        .iov_len = context->length,
    };
    union control_buffer control = {0};
    struct msghdr message = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes),
    };
    if (context->operation == send_operation) {
      struct cmsghdr* header = CMSG_FIRSTHDR(&message);
      header->cmsg_len = CMSG_LEN(sizeof(int));
      header->cmsg_level = SOL_SOCKET;
      header->cmsg_type = SCM_RIGHTS;
      memcpy(CMSG_DATA(header), &context->rights_descriptor, sizeof(context->rights_descriptor));
      context->result = sendmsg(context->descriptor, &message, MSG_NOSIGNAL);
    } else {
      context->result = recvmsg(context->descriptor, &message, 0);
    }
  } else if (context->operation == receive_operation) {
    context->result = recv(context->descriptor, context->payload, context->length, 0);
  } else {
    context->result = send(context->descriptor, context->payload, context->length, MSG_NOSIGNAL);
  }
  context->error = errno;
  __atomic_store_n(&context->returned, 1, __ATOMIC_RELEASE);
  return 0;
}

static int wait_for_value(volatile int* value) {
  for (size_t attempt = 0; attempt < wait_attempts; ++attempt) {
    if (__atomic_load_n(value, __ATOMIC_ACQUIRE))
      return 0;
    sched_yield();
  }
  return -1;
}

static int install_signal_handler(void) {
  struct sigaction action = {0};
  signal_calls = 0;
  __atomic_store_n(&close_from_signal, -1, __ATOMIC_RELAXED);
  __atomic_store_n(&signal_close_result, -2, __ATOMIC_RELAXED);
  action.sa_handler = signal_handler;
  return sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0) ||
         signal(SIGPIPE, SIG_IGN) == SIG_ERR;
}

static int interrupt_worker(struct io_context* context) {
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    if (__atomic_load_n(&context->returned, __ATOMIC_ACQUIRE))
      return 0;
    if (syscall(SYS_tkill, context->tid, SIGUSR1))
      return -1;
    for (size_t pause = 0; pause < 32; ++pause) {
      if (__atomic_load_n(&context->returned, __ATOMIC_ACQUIRE))
        return 0;
      sched_yield();
    }
  }
  return -1;
}

static int fill_send_queue(int descriptor, const char* payload, size_t length) {
  const int flags = fcntl(descriptor, F_GETFL);
  if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK))
    return -1;

  size_t written = 0;
  while (1) {
    errno = 0;
    const ssize_t result = send(descriptor, payload, length, MSG_NOSIGNAL);
    if (result > 0) {
      written += (size_t)result;
      continue;
    }
    if (result != -1 || (errno != EAGAIN && errno != EWOULDBLOCK))
      return -1;
    break;
  }

  return written && !fcntl(descriptor, F_SETFL, flags) ? 0 : -1;
}

static int interrupted_receive_child(void) {
  alarm(10);
  if (install_signal_handler())
    return 10;

  int sockets[2];
  char payload = 0;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) {
    fprintf(stderr, "interrupted receive socketpair failed: errno=%d\n", errno);
    return 11;
  }

  struct io_context context = {
      .descriptor = sockets[1],
      .payload = &payload,
      .length = 1,
      .operation = receive_operation,
      .result = -2,
  };
  pthread_t worker;
  if (pthread_create(&worker, 0, run_io, &context) || wait_for_value(&context.entered) ||
      context.tid <= 0 || interrupt_worker(&context) || pthread_join(worker, 0))
    return 12;

  if (context.result != -1 || context.error != EINTR || signal_calls < 1)
    return 13;
  return close(sockets[0]) || close(sockets[1]) ? 14 : 0;
}

static int partial_send_child(void) {
  alarm(10);
  if (install_signal_handler())
    return 20;

  int sockets[2];
  char payload[1024];
  memset(payload, 'q', sizeof(payload));
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) ||
      fill_send_queue(sockets[0], payload, sizeof(payload)) || recv(sockets[1], payload, 1, 0) != 1)
    return 21;

  payload[0] = 'x';
  payload[1] = 'y';
  struct io_context context = {
      .descriptor = sockets[0],
      .payload = payload,
      .length = 2,
      .operation = send_operation,
      .result = -2,
  };
  pthread_t worker;
  if (pthread_create(&worker, 0, run_io, &context) || wait_for_value(&context.entered) ||
      context.tid <= 0 || interrupt_worker(&context) || pthread_join(worker, 0))
    return 22;

  if (context.result != 1 || context.error || signal_calls < 1)
    return 23;
  return close(sockets[0]) || close(sockets[1]) ? 24 : 0;
}

static int interrupted_send_child(void) {
  alarm(10);
  if (install_signal_handler())
    return 25;

  int sockets[2];
  char payload[1024];
  memset(payload, 'i', sizeof(payload));
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) ||
      fill_send_queue(sockets[0], payload, sizeof(payload)))
    return 26;

  struct io_context context = {
      .descriptor = sockets[0],
      .payload = payload,
      .length = 1,
      .operation = send_operation,
      .result = -2,
  };
  pthread_t worker;
  if (pthread_create(&worker, 0, run_io, &context) || wait_for_value(&context.entered) ||
      context.tid <= 0 || interrupt_worker(&context) || pthread_join(worker, 0))
    return 27;

  if (context.result != -1 || context.error != EINTR || signal_calls < 1)
    return 28;
  return close(sockets[0]) || close(sockets[1]) ? 29 : 0;
}

static int signal_handler_close_child(enum io_operation operation) {
  alarm(10);
  if (install_signal_handler())
    return 50;

  int sockets[2];
  char payload[1024];
  memset(payload, 'h', sizeof(payload));
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
    return 51;
  if (operation == send_operation && fill_send_queue(sockets[0], payload, sizeof(payload)))
    return 52;

  int rights_pipe[2] = {-1, -1};
  if (operation == send_operation && pipe(rights_pipe))
    return 53;

  const int operation_side = operation == receive_operation ? 1 : 0;
  const int close_side = operation == receive_operation ? operation_side : 1 - operation_side;
  struct io_context context = {
      .descriptor = sockets[operation_side],
      .payload = payload,
      .length = 1,
      .operation = operation,
      .use_message = 1,
      .rights_descriptor = rights_pipe[0],
      .result = -2,
  };
  pthread_t worker;
  if (pthread_create(&worker, 0, run_io, &context) || wait_for_value(&context.entered))
    return 54;
  for (size_t pause = 0; pause < 512; ++pause)
    sched_yield();

  __atomic_store_n(&close_from_signal, sockets[close_side], __ATOMIC_RELEASE);
  if (__atomic_load_n(&context.returned, __ATOMIC_ACQUIRE) || context.tid <= 0 ||
      interrupt_worker(&context) || pthread_join(worker, 0))
    return 55;
  if (__atomic_load_n(&signal_close_result, __ATOMIC_ACQUIRE) || context.result != -1 ||
      context.error != EINTR || signal_calls < 1)
    return 56;

  sockets[close_side] = -1;
  if (sockets[0] >= 0 && close(sockets[0]))
    return 57;
  if (sockets[1] >= 0 && close(sockets[1]))
    return 58;
  if (rights_pipe[0] >= 0 && close(rights_pipe[0]))
    return 59;
  if (rights_pipe[1] >= 0 && close(rights_pipe[1]))
    return 60;
  return 0;
}

static int receive_signal_handler_close_child(void) {
  return signal_handler_close_child(receive_operation);
}

static int send_signal_handler_close_child(void) {
  return signal_handler_close_child(send_operation);
}

static int serialized_wait_child(enum io_operation operation) {
  alarm(10);
  if (install_signal_handler())
    return 40;

  int sockets[2];
  char payload[1024];
  memset(payload, 's', sizeof(payload));
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
    return 41;
  if (operation == send_operation && fill_send_queue(sockets[0], payload, sizeof(payload)))
    return 42;

  const int operation_side = operation == receive_operation ? 1 : 0;
  struct io_context holder = {
      .descriptor = sockets[operation_side],
      .payload = payload,
      .length = 1,
      .operation = operation,
      .result = -2,
  };
  struct io_context waiter = {
      .descriptor = sockets[operation_side],
      .payload = payload,
      .length = 1,
      .operation = operation,
      .result = -2,
  };
  pthread_t holder_thread;
  pthread_t waiter_thread;
  if (pthread_create(&holder_thread, 0, run_io, &holder) || wait_for_value(&holder.entered))
    return 43;
  for (size_t pause = 0; pause < 512; ++pause)
    sched_yield();
  if (__atomic_load_n(&holder.returned, __ATOMIC_ACQUIRE) ||
      pthread_create(&waiter_thread, 0, run_io, &waiter) || wait_for_value(&waiter.entered) ||
      waiter.tid <= 0 || interrupt_worker(&waiter) || pthread_join(waiter_thread, 0))
    return 44;

  if (waiter.result != -1 || waiter.error != EINTR || signal_calls < 1 ||
      __atomic_load_n(&holder.returned, __ATOMIC_ACQUIRE))
    return 45;
  if (close(sockets[0]) || close(sockets[1]) || pthread_join(holder_thread, 0))
    return 46;

  const int holder_result_ok = operation == receive_operation
                                   ? holder.result == 0
                                   : holder.result == -1 && holder.error == EPIPE;
  return holder_result_ok ? 0 : 47;
}

static int serialized_receive_wait_child(void) {
  return serialized_wait_child(receive_operation);
}

static int serialized_send_wait_child(void) {
  return serialized_wait_child(send_operation);
}

static int close_wake_child(enum io_operation operation, int close_local) {
  alarm(10);
  if (install_signal_handler())
    return 30;

  int sockets[2];
  char payload[1024];
  memset(payload, 'c', sizeof(payload));
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
    return 31;
  if (operation == send_operation && fill_send_queue(sockets[0], payload, sizeof(payload)))
    return 32;

  const int operation_side = operation == receive_operation ? 1 : 0;
  const int close_side = close_local ? operation_side : 1 - operation_side;
  struct io_context context = {
      .descriptor = sockets[operation_side],
      .payload = payload,
      .length = 1,
      .operation = operation,
      .result = -2,
  };
  pthread_t worker;
  if (pthread_create(&worker, 0, run_io, &context) || wait_for_value(&context.entered))
    return 33;
  for (size_t pause = 0; pause < 256; ++pause)
    sched_yield();

  if (close(sockets[close_side]))
    return 34;
  sockets[close_side] = -1;
  const int returned_without_rescue = !wait_for_value(&context.returned);
  if (!returned_without_rescue) {
    if (sockets[0] >= 0)
      close(sockets[0]);
    if (sockets[1] >= 0)
      close(sockets[1]);
    (void)syscall(SYS_tkill, context.tid, SIGUSR1);
  }
  if (pthread_join(worker, 0))
    return 35;

  const int result_ok = operation == receive_operation
                            ? context.result == 0
                            : context.result == -1 && context.error == EPIPE;
  if (!returned_without_rescue || !result_ok)
    return 36;
  if (sockets[0] >= 0 && close(sockets[0]))
    return 37;
  if (sockets[1] >= 0 && close(sockets[1]))
    return 38;
  return 0;
}

static void run_bounded(int (*child_test)(void)) {
  const pid_t child = fork();
  if (child < 0)
    fail();
  if (!child)
    _exit(child_test());

  int status_code = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status_code, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child || !WIFEXITED(status_code) || WEXITSTATUS(status_code)) {
    if (waited == child && WIFEXITED(status_code))
      fprintf(stderr, "AF_UNIX interruption child failed: code=%d\n", WEXITSTATUS(status_code));
    fail();
  }
}

static int peer_receive_close_child(void) {
  return close_wake_child(receive_operation, 0);
}

static int local_receive_close_child(void) {
  return close_wake_child(receive_operation, 1);
}

static int peer_send_close_child(void) {
  return close_wake_child(send_operation, 0);
}

static int local_send_close_child(void) {
  return close_wake_child(send_operation, 1);
}

void test_unix_stream_interruption(void) {
  puts("Testing AF_UNIX stream interruption and close wakeups... ");
  fflush(stdout);
  run_bounded(interrupted_receive_child);
  run_bounded(interrupted_send_child);
  run_bounded(receive_signal_handler_close_child);
  run_bounded(send_signal_handler_close_child);
  run_bounded(serialized_receive_wait_child);
  run_bounded(serialized_send_wait_child);
  run_bounded(partial_send_child);
  run_bounded(peer_receive_close_child);
  run_bounded(local_receive_close_child);
  run_bounded(peer_send_close_child);
  run_bounded(local_send_close_child);
  puts("OK\n");
  fflush(stdout);
}
