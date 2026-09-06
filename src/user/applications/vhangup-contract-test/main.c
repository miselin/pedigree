/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define CHECK(expression)                                                            \
  do {                                                                               \
    if (!(expression)) {                                                             \
      fprintf(stderr, "VHANGUP-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      return 1;                                                                      \
    }                                                                                \
  } while (0)

static volatile sig_atomic_t hangups, continuations;
static volatile sig_atomic_t control_interrupts, control_error;
static int control_terminal = -1;

static void control_handler(int signal) {
  (void)signal;
  int saved_errno = errno;
  struct winsize size;
  if (ioctl(control_terminal, TIOCGWINSZ, &size) || tcgetpgrp(control_terminal) != getpgrp() ||
      tcsetpgrp(control_terminal, getpgrp()) || syscall(SYS_vhangup))
    control_error = 1;
  ++control_interrupts;
  errno = saved_errno;
}
static void signal_handler(int signal) {
  if (signal == SIGHUP)
    ++hangups;
  if (signal == SIGCONT)
    ++continuations;
}

struct blocked_io {
  int fd;
  int writing;
  ssize_t result;
  int error;
};

static void* blocked_io(void* argument) {
  struct blocked_io* io = argument;
  char data[16] = "blocked";
  io->result = io->writing ? write(io->fd, data, sizeof(data)) : read(io->fd, data, sizeof(data));
  io->error = errno;
  return NULL;
}

static int transferred_fd(int fd) {
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
    return -1;
  union {
    struct cmsghdr alignment;
    char data[CMSG_SPACE(sizeof(int))];
  } control = {0};
  char byte = 'f';
  struct iovec vector = {&byte, 1};
  struct msghdr message = {.msg_iov = &vector,
                           .msg_iovlen = 1,
                           .msg_control = control.data,
                           .msg_controllen = sizeof(control)};
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(header), &fd, sizeof(fd));
  int received = -1;
  if (sendmsg(sockets[0], &message, 0) == 1 && recvmsg(sockets[1], &message, 0) == 1) {
    header = CMSG_FIRSTHDR(&message);
    if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS)
      memcpy(&received, CMSG_DATA(header), sizeof(received));
  }
  close(sockets[0]);
  close(sockets[1]);
  return received;
}

static int revoked_descriptor(int fd) {
  char byte;
  struct winsize size;
  CHECK(read(fd, &byte, 1) == 0);
  CHECK(write(fd, "x", 1) == -1 && errno == EIO);
  CHECK(ioctl(fd, TIOCGWINSZ, &size) == -1 && errno == EIO);
  CHECK(tcsetpgrp(fd, getpgrp()) == -1 && errno == ENOTTY);
  struct pollfd pollfd = {.fd = fd, .events = POLLIN | POLLOUT};
  CHECK(poll(&pollfd, 1, 0) == 1);
  CHECK((pollfd.revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) ==
        (POLLIN | POLLOUT | POLLERR | POLLHUP));
  return 0;
}

static int control_character_test(void) {
  puts("VHANGUP-CONTRACT: control-character");
  int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
  CHECK(master >= 0);
  unsigned int number;
  CHECK(ioctl(master, TIOCGPTN, &number) == 0);
  char path[64];
  snprintf(path, sizeof(path), "/dev/pts/%u", number);
  int slave = open(path, O_RDWR | O_NOCTTY);
  CHECK(slave >= 0 && ioctl(slave, TIOCSCTTY, 0) == 0);
  struct termios attributes;
  CHECK(tcgetattr(slave, &attributes) == 0);
  cfmakeraw(&attributes);
  attributes.c_lflag |= ISIG;
  CHECK(tcsetattr(slave, TCSANOW, &attributes) == 0);
  struct sigaction action = {.sa_handler = control_handler};
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGINT, &action, NULL) == 0);
  control_terminal = slave;
  char interrupt = attributes.c_cc[VINTR];
  CHECK(interrupt && write(master, &interrupt, 1) == 1);
  for (int i = 0; i < 100 && !control_interrupts; ++i)
    usleep(10000);
  CHECK(control_interrupts == 1 && !control_error);
  CHECK(hangups == 1 && continuations == 1);
  CHECK(revoked_descriptor(slave) == 0);
  CHECK(open("/dev/tty", O_RDWR) == -1 && errno == ENXIO);
  CHECK(close(slave) == 0 && close(master) == 0);
  control_terminal = -1;
  hangups = continuations = 0;
  puts("VHANGUP-CONTRACT: control-character-reentry-pass");
  return 0;
}

