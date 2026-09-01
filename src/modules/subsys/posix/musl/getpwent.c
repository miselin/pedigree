#include <errno.h>
#include <pwd.h>

#include <pedigree/kernel/processor/Syscalls.h>
#include <posix-syscall.h>
#include <posixSyscallNumbers.h>

static struct passwd g_Passwd;
static int g_PasswdNumber;
static char g_PasswdStrings[256];

void setpwent(void) {
  g_PasswdNumber = 0;
}

void endpwent(void) {
  g_PasswdNumber = 0;
}

struct passwd* getpwent(void) {
  if (syscall3(POSIX_GETPWENT, (long) &g_Passwd, g_PasswdNumber, (long) &g_PasswdStrings) != 0)
    return 0;

  g_PasswdNumber++;
  return &g_Passwd;
}

struct passwd* getpwuid(uid_t uid) {
  if (syscall3(POSIX_GETPWENT, (long) &g_Passwd, (long) uid, (long) &g_PasswdStrings) != 0)
    return 0;

  return &g_Passwd;
}

struct passwd* getpwnam(const char* name) {
  if (syscall3(POSIX_GETPWNAM, (long) &g_Passwd, (long) name, (long) &g_PasswdStrings) != 0)
    return 0;

  return &g_Passwd;
}
