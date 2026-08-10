/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MACHINE_NETWORK_STACK_H
#define MACHINE_NETWORK_STACK_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/MemoryPool.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/Vector.h"

// lwIP network interface type
struct netif;

/**
 * The Pedigree network stack
 * This function is the base for receiving packets, and provides functionality
 * for keeping track of network devices in the system.
 */
class EXPORTED_PUBLIC NetworkStack : public RequestQueue {
 public:
  NetworkStack();
  virtual ~NetworkStack();

  /** For access to the stack without declaring an instance of it */
  static NetworkStack& instance() {
    return *stack;
  }

  /** Called when a packet arrives */
  void receive(size_t nBytes, uintptr_t packet, Network* pCard, uint32_t offset);

  /** Registers a given network device with the stack */
  void registerDevice(Network* pDevice);

  /** Returns the n'th registered network device */
  Network* getDevice(size_t n);

  /** Returns the number of devices registered with the stack */
  size_t getNumDevices();

  /** Unregisters a given network device from the stack */
  void deRegisterDevice(Network* pDevice);

  /** Sets the loopback device for the stack */
  void setLoopback(Network* pCard) {
    m_pLoopback = pCard;
  }

  /** Gets the loopback device for the stack */
  inline Network* getLoopback() {
    return m_pLoopback;
  }

  /** Grabs the memory pool for networking use */
  inline MemoryPool& getMemPool() {
    return m_MemPool;
  }

  /** Abstraction for a packet. */
  class Packet {
    friend class NetworkStack;

   public:
    Packet();
    virtual ~Packet();

    uintptr_t getBuffer() const {
      return m_Buffer;
    }

    size_t getLength() const {
      return m_PacketLength;
    }

    Network* getCard() const {
      return m_pCard;
    }

    uint32_t getOffset() const {
      return m_Offset;
    }

   private:
    bool copyFrom(uintptr_t otherPacket, size_t size);

    uintptr_t m_Buffer;
    size_t m_PacketLength;
    Network* m_pCard;
    uint32_t m_Offset;
    Mutex m_Pushed;
  };

  /** Get an interface for a card. */
  struct netif* getInterface(Network* pCard) const {
    return m_Interfaces.lookup(pCard);
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  enum class HostedReceiveEvent {
    Queued,
    BeforeDispatch,
    Delivered,
    DiscardedStale,
    Cancelled,
  };

  using HostedReceiveHook = void (*)(HostedReceiveEvent event, uintptr_t buffer, Network* card,
                                     size_t generation);

  static void setHostedReceiveHook(HostedReceiveHook hook);
  static size_t getHostedRegistrationGeneration(Network* card);
  static size_t getHostedReceiveRequestCapacity();
#endif

 private:
  static NetworkStack* stack;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  static HostedReceiveHook m_HostedReceiveHook;
#endif

  virtual uint64_t executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
                                  uint64_t p6, uint64_t p7, uint64_t p8);
  virtual void cancelRequest(const Request& request);

  /** Releases a receive payload which the queue did not execute. */
  static void cancelReceive(uintptr_t buffer, Network* card, size_t generation);

  static constexpr size_t ReceiveRequestCapacity = 256;

  /** Loopback device */
  Network* m_pLoopback;

  /** Network devices registered with the stack. */
  Vector<Network*> m_Children;

  /** Networking memory pool */
  MemoryPool m_MemPool;

#if THREADS || UTILITY_LINUX
  Mutex m_Lock;
#endif

  /** lwIP interfaces for each of our cards. */
  Tree<Network*, struct netif*> m_Interfaces;

  /** Next interface number to assign. */
  size_t m_NextInterfaceNumber;

  /** Next non-zero registration generation. */
  size_t m_NextDeviceGeneration;

  // Keep preallocated publication state after the pre-existing fields so
  // their offsets remain stable for inline accessors compiled into driver
  // modules.
  PreallocatedRequest m_ReceiveRequests[ReceiveRequestCapacity];

  /** Starting token for the next bounded preallocated-publication scan. */
  Atomic<size_t> m_NextReceiveRequest;
};

#endif