static int session_test(void) {
  alarm(25);
  CHECK(setsid() == getpid());
  CHECK(syscall(SYS_vhangup) == 0);
  pid_t unprivileged = fork();
  CHECK(unprivileged >= 0);
  if (!unprivileged) {
    if (seteuid(1000))
      _exit(2);
    _exit(syscall(SYS_vhangup) == -1 && errno == EPERM ? 0 : 3);
  }
  int status;
  CHECK(waitpid(unprivileged, &status, 0) == unprivileged && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  struct sigaction action = {.sa_handler = signal_handler};
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGHUP, &action, NULL) == 0 && sigaction(SIGCONT, &action, NULL) == 0);
  CHECK(control_character_test() == 0);

  puts("VHANGUP-CONTRACT: private-pty");
  int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
  CHECK(master >= 0);
  unsigned int number;
  CHECK(ioctl(master, TIOCGPTN, &number) == 0);
  char path[64];
  snprintf(path, sizeof(path), "/dev/pts/%u", number);
  int slave = open(path, O_RDWR | O_NOCTTY);
  CHECK(slave >= 0);
  CHECK(open("/dev/tty", O_RDWR) == -1 && errno == ENXIO);
  CHECK(ioctl(slave, TIOCSCTTY, 0) == 0);
  CHECK(tcgetpgrp(slave) == getpgrp());
  struct termios attributes;
  CHECK(tcgetattr(slave, &attributes) == 0);
  cfmakeraw(&attributes);
  CHECK(tcsetattr(slave, TCSANOW, &attributes) == 0);
  int duplicate = dup(slave), passed = transferred_fd(slave);
  CHECK(duplicate >= 0 && passed >= 0);

  pid_t foreign = fork();
  CHECK(foreign >= 0);
  if (!foreign) {
    if (setsid() != getpid())
      _exit(4);
    int other = open(path, O_RDWR | O_NOCTTY);
    if (other < 0)
      _exit(5);
    _exit(ioctl(other, TIOCSCTTY, 0) == -1 && errno == EPERM ? 0 : 6);
  }
  CHECK(waitpid(foreign, &status, 0) == foreign && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  puts("VHANGUP-CONTRACT: foreign-session-refused");
  CHECK(tcsetpgrp(slave, 0) == -1 && errno == EPERM);

  int commands[2], ready[2];
  CHECK(pipe(commands) == 0 && pipe(ready) == 0);
  puts("VHANGUP-CONTRACT: foreground-fork");
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(commands[1]);
    close(ready[0]);
    if (setpgid(0, 0) || write(ready[1], "r", 1) != 1)
      _exit(7);
    char byte;
    if (read(commands[0], &byte, 1) != 1)
      _exit(8);
    if (hangups || continuations || open("/dev/tty", O_RDWR) != -1 || errno != ENXIO ||
        revoked_descriptor(slave))
      _exit(9);
    _exit(0);
  }
  close(commands[0]);
  close(ready[1]);
  char byte;
  CHECK(read(ready[0], &byte, 1) == 1);
  puts("VHANGUP-CONTRACT: foreground-assign");
  CHECK(tcsetpgrp(slave, child) == 0 && tcgetpgrp(slave) == child);

  int epoll = epoll_create1(EPOLL_CLOEXEC);
  CHECK(epoll >= 0);
  struct epoll_event event = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.fd = slave};
  puts("VHANGUP-CONTRACT: epoll-add");
  CHECK(epoll_ctl(epoll, EPOLL_CTL_ADD, slave, &event) == 0);
  puts("VHANGUP-CONTRACT: epoll-initial");
  CHECK(epoll_wait(epoll, &event, 1, 0) == 1 && (event.events & EPOLLOUT));
  CHECK(fcntl(slave, F_SETFL, O_NONBLOCK) == 0);
  char fill[512];
  memset(fill, 'q', sizeof(fill));
  puts("VHANGUP-CONTRACT: fill-output");
  size_t total = 0;
  for (;;) {
    ssize_t amount = write(slave, fill, sizeof(fill));
    if (amount < 0) {
      CHECK(errno == EAGAIN);
      break;
    }
    CHECK(amount > 0);
    total += amount;
    CHECK(total < 16 * 1024 * 1024);
  }
  puts("VHANGUP-CONTRACT: output-full");
  CHECK(fcntl(slave, F_SETFL, 0) == 0);
  struct blocked_io reader = {.fd = duplicate, .result = -2};
  struct blocked_io writer = {.fd = passed, .writing = 1, .result = -2};
  pthread_t read_thread, write_thread;
  CHECK(pthread_create(&read_thread, NULL, blocked_io, &reader) == 0);
  CHECK(pthread_create(&write_thread, NULL, blocked_io, &writer) == 0);
  usleep(50000);
  puts("VHANGUP-CONTRACT: revoke");
  CHECK(syscall(SYS_vhangup) == 0);
  CHECK(pthread_join(read_thread, NULL) == 0 && pthread_join(write_thread, NULL) == 0);
  CHECK(reader.result == 0 && writer.result == -1 && writer.error == EIO);
  CHECK(hangups == 1 && continuations == 1);
  CHECK(revoked_descriptor(slave) == 0 && revoked_descriptor(duplicate) == 0 &&
        revoked_descriptor(passed) == 0);
  CHECK(epoll_wait(epoll, &event, 1, 0) == 1 &&
        (event.events & (EPOLLIN | EPOLLERR | EPOLLHUP)) == (EPOLLIN | EPOLLERR | EPOLLHUP));
  CHECK(epoll_wait(epoll, &event, 1, 0) == 0);
  CHECK(open("/dev/tty", O_RDWR) == -1 && errno == ENXIO);
  CHECK(write(commands[1], "g", 1) == 1);
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

  puts("VHANGUP-CONTRACT: reopen");
  int fresh = open(path, O_RDWR | O_NOCTTY);
  CHECK(fresh >= 0);
  struct pollfd pollfd = {.fd = fresh, .events = POLLIN | POLLOUT};
  CHECK(poll(&pollfd, 1, 0) == 1 && !(pollfd.revents & (POLLERR | POLLHUP)));
  CHECK(tcgetattr(fresh, &attributes) == 0 && (attributes.c_lflag & ICANON));
  cfmakeraw(&attributes);
  CHECK(tcsetattr(fresh, TCSANOW, &attributes) == 0);
  CHECK(write(master, "new", 3) == 3);
  char data[4];
  CHECK(read(fresh, data, 3) == 3 && !memcmp(data, "new", 3));
  CHECK(write(fresh, "out", 3) == 3);
  CHECK(read(master, data, 3) == 3 && !memcmp(data, "out", 3));
  CHECK(ioctl(fresh, TIOCSCTTY, 0) == 0 && tcgetpgrp(fresh) == getpgrp());
  CHECK(revoked_descriptor(slave) == 0);
  CHECK(syscall(SYS_vhangup) == 0);
  CHECK(close(fresh) == 0 && close(master) == 0 && close(slave) == 0 && close(duplicate) == 0 &&
        close(passed) == 0 && close(epoll) == 0);
  close(commands[1]);
  close(ready[0]);
  return 0;
}

