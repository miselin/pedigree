/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "NvmeQueue.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/utility.h"

NvmeQueue::Slot::Slot()
    : data("NVMe transfer"),
      prps("NVMe PRP list"),
      completion(0, false),
      active(false),
      done(false),
      status(0),
      result(0) {}
NvmeQueue::NvmeQueue()
    : m_Registers(nullptr),
      m_Submission("NVMe SQ"),
      m_Completion("NVMe CQ"),
      m_Available(0, false),
      m_Stride(0),
      m_TransferBytes(0),
      m_Id(0),
      m_Depth(0),
      m_Tail(0),
      m_Head(0),
      m_SubmissionHead(0),
      m_Phase(true),
      m_Online(false),
      m_PolledInterrupt(false),
      m_InterruptCompletions(0),
      m_Outstanding(0),
      m_MaximumOutstanding(0) {}

bool NvmeQueue::initialise(IoBase* registers, uint16_t id, uint16_t depth, size_t stride,
                           size_t transferBytes) {
  if (TargetInfo::getPageSize() != Nvme::PageSize || depth < 2 || depth > Nvme::QueueDepth ||
      !transferBytes || transferBytes > Nvme::MaxTransfer || transferBytes % Nvme::PageSize)
    return false;
  m_Registers = registers;
  m_Id = id;
  m_Depth = depth;
  m_Stride = stride;
  m_TransferBytes = transferBytes;
  auto& memory = PhysicalMemoryManager::instance();
  const size_t constraints = PhysicalMemoryManager::continuous | PhysicalMemoryManager::below4GB;
  const size_t flags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;
  if (!memory.allocateRegion(m_Submission, 1, constraints, flags) ||
      !memory.allocateRegion(m_Completion, 1, constraints, flags))
    return false;
  ByteSet(m_Submission.virtualAddress(), 0, Nvme::PageSize);
  ByteSet(m_Completion.virtualAddress(), 0, Nvme::PageSize);
  for (size_t i = 0; i < depth - 1U; ++i) {
    auto& slot = m_Slots[i];
    if (!memory.allocateRegion(slot.data, transferBytes / Nvme::PageSize,
                               PhysicalMemoryManager::below4GB, flags) ||
        !memory.allocateRegion(slot.prps, 1, constraints, flags))
      return false;
    ByteSet(slot.data.virtualAddress(), 0, transferBytes);
    ByteSet(slot.prps.virtualAddress(), 0, Nvme::PageSize);
    auto* prps = static_cast<uint64_t*>(slot.prps.virtualAddress());
    // PRPs do not require adjacent physical pages. Preserve the low DMA pool
    // for devices whose descriptors really need contiguous allocations.
    for (size_t page = 0; page < transferBytes / Nvme::PageSize; ++page) {
      physical_uintptr_t address = 0;
      size_t mappingFlags = 0;
      VirtualAddressSpace::getKernelAddressSpace().getMapping(
          static_cast<uint8_t*>(slot.data.virtualAddress()) + page * Nvme::PageSize, address,
          mappingFlags);
      if (address >= (uint64_t{1} << 32))
        return false;
      if (!page)
        slot.firstPage = address;
      else
        prps[page - 1] = address;
    }
  }
  FENCE();
  m_Online = true;
  m_Available.release(depth - 1);
  return true;
}

void NvmeQueue::stopLocked() {
  if (!m_Online)
    return;
  m_Online = false;
  for (size_t i = 0; i < m_Depth - 1U; ++i) {
    if (m_Slots[i].active)
      m_Slots[i].completion.release();
  }
  m_Available.release(m_Depth);
}
void NvmeQueue::stop() {
  LockGuard<Mutex> guard(m_Lock);
  stopLocked();
}

bool NvmeQueue::observe(bool fromInterrupt, bool interruptsEnabled) {
  const bool credited = fromInterrupt && m_PolledInterrupt;
  if (fromInterrupt)
    m_PolledInterrupt = false;
  if (!m_Online)
    return credited;
  auto* entries = static_cast<volatile Nvme::Completion*>(m_Completion.virtualAddress());
  bool consumed = false;
  for (size_t count = 0; count < m_Depth; ++count) {
    volatile auto& entry = entries[m_Head];
    const uint16_t status = entry.status;
    if ((status & 1U) != m_Phase)
      break;
    // The phase bit publishes the remainder of the coherent DMA completion.
    FENCE();
    const uint16_t cid = entry.cid;
    if (entry.sqId != m_Id || entry.sqHead >= m_Depth || cid >= m_Depth - 1U ||
        !m_Slots[cid].active || m_Slots[cid].done) {
      ERROR("NVMe: invalid completion on queue " << m_Id);
      stopLocked();
      return true;
    }
    auto& slot = m_Slots[cid];
    m_SubmissionHead = entry.sqHead;
    slot.status = (status >> 1) & 0x7ffU;
    slot.result = entry.result;
    slot.done = true;
    --m_Outstanding;
    if (fromInterrupt)
      ++m_InterruptCompletions;
    slot.completion.release();
    if (++m_Head == m_Depth) {
      m_Head = 0;
      m_Phase = !m_Phase;
    }
    consumed = true;
  }
  if (consumed) {
    // Advancing CQ head can withdraw INTx before its IRQ worker observes it.
    if (!fromInterrupt && interruptsEnabled)
      m_PolledInterrupt = true;
    FENCE();
    m_Registers->write32(m_Head, Nvme::Doorbells + (2U * m_Id + 1U) * m_Stride);
    (void)m_Registers->read32(Nvme::Status);
  }
  return consumed || credited;
}
bool NvmeQueue::complete(bool fromInterrupt) {
  LockGuard<Mutex> guard(m_Lock);
  return observe(fromInterrupt);
}

