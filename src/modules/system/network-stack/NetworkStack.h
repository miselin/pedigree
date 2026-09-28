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
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/MemoryPool.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/Vector.h"

#include <config.h>

// lwIP network interface type
struct netif;

/**
 * The Pedigree network stack
 * This function is the base for receiving packets, and provides functionality
 * for keeping track of network devices in the system.
 */
class EXPORTED_PUBLIC NetworkStack {
 public:
  /**
   * Pins one registered device and its lwIP interface until the lease leaves
   * scope. Device deregistration removes the registration from discovery and
   * waits for every admitted lease before returning. Device owners must
   * deregister before the most-derived destructor begins and keep
   * reader-visible state stable until deregistration returns.
   */
  class EXPORTED_PUBLIC DeviceLease {
   public:
    DeviceLease();
    DeviceLease(DeviceLease&& other);
    ~DeviceLease();

    DeviceLease& operator=(DeviceLease&& other);

    Network* device() const {
      return m_Device;
    }

    struct netif* interface() const {
      return m_Interface;
    }

    explicit operator bool() const {
      return m_Device != nullptr;
    }

   private:
    friend class NetworkStack;

    DeviceLease(Network* device, struct netif* interface, OperationBarrier::Lease&& lease);

    DeviceLease(const DeviceLease&) = delete;
    DeviceLease& operator=(const DeviceLease&) = delete;

    Network* m_Device;
    struct netif* m_Interface;
    OperationBarrier::Lease m_Lease;
  };

  NetworkStack();
  virtual ~NetworkStack();

  /** For access to the stack without declaring an instance of it */
  static NetworkStack& instance() {
    return *__atomic_load_n(&stack, __ATOMIC_ACQUIRE);
  }

  static NetworkStack* instanceIfAvailable() {
    return __atomic_load_n(&stack, __ATOMIC_ACQUIRE);
  }

  /** Delivers a packet from a driver worker; must not run in a hard IRQ. */
  void receive(size_t nBytes, uintptr_t packet, Network* pCard, uint32_t offset);

  /** Registers a given network device with the stack */
  void registerDevice(Network* pDevice);

  /** Pins the n'th registered network device and interface. */
  MUST_USE_RESULT bool acquireDevice(size_t n, DeviceLease& lease);

  /** Pins a specific registered network device and interface. */
  MUST_USE_RESULT bool acquireDevice(Network* pDevice, DeviceLease& lease);

  /** Unregisters a given network device from the stack */
  void deRegisterDevice(Network* pDevice);

  /** Grabs the memory pool for networking use */
  inline MemoryPool& getMemPool() {
    return m_MemPool;
  }

 private:
  struct DeviceRegistration;

  static NetworkStack* stack;

  /** Network devices registered with the stack. */
  Vector<Network*> m_Children;

  /** Networking memory pool */
  MemoryPool m_MemPool;

#if THREADS || UTILITY_LINUX
  Mutex m_Lock;
#endif

  /** Next interface number to assign. */
  size_t m_NextInterfaceNumber;

  /** Read-side lifetime state for registered devices and interfaces. */
  Tree<Network*, DeviceRegistration*> m_Registrations;
};

#endif