static int session_exit_test(void) {
  puts("VHANGUP-CONTRACT: session-exit");
  int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
  CHECK(master >= 0);
  unsigned int number;
  CHECK(ioctl(master, TIOCGPTN, &number) == 0);
  char path[64];
  snprintf(path, sizeof(path), "/dev/pts/%u", number);
  int slave = open(path, O_RDWR | O_NOCTTY);
  CHECK(slave >= 0);
  pid_t leader = fork();
  CHECK(leader >= 0);
  if (!leader) {
    if (setsid() != getpid() || ioctl(slave, TIOCSCTTY, 0))
      _exit(2);
    _exit(0);
  }
  int status;
  CHECK(waitpid(leader, &status, 0) == leader && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(revoked_descriptor(slave) == 0);
  int fresh = open(path, O_RDWR | O_NOCTTY);
  CHECK(fresh >= 0);
  struct termios attributes;
  CHECK(tcgetattr(fresh, &attributes) == 0);
  cfmakeraw(&attributes);
  CHECK(tcsetattr(fresh, TCSANOW, &attributes) == 0);
  CHECK(write(master, "e", 1) == 1);
  char byte;
  CHECK(read(fresh, &byte, 1) == 1 && byte == 'e');
  CHECK(close(fresh) == 0 && close(slave) == 0 && close(master) == 0);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(30);
  CHECK(geteuid() == 0);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child)
    _exit(session_test());
  int status;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(session_exit_test() == 0);
  puts("VHANGUP-CONTRACT: PASS");
  return 0;
}
