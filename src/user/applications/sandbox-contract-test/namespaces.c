/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>

#define CHECK(expression)                                                                      \
  do {                                                                                         \
    if (!(expression)) {                                                                       \
      fprintf(stderr, "NAMESPACE-CONTRACT: FAIL line=%d %s errno=%d\n", __LINE__, #expression, \
              errno);                                                                          \
      return 1;                                                                                \
    }                                                                                          \
  } while (0)

static pid_t clone_process(unsigned long flags) {
  return syscall(SYS_clone, flags | SIGCHLD, NULL, NULL, NULL, 0);
}

static int reap(pid_t child) {
  int status = 0;
  CHECK(child > 0 && waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return 0;
}

static int pid_child(int report, int ready_read, int ready_write, pid_t outside) {
  CHECK(getpid() == 1 && syscall(SYS_gettid) == 1 && getppid() == 0);
  errno = 0;
  CHECK(kill(outside, 0) == -1 && errno == ESRCH);
  pid_t worker = fork();
  CHECK(worker >= 0);
  if (!worker) {
    CHECK(getpid() == 2 && syscall(SYS_gettid) == 2 && getppid() == 1);
    /* The inherited proc mount still reports IDs in the parent's namespace. */
    char identity[32] = {0};
    ssize_t length = readlink("/proc/self", identity, sizeof(identity) - 1);
    CHECK(length > 0);
    pid_t outer = strtol(identity, NULL, 10);
    CHECK(outer > 2 && write(report, &outer, sizeof(outer)) == sizeof(outer));
    CHECK(write(ready_write, "x", 1) == 1);
    for (;;) {
      pause();
    }
  }
  CHECK(worker == 2);
  char ready;
  CHECK(read(ready_read, &ready, 1) == 1);
  /* Exiting PID 1 must kill its still-running descendant before wait returns. */
  return 0;
}

static int pending_pid_namespace(void) {
  pid_t before = getpid();
  CHECK(unshare(CLONE_NEWPID) == 0 && getpid() == before);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    _exit(getpid() == 1 && syscall(SYS_gettid) == 1 && getppid() == 0 ? 0 : 1);
  }
  CHECK(!reap(child));
  errno = 0;
  CHECK(fork() == -1 && errno == ENOMEM);
  return 0;
}

static int pid_namespace(void) {
  const pid_t before = getpid();
  int report[2], ready[2];
  CHECK(pipe(report) == 0 && pipe(ready) == 0);
  pid_t child = clone_process(CLONE_NEWPID);
  CHECK(child >= 0);
  if (!child) {
    _exit(pid_child(report[1], ready[0], ready[1], before));
  }
  close(report[1]);
  close(ready[0]);
  close(ready[1]);
  pid_t descendant = 0;
  CHECK(read(report[0], &descendant, sizeof(descendant)) == sizeof(descendant));
  close(report[0]);
  CHECK(!reap(child));
  CHECK(getpid() == before && syscall(SYS_gettid) == before);
  errno = 0;
  CHECK(kill(descendant, 0) == -1 && errno == ESRCH);
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    _exit(pending_pid_namespace());
  }
  CHECK(!reap(child));
  puts("NAMESPACE-CONTRACT: pid PASS");
  return 0;
}

struct message {
  long type;
  char value;
};

