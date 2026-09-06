/* Copyright (c) 2026, Pedigree Developers. */
#include "terminal-syscalls.h"

#include "TerminalControl.h"

int posix_vhangup() {
  return TerminalControl::hangup();
}
