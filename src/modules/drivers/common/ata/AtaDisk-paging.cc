/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

#include "AtaDisk.h"
#include "modules/drivers/common/scsi/ScsiController.h"

class AtaPagingTransport final : public PagingTransport {
 public:
  explicit AtaPagingTransport(AtaDisk& disk)
      : m_Disk(disk), m_Controller(nullptr), m_Active(false), m_Request{} {}

  PagingStatus prepare() {
    TerminationDeferral lifetime;
    LockGuard<Mutex> guard(m_Lock);
    if (m_Active)
      return PagingStatus::Busy;
    m_Controller = m_Disk.acquirePagingController(m_ControllerUse);
    if (!m_Controller)
      return PagingStatus::Closed;
    m_Active = true;
    return PagingStatus::Success;
  }

  PagingStatus transfer(PagingOperation operation, uint64_t offset, void* page) override {
    // Rejection must precede taking the serialization mutex: the controller
    // worker can be the thread which an existing transfer is waiting for.
    if (!m_Controller || !m_Controller->canWaitForCompletion())
      return PagingStatus::Busy;
    TerminationDeferral lifetime;
    LockGuard<Mutex> guard(m_Lock);
    if (!m_Active)
      return PagingStatus::Closed;
    if (operation != PagingOperation::Flush && !page)
      return PagingStatus::Invalid;
    if (operation == PagingOperation::Write)
      MemoryCopy(m_Page, page, sizeof(m_Page));
    m_Request = {operation, offset, m_Page, PagingStatus::Closed};
    const auto admitted = m_Controller->publishPreallocated(
        m_Token, 0, SCSI_REQUEST_PAGING, reinterpret_cast<uintptr_t>(&m_Disk),
        reinterpret_cast<uintptr_t>(&m_Request));
    if (admitted != RequestQueue::PreallocatedPublishResult::Accepted)
      return admitted == RequestQueue::PreallocatedPublishResult::TokenBusy ? PagingStatus::Busy
                                                                            : PagingStatus::Closed;
    // Both success and cancellation retire the permanent request token. No
    // callback can retain the caller's page or free this transport.
    if (!m_Controller->waitForPreallocated(m_Token))
      panic("Admitted paging request could not join its completion");
    if (m_Request.result == PagingStatus::Success && operation == PagingOperation::Read)
      MemoryCopy(page, m_Page, sizeof(m_Page));
    return m_Request.result;
  }

  void release() override {
    TerminationDeferral lifetime;
    LockGuard<Mutex> guard(m_Lock);
    m_Active = false;
    m_ControllerUse = OperationBarrier::Lease();
    m_Controller = nullptr;
  }

 private:
  AtaDisk& m_Disk;
  ScsiController* m_Controller;
  Mutex m_Lock;
  OperationBarrier::Lease m_ControllerUse;
  RequestQueue::PreallocatedRequest m_Token;
  bool m_Active;
  PagingRequest m_Request;
  alignas(16) uint8_t m_Page[PagingChannel::PageBytes];
};

void AtaDisk::preparePagingStorage() {
#if !CRIPPLE_HDD
  if (m_AtaDiskType == NotPacket && getNativeBlockSize() == 512 && !m_Paging)
    m_Paging = new AtaPagingTransport(*this);
#endif
}
void AtaDisk::releasePagingStorage() {
  retireEndpoint();
  delete m_Paging;
  m_Paging = nullptr;
}
PagingStatus AtaDisk::preparePagingTransport(PagingTransport*& transport) {
  transport = nullptr;
#if CRIPPLE_HDD
  return PagingStatus::Unsupported;
#else
  if (m_AtaDiskType != NotPacket || getNativeBlockSize() != 512 ||
      getSize() < PagingChannel::PageBytes || getSize() % PagingChannel::PageBytes ||
      (m_pIdent.__raw[83] & 0xC000) != 0x4000 ||
      (!m_pIdent.data.command_sets_support.flush_cache &&
       !(m_SupportsLBA48 && m_pIdent.data.command_sets_support.flush_cache_ext)))
    return PagingStatus::Unsupported;
  if (!m_Paging)
    return PagingStatus::NoMemory;
  PagingStatus status = m_Paging->prepare();
  if (status != PagingStatus::Success)
    return status;
  status = preparePagingCache(*m_Paging);
  if (status != PagingStatus::Success) {
    m_Paging->release();
    return status;
  }
  transport = m_Paging;
  return PagingStatus::Success;
#endif
}

PagingStatus AtaDisk::doPagingTransfer(PagingOperation operation, uint64_t offset, void* page) {
#if CRIPPLE_HDD
  return PagingStatus::Unsupported;
#else
  if (m_AtaDiskType != NotPacket || getNativeBlockSize() != 512)
    return PagingStatus::Unsupported;
  if (operation == PagingOperation::Flush)
    return doSync(SyncWholeDevice) ? PagingStatus::Success : PagingStatus::IoError;
  if (!page || (offset % PagingChannel::PageBytes) || offset > getSize() ||
      PagingChannel::PageBytes > getSize() - offset || !m_CommandRegs || !m_ControlRegs)
    return PagingStatus::Invalid;
  constexpr uint32_t sectors = PagingChannel::PageBytes / 512;
  const uint64_t sector = offset / 512;
  if ((!m_SupportsLBA48 && sector > 0x0fffffff - (sectors - 1)) ||
      sector > 0xffffffffffffULL - (sectors - 1))
    return PagingStatus::Unsupported;

  // The controller worker serializes this polled command with ordinary I/O.
  // No cache, DMA descriptor, IRQ completion, or heap storage is involved.
  m_ControlRegs->write8(2, 2);
  AtaStatus status = ataWait(m_CommandRegs, m_ControlRegs);
  if (status.reg.bsy || status.reg.drq)
    return PagingStatus::IoError;
  m_CommandRegs->write8(m_IsMaster ? 0xe0 : 0xf0, 6);
  status = ataWait(m_CommandRegs, m_ControlRegs);
  if (status.reg.bsy || status.reg.drq || status.reg.df || !status.reg.drdy)
    return PagingStatus::IoError;
  if (m_SupportsLBA48)
    setupLBA48(sector, sectors);
  else
    setupLBA28(sector, sectors);
  AtaPioPollBudget budget = {Time::getTicks(), 30 * Time::Multiplier::Second, 0, 30000000};
  const bool write = operation == PagingOperation::Write;
  m_CommandRegs->write8(write ? (m_SupportsLBA48 ? 0x34 : 0x30) : (m_SupportsLBA48 ? 0x24 : 0x20),
                        7);
  bool succeeded =
      write ? ataPioWrite512ByteSectors(m_CommandRegs, m_ControlRegs, static_cast<uint16_t*>(page),
                                        sectors, budget, status)
            : ataPioRead512ByteSectors(m_CommandRegs, m_ControlRegs, static_cast<uint16_t*>(page),
                                       sectors, budget, status);
  return succeeded ? PagingStatus::Success : PagingStatus::IoError;
#endif
}
