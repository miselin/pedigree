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

#include "modules/Module.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/Processor.h"

#include "modules/system/lwip/include/lwip/etharp.h"
#include "modules/system/lwip/include/lwip/ethip6.h"
#include "modules/system/lwip/include/lwip/netif.h"
#include "modules/system/lwip/include/lwip/tcpip.h"
#include "modules/system/lwip/include/netif/ethernet.h"

#include "Filter.h"

NetworkStack *NetworkStack::stack = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
NetworkStack::HostedReceiveHook NetworkStack::m_HostedReceiveHook = nullptr;
#endif

static NetworkStack *g_NetworkStack = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS && PEDIGREE_HOSTED_NETWORK_REGRESSION
extern bool runHostedNetworkStackRegressions();
#endif

static err_t linkOutput(struct netif *netif, struct pbuf *p)
{
    Network *pDevice = reinterpret_cast<Network *>(netif->state);

    size_t totalLength = p->tot_len;

    // pull the chain of pbufs into a single packet to transmit
    size_t offset = 0;
    char *output = new char[totalLength];

    pbuf_copy_partial(p, output, totalLength, 0);

    // Check for filtering
    if (!NetworkFilter::instance().filter(
            1, reinterpret_cast<uintptr_t>(output), totalLength))
    {
        pDevice->droppedPacket();
        delete[] output;
        return ERR_IF;  // Drop the packet.
    }

    // transmit!
    err_t e = ERR_OK;
    if (!pDevice->send(totalLength, reinterpret_cast<uintptr_t>(output)))
    {
        e = ERR_IF;
    }

    delete[] output;

    return e;
}

static void netifStatusUpdate(struct netif *netif)
{
    if (netif_is_up(netif))
    {
        NOTICE("netif " << String(netif->name, 2) << Dec << netif->num << ": is now up");
    }
    else
    {
        NOTICE("netif " << String(netif->name, 2) << Dec << netif->num << ": is now down");
    }
}

static void netifLinkUpdate(struct netif *netif)
{
    if (netif_is_link_up(netif))
    {
        NOTICE("netif " << String(netif->name, 2) << Dec << netif->num << ": link is now up");
    }
    else
    {
        NOTICE("netif " << String(netif->name, 2) << Dec << netif->num << ": link is now down");
    }
}

static err_t netifInit(struct netif *netif)
{
    Network *pDevice = reinterpret_cast<Network *>(netif->state);
    StationInfo info = pDevice->getStationInfo();

    /// \todo a lot of this is hardcoded, which is not great
    netif->hwaddr_len = 6;
    MemoryCopy(netif->hwaddr, info.mac.getMac(), 6);
    netif->mtu = 1400;
    netif->flags = NETIF_FLAG_LINK_UP | NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->linkoutput = linkOutput;
    netif->output = etharp_output;
    netif->output_ip6 = ethip6_output;

    netif_set_status_callback(netif, netifStatusUpdate);
    netif_set_link_callback(netif, netifLinkUpdate);

    return ERR_OK;
}

NetworkStack::NetworkStack()
    : RequestQueue(MakeConstantString("Network Stack")), m_pLoopback(0), m_Children(),
      m_MemPool("network-pool")
#if THREADS || UTILITY_LINUX
      ,
      m_Lock()
