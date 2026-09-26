/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef NVME_REGISTERS_H
#define NVME_REGISTERS_H
#include "pedigree/kernel/processor/types.h"

namespace Nvme {
constexpr size_t PageSize = 4096;
constexpr size_t QueueDepth = 16;
constexpr size_t MaxIoQueues = 8;
// Bound permanently mapped transfer buffers across all I/O queues.
constexpr size_t MaxIoSlots = 32;
constexpr size_t MaxTransfer = 65536;
enum class VirtualResource : uint8_t { Queue = 0, Interrupt = 1 };
enum class VirtualizationAction : uint8_t {
  AllocatePrimary = 1,
  Offline = 7,
  Assign = 8,
  Online = 9,
};
struct VirtualResourceCapabilities {
  uint32_t flexibleTotal;
  uint32_t flexibleAssigned;
  uint16_t flexibleAllocatedToPrimary;
  uint16_t privateTotal;
  uint16_t secondaryMaximum;
  uint16_t preferredGranularity;
};
struct PrimaryControllerCapabilities {
  uint16_t controllerId;
  uint16_t portId;
  uint8_t resourceTypes;
  VirtualResourceCapabilities queues;
  VirtualResourceCapabilities interrupts;
};
struct SecondaryController {
  uint16_t controllerId;
  uint16_t primaryControllerId;
  bool online;
  // The NVMe VF Number is one-based; zero denotes a non-SR-IOV controller.
  uint16_t virtualFunction;
  uint16_t queueResources;
  uint16_t interruptResources;
};
struct SecondaryControllerList {
  uint8_t count;
  SecondaryController entries[127];
};
enum Register : size_t {
  Cap = 0x00,
  Version = 0x08,
  InterruptMaskSet = 0x0c,
  InterruptMaskClear = 0x10,
  Configuration = 0x14,
  Status = 0x1c,
  AdminAttributes = 0x24,
  AdminSubmission = 0x28,
  AdminCompletion = 0x30,
  Doorbells = 0x1000,
};
struct Command {
  uint32_t opcode;
  uint32_t nsid;
  uint64_t reserved;
  uint64_t metadata;
  uint64_t prp1;
  uint64_t prp2;
  uint32_t cdw10;
  uint32_t cdw11;
  uint32_t cdw12;
  uint32_t cdw13;
  uint32_t cdw14;
  uint32_t cdw15;
};
struct Completion {
  uint32_t result;
  uint32_t reserved;
  uint16_t sqHead;
  uint16_t sqId;
  uint16_t cid;
  uint16_t status;
};
static_assert(sizeof(Command) == 64);
static_assert(sizeof(Completion) == 16);
}  // namespace Nvme
#endif
