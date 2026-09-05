/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));

static void require(int condition, const char* operation) {
  if (!condition) {
    printf("SYSCALL-CONTRACT: FAIL %s errno=%d\n", operation, errno);
    fail();
  }
}

static void file_contracts(void) {
  char path[80];
  snprintf(path, sizeof(path), "/tmp/syscall-contract-%d", getpid());
  int file = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
  require(file >= 0, "create");
  require(write(file, "data", 4) == 4, "write");
  struct stat status;
  require(fstat(file, &status) == 0 && status.st_size == 4 && S_ISREG(status.st_mode) &&
              status.st_blocks == 1,
          "file metadata");
  close(file);

  char data[4] = {0};
  struct iovec vector = {.iov_base = data, .iov_len = sizeof(data)};
  file = open(path, O_RDONLY | O_CLOEXEC);
  require(file >= 0, "read-only open");
  require(read(file, data, sizeof(data)) == 4 && !memcmp(data, "data", 4), "read");
  errno = 0;
  require(write(file, data, 1) == -1 && errno == EBADF, "read-only write");
  errno = 0;
  require(writev(file, &vector, 1) == -1 && errno == EBADF, "read-only writev");
  errno = 0;
  require(write(file, data, 0) == -1 && errno == EBADF, "read-only empty write");
  require(lseek(file, 0, SEEK_SET) == 0 && readv(file, &vector, 1) == 4, "readv");
  close(file);

  file = open(path, O_WRONLY | O_CLOEXEC);
  require(file >= 0, "write-only open");
  errno = 0;
  require(read(file, data, 1) == -1 && errno == EBADF, "write-only read");
  errno = 0;
  require(readv(file, &vector, 1) == -1 && errno == EBADF, "write-only readv");
  errno = 0;
  require(read(file, data, 0) == -1 && errno == EBADF, "write-only empty read");
  require(writev(file, &vector, 1) == 4, "writev");
  close(file);
  require(unlink(path) == 0, "unlink");

  int descriptors[2];
  require(pipe(descriptors) == 0, "pipe");
  require(fstat(descriptors[0], &status) == 0 && S_ISFIFO(status.st_mode), "pipe metadata");
  close(descriptors[0]);
  close(descriptors[1]);
  puts("SYSCALL-CONTRACT: PASS file-modes-and-stat");
}

static void socket_name_contracts(void) {
  int sockets[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair");
  for (int peer = 0; peer < 2; ++peer) {
    struct sockaddr_storage complete;
    memset(&complete, 0, sizeof(complete));
    socklen_t full_length = sizeof(complete);
    int result = peer ? getpeername(sockets[0], (struct sockaddr*)&complete, &full_length)
                      : getsockname(sockets[0], (struct sockaddr*)&complete, &full_length);
    require(result == 0 && full_length == sizeof(sa_family_t) && complete.ss_family == AF_UNIX,
            "complete socket name");

    struct sockaddr_storage short_name;
    memset(&short_name, 0xA5, sizeof(short_name));
    socklen_t capacity = 1;
    result = peer ? getpeername(sockets[0], (struct sockaddr*)&short_name, &capacity)
                  : getsockname(sockets[0], (struct sockaddr*)&short_name, &capacity);
    require(result == 0 && capacity == full_length &&
                ((unsigned char*)&short_name)[0] == ((unsigned char*)&complete)[0],
            "short socket name");
    for (size_t i = 1; i < sizeof(short_name); ++i)
      require(((unsigned char*)&short_name)[i] == 0xA5, "socket name capacity");

    capacity = 0;
    result =
        peer ? getpeername(sockets[0], NULL, &capacity) : getsockname(sockets[0], NULL, &capacity);
    require(result == 0 && capacity == full_length, "zero-capacity socket name");
  }
  close(sockets[0]);
  close(sockets[1]);
  puts("SYSCALL-CONTRACT: PASS socket-name-capacity");
}

static void termios_contracts(void) {
  int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
  require(master >= 0, "open PTY");
  unsigned int terminal = 0;
  unsigned int raw_terminal = 0;
  require(ioctl(master, TIOCGPTN, &terminal) == 0, "ioctl signed request encoding");
  require(syscall(SYS_ioctl, master, (unsigned long)TIOCGPTN, &raw_terminal) == 0 &&
              terminal == raw_terminal,
          "ioctl raw request encoding");
  char path[80];
  require(ptsname_r(master, path, sizeof(path)) == 0, "PTY name");
  int slave = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  require(slave >= 0, "open PTY slave");

  struct termios attributes;
  memset(&attributes, 0xA5, sizeof(attributes));
  require(tcgetattr(slave, &attributes) == 0, "tcgetattr");
  require(attributes.c_cc[VINTR] == 3 && attributes.c_cc[VERASE] == 8 &&
              attributes.c_cc[VEOF] == 4 && attributes.c_cc[VSUSP] == 26,
          "termios default character indices");
  attributes.c_lflag |= ICANON;
  attributes.c_lflag &= ~(ECHO | ISIG);
  attributes.c_cc[VEOF] = '~';
  require(tcsetattr(slave, TCSANOW, &attributes) == 0, "tcsetattr");
  require(write(master, "~", 1) == 1, "PTY EOF input");
  char character;
  require(read(slave, &character, 1) == 0, "termios EOF character behavior");
  close(slave);
  close(master);
  puts("SYSCALL-CONTRACT: PASS termios-musl-layout");
}

static volatile sig_atomic_t caught_signal;

static void caught(int signal) {
  (void)signal;
  caught_signal = 1;
}

static void process_contracts(void) {
  errno = 0;
  require(getpriority(PRIO_PROCESS, 0) == 0 && errno == 0, "getpriority libc encoding");
  require(getpriority(PRIO_PROCESS, getpid()) == 0, "getpriority process target");
  require(syscall(SYS_getpriority, PRIO_PROCESS, 0) == 20, "getpriority raw encoding");
  errno = 0;
  require(getpriority(-1, 0) == -1 && errno == EINVAL, "getpriority selector");

  struct sigaction action = {0};
  struct sigaction previous;
  action.sa_handler = caught;
  sigemptyset(&action.sa_mask);
  require(sigaction(SIGUSR1, &action, &previous) == 0, "wait signal disposition");
  caught_signal = 0;
  pid_t child = fork();
  require(child >= 0, "wait child fork");
  if (!child) {
    usleep(250000);
    kill(getppid(), SIGUSR1);
    usleep(250000);
    _exit(0);
  }

  int status = 0;
  errno = 0;
  pid_t result = waitpid(child, &status, 0);
  int interrupted = result == -1 && errno == EINTR && caught_signal;
  if (result != child) {
    do {
      result = waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);
  }
  require(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "reap wait child");
  require(sigaction(SIGUSR1, &previous, NULL) == 0, "restore signal disposition");
  require(interrupted, "caught signal interrupts waitpid");
  puts("SYSCALL-CONTRACT: PASS priority-and-wait-interruption");
}

void test_syscall_contracts(void) {
  file_contracts();
  socket_name_contracts();
  termios_contracts();
  process_contracts();
  puts("SYSCALL-CONTRACT: PASS all");
}
