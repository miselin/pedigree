/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */
#include "ScsiController.h"
#include "ScsiDisk.h"

ScsiController* ScsiDisk::acquirePagingController(OperationBarrier::Lease& use) {
  auto* controller = static_cast<ScsiController*>(m_pParent);
  if (!controller || !controller->canWaitForCompletion() || !controller->acquireDiskOperation(use))
    return nullptr;
  return controller;
}
PagingStatus ScsiDisk::doPagingTransfer(PagingOperation, uint64_t, void*) {
  return PagingStatus::Unsupported;
}
PagingStatus ScsiDisk::preparePagingCache(PagingTransport& transport) {
  UniquePointer<Cache::PreparedDiscard> discard;
  const auto prepared = m_Cache.prepareDiscardFrom(0, nullptr, 0, discard);
  if (prepared != Cache::DiscardStatus::Ready) {
    if (prepared == Cache::DiscardStatus::NoMemory)
      return PagingStatus::NoMemory;
    return prepared == Cache::DiscardStatus::Closed ? PagingStatus::Closed : PagingStatus::Busy;
  }
  if (!discard.get()->writeback(retireCachePageCallback, this))
    return PagingStatus::IoError;
  // The volatile hardware cache must drain even if no RAM cache page existed.
  const PagingStatus flushed = transport.transfer(PagingOperation::Flush, 0, nullptr);
  if (flushed != PagingStatus::Success)
    return flushed;
  discard.get()->commit();
  return PagingStatus::Success;
}

bool ScsiDisk::hasNoCacheLoans() {
  UniquePointer<Cache::PreparedDiscard> discard;
  return m_Cache.prepareDiscardFrom(0, nullptr, 0, discard) == Cache::DiscardStatus::Ready;
}
