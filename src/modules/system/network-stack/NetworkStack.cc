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

#include "NetworkStack.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/Processor.h"

#include "Filter.h"
#include "modules/Module.h"
#include "modules/system/lwip/include/lwip/autoip.h"
#include "modules/system/lwip/include/lwip/dhcp.h"
#include "modules/system/lwip/include/lwip/etharp.h"
#include "modules/system/lwip/include/lwip/ethip6.h"
#include "modules/system/lwip/include/lwip/netif.h"
#include "modules/system/lwip/include/lwip/tcpip.h"
#include "modules/system/lwip/include/netif/ethernet.h"

NetworkStack* NetworkStack::stack = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
NetworkStack::HostedReceiveHook NetworkStack::m_HostedReceiveHook = nullptr;
#endif

static NetworkStack* g_NetworkStack = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS && PEDIGREE_HOSTED_NETWORK_REGRESSION
extern bool runHostedNetworkStackRegressions();
#endif

static err_t linkOutput(struct netif* netif, struct pbuf* p) {
  Network* pDevice = reinterpret_cast<Network*>(netif->state);

  size_t totalLength = p->tot_len;

  // pull the chain of pbufs into a single packet to transmit
  size_t offset = 0;
  char* output = new char[totalLength];

  pbuf_copy_partial(p, output, totalLength, 0);

  // Check for filtering
  if (!NetworkFilter::instance().filter(1, reinterpret_cast<uintptr_t>(output), totalLength)) {
    pDevice->droppedPacket();
    delete[] output;
    return ERR_IF;  // Drop the packet.
  }

  // transmit!
  err_t e = ERR_OK;
  if (!pDevice->send(totalLength, reinterpret_cast<uintptr_t>(output))) {
    e = ERR_IF;
  }

  delete[] output;

  return e;
}

static void netifStatusUpdate(struct netif* netif) {
  if (netif_is_up(netif)) {
    NOTICE("netif " << String(netif->name, 2) << Dec << netif->num << ": is now up");
  } else {
    NOTICE("netif " << String(netif->name, 2) << Dec << netif->num << ": is now down");
  }
}

static void netifLinkUpdate(struct netif* netif) {
  if (netif_is_link_up(netif)) {
    NOTICE("netif " << String(netif->name, 2) << Dec << netif->num << ": link is now up");
  } else {
    NOTICE("netif " << String(netif->name, 2) << Dec << netif->num << ": link is now down");
  }
}

static err_t netifInit(struct netif* netif) {
  Network* pDevice = reinterpret_cast<Network*>(netif->state);
  StationInfo info = pDevice->getStationInfo();

  /// \todo a lot of this is hardcoded, which is not great
  netif->hwaddr_len = 6;
  MemoryCopy(netif->hwaddr, info.mac.getMac(), 6);
  netif->mtu = 1400;
  netif->flags =
      NETIF_FLAG_LINK_UP | NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  netif->linkoutput = linkOutput;
  netif->output = etharp_output;
  netif->output_ip6 = ethip6_output;

  netif_set_status_callback(netif, netifStatusUpdate);
  netif_set_link_callback(netif, netifLinkUpdate);

  return ERR_OK;
}

static void removeInterface(void* context) {
  struct netif* iface = reinterpret_cast<struct netif*>(context);
#if LWIP_DHCP
  dhcp_stop(iface);
#endif
#if LWIP_AUTOIP
  autoip_stop(iface);
#endif
  netif_remove(iface);
}

struct AddInterfaceContext {
  struct netif* iface;
  const ip4_addr_t* ipaddr;
  const ip4_addr_t* netmask;
  const ip4_addr_t* gateway;
  Network* device;
  struct netif* result;
};

static void addInterface(void* context) {
  AddInterfaceContext* add = reinterpret_cast<AddInterfaceContext*>(context);
  add->result = netif_add(add->iface, add->ipaddr, add->netmask, add->gateway, add->device,
                          netifInit, tcpip_input);
}

