/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/machine/DiskPaging.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

struct DiskEndpoint {
  Disk* disk = nullptr;
  uint32_t id = 0;
  size_t users = 0;
  size_t transfers = 0;
  bool published = false;
  bool retiring = false;
  bool removal = false;
  bool preparing = false;
  bool closing = false;
  PagingChannel* channel = nullptr;
  PagingTransport* transport = nullptr;
  uint64_t bytes = 0;
};

namespace {
DiskEndpoint endpoints[DiskEndpoints::Capacity];
WaitQueue endpointWaiters;
uint32_t nextEndpointId = 1;

bool canWait() {
#if THREADS
  return Processor::getInterrupts() && Processor::information().getCurrentThread();
#else
  return true;
#endif
}
}  // namespace

DiskUse::DiskUse() : m_Endpoint(nullptr) {}
DiskUse::DiskUse(DiskUse&& other) noexcept : m_Endpoint(other.m_Endpoint) {
  other.m_Endpoint = nullptr;
}
DiskUse::~DiskUse() {
  reset();
}
DiskUse& DiskUse::operator=(DiskUse&& other) noexcept {
  if (this != &other) {
    reset();
    m_Endpoint = other.m_Endpoint;
    other.m_Endpoint = nullptr;
  }
  return *this;
}
void DiskUse::reset() {
  if (!m_Endpoint)
    return;
  auto guard = endpointWaiters.acquire();
  DiskEndpoint* endpoint = m_Endpoint;
  m_Endpoint = nullptr;
  if (!endpoint->users)
    panic("Disk endpoint admission underflow");
  --endpoint->users;
  guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(endpoint));
}
Disk* DiskUse::get() const {
  return m_Endpoint ? m_Endpoint->disk : nullptr;
}
DiskUse::operator bool() const {
  return m_Endpoint != nullptr;
}

DiskEndpoint* DiskEndpoints::reserve(Disk* disk) {
  auto guard = endpointWaiters.acquire();
  // The public device encoding reserves twenty bits for the minor number.
  if (!disk || nextEndpointId > 0xfffff)
    return nullptr;
  for (auto& endpoint : endpoints) {
    if (!endpoint.disk) {
      endpoint = DiskEndpoint();
      endpoint.disk = disk;
      endpoint.id = nextEndpointId++;
      return &endpoint;
    }
  }
  return nullptr;
}
void DiskEndpoints::publish(DiskEndpoint* endpoint) {
  if (!endpoint)
    return;
  auto guard = endpointWaiters.acquire();
  if (!endpoint->retiring) {
    endpoint->bytes = endpoint->disk->getSize();
    endpoint->published = true;
  }
}
bool DiskEndpoints::acquire(DiskEndpoint* endpoint, Disk* disk, DiskUse& use) {
  use.reset();
  auto guard = endpointWaiters.acquire();
  if (!endpoint || endpoint->disk != disk || endpoint->retiring || endpoint->removal ||
      endpoint->preparing || endpoint->channel || endpoint->users == ~static_cast<size_t>(0))
    return false;
  ++endpoint->users;
  use.m_Endpoint = endpoint;
  return true;
}
bool DiskEndpoints::acquire(uint32_t id, DiskUse& use) {
  use.reset();
  auto guard = endpointWaiters.acquire();
  for (auto& endpoint : endpoints) {
    if (endpoint.id != id || !endpoint.disk || !endpoint.published || endpoint.retiring)
      continue;
    if (endpoint.removal || endpoint.preparing || endpoint.channel ||
        endpoint.users == ~static_cast<size_t>(0))
      return false;
    ++endpoint.users;
    use.m_Endpoint = &endpoint;
    return true;
  }
  return false;
}
uint32_t DiskEndpoints::id(DiskEndpoint* endpoint, Disk* disk) {
  auto guard = endpointWaiters.acquire();
  return endpoint && endpoint->disk == disk && !endpoint->retiring ? endpoint->id : 0;
}
bool DiskEndpoints::describe(uint32_t id, uint64_t& bytes) {
  auto guard = endpointWaiters.acquire();
  for (const auto& endpoint : endpoints) {
    if (endpoint.id == id && endpoint.disk && endpoint.published && !endpoint.retiring) {
      bytes = endpoint.bytes;
      return true;
    }
  }
  bytes = 0;
  return false;
}
size_t DiskEndpoints::snapshot(uint32_t* ids, size_t capacity) {
  if (!ids)
    return 0;
  auto guard = endpointWaiters.acquire();
  size_t count = 0;
  for (const auto& endpoint : endpoints) {
    if (endpoint.disk && endpoint.published && !endpoint.retiring && count < capacity)
      ids[count++] = endpoint.id;
  }
  return count;
}
bool DiskEndpoints::tryClose(DiskEndpoint* endpoint, Disk* disk) {
  auto guard = endpointWaiters.acquire();
  if (!endpoint || endpoint->disk != disk || endpoint->retiring || endpoint->users ||
      endpoint->preparing || endpoint->channel)
    return false;
  endpoint->removal = true;
  return true;
}
void DiskEndpoints::reopen(DiskEndpoint* endpoint, Disk* disk) {
  auto guard = endpointWaiters.acquire();
  if (endpoint && endpoint->disk == disk && !endpoint->retiring)
    endpoint->removal = false;
}
void DiskEndpoints::retire(DiskEndpoint* endpoint, Disk* disk) {
  if (!endpoint)
    return;
  TerminationDeferral lifetime;
  while (true) {
    auto guard = endpointWaiters.acquire();
    if (endpoint->disk != disk)
      return;
    endpoint->published = false;
    endpoint->retiring = true;
    if (!endpoint->users && !endpoint->preparing && !endpoint->channel) {
      endpoint->disk = nullptr;
      return;
    }
    const auto reason =
        guard.waitForCompletion(WaitQueue::Channel(endpoint), Thread::CallbackDrain);
    (void)reason;
  }
}

