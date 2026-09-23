/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "VirtioRng.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/SecureRandom.h"
#include "pedigree/kernel/utilities/lib.h"

VirtioRng::VirtioRng(Device* pci)
    : m_Pci(pci), m_Transport(pci), m_Queue(), m_Data("virtio-rng data"), m_Shutdown(false) {}

VirtioRng::~VirtioRng() {
  shutdown();
  if (m_Data.virtualAddress()) {
    pedigree_random::erase(m_Data.virtualAddress(), PhysicalMemoryManager::getPageSize());
  }
}

bool VirtioRng::seedKernel() {
  if (!m_Pci || m_Pci->getPciVendorId() != 0x1af4 ||
      (m_Pci->getPciDeviceId() != 0x1044 && m_Pci->getPciDeviceId() != 0x1005) ||
      !m_Transport.initialise() || !m_Transport.negotiate(0) ||
      !m_Transport.setupQueue(0, m_Queue)) {
    return false;
  }

  auto& memory = PhysicalMemoryManager::instance();
  const size_t flags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;
  if (!memory.allocateRegion(m_Data, 1, PhysicalMemoryManager::continuous, flags)) {
    return false;
  }
  // This one-shot bootstrap runs during module initialization, where IRQ
  // handler retirement cannot wait for a thread-context dispatch to finish.
  if (!m_Transport.ready() || !PciBus::instance().updateCommand(m_Pci, 0, 0x400U)) {
    return false;
  }

  uint8_t seed[32] = {};
  size_t received = 0;
  const auto deadline = Time::getTicks() + 5 * Time::Multiplier::Second;
  while (received < sizeof(seed) && Time::getTicks() < deadline) {
    const size_t requested = sizeof(seed) - received;
    Virtio::Buffer buffer = {m_Data.physicalAddress(), static_cast<uint32_t>(requested), true};
    if (!m_Queue.submit(&buffer, 1, this)) {
      break;
    }
    m_Transport.notify(0);

    Virtio::Completion completion{};
    bool completed = false;
    while (Time::getTicks() < deadline) {
      if (m_Queue.pop(completion)) {
        completed = true;
        break;
      }
      Time::delay(Time::Multiplier::Millisecond);
    }
    if (!completed) {
      ERROR("virtio-rng: seed request timed out");
      break;
    }
    if (completion.cookie != this || !completion.length || completion.length > requested) {
      ERROR("virtio-rng: malformed completion");
      break;
    }
    FENCE();
    MemoryCopy(seed + received, m_Data.virtualAddress(), completion.length);
    received += completion.length;
  }

  const bool seeded = received == sizeof(seed) && secure_random_seed(seed, sizeof(seed));
  shutdown();
  pedigree_random::erase(seed, sizeof(seed));
  if (seeded) {
    NOTICE("virtio-rng: seeded secure random generator");
  }
  return seeded;
}

void VirtioRng::shutdown() {
  if (m_Shutdown) {
    return;
  }
  if (!m_Transport.reset()) {
    panic("virtio-rng: device did not stop DMA");
  }
  m_Queue.stop();
  m_Shutdown = true;
}
