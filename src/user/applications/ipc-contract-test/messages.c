/* Copyright (c) 2026, Pedigree Developers. See LICENSE for licensing details. */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/msg.h>
#include <sys/wait.h>

#ifndef MSG_COPY
#define MSG_COPY 040000
#endif

struct message {
  long type;
  char bytes[8];
};

#define CHECK(condition, label)                                 \
  do {                                                          \
    if (!(condition)) {                                         \
      printf("IPC-MESSAGES: FAIL %s errno=%d\n", label, errno); \
      return 1;                                                 \
    }                                                           \
  } while (0)

static int send_byte(int id, long type, char value) {
  struct message message = {.type = type, .bytes = {value}};
  return msgsnd(id, &message, 1, IPC_NOWAIT);
}

static int receive_byte(int id, long type, int flags, long expected_type, char value) {
  struct message message = {0};
  return msgrcv(id, &message, sizeof(message.bytes), type, flags | IPC_NOWAIT) == 1 &&
         message.type == expected_type && message.bytes[0] == value;
}

static int selection(int id) {
  struct msqid_ds status;
  CHECK(!msgctl(id, IPC_STAT, &status), "initial status");
  CHECK(status.msg_qnum == 0 && status.msg_cbytes == 0 && status.msg_qbytes > 0 &&
            status.msg_perm.uid == geteuid() && status.msg_perm.cuid == geteuid() &&
            !status.msg_lspid && !status.msg_lrpid && !status.msg_stime && !status.msg_rtime,
        "initial queue metadata");
  CHECK(msgget(status.msg_perm.__ipc_perm_key, 0600) == id, "named queue lookup");
  errno = 0;
  CHECK(
      msgget(status.msg_perm.__ipc_perm_key, IPC_CREAT | IPC_EXCL | 0600) == -1 && errno == EEXIST,
      "exclusive queue creation");
  int private_id = msgget(IPC_PRIVATE, 0600);
  CHECK(private_id >= 0 && private_id != id, "private queue identity");
  CHECK(!msgctl(private_id, IPC_RMID, NULL), "private queue removal");
  struct msginfo info;
  int highest = msgctl(0, MSG_INFO, (struct msqid_ds*)&info);
  CHECK(highest >= 0 && info.msgpool >= 1 && info.msgmax >= 8 && info.msgmni >= 1,
        "queue usage query");
  int found = 0;
  for (int slot = 0; slot <= highest; ++slot) {
    if (msgctl(slot, MSG_STAT, &status) == id) {
      CHECK(msgctl(slot, MSG_STAT_ANY, &status) == id, "unrestricted index query");
      found = 1;
      break;
    }
  }
  CHECK(found, "queue index enumeration");
  CHECK(msgctl(0, IPC_INFO, (struct msqid_ds*)&info) >= 0 && info.msgmnb > 0, "queue limits query");
  CHECK(!send_byte(id, 7, 'a') && !send_byte(id, 2, 'b') && !send_byte(id, 2, 'c') &&
            !send_byte(id, 5, 'd'),
        "queue typed records");
  CHECK(!msgctl(id, IPC_STAT, &status) && status.msg_qnum == 4 && status.msg_cbytes == 4 &&
            status.msg_lspid == getpid(),
        "send metadata");
  CHECK(receive_byte(id, 1, MSG_COPY, 2, 'b'), "ordinal nondestructive copy");
  CHECK(!msgctl(id, IPC_STAT, &status) && status.msg_qnum == 4 && status.msg_lrpid == 0,
        "copy preserves metadata");
  CHECK(receive_byte(id, -6, 0, 2, 'b'), "negative selector lowest type");
  CHECK(receive_byte(id, 2, 0, 2, 'c'), "equal type FIFO order");
  CHECK(receive_byte(id, 7, MSG_EXCEPT, 5, 'd'), "excluded type");
  CHECK(receive_byte(id, 0, 0, 7, 'a'), "zero selector FIFO");
  CHECK(!send_byte(id, LONG_MAX, 'z') && receive_byte(id, LONG_MIN, 0, LONG_MAX, 'z'),
        "minimum signed selector");
  CHECK(!msgctl(id, IPC_STAT, &status) && !status.msg_qnum && !status.msg_cbytes &&
            status.msg_lrpid == getpid(),
        "receive metadata");
  struct message message = {.type = 1};
  errno = 0;
  CHECK(msgrcv(id, &message, sizeof(message.bytes), 0, IPC_NOWAIT) == -1 && errno == ENOMSG,
        "empty queue nonblocking receive");
  CHECK(!msgsnd(id, &message, 0, IPC_NOWAIT) && msgrcv(id, &message, 0, 0, IPC_NOWAIT) == 0,
        "zero length message");
  message.type = 0;
  errno = 0;
  CHECK(msgsnd(id, &message, 0, IPC_NOWAIT) == -1 && errno == EINVAL, "invalid message type");
  return 0;
}