static int ipc_child(key_t key, int parent_sem, int parent_shm, int parent_msg,
                     int* inherited_mapping, const char* name) {
  errno = 0;
  CHECK(semget(key, 1, 0) == -1 && errno == ENOENT);
  errno = 0;
  CHECK(semctl(parent_sem, 0, GETVAL) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(shmget(key, 4096, 0) == -1 && errno == ENOENT);
  errno = 0;
  CHECK(shmat(parent_shm, NULL, 0) == (void*)-1 && errno == EINVAL);
  errno = 0;
  CHECK(msgget(key, 0) == -1 && errno == ENOENT);
  struct msqid_ds status;
  errno = 0;
  CHECK(msgctl(parent_msg, IPC_STAT, &status) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(mq_open(name, O_RDONLY) == (mqd_t)-1 && errno == ENOENT);

  int sem = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
  int shm = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
  int msg = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
  CHECK(sem >= 0 && shm >= 0 && msg >= 0);
  CHECK(semctl(sem, 0, SETVAL, 7) == 0 && semctl(sem, 0, GETVAL) == 7);
  int* mapping = shmat(shm, NULL, 0);
  CHECK(mapping != (void*)-1);
  *mapping = 8;
  CHECK(*inherited_mapping == 17 && shmdt(mapping) == 0);
  struct mq_attr attr = {.mq_maxmsg = 2, .mq_msgsize = 32};
  mqd_t mq = mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
  CHECK(mq != (mqd_t)-1);
  CHECK(mq_send(mq, "private", 8, 0) == 0);
  char received[32];
  CHECK(mq_receive(mq, received, sizeof(received), NULL) == 8);
  CHECK(!strcmp(received, "private") && mq_close(mq) == 0);
  /* Namespace teardown owns the named objects deliberately left registered. */
  return 0;
}

static int ipc_namespace(void) {
  key_t key = 0x41000000 | getpid();
  char name[64];
  snprintf(name, sizeof(name), "/namespace-%ld", (long)getpid());
  int sem = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
  int shm = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
  int msg = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
  CHECK(sem >= 0 && shm >= 0 && msg >= 0);
  CHECK(semctl(sem, 0, SETVAL, 17) == 0);
  int* mapping = shmat(shm, NULL, 0);
  CHECK(mapping != (void*)-1);
  *mapping = 17;
  struct mq_attr attr = {.mq_maxmsg = 2, .mq_msgsize = 32};
  mqd_t mq = mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
  CHECK(mq != (mqd_t)-1 && mq_send(mq, "parent", 7, 0) == 0);
  pid_t child = clone_process(CLONE_NEWIPC);
  CHECK(child >= 0);
  if (!child) {
    _exit(ipc_child(key, sem, shm, msg, mapping, name));
  }
  CHECK(!reap(child));
  CHECK(semctl(sem, 0, GETVAL) == 17 && *mapping == 17);
  char received[32];
  CHECK(mq_receive(mq, received, sizeof(received), NULL) == 7);
  CHECK(!strcmp(received, "parent"));
  CHECK(mq_close(mq) == 0 && mq_unlink(name) == 0);
  CHECK(semctl(sem, 0, IPC_RMID) == 0 && msgctl(msg, IPC_RMID, NULL) == 0);
  CHECK(shmdt(mapping) == 0 && shmctl(shm, IPC_RMID, NULL) == 0);
  puts("NAMESPACE-CONTRACT: ipc PASS");
  return 0;
}

static int write_file(const char* path, const char* value) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(fd >= 0);
  CHECK(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
  CHECK(close(fd) == 0);
  return 0;
}

static int write_control(const char* path, const char* value) {
  int fd = open(path, O_WRONLY);
  CHECK(fd >= 0);
  CHECK(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
  CHECK(close(fd) == 0);
  return 0;
}

static int map_user(uid_t uid, gid_t gid) {
  char map[64];
  snprintf(map, sizeof(map), "0 %u 1\n", uid);
  CHECK(!write_control("/proc/self/uid_map", map));
  CHECK(!write_control("/proc/self/setgroups", "deny\n"));
  snprintf(map, sizeof(map), "0 %u 1\n", gid);
  CHECK(!write_control("/proc/self/gid_map", map));
  CHECK(getuid() == 0 && getgid() == 0);
  return 0;
}

static int mount_child(const char* source, const char* target, const char* writable, uid_t uid,
                       gid_t gid, int inherited_proc) {
  int self = openat(inherited_proc, "self", O_PATH | O_DIRECTORY);
  CHECK(self >= 0);
  int uid_map = openat(self, "uid_map", O_RDONLY);
  CHECK(uid_map >= 0);
  CHECK(close(uid_map) == 0 && close(self) == 0 && close(inherited_proc) == 0);
  CHECK(!map_user(uid, gid));
  CHECK(mount(source, target, NULL, MS_BIND, NULL) == 0);
  char file[512], workspace[512], output[1024];
  snprintf(file, sizeof(file), "%s/sentinel", target);
  snprintf(workspace, sizeof(workspace), "%s/work", target);
  snprintf(output, sizeof(output), "%s/output", workspace);
  int held = open(file, O_WRONLY);
  CHECK(held >= 0);
  errno = 0;
  CHECK(mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) == -1 && errno == EBUSY);
  CHECK(close(held) == 0);
  CHECK(mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) == 0);
  CHECK(mount(writable, workspace, NULL, MS_BIND, NULL) == 0);
  errno = 0;
  CHECK(open(file, O_WRONLY | O_TRUNC) == -1 && errno == EROFS);
  CHECK(!write_file(output, "workspace"));
  pid_t child = clone_process(CLONE_NEWUSER | CLONE_NEWNS);
  CHECK(child >= 0);
  if (!child) {
    errno = 0;
    int result = mount(NULL, target, NULL, MS_BIND | MS_REMOUNT, NULL);
    _exit(result == -1 && errno == EPERM ? 0 : 1);
  }
  CHECK(!reap(child));
  return 0;
}

static int mount_namespace(void) {
  char base[256], source[320], target[320], writable[320], work[384], file[384], output[384];
  snprintf(base, sizeof(base), "/tmp/namespace-contract-%ld", (long)getpid());
  snprintf(source, sizeof(source), "%s/source", base);
  snprintf(target, sizeof(target), "%s/target", base);
  snprintf(writable, sizeof(writable), "%s/writable", base);
  snprintf(work, sizeof(work), "%s/work", source);
  snprintf(file, sizeof(file), "%s/sentinel", source);
  snprintf(output, sizeof(output), "%s/output", writable);
  CHECK(mkdir(base, 0700) == 0 && mkdir(source, 0700) == 0 && mkdir(target, 0700) == 0);
  CHECK(mkdir(writable, 0700) == 0 && mkdir(work, 0700) == 0);
  CHECK(!write_file(file, "parent"));
  uid_t uid = getuid();
  gid_t gid = getgid();
  int inherited_proc = open("/proc", O_PATH | O_DIRECTORY);
  CHECK(inherited_proc >= 0);
  pid_t child = clone_process(CLONE_NEWUSER | CLONE_NEWNS);
  CHECK(child >= 0);
  if (!child) {
    _exit(mount_child(source, target, writable, uid, gid, inherited_proc));
  }
  CHECK(close(inherited_proc) == 0);
  CHECK(!reap(child));
  CHECK(!write_file(file, "parent-still-writable"));
  char hidden[384];
  snprintf(hidden, sizeof(hidden), "%s/sentinel", target);
  errno = 0;
  CHECK(access(hidden, F_OK) == -1 && errno == ENOENT);
  CHECK(access(output, F_OK) == 0);
  CHECK(unlink(output) == 0 && unlink(file) == 0 && rmdir(work) == 0);
  CHECK(rmdir(writable) == 0 && rmdir(target) == 0 && rmdir(source) == 0 && rmdir(base) == 0);
  puts("NAMESPACE-CONTRACT: mounts PASS");
  return 0;
}

static int route_request(int route, void* request, size_t length) {
  struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
  CHECK(sendto(route, request, length, 0, (struct sockaddr*)&kernel, sizeof(kernel)) ==
        (ssize_t)length);
  char response[512];
  ssize_t received = recv(route, response, sizeof(response), 0);
  CHECK(received >= (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)));
  struct nlmsghdr* header = (struct nlmsghdr*)response;
  CHECK(header->nlmsg_type == NLMSG_ERROR && header->nlmsg_len <= (unsigned)received);
  struct nlmsgerr* error = NLMSG_DATA(header);
  CHECK(error->error == 0);
  return 0;
}

