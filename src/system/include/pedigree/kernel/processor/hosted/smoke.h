/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESSOR_HOSTED_SMOKE_H
#define PEDIGREE_KERNEL_PROCESSOR_HOSTED_SMOKE_H
#include <config.h>

enum HostedSmokeStage {
  HostedSmokeNone,
  HostedSmokeRoot,
  HostedSmokeInit,
  HostedSmokeCommand,
  HostedSmokeShutdown,
};

extern "C" HostedSmokeStage g_HostedSmokeStage;

#endif
