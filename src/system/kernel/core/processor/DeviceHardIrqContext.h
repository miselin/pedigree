/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_CORE_PROCESSOR_DEVICEHARDIRQCONTEXT_H
#define PEDIGREE_KERNEL_CORE_PROCESSOR_DEVICEHARDIRQCONTEXT_H
#include "pedigree/kernel/process/ExecutionContext.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include <config.h>

/** Marks the dynamic extent of one explicit device hard-IRQ callback. */
class DeviceHardIrqContext {
 public:
  DeviceHardIrqContext(size_t& previousDepth, bool& restorationArmed);
  ~DeviceHardIrqContext();

  /** Restores a depth captured before a callback began. */
  static void restoreDepth(size_t previousDepth);

 private:
  DeviceHardIrqContext(const DeviceHardIrqContext&);
  DeviceHardIrqContext& operator=(const DeviceHardIrqContext&);

  ProcessorInformation& m_Information;
  bool& m_RestorationArmed;
  size_t m_PreviousDepth;
  ExecutionContextGuard m_ExecutionContext;
};

/**
 * Suspends the device marker while a scheduler timer may switch contexts.
 *
 * This scope is valid only directly inside the outermost hard callback.
 */
class SuspendDeviceHardIrqContext {
 public:
  SuspendDeviceHardIrqContext();
  ~SuspendDeviceHardIrqContext();

 private:
  SuspendDeviceHardIrqContext(const SuspendDeviceHardIrqContext&);
  SuspendDeviceHardIrqContext& operator=(const SuspendDeviceHardIrqContext&);

  ProcessorInformation& m_Information;
};

#endif
