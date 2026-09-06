/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/utilities/assert.h"

#include "Ext2Quota.h"

Ext2QuotaLedger::~Ext2QuotaLedger() {
  for (auto it = m_Inodes.begin(); it != m_Inodes.end(); ++it)
    delete it.value();
}

Ext2QuotaLedger::InodeCharge* Ext2QuotaLedger::find(uint32_t inode) const {
  return m_Inodes.lookup(inode);
}

QuotaStatus Ext2QuotaLedger::prepareCharge(uint32_t uid, uint32_t gid, uint64_t bytes,
                                           uint64_t inodes, bool enforce) {
  const uint32_t ids[] = {uid, gid};
  for (size_t type = 0; type < 2; ++type) {
    if (!m_Enabled[type])
      continue;
    if (ids[type] > m_MaximumId[type])
      return QuotaStatus::Overflow;
    auto* entry = m_Tables[type].prepare(ids[type]);
    if (!entry)
      return QuotaStatus::NoMemory;
    const auto status = QuotaTable::canCharge(*entry, bytes, inodes, enforce);
    if (status != QuotaStatus::Success)
      return status;
  }
  return QuotaStatus::Success;
}

void Ext2QuotaLedger::charge(uint32_t uid, uint32_t gid, uint64_t bytes, uint64_t inodes) {
  const uint32_t ids[] = {uid, gid};
  for (size_t type = 0; type < 2; ++type) {
    if (m_Enabled[type])
      QuotaTable::charge(*m_Tables[type].find(ids[type]), bytes, inodes);
  }
}

void Ext2QuotaLedger::refund(uint32_t uid, uint32_t gid, uint64_t bytes, uint64_t inodes) {
  const uint32_t ids[] = {uid, gid};
  for (size_t type = 0; type < 2; ++type) {
    if (m_Enabled[type])
      QuotaTable::refund(*m_Tables[type].find(ids[type]), bytes, inodes);
  }
}

QuotaStatus Ext2QuotaLedger::track(uint32_t inode, uint32_t uid, uint32_t gid, uint64_t bytes) {
  if (find(inode))
    return QuotaStatus::Success;
  const auto status = prepareCharge(uid, gid, bytes, 1, false);
  if (status != QuotaStatus::Success)
    return status;
  auto* entry = new InodeCharge;
  if (!entry)
    return QuotaStatus::NoMemory;
  entry->uid = uid;
  entry->gid = gid;
  entry->bytes = bytes;
  if (!m_Inodes.tryInsert(inode, entry)) {
    delete entry;
    return QuotaStatus::NoMemory;
  }
  charge(uid, gid, bytes, 1);
  return QuotaStatus::Success;
}

QuotaStatus Ext2QuotaLedger::create(uint32_t inode, uint32_t uid, uint32_t gid) {
  assert(!find(inode));
  const auto status = prepareCharge(uid, gid, 0, 1, true);
  return status == QuotaStatus::Success ? track(inode, uid, gid, 0) : status;
}

QuotaStatus Ext2QuotaLedger::reserve(uint32_t inode, uint64_t bytes) {
  auto* entry = find(inode);
  assert(entry);
  if (bytes > ~uint64_t(0) - entry->bytes)
    return QuotaStatus::Overflow;
  if (!entry->exempt) {
    const auto status = prepareCharge(entry->uid, entry->gid, bytes, 0, true);
    if (status != QuotaStatus::Success)
      return status;
    charge(entry->uid, entry->gid, bytes, 0);
  }
  entry->bytes += bytes;
  return QuotaStatus::Success;
}

void Ext2QuotaLedger::refund(uint32_t inode, uint64_t bytes) {
  auto* entry = find(inode);
  // Before the first enable, an untouched inode needs no overlay: trim updates
  // i_blocks and releases the same blocks under this allocation lock.
  if (!entry)
    return;
  assert(bytes <= entry->bytes);
  if (!entry->exempt)
    refund(entry->uid, entry->gid, bytes, 0);
  entry->bytes -= bytes;
}