static int copies(int id) {
  struct message message = {.type = 3, .bytes = "text"};
  CHECK(!msgsnd(id, &message, 4, IPC_NOWAIT), "queue complete payload");
  errno = 0;
  CHECK(msgrcv(id, &message, 2, 0, IPC_NOWAIT) == -1 && errno == E2BIG,
        "short receive preserves message");
  errno = 0;
  CHECK(msgrcv(id, NULL, 4, 0, IPC_NOWAIT) == -1 && errno == EFAULT,
        "invalid destination rejected");
  struct msqid_ds status;
  CHECK(!msgctl(id, IPC_STAT, &status) && status.msg_qnum == 1 && status.msg_cbytes == 4,
        "failed copy preserves message");
  memset(&message, 0, sizeof(message));
  CHECK(msgrcv(id, &message, 2, 0, MSG_NOERROR | IPC_NOWAIT) == 2 && message.type == 3 &&
            !memcmp(message.bytes, "te", 2) && message.bytes[2] == 0,
        "explicit truncation");
  CHECK(!msgctl(id, IPC_STAT, &status) && status.msg_qnum == 0,
        "truncation consumes complete message");
  errno = 0;
  CHECK(msgsnd(id, NULL, 1, IPC_NOWAIT) == -1 && errno == EFAULT, "invalid sender buffer");
  errno = 0;
  CHECK(msgctl(id, IPC_STAT, NULL) == -1 && errno == EFAULT, "invalid status buffer");
  errno = 0;
  CHECK(msgrcv(id, &message, 0, 0, MSG_COPY) == -1 && errno == EINVAL, "copy requires nonblocking");
  status.msg_qbytes = 2;
  CHECK(!msgctl(id, IPC_SET, &status), "limit zero length records");
  message.type = 1;
  CHECK(!msgsnd(id, &message, 0, IPC_NOWAIT) && !msgsnd(id, &message, 0, IPC_NOWAIT),
        "fill queue with zero length records");
  errno = 0;
  CHECK(msgsnd(id, &message, 0, IPC_NOWAIT) == -1 && errno == EAGAIN,
        "zero length records consume queue capacity");
  CHECK(msgrcv(id, &message, 0, 0, IPC_NOWAIT) == 0 && msgrcv(id, &message, 0, 0, IPC_NOWAIT) == 0,
        "drain zero length records");
  return 0;
}

