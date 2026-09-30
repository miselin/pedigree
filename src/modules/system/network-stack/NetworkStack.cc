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
#include "pedigree/kernel/Metrics.h"
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

static NetworkStack* g_NetworkStack = 0;

struct NetworkStack::DeviceRegistration {
  explicit DeviceRegistration(struct netif* registeredInterface)
      : interface(registeredInterface), readers() {}

  struct netif* interface;
  OperationBarrier readers;
};

NetworkStack::DeviceLease::DeviceLease() : m_Device(nullptr), m_Interface(nullptr), m_Lease() {}

NetworkStack::DeviceLease::DeviceLease(Network* device, struct netif* interface,
                                       OperationBarrier::Lease&& lease)
    : m_Device(device), m_Interface(interface), m_Lease(pedigree_std::move(lease)) {}

NetworkStack::DeviceLease::DeviceLease(DeviceLease&& other)
    : m_Device(other.m_Device),
      m_Interface(other.m_Interface),
      m_Lease(pedigree_std::move(other.m_Lease)) {
  other.m_Device = nullptr;
  other.m_Interface = nullptr;
}

NetworkStack::DeviceLease::~DeviceLease() {}

NetworkStack::DeviceLease& NetworkStack::DeviceLease::operator=(DeviceLease&& other) {
  if (this != &other) {
    m_Lease = pedigree_std::move(other.m_Lease);
    m_Device = other.m_Device;
    m_Interface = other.m_Interface;
    other.m_Device = nullptr;
    other.m_Interface = nullptr;
  }
  return *this;
}

#if PEDIGREE_HOSTED_NETWORK_REGRESSION
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
    Metrics::increment(Metrics::NetworkTxFiltered);
    pDevice->droppedPacket();
    delete[] output;
    return ERR_IF;  // Drop the packet.
  }

  // transmit!
  err_t e = ERR_OK;
  if (!pDevice->send(totalLength, reinterpret_cast<uintptr_t>(output))) {
    Metrics::increment(Metrics::NetworkTxSendFailed);
    e = ERR_IF;
  } else {
    Metrics::increment(Metrics::NetworkTxAccepted);
    Metrics::add(Metrics::NetworkTxBytes, totalLength);
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
    : m_Children(),
      m_MemPool("network-pool")
#if THREADS || UTILITY_LINUX
      ,
      m_Lock()
#endif
      ,
      m_NextInterfaceNumber(0),
      m_Registrations() {
  if (__atomic_load_n(&stack, __ATOMIC_ACQUIRE)) {
    FATAL("NetworkStack created multiple times.");
  }

  __atomic_store_n(&stack, this, __ATOMIC_RELEASE);

  if (!m_MemPool.initialise(4096, 1600) && !m_MemPool.initialise(2048, 1600) &&
      !m_MemPool.initialise(1024, 1600)) {
    ERROR("Couldn't get a valid buffer pool for networking use");
  }
}

NetworkStack::~NetworkStack() {
  Vector<DeviceRegistration*> registrations;
#if THREADS || UTILITY_LINUX
  m_Lock.acquire();
#endif
  for (Tree<Network*, DeviceRegistration*>::Iterator it = m_Registrations.begin();
       it != m_Registrations.end(); ++it) {
    DeviceRegistration* registration = it.value();
    registration->readers.close();
    registrations.pushBack(registration);
  }
  m_Registrations.clear();
  m_Children.clear();
#if THREADS || UTILITY_LINUX
  m_Lock.release();
#endif

  for (Vector<DeviceRegistration*>::Iterator it = registrations.begin(); it != registrations.end();
       ++it) {
    DeviceRegistration* registration = *it;
    registration->readers.wait();
    if (tcpip_callback_wait(removeInterface, registration->interface) != ERR_OK) {
      FATAL("NetworkStack could not retire a network interface");
    }
    delete registration->interface;
    delete registration;
  }

  __atomic_store_n(&stack, static_cast<NetworkStack*>(nullptr), __ATOMIC_RELEASE);
}

void NetworkStack::receive(size_t nBytes, uintptr_t packet, Network* pCard, uint32_t offset) {
  assert(!Processor::inDeviceHardIrq());

  DeviceLease device;
  if (!acquireDevice(pCard, device)) {
    Metrics::increment(Metrics::NetworkRxNoDevice);
    return;
  }

  receive(nBytes, packet, device, offset);
}

void NetworkStack::receive(size_t nBytes, uintptr_t packet, const DeviceLease& device,
                           uint32_t offset) {
  assert(!Processor::inDeviceHardIrq());
  assert(device.device());
  Network* pCard = device.device();
  packet += offset;

  const size_t packetLength = nBytes;

  if (!NetworkFilter::instance().filter(1, packet, nBytes)) {
    Metrics::increment(Metrics::NetworkRxFiltered);
    pCard->droppedPacket();
    return;  // Drop the packet.
  }

  struct pbuf* p = pbuf_alloc(PBUF_RAW, nBytes, PBUF_POOL);
  if (p != 0) {
    struct pbuf* buf = p;
    while (buf != nullptr) {
      MemoryCopy(buf->payload, reinterpret_cast<void*>(packet), buf->len);

      packet += buf->len;
      nBytes -= buf->len;

      buf = buf->next;
    }
  } else {
    Metrics::increment(Metrics::NetworkRxNoBuffer);
    ERROR(
        "Network Stack: Out of memory pool space, dropping incoming "
        "packet");
    pCard->droppedPacket();
    return;
  }

  struct netif* iface = device.interface();
  if (iface->input(p, iface) != ERR_OK) {
    Metrics::increment(Metrics::NetworkRxInputFailed);
    pbuf_free(p);
    pCard->droppedPacket();
  } else {
    Metrics::increment(Metrics::NetworkRxAccepted);
    Metrics::add(Metrics::NetworkRxBytes, packetLength);
  }
}

void NetworkStack::registerDevice(Network* pDevice) {
#if THREADS || UTILITY_LINUX
  LockGuard<Mutex> guard(m_Lock);
#endif

  size_t interfaceNumber = m_NextInterfaceNumber++;

  if (interfaceNumber >= 0xFFU) {
    FATAL("Too many network interfaces!");
  }

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
    delete iface;
    FATAL("NetworkStack could not register a network interface");
    return;
  }
  iface = add.result;

  DeviceRegistration* registration = new DeviceRegistration(iface);
  m_Children.pushBack(pDevice);
  m_Registrations.insert(pDevice, registration);
}

