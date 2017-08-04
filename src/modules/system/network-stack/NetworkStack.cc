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
#include "pedigree/kernel/processor/Processor.h"

#include "Ethernet.h"
#include "Ipv6.h"
#ifndef DISABLE_TCP
#include "TcpManager.h"
#endif

NetworkStack *NetworkStack::stack = 0;

static NetworkStack *g_NetworkStack = 0;
#ifndef DISABLE_TCP
static TcpManager *g_TcpManager = 0;
#endif

NetworkStack::NetworkStack()
    : RequestQueue(), m_pLoopback(0), m_Children(), m_MemPool("network-pool")
#ifdef UTILITY_LINUX
    , m_Lock(false)
#endif
{
    if (stack)
    {
        FATAL("NetworkStack created multiple times.");
    }

    stack = this;

    initialise();

    /// \todo handle errors in Ethernet initialisation
    Ethernet::instance().initialise();

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
    Packet *pack = reinterpret_cast<Packet *>(p1);
    if (!pack)
        return 0;

    // OK, we are now processing the packet.
    // We hold a lock that allows us to handle concurrency (not an issue with
    // a true RequestQueue, but is an issue on other environments).
#if defined(THREADS) || defined(UTILITY_LINUX)
    LockGuard<Mutex> guard(m_Lock);
#endif

    if (!pack->getBuffer() || !pack->getLength())
    {
        delete pack;
        return 0;
    }

    // Pass onto the ethernet layer
    /// \todo We should accept a parameter here that specifies the type of
    /// packet
    ///       so we can pass it on to the correct handler, rather than assuming
    ///       Ethernet.
    Ethernet::instance().receive(pack);

    return 0;
}

void NetworkStack::receive(
    size_t nBytes, uintptr_t packet, Network *pCard, uint32_t offset)
{
    if (!packet || !nBytes)
        return;

    pCard->gotPacket();

    Packet *p = new Packet;

    // Some cards might be giving us a DMA address or something, so we copy
    // before passing on to the worker thread...
    if (!p->copyFrom(packet, nBytes))
    {
        ERROR("Network Stack: Out of memory pool space, dropping incoming "
              "packet");
        pCard->droppedPacket();
        return;
    }

    p->m_pCard = pCard;
    p->m_Offset = offset;

    uint64_t result = addRequest(0, reinterpret_cast<uint64_t>(p));
}

void NetworkStack::registerDevice(Network *pDevice)
{
    m_Children.pushBack(pDevice);
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

#ifndef NO_NETWORK_SERVICES
extern Ipv6Service *g_pIpv6Service;
extern ServiceFeatures *g_pIpv6Features;
#endif

static bool entry()
{
    g_NetworkStack = new NetworkStack();
#ifndef DISABLE_TCP
    g_TcpManager = new TcpManager();
#endif

#ifndef NO_NETWORK_SERVICES
    // Install the IPv6 Service
    g_pIpv6Service = new Ipv6Service;
    g_pIpv6Features = new ServiceFeatures;
    g_pIpv6Features->add(ServiceFeatures::touch);
    ServiceManager::instance().addService(
        String("ipv6"), g_pIpv6Service, g_pIpv6Features);
#endif

    return true;
}

static void exit()
{
#ifndef NO_NETWORK_SERVICES
    ServiceManager::instance().removeService(String("ipv6"));
    delete g_pIpv6Features;
    delete g_pIpv6Service;
#endif

#ifndef DISABLE_TCP
    delete g_TcpManager;
#endif
    delete g_NetworkStack;
}

// NetManager exposes a Filesystem, and so needs the vfs module.
MODULE_INFO("network-stack", &entry, &exit, "config", "vfs");
