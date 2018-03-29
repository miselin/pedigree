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
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/processor/Processor.h"

#include "lwip/include/lwip/netif.h"
#include "lwip/include/lwip/etharp.h"
#include "lwip/include/lwip/ethip6.h"
#include "lwip/include/lwip/tcpip.h"
#include "lwip/include/netif/ethernet.h"

#include "Filter.h"

NetworkStack *NetworkStack::stack = 0;

static NetworkStack *g_NetworkStack = 0;

static err_t linkOutput(struct netif *netif, struct pbuf *p)
{
    Network *pDevice = reinterpret_cast<Network *>(netif->state);

    size_t totalLength = p->tot_len;

    // pull the chain of pbufs into a single packet to transmit
    size_t offset = 0;
    char *output = new char[totalLength];

    pbuf_copy_partial(p, output, totalLength, 0);

    // Check for filtering
    if (!NetworkFilter::instance().filter(1, reinterpret_cast<uintptr_t>(output), totalLength))
    {
        pDevice->droppedPacket();
        return ERR_IF;  // Drop the packet.
    }

    // transmit!
    err_t e = ERR_OK;
    if (!pDevice->send(totalLength, reinterpret_cast<uintptr_t>(output)))
    {
        e = ERR_IF;
    }

    delete [] output;

    return e;
}

void netifStatusUpdate(struct netif *netif)
{
    // something updated
    NOTICE("netifStatusUpdate");
}

static err_t netifInit(struct netif *netif)
{
    Network *pDevice = reinterpret_cast<Network *>(netif->state);
    StationInfo info = pDevice->getStationInfo();

    /// \todo a lot of this is hardcoded, which is not great
    netif->hwaddr_len = 6;
    MemoryCopy(netif->hwaddr, info.mac.getMac(), 6);
    netif->mtu = 1400;
    netif->flags = NETIF_FLAG_UP | NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    netif->linkoutput = linkOutput;
    netif->output = etharp_output;
    netif->output_ip6 = ethip6_output;

    netif_set_status_callback(netif, netifStatusUpdate);

    return ERR_OK;
}

NetworkStack::NetworkStack()
    : RequestQueue("Network Stack"), m_pLoopback(0), m_Children(), m_MemPool("network-pool")
#ifdef UTILITY_LINUX
    , m_Lock(false)
#endif
    , m_NextInterfaceNumber(0)
{
    if (stack)
    {
        FATAL("NetworkStack created multiple times.");
    }

    stack = this;

    initialise();

#if defined(X86_COMMON) || defined(HOSTED)
    // Lots of RAM to burn! Try 16 MB, then 8 MB, then 4 MB, then give up
    if (!m_MemPool.initialise(4096, 1600))
        if (!m_MemPool.initialise(2048, 1600))
            if (!m_MemPool.initialise(1024, 1600))
                ERROR("Couldn't get a valid buffer pool for networking use");
#elif defined(ARM_COMMON)
    // Probably very little RAM to burn - 4 MB then 2 MB, then 512 KB
    NOTICE("allocating memory pool");
    if (!m_MemPool.initialise(1024, 1600))
        if (!m_MemPool.initialise(512, 1600))
            if (!m_MemPool.initialise(128, 1600))
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
    // OK, we are now processing the packet.
    // We hold a lock that allows us to handle concurrency (not an issue with
    // a true RequestQueue, but is an issue on other environments).
#if defined(THREADS) || defined(UTILITY_LINUX)
    LockGuard<Mutex> guard(m_Lock);
#endif

    struct pbuf *p = reinterpret_cast<struct pbuf *>(p1);
    struct netif *iface = reinterpret_cast<struct netif *>(p2);

    iface->input(p, iface);

    return 0;
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

    struct netif *iface = getInterface(pCard);
    if (!iface)
    {
        ERROR("Network Stack: no lwIP interface for received packet");
        pCard->droppedPacket();
        return;
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, nBytes, PBUF_POOL);
    if (p != 0)
    {
        struct pbuf *buf = p;
        while (buf != nullptr)
        {
            size_t copyLength = buf->len;
            MemoryCopy(buf->payload, reinterpret_cast<void *>(packet), buf->len);

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

    uint64_t result = addRequest(0, reinterpret_cast<uint64_t>(p), reinterpret_cast<uintptr_t>(iface));
}

void NetworkStack::registerDevice(Network *pDevice)
{
#if defined(THREADS) || defined(UTILITY_LINUX)
    LockGuard<Mutex> guard(m_Lock);
#endif

    size_t interfaceNumber = m_NextInterfaceNumber++;

    if (interfaceNumber >= 0xFFU)
    {
        FATAL("Too many network interfaces!");
    }

    m_Children.pushBack(pDevice);

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

    m_Interfaces.insert(pDevice, iface);

    netif_add(iface, &ipaddr, &netmask, &gateway, pDevice, netifInit, tcpip_input);
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
    uint8_t *safePacket = reinterpret_cast<uint8_t *>(NetworkStack::instance().m_MemPool.allocateNow());
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

    return true;
}

static void exit()
{
    delete g_NetworkStack;
}

// NetManager exposes a Filesystem, and so needs the vfs module.
MODULE_INFO("network-stack", &entry, &exit, "config", "vfs", "lwip");