bool NetworkStack::acquireDevice(size_t n, DeviceLease& lease) {
  lease = DeviceLease();
#if THREADS || UTILITY_LINUX
  LockGuard<Mutex> guard(m_Lock);
#endif
  if (n >= m_Children.count()) {
    return false;
  }

  Network* device = m_Children[n];
  DeviceRegistration* registration = m_Registrations.lookup(device);
  OperationBarrier::Lease reader;
  if (!registration || !registration->readers.tryAcquire(reader)) {
    return false;
  }

  lease = DeviceLease(device, registration->interface, pedigree_std::move(reader));
  return true;
}

bool NetworkStack::acquireDevice(Network* pDevice, DeviceLease& lease) {
  lease = DeviceLease();
#if THREADS || UTILITY_LINUX
  LockGuard<Mutex> guard(m_Lock);
#endif
  DeviceRegistration* registration = m_Registrations.lookup(pDevice);
  OperationBarrier::Lease reader;
  if (!registration || !registration->readers.tryAcquire(reader)) {
    return false;
  }

  lease = DeviceLease(pDevice, registration->interface, pedigree_std::move(reader));
  return true;
}

void NetworkStack::deRegisterDevice(Network* pDevice) {
  DeviceRegistration* registration = nullptr;
#if THREADS || UTILITY_LINUX
  m_Lock.acquire();
#endif

  registration = m_Registrations.lookup(pDevice);
  if (!registration) {
#if THREADS || UTILITY_LINUX
    m_Lock.release();
#endif
    return;
  }

  for (Vector<Network*>::Iterator it = m_Children.begin(); it != m_Children.end(); ++it) {
    if (*it == pDevice) {
      m_Children.erase(it);
      break;
    }
  }

  m_Registrations.remove(pDevice);
  registration->readers.close();

#if THREADS || UTILITY_LINUX
  m_Lock.release();
#endif

  registration->readers.wait();
  // Every admitted receive has finished posting to the lwIP mailbox. Its
  // FIFO order retires those packets before the interface can be removed.
  if (tcpip_callback_wait(removeInterface, registration->interface) != ERR_OK) {
    FATAL("NetworkStack could not retire a network interface");
  }
  delete registration->interface;
  delete registration;
}

static bool entry() {
  g_NetworkStack = new NetworkStack();

#if PEDIGREE_HOSTED_NETWORK_REGRESSION
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
MODULE_INFO("network-stack", &entry, &exit, "vfs", "lwip");