static int wait_child(pid_t child) {
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int wait_ready(int descriptor, pid_t child) {
  char ready = 0;
  int status = 0;
  const struct timespec delay = {.tv_nsec = 100000000};
  if (read(descriptor, &ready, 1) != 1 || ready != 'r' || nanosleep(&delay, NULL)) {
    return 0;
  }
  return waitpid(child, &status, WNOHANG) == 0;
}

/* Each peer announces entry and has its own terminal timeout. */
static int blocking(int id, int sending, int removing, int terminating) {
  struct msqid_ds status;
  CHECK(!msgctl(id, IPC_STAT, &status), "read queue capacity");
  status.msg_qbytes = 1;
  CHECK(!msgctl(id, IPC_SET, &status), "set queue capacity");
  if (sending) {
    CHECK(!send_byte(id, 1, 'a'), "fill queue");
    errno = 0;
    CHECK(send_byte(id, 1, 'b') == -1 && errno == EAGAIN, "full queue nonblocking send");
  }
  int ready[2];
  CHECK(!pipe(ready), "peer readiness pipe");
  pid_t child = fork();
  CHECK(child >= 0, "fork blocking peer");
  if (!child) {
    close(ready[0]);
    alarm(4);
    struct message message = {.type = 1, .bytes = {'b'}};
    if (write(ready[1], "r", 1) != 1) {
      _exit(2);
    }
    close(ready[1]);
    errno = 0;
    ssize_t result = sending ? msgsnd(id, &message, 1, 0) : msgrcv(id, &message, 1, 0, 0);
    int error = errno;
    if (removing) {
      _exit(result == -1 && error == EIDRM ? 0 : 3);
    }
    _exit((sending ? result == 0 : result == 1 && message.bytes[0] == 'a') ? 0 : 4);
  }
  close(ready[1]);
  CHECK(wait_ready(ready[0], child), "peer remains blocked");
  close(ready[0]);
  if (terminating) {
    CHECK(!kill(child, SIGKILL), "terminate blocked peer");
    int child_status;
    CHECK(waitpid(child, &child_status, 0) == child && WIFSIGNALED(child_status) &&
              WTERMSIG(child_status) == SIGKILL,
          "reap terminated peer");
    CHECK(!send_byte(id, 1, 'k') && receive_byte(id, 0, 0, 1, 'k'),
          "queue survives peer termination");
    return 0;
  }
  if (removing) {
    CHECK(!msgctl(id, IPC_RMID, NULL), "remove queue with blocked peer");
  } else if (sending) {
    CHECK(receive_byte(id, 0, 0, 1, 'a'), "receive releases sender capacity");
  } else {
    CHECK(!send_byte(id, 1, 'a'), "send wakes receiver");
  }
  CHECK(wait_child(child), "blocking peer result");
  if (sending && !removing) {
    CHECK(receive_byte(id, 0, 0, 1, 'b'), "blocked sender delivered payload");
  }
  if (removing) {
    errno = 0;
    CHECK(msgctl(id, IPC_STAT, &status) == -1 && errno == EINVAL, "removed identifier retired");
  }
  return 0;
}

static volatile sig_atomic_t interrupted;
static void interrupt_handler(int signal_number) {
  if (signal_number == SIGALRM) {
    if (++interrupted >= 3) {
      _exit(124);
    }
    alarm(1);
  }
}

static int interruption(int id, int sending) {
  struct sigaction action = {.sa_handler = interrupt_handler, .sa_flags = SA_RESTART};
  CHECK(!sigemptyset(&action.sa_mask) && !sigaction(SIGALRM, &action, NULL),
        "install interrupt handler");
  if (sending) {
    struct msqid_ds status;
    CHECK(!msgctl(id, IPC_STAT, &status), "interrupted sender status");
    status.msg_qbytes = 1;
    CHECK(!msgctl(id, IPC_SET, &status) && !send_byte(id, 1, 'a'), "fill interrupted sender");
  }
  struct message message = {.type = 1, .bytes = {'b'}};
  interrupted = 0;
  alarm(1);
  errno = 0;
  ssize_t result = sending ? msgsnd(id, &message, 1, 0) : msgrcv(id, &message, 1, 0, 0);
  int error = errno;
  alarm(0);
  CHECK(result == -1 && error == EINTR && interrupted, "IPC is interrupted with SA_RESTART");
  if (sending) {
    CHECK(receive_byte(id, 0, 0, 1, 'a'), "interrupted sender preserved queued payload");
  }
  return 0;
}

static int permissions(int id) {
  if (geteuid() != 0) {
    puts("IPC-MESSAGES: SKIP alternate credentials requires root");
    return 0;
  }
  struct msqid_ds status;
  CHECK(!msgctl(id, IPC_STAT, &status), "permission status");
  pid_t child = fork();
  CHECK(child >= 0, "fork credential peer");
  if (!child) {
    alarm(4);
    if (setgroups(0, NULL) || setgid(65534) || setuid(65534)) {
      _exit(2);
    }
    struct message message = {.type = 1};
    errno = 0;
    if (msgget(status.msg_perm.__ipc_perm_key, 0600) != -1 || errno != EACCES) {
      _exit(3);
    }
    errno = 0;
    if (msgsnd(id, &message, 0, IPC_NOWAIT) != -1 || errno != EACCES) {
      _exit(4);
    }
    errno = 0;
    if (msgrcv(id, &message, 0, 0, IPC_NOWAIT) != -1 || errno != EACCES) {
      _exit(5);
    }
    errno = 0;
    if (msgctl(id, IPC_STAT, &status) != -1 || errno != EACCES) {
      _exit(6);
    }
    errno = 0;
    if (msgctl(id, IPC_SET, &status) != -1 || errno != EPERM) {
      _exit(7);
    }
    errno = 0;
    _exit(msgctl(id, IPC_RMID, NULL) == -1 && errno == EPERM ? 0 : 8);
  }
  CHECK(wait_child(child), "enforce queue permissions");
  return 0;
}

int ipc_test_messages(void) {
  static const char* names[] = {
      "selection",      "copies",         "send-wait",         "receive-wait", "send-remove",
      "receive-remove", "send-interrupt", "receive-interrupt", "permissions",  "peer-termination"};
  for (unsigned int test = 0; test < sizeof(names) / sizeof(names[0]); ++test) {
    key_t key = (key_t)(0x4d000000U | ((unsigned int)getpid() & 0xffffU) << 8 | test);
    int id = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(id >= 0, "create case queue");
    pid_t child = fork();
    if (!child) {
      alarm(8);
      int result;
      switch (test) {
        case 0:
          result = selection(id);
          break;
        case 1:
          result = copies(id);
          break;
        case 2:
          result = blocking(id, 1, 0, 0);
          break;
        case 3:
          result = blocking(id, 0, 0, 0);
          break;
        case 4:
          result = blocking(id, 1, 1, 0);
          break;
        case 5:
          result = blocking(id, 0, 1, 0);
          break;
        case 6:
          result = interruption(id, 1);
          break;
        case 7:
          result = interruption(id, 0);
          break;
        case 8:
          result = permissions(id);
          break;
        default:
          result = blocking(id, 0, 0, 1);
          break;
      }
      _exit(result);
    }
    int passed = child > 0 && wait_child(child);
    int removed = msgctl(id, IPC_RMID, NULL);
    if (removed && errno != EINVAL) {
      passed = 0;
    }
    printf("IPC-MESSAGES: %s %s\n", passed ? "PASS" : "FAIL", names[test]);
    if (!passed) {
      return 1;
    }
  }
  return 0;
}
