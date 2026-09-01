#include <errno.h>
#include <pwd.h>
#include <string.h>

#include <pedigree/kernel/processor/Syscalls.h>
#include <posix-syscall.h>
#include <posixSyscallNumbers.h>

static size_t passwdStringSize(const struct passwd* passwd) {
  return strlen(passwd->pw_name) + 1 + strlen(passwd->pw_passwd) + 1 +
         strlen(passwd->pw_gecos) + 1 + strlen(passwd->pw_dir) + 1 +
         strlen(passwd->pw_shell) + 1;
}

static char* copyPasswdString(char** destination, const char* source) {
  size_t length = strlen(source) + 1;
  char* result = *destination;
  memcpy(result, source, length);
  *destination += length;
  return result;
}

static int getpw_r(const char* name, uid_t uid, struct passwd* passwd, char* buffer, size_t size,
                   struct passwd** result) {
  struct passwd source;
  char sourceStrings[256];
  long syscallResult;

  *result = 0;
  if (name) {
    syscallResult = syscall3(POSIX_GETPWNAM, (long) &source, (long) name,
                              (long) &sourceStrings);
  } else {
    syscallResult = syscall3(POSIX_GETPWENT, (long) &source, (long) uid,
                              (long) &sourceStrings);
  }

  if (syscallResult != 0)
    return errno ? errno : ENOENT;

  if (passwdStringSize(&source) > size)
    return ERANGE;

  char* destination = buffer;
  passwd->pw_name = copyPasswdString(&destination, source.pw_name);
  passwd->pw_passwd = copyPasswdString(&destination, source.pw_passwd);
  passwd->pw_uid = source.pw_uid;
  passwd->pw_gid = source.pw_gid;
  passwd->pw_gecos = copyPasswdString(&destination, source.pw_gecos);
  passwd->pw_dir = copyPasswdString(&destination, source.pw_dir);
  passwd->pw_shell = copyPasswdString(&destination, source.pw_shell);
  *result = passwd;
  return 0;
}

int getpwnam_r(const char* name, struct passwd* passwd, char* buffer, size_t size,
              struct passwd** result) {
  return getpw_r(name, 0, passwd, buffer, size, result);
}

int getpwuid_r(uid_t uid, struct passwd* passwd, char* buffer, size_t size,
               struct passwd** result) {
  return getpw_r(0, uid, passwd, buffer, size, result);
}