NetworkStack::NetworkStack()
    : RequestQueue(MakeConstantString("Network Stack")),
      m_pLoopback(0),
      m_Children(),
      m_MemPool("network-pool")
#if THREADS || UTILITY_LINUX
      ,
      m_Lock()
#endif
      ,
      m_Interfaces(),
      m_NextInterfaceNumber(0),
      m_NextDeviceGeneration(1),
      m_NextReceiveRequest(0) {
  if (__atomic_load_n(&stack, __ATOMIC_ACQUIRE)) {
    FATAL("NetworkStack created multiple times.");
  }

  __atomic_store_n(&stack, this, __ATOMIC_RELEASE);

  // Couple the queue's admission bound to the preallocated publication
  // slots rather than relying on RequestQueue's default remaining 256.
  m_nMaxAsyncRequests = ReceiveRequestCapacity;

  initialise();

#if X86_COMMON || HOSTED
  // Lots of RAM to burn! Try 16 MB, then 8 MB, then 4 MB, then give up
  if (!m_MemPool.initialise(4096, 1600))
    if (!m_MemPool.initialise(2048, 1600))
      if (!m_MemPool.initialise(1024, 1600))
        ERROR("Couldn't get a valid buffer pool for networking use");
#else
#warning Unhandled architecture for the NetworkStack buffer pool
#endif
}

NetworkStack::~NetworkStack() {
  destroy();

  Vector<struct netif*> interfaces;
#if THREADS || UTILITY_LINUX
  m_Lock.acquire();
#endif
  for (Tree<Network*, struct netif*>::Iterator it = m_Interfaces.begin();
       it != m_Interfaces.end(); ++it) {
    interfaces.pushBack(it.value());
  }
  m_Interfaces.clear();
  m_Children.clear();
  m_pLoopback = nullptr;
#if THREADS || UTILITY_LINUX
  m_Lock.release();
#endif

  for (Vector<struct netif*>::Iterator it = interfaces.begin(); it != interfaces.end(); ++it) {
    if (tcpip_callback_wait(removeInterface, *it) != ERR_OK) {
      FATAL("NetworkStack could not retire a network interface");
    }
    delete *it;
  }

  __atomic_store_n(&stack, static_cast<NetworkStack*>(nullptr), __ATOMIC_RELEASE);
}

uint64_t NetworkStack::executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
                                      uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8) {
  struct pbuf* p = reinterpret_cast<struct pbuf*>(p1);
  Network* card = reinterpret_cast<Network*>(p2);
  const size_t generation = static_cast<size_t>(p3);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  HostedReceiveHook hook = __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(HostedReceiveEvent::BeforeDispatch, reinterpret_cast<uintptr_t>(p), card, generation);
  }
#endif

  // OK, we are now processing the packet.
  // We hold a lock that allows us to handle concurrency (not an issue with
  // a true RequestQueue, but is an issue on other environments).
#if THREADS || UTILITY_LINUX
  LockGuard<Mutex> guard(m_Lock);
#endif

  struct netif* iface = m_Interfaces.lookup(card);
  const size_t activeGeneration =
      iface ? __atomic_load_n(&card->m_NetworkStackGeneration, __ATOMIC_ACQUIRE) : 0;

  if (iface && generation && generation == activeGeneration) {
    const err_t inputResult = iface->input(p, iface);
    if (inputResult != ERR_OK) {
      // lwIP transfers pbuf ownership only when the input function succeeds.
      pbuf_free(p);
    }
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    hook = __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
    if (hook) {
      hook(HostedReceiveEvent::Delivered, reinterpret_cast<uintptr_t>(p), card, generation);
    }
#endif
  } else {
    // Device removal can overtake work which was already copied into the
    // queue. Resolve the interface under the same lock as deregistration
    // instead of retaining a freed netif pointer in the request.
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    hook = __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
    if (hook) {
      hook(HostedReceiveEvent::DiscardedStale, reinterpret_cast<uintptr_t>(p), card, generation);
    }
#endif
    pbuf_free(p);
  }

  return 0;
}

