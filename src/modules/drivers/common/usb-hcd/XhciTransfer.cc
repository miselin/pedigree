/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Xhci.h"
using namespace XhciHw;
uintptr_t Xhci::createTransaction(UsbEndpoint description) {
  OperationBarrier::Lease admission;
  if (!m_Submissions.tryAcquire(admission) || description.nAddress >= 128 ||
      !description.nMaxPacketSize || description.nEndpoint > 15)
    return ~uintptr_t{0};
  LockGuard<Mutex> configuration(m_ConfigurationLock);
  uint8_t slot;
  {
    LockGuard<Mutex> lock(m_Lock);
    slot = m_Online ? findSlot(description) : 0;
  }
  if (!slot)
    return ~uintptr_t{0};
  Endpoint* endpoint = ensureEndpoint(slot, description);
  if (!endpoint)
    return ~uintptr_t{0};
  auto* transaction = new Transaction;
  if (!allocate(transaction->bounce, MaxTransfer / PageBytes, false)) {
    delete transaction;
    return ~uintptr_t{0};
  }
  for (size_t i = 0; i < MaxTransfer / PageBytes; ++i) {
    size_t flags = 0;
    VirtualAddressSpace::getKernelAddressSpace().getMapping(
        static_cast<uint8_t*>(transaction->bounce.virtualAddress()) + i * PageBytes,
        transaction->pages[i], flags);
  }
  LockGuard<Mutex> lock(m_Lock);
  for (size_t i = 0; m_Online && i < MaxTransactions; ++i) {
    if (m_Transactions[i])
      continue;
    transaction->description = description;
    transaction->endpoint = endpoint;
    transaction->id = i;
    transaction->slot = slot;
    transaction->dci = description.nEndpoint ? description.nEndpoint * 2 + description.nIn : 1;
    transaction->generation = m_Deliveries.nextGeneration();
    m_Transactions[i] = transaction;
    return i;
  }
  delete transaction;
  return ~uintptr_t{0};
}
void Xhci::addTransferToTransaction(uintptr_t id, bool, UsbPid pid, uintptr_t buffer,
                                    size_t bytes) {
  OperationBarrier::Lease admission;
  if (!m_Submissions.tryAcquire(admission))
    return;
  LockGuard<Mutex> lock(m_Lock);
  if (id >= MaxTransactions || !m_Transactions[id])
    return;
  auto& transaction = *m_Transactions[id];
  if (transaction.submitted || transaction.bad)
    return;
  if (pid == UsbPidSetup) {
    if (transaction.dci != 1 || transaction.setupPresent || bytes != 8 || !buffer) {
      transaction.bad = true;
      return;
    }
    MemoryCopy(&transaction.setup, reinterpret_cast<void*>(buffer), 8);
    transaction.setupPresent = true;
    transaction.input = transaction.setup & 0x80U;
    return;
  }
  if (pid != UsbPidIn && pid != UsbPidOut) {
    transaction.bad = true;
    return;
  }
  if (!bytes && transaction.setupPresent) {
    transaction.statusPresent = true;
    if (bool(pid == UsbPidIn) != (!transaction.input || !(transaction.setup >> 48)))
      transaction.bad = true;
    return;
  }
  if (!buffer || !bytes || bytes > MaxTransfer - transaction.bytes || transaction.statusPresent ||
      (transaction.client && buffer != transaction.client + transaction.bytes)) {
    transaction.bad = true;
    return;
  }
  if (!transaction.bytes) {
    transaction.client = buffer;
    if (!transaction.setupPresent)
      transaction.input = pid == UsbPidIn;
  }
  if (transaction.input != (pid == UsbPidIn)) {
    transaction.bad = true;
    return;
  }
  if (!transaction.input)
    MemoryCopy(static_cast<uint8_t*>(transaction.bounce.virtualAddress()) + transaction.bytes,
               reinterpret_cast<void*>(buffer), bytes);
  transaction.bytes += bytes;
}
bool Xhci::submitLocked(Transaction& transaction) {
  Trb trbs[20]{};
  size_t count = 0;
  const bool control = transaction.dci == 1;
  if (!m_Online || findSlot(transaction.description) != transaction.slot ||
      transaction.endpoint->needsReset ||
      (control && (!transaction.setupPresent || !transaction.statusPresent ||
                   transaction.bytes != (transaction.setup >> 48))))
    return false;
  if (control) {
    transaction.trbTypes[count] = 2;
    transaction.trbOffsets[count] = transaction.trbLengths[count] = 0;
    trbs[count++] = {
        transaction.setup, 8,
        (2U << 10) | (1U << 6) | (transaction.bytes ? (transaction.input ? 3U : 2U) << 16 : 0)};
  }
  for (size_t offset = 0; offset < transaction.bytes;) {
    const size_t remaining = transaction.bytes - offset;
    const size_t bytes = remaining < PageBytes ? remaining : PageBytes;
    const bool last = bytes == remaining;
    const uint8_t type = control && !offset ? 3 : 1;
    transaction.trbTypes[count] = type;
    transaction.trbOffsets[count] = offset;
    transaction.trbLengths[count] = bytes;
    const size_t packets = (remaining - bytes + transaction.description.nMaxPacketSize - 1) /
                           transaction.description.nMaxPacketSize;
    trbs[count++] = {transaction.pages[offset / PageBytes],
                     static_cast<uint32_t>(bytes | ((packets < 31 ? packets : 31) << 17)),
                     (uint32_t{type} << 10) |
                         (!last      ? Chain
                          : !control ? Ioc
                                     : 0) |
                         (transaction.input ? Isp : 0) |
                         (type == 3 && transaction.input ? 1U << 16 : 0)};
    offset += bytes;
  }
  if (control) {
    transaction.trbTypes[count] = 4;
    transaction.trbOffsets[count] = transaction.trbLengths[count] = 0;
    trbs[count++] = {0, 0,
                     (4U << 10) | Ioc | (!transaction.input || !transaction.bytes ? 1U << 16 : 0)};
  }
  if (!count)
    return false;
  const size_t wraps = transaction.endpoint->ring.wraps();
  if (!transaction.endpoint->ring.enqueue(trbs, count, transaction.trbAddresses))
    return false;
  transaction.trbCount = count;
  transaction.submitted = true;
  transaction.shortSeen = false;
  transaction.captured = false;
  transaction.actual = 0;
  if (!wraps && transaction.endpoint->ring.wraps()) {
#if PEDIGREE_USB_SMOKE_TESTS
    NOTICE("XHCI-SMOKE: transfer-ring-wrap");
#endif
    NOTICE("xHCI: transfer ring wrapped slot " << transaction.slot << " endpoint "
                                               << transaction.dci);
  }
  write(m_Doorbells + transaction.slot * 4, transaction.dci);
  (void)read(m_Op + 4);
  return true;
}
bool Xhci::accept(uintptr_t id, void (*callback)(uintptr_t, ssize_t), uintptr_t parameter,
                  UsbInterruptInHandle* handle) {
  LockGuard<Mutex> lock(m_Lock);
  if (id >= MaxTransactions || !m_Transactions[id])
    return false;
  auto& transaction = *m_Transactions[id];
  if (transaction.submitted)
    return false;
  if (transaction.bad || transaction.endpoint->active || !m_Online || !m_Transfers.tryEnter()) {
    freeTransactionLocked(id);
    return false;
  }
  transaction.callback = callback;
  transaction.parameter = parameter;
  transaction.periodic = handle != nullptr;
  transaction.endpoint->active = &transaction;
  if (!submitLocked(transaction)) {
    transaction.endpoint->active = nullptr;
    m_Transfers.leave();
    freeTransactionLocked(id);
    return false;
  }
  // The event lock prevents completion until the callback owner is published.
  if (handle) {
    if (!publishInterruptInHandle(*handle, {id, transaction.generation}, callback, parameter))
      panic("xHCI: interrupt subscription publication failed");
  } else
    transaction.completion.arm(callback, parameter, transaction.generation);
  return true;
}
bool Xhci::doAsync(uintptr_t id, void (*callback)(uintptr_t, ssize_t), uintptr_t parameter) {
  OperationBarrier::Lease admission;
  if (!m_Submissions.tryAcquire(admission))
    return false;
  return accept(id, callback, parameter, nullptr);
}
bool Xhci::addInterruptInHandler(UsbEndpoint description, uintptr_t buffer, uint16_t bytes,
                                 void (*callback)(uintptr_t, ssize_t), UsbInterruptInHandle& handle,
                                 uintptr_t parameter) {
  OperationBarrier::Lease admission;
  if (!m_Submissions.tryAcquire(admission) || handle || !callback || !bytes ||
      description.nTransferType != 3 || !description.nIn || bytes > description.nMaxPacketSize)
    return false;
  const uintptr_t transaction = createTransaction(description);
  if (transaction == ~uintptr_t{0})
    return false;
  addTransferToTransaction(transaction, false, UsbPidIn, buffer, bytes);
  return accept(transaction, callback, parameter, &handle);
}
bool Xhci::decodeCompletion(Transaction& transaction, size_t index, uint8_t code, size_t residual,
                            bool& complete, size_t& actual) {
  if (index >= transaction.trbCount || (code != 1 && code != 13))
    return false;
  // ED=0 residuals describe the referenced TRB, not the entire transfer.
  if (code == 1 && residual)
    return false;
  if (code == 13 && !transaction.shortSeen) {
    if (!transaction.input || !transaction.trbLengths[index] ||
        residual > transaction.trbLengths[index])
      return false;
    transaction.actual = transaction.trbOffsets[index] + transaction.trbLengths[index] - residual;
    transaction.shortSeen = true;
  }
  actual = transaction.shortSeen ? transaction.actual : transaction.bytes;
  if (actual > transaction.bytes)
    return false;
  // A short ends a bulk/interrupt TD immediately. Control must still finish Status.
  complete = transaction.dci == 1 ? transaction.trbTypes[index] == 4
                                  : code == 13 || index + 1 == transaction.trbCount;
  return true;
}
bool Xhci::completionRegressions() {
  Transaction transaction;
  transaction.input = true;
  transaction.bytes = 8192;
  transaction.dci = 3;
  transaction.trbCount = 2;
  transaction.trbTypes[0] = transaction.trbTypes[1] = 1;
  transaction.trbLengths[0] = transaction.trbLengths[1] = 4096;
  transaction.trbOffsets[1] = 4096;
  bool complete = false;
  size_t actual = 0;
  if (!decodeCompletion(transaction, 0, 13, 3072, complete, actual) || !complete || actual != 1024)
    return false;
  transaction.shortSeen = false;
  transaction.dci = 1;
  transaction.trbCount = 3;
  transaction.trbTypes[0] = 3;
  transaction.trbTypes[2] = 4;
  if (!decodeCompletion(transaction, 0, 13, 3072, complete, actual) || complete || actual != 1024 ||
      !decodeCompletion(transaction, 2, 1, 0, complete, actual) || !complete || actual != 1024)
    return false;
  transaction.shortSeen = false;
  if (decodeCompletion(transaction, 0, 1, 1, complete, actual) ||
      decodeCompletion(transaction, 0, 13, 4097, complete, actual))
    return false;
#if PEDIGREE_USB_SMOKE_TESTS
  NOTICE("XHCI-SMOKE: completion-contracts");
#endif
  return true;
}
bool Xhci::transferEventLocked(const Trb& event) {
  const uint8_t slot = event.control >> 24, dci = (event.control >> 16) & 31U;
  if (!slot || slot > m_SlotCount || !dci || !m_Slots[slot].endpoints[dci]) {
    failLocked();
    return false;
  }
  auto* endpoint = m_Slots[slot].endpoints[dci];
  Transaction* transaction = endpoint->active;
  if (!transaction || !transaction->submitted || transaction->cancelling)
    return false;
  size_t index = 0;
  while (index < transaction->trbCount && transaction->trbAddresses[index] != event.parameter)
    ++index;
  const uint8_t code = event.status >> 24;
  if (index == transaction->trbCount && (code == 1 || code == 13)) {
    // Some controllers also report a skipped IOC tail after the earlier short event.
    for (size_t i = 0; i < endpoint->retiredCount; ++i)
      if (endpoint->retiredAddresses[i] == event.parameter)
        return false;
  }
  if (index == transaction->trbCount || (event.control & 4U)) {
    failLocked();
    return false;
  }
  const bool stale = transaction->periodic && findSlot(transaction->description) != slot;
  if (code == 1 || code == 13) {
    bool complete = false;
    size_t actual = 0;
    if (!decodeCompletion(*transaction, index, code, event.status & 0xffffffU, complete, actual)) {
      failLocked();
      return false;
    }
    if (!complete)
      return false;
    if (!stale && transaction->input && actual)
      MemoryCopy(reinterpret_cast<void*>(transaction->client), transaction->bounce.virtualAddress(),
                 actual);
    if (!stale)
      finishLocked(*transaction, actual + (transaction->dci == 1 ? 8 : 0), true);
  } else {
    // Halted/stopped endpoints cannot retain access to the completed TD's DMA.
    const uint32_t state = outputContext(slot, dci)[0] & 7U;
    if (state != 2 && state != 3) {
      failLocked();
      return false;
    }
    endpoint->needsReset = true;
    if (!stale)
      finishLocked(*transaction, code == 6 ? -Stall : -TransactionError, true);
  }
  if (stale) {
    endpoint->ring.retire(transaction->trbCount);
    transaction->submitted = false;
    transaction->captured = true;
  }
  return !stale;
}
void Xhci::enqueueDeliveryLocked(UsbHcd::CallbackDeliveryQueue::Record* record) {
  List<UsbHcd::CallbackDeliveryQueue::Record*> batch;
  batch.pushBack(record);
  m_Deliveries.publish(batch);
  m_ReadyDeliveries.pushBack(record);
  m_DeliveryReady.release();
}
void Xhci::finishLocked(Transaction& transaction, ssize_t result, bool natural) {
  if (transaction.captured)
    return;
  transaction.captured = true;
  transaction.endpoint->retiredCount = transaction.trbCount;
  MemoryCopy(transaction.endpoint->retiredAddresses, transaction.trbAddresses,
             transaction.trbCount * sizeof(transaction.trbAddresses[0]));
  transaction.endpoint->ring.retire(transaction.trbCount);
  transaction.submitted = false;
  if (!transaction.periodic)
    transaction.endpoint->active = nullptr;
  if (transaction.periodic) {
    auto* delivery = new Delivery{this, transaction.id, transaction.generation, result >= 0};
    enqueueDeliveryLocked(
        m_Deliveries.create({transaction.id, m_Deliveries.nextGeneration(), transaction.generation},
                            transaction.callback, transaction.parameter, result, afterDelivery,
                            delivery, destroyDelivery, delivery));
  } else {
    UsbHcd::TransferCompletion::Claim claim;
    const bool claimed = natural ? transaction.completion.captureNatural(result) &&
                                       transaction.completion.claimCaptured(claim)
                                 : transaction.completion.claimForTeardown(result, claim);
    if (!claimed)
      return;
    auto* delivery = new Delivery{this, transaction.id, transaction.generation, false};
    enqueueDeliveryLocked(m_Deliveries.create({transaction.id, claim.generation}, claim.callback,
                                              claim.parameter, claim.result, afterDelivery,
                                              delivery, destroyDelivery, delivery));
  }
}
void Xhci::afterDelivery(void* context) {
  auto* delivery = static_cast<Delivery*>(context);
  auto* controller = delivery->controller;
  LockGuard<Mutex> lock(controller->m_Lock);
  const uintptr_t id = delivery->transaction;
  Transaction* transaction = controller->m_Transactions[id];
  if (!transaction || transaction->generation != delivery->generation)
    return;
  if (!transaction->periodic) {
    controller->freeTransactionLocked(id);
    controller->m_Transfers.leave();
  } else if (delivery->rearm && !transaction->cancelling && controller->m_Online &&
             controller->findSlot(transaction->description) == transaction->slot) {
    if (!controller->submitLocked(*transaction))
      transaction->endpoint->needsReset = true;
  }
}
void Xhci::destroyDelivery(void* context) {
  delete static_cast<Delivery*>(context);
}
void Xhci::freeTransactionLocked(uintptr_t id) {
  delete m_Transactions[id];
  m_Transactions[id] = nullptr;
}
int Xhci::deliveryWorker(void* context) {
  TerminationDeferral lifetime;
  auto* controller = static_cast<Xhci*>(context);
  for (;;) {
    if (!controller->m_DeliveryReady.acquireForCompletion())
      continue;
    UsbHcd::CallbackDeliveryQueue::Record* record = nullptr;
    {
      LockGuard<Mutex> lock(controller->m_Lock);
      if (controller->m_ReadyDeliveries.count())
        record = controller->m_ReadyDeliveries.popFront();
      else if (controller->m_DeliveryStopping)
        return 0;
    }
    if (record)
      controller->m_Deliveries.deliver(record);
  }
}