static int configure_loopback(void) {
  int index = if_nametoindex("lo");
  CHECK(index > 0);
  int route = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  CHECK(route >= 0);
  struct sockaddr_nl local = {.nl_family = AF_NETLINK, .nl_pid = getpid()};
  CHECK(bind(route, (struct sockaddr*)&local, sizeof(local)) == 0);
  char buffer[128] = {0};
  struct nlmsghdr* header = (struct nlmsghdr*)buffer;
  header->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
  header->nlmsg_type = RTM_NEWADDR;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
  header->nlmsg_seq = 1;
  struct ifaddrmsg* address = NLMSG_DATA(header);
  address->ifa_family = AF_INET;
  address->ifa_prefixlen = 8;
  address->ifa_scope = RT_SCOPE_HOST;
  address->ifa_index = index;
  for (int field = IFA_ADDRESS; field <= IFA_LOCAL; ++field) {
    struct rtattr* attribute = (struct rtattr*)(buffer + NLMSG_ALIGN(header->nlmsg_len));
    attribute->rta_type = field;
    attribute->rta_len = RTA_LENGTH(sizeof(uint32_t));
    uint32_t loopback = htonl(INADDR_LOOPBACK);
    memcpy(RTA_DATA(attribute), &loopback, sizeof(loopback));
    header->nlmsg_len = NLMSG_ALIGN(header->nlmsg_len) + RTA_ALIGN(attribute->rta_len);
  }
  CHECK(!route_request(route, buffer, header->nlmsg_len));
  memset(buffer, 0, sizeof(buffer));
  header->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
  header->nlmsg_type = RTM_NEWLINK;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  header->nlmsg_seq = 2;
  struct ifinfomsg* link = NLMSG_DATA(header);
  link->ifi_index = index;
  link->ifi_flags = link->ifi_change = IFF_UP;
  CHECK(!route_request(route, buffer, header->nlmsg_len));
  CHECK(close(route) == 0);
  return 0;
}

