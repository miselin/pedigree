/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_PROCESS_ACCOUNTING_H
#define POSIX_PROCESS_ACCOUNTING_H

#include "pedigree/kernel/time/Time.h"

class PosixProcess;

struct ProcessAccountingLifetime {
  explicit ProcessAccountingLifetime(bool fromFork = false)
      : birth(Time::getTimeNanoseconds()), started(Time::getTicks()), forked(fromFork) {}
  const Time::Timestamp birth;
  const Time::Timestamp started;
  const bool forked;
  uint64_t virtualKilobytes = 0;
};

int posix_acct(const char* path);
void posix_account_process_exit(PosixProcess& process, const ProcessAccountingLifetime& lifetime);
void posix_stop_accounting();

#endif
