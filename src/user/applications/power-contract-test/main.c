/* Copyright (c) 2026, Pedigree Developers. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/wait.h>

static int failures;

static void check(int condition, const char* description) {
  printf("POWER-CONTRACT: %s %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition)
    ++failures;
}

static int call(uint32_t magic1, uint32_t magic2, uint32_t command) {
  errno = 0;
  return syscall(SYS_reboot, magic1, magic2, command, 0);
}

int main(void) {
  if (geteuid() != 0) {
    fprintf(stderr, "power-contract-test must run as root\n");
    return 1;
  }
  check(call(0, 672274793, 0x01234567) == -1 && errno == EINVAL, "invalid first magic");
  check(call(0xfee1dead, 0, 0x01234567) == -1 && errno == EINVAL, "invalid second magic");
  check(call(0xfee1dead, 672274793, 0x123) == -1 && errno == EINVAL, "unknown command");
  check(call(0xfee1dead, 672274793, 0xa1b2c3d4) == -1 && errno == EINVAL, "unsupported restart2");
  check(call(0xfee1dead, 672274793, 0xd000fce2) == -1 && errno == EINVAL, "unsupported suspend");
  check(call(0xfee1dead, 672274793, 0x45584543) == -1 && errno == EINVAL, "unsupported kexec");
  const uint32_t second[] = {672274793, 85072278, 369367448, 537993216};
  for (unsigned i = 0; i < sizeof(second) / sizeof(second[0]); ++i) {
    check(call(0xfee1dead, second[i], 0) == 0, "CAD_OFF is nonterminal");
    check(call(0xfee1dead, second[i], 0x89abcdef) == 0, "CAD_ON is nonterminal");
  }
  fflush(stdout);
  pid_t child = fork();
  if (child == 0) {
    if (setuid(65534) != 0)
      _exit(1);
    const uint32_t commands[] = {0x01234567, 0xcdef0123, 0x4321fedc, 0, 0x89abcdef};
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
      if (call(0xfee1dead, 672274793, commands[i]) != -1 || errno != EPERM)
        _exit(1);
    _exit(0);
  }
  int status = 0;
  check(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0,
        "unprivileged commands denied");
  printf("POWER-CONTRACT: END failures=%d\n", failures);
  return failures != 0;
}