#endif
      ,
      m_Interfaces(), m_NextInterfaceNumber(0), m_NextDeviceGeneration(1),
      m_NextReceiveRequest(0)
{
    if (stack)
    {
        FATAL("NetworkStack created multiple times.");
    }

    stack = this;

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

NetworkStack::~NetworkStack()
{
    destroy();

    stack = 0;
}

uint64_t NetworkStack::executeRequest(
    uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
    uint64_t p6, uint64_t p7, uint64_t p8)
{
    struct pbuf *p = reinterpret_cast<struct pbuf *>(p1);
    Network *card = reinterpret_cast<Network *>(p2);
    const size_t generation = static_cast<size_t>(p3);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HostedReceiveHook hook =
        __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
    if (hook)
    {
        hook(
            HostedReceiveEvent::BeforeDispatch, reinterpret_cast<uintptr_t>(p),
            card, generation);
    }
#endif

    // OK, we are now processing the packet.
    // We hold a lock that allows us to handle concurrency (not an issue with
    // a true RequestQueue, but is an issue on other environments).
#if THREADS || UTILITY_LINUX
    LockGuard<Mutex> guard(m_Lock);
#endif

    struct netif *iface = m_Interfaces.lookup(card);
    const size_t activeGeneration =
        iface ? __atomic_load_n(
                    &card->m_NetworkStackGeneration, __ATOMIC_ACQUIRE)
              : 0;

    if (iface && generation && generation == activeGeneration)
    {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        hook = __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(
                HostedReceiveEvent::Delivered, reinterpret_cast<uintptr_t>(p),
                card, generation);
        }
#endif
        iface->input(p, iface);
    }
    else
    {
        // Device removal can overtake work which was already copied into the
        // queue. Resolve the interface under the same lock as deregistration
        // instead of retaining a freed netif pointer in the request.
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        hook = __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(
                HostedReceiveEvent::DiscardedStale,
                reinterpret_cast<uintptr_t>(p), card, generation);
        }
#endif
        pbuf_free(p);
    }

    return 0;
}

void NetworkStack::cancelRequest(const Request &request)
{
    cancelReceive(
        static_cast<uintptr_t>(request.p1),
        reinterpret_cast<Network *>(request.p2),
        static_cast<size_t>(request.p3));
}

void NetworkStack::cancelReceive(
    uintptr_t buffer, Network *card, size_t generation)
{
    if (buffer)
    {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HostedReceiveHook hook =
            __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(
                HostedReceiveEvent::Cancelled, buffer, card, generation);
        }
#endif
        pbuf_free(reinterpret_cast<struct pbuf *>(buffer));
    }
}

