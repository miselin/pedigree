/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"

#include "Xhci.h"
using namespace XhciHw;
bool Xhci::halt() {
  if (!m_HardwareOwned)
    return true;
  write(m_Runtime + 0x20, 1);
  write(m_Op, read(m_Op) & ~5U);
  (void)read(m_Op);
  return wait(m_Op + 4, 1, 1, 1000);
}
void Xhci::failLocked() {
  if (m_Failed)
    return;
  if (!halt())
    panic("xHCI: DMA did not halt; refusing to release memory");
  m_Online = false;
  m_Stopping = true;
  m_Failed = true;
  ERROR("xHCI: controller halted after transport failure");
  m_CommandDone.release();
  for (auto* transaction : m_Transactions)
    if (transaction && transaction->submitted && !transaction->captured)
      finishLocked(*transaction, -TransactionError, false);
}
void Xhci::cancelTransfer(uintptr_t id, size_t generation, bool recurring,
                          void (*callback)(uintptr_t, ssize_t), uintptr_t parameter) {
  TerminationDeferral lifetime;
  {
    LockGuard<Mutex> cancellation(m_CancelLock);
    LockGuard<Mutex> configuration(m_ConfigurationLock);
    uint8_t slot = 0, dci = 0;
    bool stop = false;
    {
      LockGuard<Mutex> lock(m_Lock);
      if (id >= MaxTransactions || !m_Transactions[id])
        return;
      auto& transaction = *m_Transactions[id];
      if (transaction.callback != callback || transaction.parameter != parameter ||
          transaction.periodic != recurring || (recurring && transaction.generation != generation))
        return;
      generation = transaction.generation;
      transaction.cancelling = true;
      slot = transaction.slot;
      dci = transaction.dci;
      stop = transaction.submitted && m_Online;
    }
    if (stop && !resetEndpointHardware(slot, dci, true)) {
      LockGuard<Mutex> lock(m_Lock);
      failLocked();
    }
    {
      LockGuard<Mutex> lock(m_Lock);
      auto* transaction = m_Transactions[id];
      if (transaction && transaction->generation == generation) {
        if (recurring) {
          if (transaction->submitted)
            transaction->endpoint->ring.retire(transaction->trbCount);
          transaction->endpoint->active = nullptr;
          freeTransactionLocked(id);
          m_Transfers.leave();
        } else if (!transaction->captured)
          finishLocked(*transaction, -TransactionError, false);
      }
    }
  }
  if (!recurring)
    (void)m_Deliveries.drain({id, generation});
}
void Xhci::cancelAsyncAndDrain(uintptr_t id, void (*callback)(uintptr_t, ssize_t),
                               uintptr_t parameter) {
  OperationBarrier::Lease admission;
  if (!m_Cancellations.tryAcquire(admission))
    return;
  cancelTransfer(id, 0, false, callback, parameter);
}
bool Xhci::cancelInterruptInAndDrain(const UsbInterruptInToken& token,
                                     void (*callback)(uintptr_t, ssize_t), uintptr_t parameter,
                                     bool producerAlreadyStopped) {
  OperationBarrier::Lease admission;
  if (!m_Cancellations.tryAcquire(admission))
    panic("xHCI: interrupt cancellation raced controller destruction");
  if (!producerAlreadyStopped)
    cancelTransfer(token.transaction, token.generation, true, callback, parameter);
  return m_Deliveries.cancelSubscription(token.transaction, token.generation);
}
void Xhci::shutdown() {
  if (m_Shutdown)
    return;
  {
    LockGuard<Mutex> lock(m_Lock);
    m_PortsClosing = true;
    for (size_t i = 0; i < m_PortCount; ++i)
      m_PortChanges[i].stopAfterQuiesce();
  }
  RequestQueue::destroy();
  disconnectAllDevices();
  m_Submissions.closeAndWait();
  {
    LockGuard<Mutex> lock(m_Lock);
    if (!halt())
      panic("xHCI: controller teardown could not halt DMA");
    m_Online = false;
    m_Stopping = true;
    for (auto* transaction : m_Transactions)
      if (transaction && transaction->submitted)
        finishLocked(*transaction, -TransactionError, false);
  }
  if (m_Irq && !Machine::instance().getIrqManager()->unregisterHandler(m_Irq, this))
    panic("xHCI: interrupt retirement failed");
  m_Irq = 0;
  m_Cancellations.closeAndWait();
  m_Transfers.closeAndWait();
  {
    LockGuard<Mutex> lock(m_Lock);
    m_DeliveryStopping = true;
    m_DeliveryReady.release();
  }
  if (m_DeliveryThread && !m_DeliveryThread->joinForCompletion())
    panic("xHCI: callback worker did not drain");
  m_DeliveryThread = nullptr;
  for (auto* transaction : m_Transactions)
    delete transaction;
  for (auto& slot : m_Slots)
    for (auto* endpoint : slot.endpoints)
      delete endpoint;
  for (auto* scratch : m_Scratch)
    delete scratch;
  if (m_HardwareOwned) {
    write64(m_Op + 0x30, 0);
    write64(m_Op + 0x18, 0);
    write(m_Runtime + 0x28, 0);
    write64(m_Runtime + 0x30, 0);
    write64(m_Runtime + 0x38, 0);
  }
  if (m_HardwareOwned && !PciBus::instance().updateCommand(m_Pci, 4, 0x400))
    panic("xHCI: teardown could not disable PCI DMA and INTx");
  m_Shutdown = true;
}