void NetworkStack::cancelRequest(const Request& request) {
  cancelReceive(static_cast<uintptr_t>(request.p1), reinterpret_cast<Network*>(request.p2),
                static_cast<size_t>(request.p3));
}

void NetworkStack::cancelReceive(uintptr_t buffer, Network* card, size_t generation) {
  if (buffer) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HostedReceiveHook hook = __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
    if (hook) {
      hook(HostedReceiveEvent::Cancelled, buffer, card, generation);
    }
#endif
    pbuf_free(reinterpret_cast<struct pbuf*>(buffer));
  }
}

void NetworkStack::receive(size_t nBytes, uintptr_t packet, Network* pCard, uint32_t offset) {
  packet += offset;

  // Check for filtering before doing anything else
  if (!NetworkFilter::instance().filter(1, packet, nBytes)) {
    pCard->droppedPacket();
    return;  // Drop the packet.
  }

  struct pbuf* p = pbuf_alloc(PBUF_RAW, nBytes, PBUF_POOL);
  if (p != 0) {
    struct pbuf* buf = p;
    while (buf != nullptr) {
      size_t copyLength = buf->len;
      MemoryCopy(buf->payload, reinterpret_cast<void*>(packet), buf->len);

      packet += buf->len;
      nBytes -= buf->len;

      buf = buf->next;
    }
  } else {
    ERROR(
        "Network Stack: Out of memory pool space, dropping incoming "
        "packet");
    pCard->droppedPacket();
    return;
  }

  const size_t generation = __atomic_load_n(&pCard->m_NetworkStackGeneration, __ATOMIC_ACQUIRE);
  if (!generation) {
    pbuf_free(p);
    return;
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  HostedReceiveHook hook = __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(HostedReceiveEvent::Queued, reinterpret_cast<uintptr_t>(p), pCard, generation);
  }
#endif

  const size_t firstRequest = (m_NextReceiveRequest += 1) - 1;
  for (size_t i = 0; i < ReceiveRequestCapacity; ++i) {
    const size_t request = (firstRequest + i) % ReceiveRequestCapacity;
    const PreallocatedPublishResult result =
        publishPreallocated(m_ReceiveRequests[request], 0, reinterpret_cast<uint64_t>(p),
                            reinterpret_cast<uintptr_t>(pCard), generation);
    if (result == PreallocatedPublishResult::Accepted) {
      return;
    }
    if (result != PreallocatedPublishResult::TokenBusy) {
      break;
    }
  }

  // No queue token retained this payload, so preserve the same cancellation
  // event and single pbuf release used by RequestQueue teardown.
  cancelReceive(reinterpret_cast<uintptr_t>(p), pCard, generation);
}

void NetworkStack::registerDevice(Network* pDevice) {
#if THREADS || UTILITY_LINUX
  LockGuard<Mutex> guard(m_Lock);
#endif

  size_t interfaceNumber = m_NextInterfaceNumber++;

  if (interfaceNumber >= 0xFFU) {
    FATAL("Too many network interfaces!");
  }

  m_Children.pushBack(pDevice);

  size_t generation = m_NextDeviceGeneration++;
  if (!generation) {
    generation = m_NextDeviceGeneration++;
  }
  __atomic_store_n(&pDevice->m_NetworkStackGeneration, generation, __ATOMIC_RELEASE);

  struct netif* iface = new struct netif;
  ByteSet(iface, 0, sizeof(*iface));

  ip4_addr_t ipaddr;
  ip4_addr_t netmask;
  ip4_addr_t gateway;

  // for dhcp/auto configuration
  ByteSet(&ipaddr, 0, sizeof(ipaddr));
  ByteSet(&netmask, 0, sizeof(netmask));
  ByteSet(&gateway, 0, sizeof(gateway));

  iface->name[0] = 'e';
  iface->name[1] = 'n';
  iface->num = interfaceNumber;

  // netif_add mutates lwIP's global interface list. Keep the registry lock
  // across this retired callback so deregistration cannot overtake a
  // half-published device while the tcpip core performs that mutation.
  AddInterfaceContext add = {iface, &ipaddr, &netmask, &gateway, pDevice, nullptr};
  if (tcpip_callback_wait(addInterface, &add) != ERR_OK || !add.result) {
    for (Vector<Network*>::Iterator it = m_Children.begin(); it != m_Children.end(); ++it) {
      if (*it == pDevice) {
        m_Children.erase(it);
        break;
      }
    }
    __atomic_store_n(&pDevice->m_NetworkStackGeneration, 0, __ATOMIC_RELEASE);
    delete iface;
    FATAL("NetworkStack could not register a network interface");
    return;
  }
  iface = add.result;

  m_Interfaces.insert(pDevice, iface);
}