NvmeQueue::Result NvmeQueue::execute(Nvme::Command command, void* buffer, size_t bytes,
                                     bool writing, bool interrupts, size_t timeoutSeconds,
                                     uint32_t* result) {
  TerminationDeferral lifetime;
  if (bytes > m_TransferBytes || (bytes && !buffer))
    return Result::CommandError;
  if (!m_Available.acquireForCompletion(1, timeoutSeconds)) {
    stop();
    return Result::TransportError;
  }
  size_t cid = 0;
  {
    LockGuard<Mutex> guard(m_Lock);
    if (!m_Online) {
      m_Available.release();
      return Result::TransportError;
    }
    for (; cid < m_Depth - 1U && m_Slots[cid].active; ++cid) {
    }
    const uint16_t nextTail = (m_Tail + 1U) % m_Depth;
    if (cid == m_Depth - 1U || nextTail == m_SubmissionHead) {
      stopLocked();
      return Result::TransportError;
    }
    auto& slot = m_Slots[cid];
    [[maybe_unused]] const size_t drained = slot.completion.drainAvailable();
    slot.active = true;
    if (++m_Outstanding > m_MaximumOutstanding)
      m_MaximumOutstanding = m_Outstanding;
    slot.done = false;
    slot.status = 0;
    if (bytes) {
      if (writing)
        MemoryCopy(slot.data.virtualAddress(), buffer, bytes);
      command.prp1 = slot.firstPage;
      command.prp2 = bytes <= Nvme::PageSize ? 0
                     : bytes <= 2 * Nvme::PageSize
                         ? static_cast<uint64_t*>(slot.prps.virtualAddress())[0]
                         : slot.prps.physicalAddress();
    }
    command.opcode = (command.opcode & 0xffffU) | (cid << 16);
    auto* submission = static_cast<Nvme::Command*>(m_Submission.virtualAddress());
    submission[m_Tail] = command;
    m_Tail = nextTail;
    FENCE();
    m_Registers->write32(m_Tail, Nvme::Doorbells + 2U * m_Id * m_Stride);
    (void)m_Registers->read32(Nvme::Status);
  }
  const auto deadline = Time::getTicks() + timeoutSeconds * Time::Multiplier::Second;
  for (;;) {
    bool poll = !interrupts;
    if (interrupts)
      poll = !m_Slots[cid].completion.acquireForCompletion(1, 0, 10000);
    {
      LockGuard<Mutex> guard(m_Lock);
      if (poll)
        observe(false, interrupts);
      auto& slot = m_Slots[cid];
      if (!m_Online || (m_Registers->read32(Nvme::Status) & 2U)) {
        stopLocked();
        slot.active = false;
        return Result::TransportError;
      }
      if (slot.done) {
        const bool success = !slot.status;
        if (success && bytes && !writing) {
          FENCE();
          MemoryCopy(buffer, slot.data.virtualAddress(), bytes);
        }
        if (result)
          *result = slot.result;
        if (!success)
          WARNING("NVMe: command " << Hex << (command.opcode & 255U) << " namespace "
                                   << command.nsid << " status " << slot.status);
        slot.active = false;
        m_Available.release();
        return success ? Result::Success : Result::CommandError;
      }
      if (Time::getTicks() >= deadline) {
        ERROR("NVMe: command timeout on queue " << m_Id << ", CID " << cid);
        stopLocked();
        slot.active = false;
        return Result::TransportError;
      }
    }
    if (!interrupts)
      Time::delay(Time::Multiplier::Millisecond);
  }
}
size_t NvmeQueue::interruptCompletions() const {
  LockGuard<Mutex> guard(m_Lock);
  return m_InterruptCompletions;
}
size_t NvmeQueue::maximumOutstanding() const {
  LockGuard<Mutex> guard(m_Lock);
  return m_MaximumOutstanding;
}