static int inet_socket(int type, unsigned port) {
  int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_in address = {
      .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(fd, (struct sockaddr*)&address, sizeof(address)) < 0 ||
      (type == SOCK_STREAM && listen(fd, 4) < 0)) {
    close(fd);
    return -1;
  }
  return fd;
}

static int network_child(unsigned port, const struct sockaddr_un* abstract, socklen_t extent) {
  CHECK(!configure_loopback());
  struct sockaddr_in address = {
      .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  int isolated = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  CHECK(isolated >= 0);
  errno = 0;
  CHECK(connect(isolated, (struct sockaddr*)&address, sizeof(address)) == -1 &&
        (errno == ECONNREFUSED || errno == ENETUNREACH || errno == EHOSTUNREACH));
  CHECK(close(isolated) == 0);
  isolated = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  CHECK(isolated >= 0);
  errno = 0;
  CHECK(connect(isolated, (const struct sockaddr*)abstract, extent) == -1 &&
        (errno == ECONNREFUSED || errno == ENOENT));
  CHECK(bind(isolated, (const struct sockaddr*)abstract, extent) == 0);
  CHECK(close(isolated) == 0);

  int listener = inet_socket(SOCK_STREAM, port);
  int udp = inet_socket(SOCK_DGRAM, port);
  CHECK(listener >= 0 && udp >= 0);
  pid_t client = fork();
  CHECK(client >= 0);
  if (!client) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0 || connect(socket_fd, (struct sockaddr*)&address, sizeof(address)) ||
        write(socket_fd, "private", 8) != 8) {
      _exit(1);
    }
    char reply;
    _exit(read(socket_fd, &reply, 1) == 1 && reply == 'x' ? 0 : 1);
  }
  int accepted = accept(listener, NULL, NULL);
  CHECK(accepted >= 0);
  char received[32];
  CHECK(read(accepted, received, sizeof(received)) == 8 && !strcmp(received, "private"));
  CHECK(write(accepted, "x", 1) == 1 && close(accepted) == 0);
  CHECK(!reap(client));
  int sender = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(sender >= 0);
  CHECK(sendto(sender, "datagram", 9, 0, (struct sockaddr*)&address, sizeof(address)) == 9);
  CHECK(recv(udp, received, sizeof(received), 0) == 9 && !strcmp(received, "datagram"));
  CHECK(close(sender) == 0 && close(udp) == 0 && close(listener) == 0);
  return 0;
}

int namespace_network_test(void) {
  unsigned port = 34000 + (unsigned)getpid() % 20000;
  int listener = inet_socket(SOCK_STREAM, port);
  int udp = inet_socket(SOCK_DGRAM, port);
  CHECK(listener >= 0 && udp >= 0);
  struct sockaddr_un abstract = {.sun_family = AF_UNIX};
  snprintf(abstract.sun_path + 1, sizeof(abstract.sun_path) - 1, "namespace-%ld", (long)getpid());
  socklen_t extent = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(abstract.sun_path + 1);
  int named = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(named >= 0 && bind(named, (struct sockaddr*)&abstract, extent) == 0);
  CHECK(listen(named, 4) == 0);
  pid_t child = clone_process(CLONE_NEWNET);
  CHECK(child >= 0);
  if (!child) {
    close(listener);
    close(udp);
    close(named);
    _exit(network_child(port, &abstract, extent));
  }
  CHECK(!reap(child));
  char received[32];
  CHECK(fcntl(udp, F_SETFL, O_NONBLOCK) == 0);
  errno = 0;
  CHECK(recv(udp, received, sizeof(received), 0) == -1 && errno == EAGAIN);
  CHECK(close(listener) == 0 && close(udp) == 0 && close(named) == 0);
  puts("NAMESPACE-CONTRACT: network PASS");
  return 0;
}

int namespace_tests(void) {
  if (pid_namespace() || ipc_namespace() || mount_namespace() || namespace_network_test()) {
    puts("NAMESPACE-CONTRACT: FAIL");
    return 1;
  }
  puts("NAMESPACE-CONTRACT: PASS");
  return 0;
}
