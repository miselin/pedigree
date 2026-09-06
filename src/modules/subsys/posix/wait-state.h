/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_POSIX_WAIT_STATE_H
#define PEDIGREE_POSIX_WAIT_STATE_H

#include <stdint.h>

namespace PosixWait {
enum class Selector { All, Pid, Pgid };
enum Event : unsigned { Exited = 1, Stopped = 2, Continued = 4 };
enum Cause : int32_t { Exit = 1, Killed = 2, Dumped = 3, Stop = 5, Continue = 6 };

struct Request {
  Selector selector = Selector::All;
  int32_t id = 0;
  unsigned events = Exited;
  bool noHang = false;
  bool noWait = false;
};

struct Report {
  int32_t pid = 0;
  int32_t cause = 0;
  int32_t status = 0;
  uint32_t uid = 0;
  uint64_t userNanoseconds = 0;
  uint64_t kernelNanoseconds = 0;
};

// Returns one report, zero for WNOHANG, or -1 with errno. No lifetime claim
// escapes: consuming calls publish the sole reaper before user copyout.
int collect(const Request& request, Report& report);
}  // namespace PosixWait
#endif