Network* NetworkStack::getDevice(size_t n) {
  return m_Children[n];
}

size_t NetworkStack::getNumDevices() {
  return m_Children.count();
}

void NetworkStack::clearLoopback(Network* pCard) {
#if THREADS || UTILITY_LINUX
  LockGuard<Mutex> guard(m_Lock);
#endif
  if (m_pLoopback == pCard) {
    m_pLoopback = nullptr;
  }
}

void NetworkStack::deRegisterDevice(Network* pDevice) {
  struct netif* iface = nullptr;
#if THREADS || UTILITY_LINUX
  m_Lock.acquire();
#endif

  __atomic_store_n(&pDevice->m_NetworkStackGeneration, 0, __ATOMIC_RELEASE);

  int i = 0;
  for (Vector<Network*>::Iterator it = m_Children.begin(); it != m_Children.end(); it++, i++)
    if (*it == pDevice) {
      m_Children.erase(it);
      break;
    }

  iface = m_Interfaces.lookup(pDevice);
  m_Interfaces.remove(pDevice);

#if THREADS || UTILITY_LINUX
  m_Lock.release();
#endif

  if (iface != nullptr) {
    if (tcpip_callback_wait(removeInterface, iface) != ERR_OK) {
      FATAL("NetworkStack could not retire a network interface");
    }
    delete iface;
  }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void NetworkStack::setHostedReceiveHook(HostedReceiveHook hook) {
  __atomic_store_n(&m_HostedReceiveHook, hook, __ATOMIC_RELEASE);
}

size_t NetworkStack::getHostedRegistrationGeneration(Network* card) {
  return __atomic_load_n(&card->m_NetworkStackGeneration, __ATOMIC_ACQUIRE);
}

size_t NetworkStack::getHostedReceiveRequestCapacity() {
  return ReceiveRequestCapacity;
}
#endif

NetworkStack::Packet::Packet() = default;

NetworkStack::Packet::~Packet() {
  // Packet destroyed, clean up our buffer if it existed.
  if (m_Buffer) {
    NetworkStack::instance().m_MemPool.free(m_Buffer);
  }
}

bool NetworkStack::Packet::copyFrom(uintptr_t otherPacket, size_t size) {
  uint8_t* safePacket =
      reinterpret_cast<uint8_t*>(NetworkStack::instance().m_MemPool.allocateNow());
  if (!safePacket) {
    return false;
  }
  MemoryCopy(safePacket, reinterpret_cast<void*>(otherPacket), size);

  m_Buffer = reinterpret_cast<uintptr_t>(safePacket);
  m_PacketLength = size;
  return true;
}

static bool entry() {
  g_NetworkStack = new NetworkStack();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS && PEDIGREE_HOSTED_NETWORK_REGRESSION
  if (!runHostedNetworkStackRegressions()) {
    delete g_NetworkStack;
    g_NetworkStack = nullptr;
    return false;
  }
#endif

  return true;
}

static void exit() {
  delete g_NetworkStack;
}

// NetManager exposes a Filesystem, and so needs the vfs module.
MODULE_INFO("network-stack", &entry, &exit, "config", "vfs", "lwip");
