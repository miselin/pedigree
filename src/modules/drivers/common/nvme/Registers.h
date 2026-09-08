/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef NVME_REGISTERS_H
#define NVME_REGISTERS_H
#include "pedigree/kernel/processor/types.h"

namespace Nvme {
constexpr size_t PageSize = 4096;
constexpr size_t QueueDepth = 16;
constexpr size_t MaxTransfer = 65536;
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
