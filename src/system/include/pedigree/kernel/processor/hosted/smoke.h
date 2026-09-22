/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESSOR_HOSTED_SMOKE_H
#define PEDIGREE_KERNEL_PROCESSOR_HOSTED_SMOKE_H
#include "pedigree/kernel/compiler.h"

#include <config.h>
#include <stddef.h>

enum HostedSmokeStage {
  HostedSmokeNone,
  HostedSmokeRoot,
  HostedSmokeInit,
  HostedSmokeCommand,
  HostedSmokeShutdown,
};

extern "C" HostedSmokeStage g_HostedSmokeStage;
EXPORTED_PUBLIC bool hostedSyscallProfileRequested();
EXPORTED_PUBLIC size_t hostedSyscallProfileDivisor();

#endif
