/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_VIRTIO_QUEUE_H
#define PEDIGREE_VIRTIO_QUEUE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/types.h"

namespace Virtio {

class PciTransport;

struct Buffer {
  uint64_t address;
  uint32_t length;
  bool deviceWrites;
};

struct Completion {
  void* cookie;
  uint32_t length;
};

class EXPORTED_PUBLIC Queue {
 public:
  Queue();
  ~Queue();

  // Payload addresses must refer to DMA-safe, physically contiguous memory.
  // The caller retains them until pop() or a successful transport reset.
  bool submit(const Buffer* buffers, size_t count, void* cookie);
  bool pop(Completion& completion);
  uint16_t depth() const {
    return m_Depth;
  }
  void stop();

 private:
  friend class PciTransport;
  static constexpr uint16_t MaxDepth = 256;
  static constexpr uint16_t Invalid = 0xffff;

  struct Descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
  } __attribute__((packed));

  bool initialise(uint16_t depth, PciTransport* transport);
  uint64_t descriptorAddress() const {
    return m_Descriptors.physicalAddress();
  }
  uint64_t availableAddress() const {
    return m_Available.physicalAddress();
  }
  uint64_t usedAddress() const {
    return m_Used.physicalAddress();
  }

  MemoryRegion m_Descriptors;
  MemoryRegion m_Available;
  MemoryRegion m_Used;
  Mutex m_Lock;
  PciTransport* m_Transport;
  void* m_Cookies[MaxDepth];
  uint16_t m_Next[MaxDepth];
  uint16_t m_ChainLength[MaxDepth];
  uint16_t m_FreeHead;
  uint16_t m_FreeCount;
  uint16_t m_Depth;
  uint16_t m_AvailableIndex;
  uint16_t m_UsedIndex;
  bool m_Active[MaxDepth];
  bool m_Online;
  bool m_DmaArmed;
  bool m_Attached;
};

}  // namespace Virtio
#endif