void NetworkStack::receive(
    size_t nBytes, uintptr_t packet, Network *pCard, uint32_t offset)
{
    packet += offset;

    // Check for filtering before doing anything else
    if (!NetworkFilter::instance().filter(1, packet, nBytes))
    {
        pCard->droppedPacket();
        return;  // Drop the packet.
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, nBytes, PBUF_POOL);
    if (p != 0)
    {
        struct pbuf *buf = p;
        while (buf != nullptr)
        {
            size_t copyLength = buf->len;
            MemoryCopy(
                buf->payload, reinterpret_cast<void *>(packet), buf->len);

            packet += buf->len;
            nBytes -= buf->len;

            buf = buf->next;
        }
    }
    else
    {
        ERROR("Network Stack: Out of memory pool space, dropping incoming "
              "packet");
        pCard->droppedPacket();
        return;
    }

    const size_t generation = __atomic_load_n(
        &pCard->m_NetworkStackGeneration, __ATOMIC_ACQUIRE);
    if (!generation)
    {
        pbuf_free(p);
        return;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HostedReceiveHook hook =
        __atomic_load_n(&m_HostedReceiveHook, __ATOMIC_ACQUIRE);
    if (hook)
    {
        hook(
            HostedReceiveEvent::Queued, reinterpret_cast<uintptr_t>(p), pCard,
            generation);
    }
#endif

    const size_t firstRequest = (m_NextReceiveRequest += 1) - 1;
    for (size_t i = 0; i < ReceiveRequestCapacity; ++i)
    {
        const size_t request = (firstRequest + i) % ReceiveRequestCapacity;
        const InterruptEnqueueResult result = enqueueFromInterrupt(
            m_ReceiveRequests[request], 0, reinterpret_cast<uint64_t>(p),
            reinterpret_cast<uintptr_t>(pCard), generation);
        if (result == InterruptEnqueueResult::Accepted)
        {
            return;
        }
        if (result != InterruptEnqueueResult::TokenBusy)
        {
            break;
        }
    }

    // No queue token retained this payload, so preserve the same cancellation
    // event and single pbuf release used by RequestQueue teardown.
    cancelReceive(reinterpret_cast<uintptr_t>(p), pCard, generation);
}

void NetworkStack::registerDevice(Network *pDevice)
{
#if THREADS || UTILITY_LINUX
    LockGuard<Mutex> guard(m_Lock);
#endif

    size_t interfaceNumber = m_NextInterfaceNumber++;

    if (interfaceNumber >= 0xFFU)
    {
        FATAL("Too many network interfaces!");
    }

    m_Children.pushBack(pDevice);

    size_t generation = m_NextDeviceGeneration++;
    if (!generation)
    {
        generation = m_NextDeviceGeneration++;
    }
    __atomic_store_n(
        &pDevice->m_NetworkStackGeneration, generation, __ATOMIC_RELEASE);

    struct netif *iface = new struct netif;
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

    iface = netif_add(iface, &ipaddr, &netmask, &gateway, pDevice, netifInit, tcpip_input);

    m_Interfaces.insert(pDevice, iface);
}

Network *NetworkStack::getDevice(size_t n)
{
    return m_Children[n];
}

size_t NetworkStack::getNumDevices()
{
    return m_Children.count();
}

void NetworkStack::deRegisterDevice(Network *pDevice)
{
#if THREADS || UTILITY_LINUX
    LockGuard<Mutex> guard(m_Lock);
#endif

    __atomic_store_n(
        &pDevice->m_NetworkStackGeneration, 0, __ATOMIC_RELEASE);

    int i = 0;
    for (Vector<Network *>::Iterator it = m_Children.begin();
         it != m_Children.end(); it++, i++)
        if (*it == pDevice)
        {
            m_Children.erase(it);
            break;
        }

    struct netif *iface = m_Interfaces.lookup(pDevice);
    m_Interfaces.remove(pDevice);

    if (iface != nullptr)
    {
        netif_remove(iface);

        delete iface;
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void NetworkStack::setHostedReceiveHook(HostedReceiveHook hook)
{
    __atomic_store_n(&m_HostedReceiveHook, hook, __ATOMIC_RELEASE);
}

size_t NetworkStack::getHostedRegistrationGeneration(Network *card)
{
    return __atomic_load_n(
        &card->m_NetworkStackGeneration, __ATOMIC_ACQUIRE);
}

size_t NetworkStack::getHostedReceiveRequestCapacity()
{
    return ReceiveRequestCapacity;
}
#endif

NetworkStack::Packet::Packet() = default;

NetworkStack::Packet::~Packet()
{
    // Packet destroyed, clean up our buffer if it existed.
    if (m_Buffer)
    {
        NetworkStack::instance().m_MemPool.free(m_Buffer);
    }
}

bool NetworkStack::Packet::copyFrom(uintptr_t otherPacket, size_t size)
{
    uint8_t *safePacket = reinterpret_cast<uint8_t *>(
        NetworkStack::instance().m_MemPool.allocateNow());
    if (!safePacket)
    {
        return false;
    }
    MemoryCopy(safePacket, reinterpret_cast<void *>(otherPacket), size);

    m_Buffer = reinterpret_cast<uintptr_t>(safePacket);
    m_PacketLength = size;
    return true;
}

static bool entry()
{
    g_NetworkStack = new NetworkStack();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS && PEDIGREE_HOSTED_NETWORK_REGRESSION
    if (!runHostedNetworkStackRegressions())
    {
        delete g_NetworkStack;
        g_NetworkStack = nullptr;
        return false;
    }
#endif

    return true;
}

static void exit()
{
    delete g_NetworkStack;
}

// NetManager exposes a Filesystem, and so needs the vfs module.
MODULE_INFO("network-stack", &entry, &exit, "config", "vfs", "lwip");
