/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef NVME_QUEUE_H
#define NVME_QUEUE_H
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/MemoryRegion.h"

#include "Registers.h"
class IoBase;

class NvmeQueue {
 public:
  enum class Result { Success, CommandError, TransportError };
  NvmeQueue();
  bool initialise(IoBase* registers, uint16_t id, uint16_t depth, size_t stride,
                  size_t transferBytes);
  Result execute(Nvme::Command command, void* buffer, size_t bytes, bool writing, bool interrupts,
                 size_t timeoutSeconds, uint32_t* result = nullptr, bool interruptProbe = false);
  bool complete(bool fromInterrupt);
  void stop();
  size_t interruptCompletions() const;
  size_t maximumOutstanding() const;
  uint64_t submissionAddress() const {
    return m_Submission.physicalAddress();
  }
  uint64_t completionAddress() const {
    return m_Completion.physicalAddress();
  }
  uint16_t depth() const {
    return m_Depth;
  }

 private:
  struct Slot {
    Slot();
    MemoryRegion data;
    physical_uintptr_t firstPage = 0;
    MemoryRegion prps;
    Semaphore completion;
    bool active;
    bool done;
    uint16_t status;
    uint32_t result;
  };
  bool observe(bool fromInterrupt, bool interruptsEnabled = false);
  void stopLocked();
  IoBase* m_Registers;
  MemoryRegion m_Submission;
  MemoryRegion m_Completion;
  Slot m_Slots[Nvme::QueueDepth - 1];
  mutable Mutex m_Lock;
  Semaphore m_Available;
  size_t m_Stride;
  size_t m_TransferBytes;
  uint16_t m_Id;
  uint16_t m_Depth;
  uint16_t m_Tail;
  uint16_t m_Head;
  uint16_t m_SubmissionHead;
  bool m_Phase;
  bool m_Online;
  bool m_PolledInterrupt;
  size_t m_InterruptCompletions;
  size_t m_Outstanding;
  size_t m_MaximumOutstanding;
};
#endif