PagingChannel::PagingChannel() : m_Endpoint(nullptr) {}
PagingChannel::~PagingChannel() {
  reset();
}
PagingStatus DiskEndpoints::prepare(uint32_t id, PagingChannel& channel) {
  if (!canWait())
    return PagingStatus::Busy;
  TerminationDeferral lifetime;
  DiskEndpoint* selected = nullptr;
  {
    auto guard = endpointWaiters.acquire();
    if (channel.m_Endpoint)
      return PagingStatus::Busy;
    for (auto& endpoint : endpoints) {
      if (endpoint.id != id || !endpoint.disk || !endpoint.published || endpoint.retiring)
        continue;
      if (endpoint.removal || endpoint.users || endpoint.preparing || endpoint.channel)
        return PagingStatus::Busy;
      endpoint.preparing = true;
      selected = &endpoint;
      break;
    }
  }
  if (!selected)
    return PagingStatus::Closed;
  PagingTransport* transport = nullptr;
  PagingStatus result = selected->disk->preparePagingTransport(transport);
  {
    auto guard = endpointWaiters.acquire();
    if (result == PagingStatus::Success && transport && !selected->retiring) {
      selected->transport = transport;
      selected->channel = &channel;
      channel.m_Endpoint = selected;
      selected->preparing = false;
      guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(selected));
      return PagingStatus::Success;
    }
  }
  // Preparation owns the endpoint until backend rollback releases controller
  // admission. Hardware teardown cannot consume the backend in this interval.
  if (transport)
    transport->release();
  {
    auto guard = endpointWaiters.acquire();
    selected->preparing = false;
    guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(selected));
  }
  return result == PagingStatus::Success ? PagingStatus::Closed : result;
}
PagingStatus PagingChannel::transfer(PagingOperation operation, uint64_t offset, void* page) {
  if (!canWait())
    return PagingStatus::Busy;
  TerminationDeferral lifetime;
  DiskEndpoint* endpoint = nullptr;
  PagingTransport* transport = nullptr;
  {
    auto guard = endpointWaiters.acquire();
    endpoint = m_Endpoint;
    if (!endpoint || endpoint->channel != this || endpoint->closing)
      return PagingStatus::Closed;
    if (operation != PagingOperation::Flush &&
        (!page || (offset % PageBytes) || offset > endpoint->bytes ||
         PageBytes > endpoint->bytes - offset))
      return PagingStatus::Invalid;
    if (endpoint->transfers == ~static_cast<size_t>(0))
      return PagingStatus::Busy;
    ++endpoint->transfers;
    transport = endpoint->transport;
  }
  const PagingStatus result = transport->transfer(operation, offset, page);
  {
    auto guard = endpointWaiters.acquire();
    --endpoint->transfers;
    guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(endpoint));
  }
  return result;
}
PagingStatus PagingChannel::readPage(uint64_t offset, void* page) {
  return transfer(PagingOperation::Read, offset, page);
}
PagingStatus PagingChannel::writePage(uint64_t offset, const void* page) {
  return transfer(PagingOperation::Write, offset, const_cast<void*>(page));
}
PagingStatus PagingChannel::flush() {
  return transfer(PagingOperation::Flush, 0, nullptr);
}
void PagingChannel::reset() {
  TerminationDeferral lifetime;
  DiskEndpoint* endpoint = nullptr;
  PagingTransport* transport = nullptr;
  bool ownsClosure = false;
  while (true) {
    auto guard = endpointWaiters.acquire();
    endpoint = m_Endpoint;
    if (!endpoint)
      return;
    if (!endpoint->closing) {
      endpoint->closing = true;
      ownsClosure = true;
    }
    if (ownsClosure && !endpoint->transfers) {
      transport = endpoint->transport;
      break;
    }
    const auto reason =
        guard.waitForCompletion(WaitQueue::Channel(endpoint), Thread::CallbackDrain);
    (void)reason;
  }
  transport->release();
  auto guard = endpointWaiters.acquire();
  endpoint->transport = nullptr;
  endpoint->channel = nullptr;
  endpoint->closing = false;
  m_Endpoint = nullptr;
  guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(endpoint));
}
uint32_t PagingChannel::endpointId() const {
  auto guard = endpointWaiters.acquire();
  return m_Endpoint ? m_Endpoint->id : 0;
}
uint64_t PagingChannel::size() const {
  auto guard = endpointWaiters.acquire();
  return m_Endpoint ? m_Endpoint->bytes : 0;
}
PagingChannel::operator bool() const {
  auto guard = endpointWaiters.acquire();
  return m_Endpoint != nullptr;
}