void Ext2QuotaLedger::forget(uint32_t inode) {
  auto* entry = find(inode);
  if (!entry)
    return;
  if (!entry->exempt)
    refund(entry->uid, entry->gid, entry->bytes, 1);
  m_Inodes.remove(inode);
  delete entry;
}

QuotaStatus Ext2QuotaLedger::transfer(uint32_t inode, uint32_t uid, uint32_t gid) {
  auto* entry = find(inode);
  assert(entry);
  const uint32_t oldIds[] = {entry->uid, entry->gid}, newIds[] = {uid, gid};
  if (!entry->exempt) {
    for (size_t type = 0; type < 2; ++type) {
      if (!m_Enabled[type] || oldIds[type] == newIds[type])
        continue;
      if (newIds[type] > m_MaximumId[type])
        return QuotaStatus::Overflow;
      auto* destination = m_Tables[type].prepare(newIds[type]);
      if (!destination)
        return QuotaStatus::NoMemory;
      const auto status = QuotaTable::canCharge(*destination, entry->bytes, 1, true);
      if (status != QuotaStatus::Success)
        return status;
    }
    for (size_t type = 0; type < 2; ++type) {
      if (!m_Enabled[type] || oldIds[type] == newIds[type])
        continue;
      QuotaTable::refund(*m_Tables[type].find(oldIds[type]), entry->bytes, 1);
      QuotaTable::charge(*m_Tables[type].find(newIds[type]), entry->bytes, 1);
    }
  }
  entry->uid = uid;
  entry->gid = gid;
  return QuotaStatus::Success;
}

QuotaStatus Ext2QuotaLedger::exempt(uint32_t inode, bool exempt) {
  auto* entry = find(inode);
  assert(entry);
  if (entry->exempt == exempt)
    return QuotaStatus::Success;
  if (exempt) {
    refund(entry->uid, entry->gid, entry->bytes, 1);
  } else {
    // Enable retains even exempt owners' empty accounts, so ending an
    // exemption cannot fail after quota-file protection has been published.
    charge(entry->uid, entry->gid, entry->bytes, 1);
  }
  entry->exempt = exempt;
  return QuotaStatus::Success;
}

QuotaStatus Ext2QuotaLedger::enable(QuotaType type, QuotaTable& loaded, uint32_t maximumId) {
  const size_t index = static_cast<size_t>(type);
  if (m_Enabled[index])
    return QuotaStatus::Busy;
  for (auto& entry : loaded.entries()) {
    entry.record.currentSpace = 0;
    entry.record.currentInodes = 0;
    entry.dirty = true;
  }
  for (auto it = m_Inodes.begin(); it != m_Inodes.end(); ++it) {
    const auto& usage = *it.value();
    if ((index ? usage.gid : usage.uid) > maximumId)
      return QuotaStatus::Overflow;
    auto* destination = loaded.prepare(index ? usage.gid : usage.uid);
    if (!destination)
      return QuotaStatus::NoMemory;
    if (usage.exempt)
      continue;
    const auto status = QuotaTable::canCharge(*destination, usage.bytes, 1, false);
    if (status != QuotaStatus::Success)
      return status;
    QuotaTable::charge(*destination, usage.bytes, 1);
  }
  m_Tables[index].swap(loaded);
  m_MaximumId[index] = maximumId;
  m_Enabled[index] = true;
  return QuotaStatus::Success;
}

void Ext2QuotaLedger::disable(QuotaType type) {
  const size_t index = static_cast<size_t>(type);
  m_Enabled[index] = false;
  m_Tables[index].clear();
}

bool Ext2QuotaLedger::enabled(QuotaType type) const {
  return m_Enabled[static_cast<size_t>(type)];
}

QuotaTable& Ext2QuotaLedger::table(QuotaType type) {
  return m_Tables[static_cast<size_t>(type)];
}
