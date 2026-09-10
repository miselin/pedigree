// SPDX-License-Identifier: ISC
// Native filesystem errors exercise the production syscall's error selection.
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include <sys/stat.h>

namespace Error {
constexpr int DoesNotExist = ENOENT;
}
struct Thread {
  int error = 0;
  int getErrno() const {
    return error;
  }
};
Thread current;
struct Processor {
  static Processor& information() {
    static Processor processor;
    return processor;
  }
  Thread* getCurrentThread() {
    return &current;
  }
};
#define F_NOTICE(...) \
  do {                \
  } while (false)
#define SYSCALL_ERROR(value) (current.error = Error::value)

struct String : std::string {
  const char* cstr() const {
    return c_str();
  }
};
struct ResolvedPath {};
struct File {
  std::string path;
};
File selected;
int forcedLookupError = -1;
size_t lookups = 0;

bool copyUserString(const char* path, String& copy) {
  if (!path) {
    current.error = EFAULT;
    return false;
  }
  copy.assign(path);
  return true;
}
void normalisePath(String& copy, const char* path) {
  copy.assign(path);
}
File* findFilePath(const String& path, ResolvedPath&) {
  ++lookups;
  if (forcedLookupError >= 0) {
    current.error = forcedLookupError;
    return nullptr;
  }
  struct stat status;
  if (stat(path.cstr(), &status)) {
    current.error = errno;
    return nullptr;
  }
  selected.path = path;
  return &selected;
}
bool doChdir(File* file, ResolvedPath&) {
  if (chdir(file->path.c_str())) {
    current.error = errno;
    return false;
  }
  return true;
}

#include "posix-chdir.inc"

int main(int argc, char** argv) {
  assert(argc == 2 && chdir(argv[1]) == 0);
  char initial[4096];
  assert(getcwd(initial, sizeof(initial)));
  int leaf = open("leaf", O_CREAT | O_EXCL | O_WRONLY, 0600);
  assert(leaf >= 0);
  close(leaf);
  auto check = [](const char* path, int result, int error) {
    current.error = 0;
    assert(posix_chdir(path) == result);
    assert(current.error == error);
  };
  check("leaf/.", -1, ENOTDIR);
  check("leaf/child", -1, ENOTDIR);
  check("leaf", -1, ENOTDIR);
  check("missing", -1, ENOENT);
  for (int error : {EACCES, EIO, EAGAIN, ENOMEM}) {
    forcedLookupError = error;
    check(".", -1, error);
  }
  forcedLookupError = 0;
  check(".", -1, ENOENT);
  forcedLookupError = -1;
  const size_t beforeCopyFailure = lookups;
  check(nullptr, -1, EFAULT);
  assert(lookups == beforeCopyFailure);
  check(".", 0, 0);
  check("./", 0, 0);
  char actual[4096];
  assert(getcwd(actual, sizeof(actual)) && std::string(actual) == initial);
  assert(unlink("leaf") == 0);
}
