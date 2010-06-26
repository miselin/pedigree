/*
 * Copyright (c) 2008 James Molloy, Jörg Pfähler, Matthew Iselin
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
#include "Ethernet.h"
#include <Module.h>
#include <Log.h>
#include <processor/Processor.h>

#include "Dns.h"

NetworkStack NetworkStack::stack;

NetworkStack::NetworkStack() :
  RequestQueue(), m_pLoopback(0), m_Children(), m_MemPool("network-pool")
{
  initialise();

  // Try 16 MB, then 8 MB, then 4 MB, then give up
  if(!m_MemPool.initialise(4096, 1600))
      if(!m_MemPool.initialise(2048, 1600))
        if(!m_MemPool.initialise(1024, 1600))
            ERROR("Couldn't get a valid buffer pool for networking use");
}

NetworkStack::~NetworkStack()
{
  destroy();
}

uint64_t NetworkStack::executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
                                uint64_t p6, uint64_t p7, uint64_t p8)
{
    // Make sure we have interrupts enabled, so we're interruptible.
    Processor::setInterrupts(true);
    
    uintptr_t packet = static_cast<uintptr_t>(p1);
    size_t packetSize = static_cast<size_t>(p2);
    Network *pCard = reinterpret_cast<Network*>(p3);
    uint32_t offset = static_cast<uint32_t>(p4);
    
    if(!packet || !packetSize)
        return 0;

  // Pass onto the ethernet layer
  /// \todo We should accept a parameter here that specifies the type of packet
  ///       so we can pass it on to the correct handler, rather than assuming
  ///       Ethernet.
  Ethernet::instance().receive(packetSize, packet, pCard, offset);
  
  uint8_t *pack = reinterpret_cast<uint8_t*>(packet);
  delete [] pack;
  
  return 0;
}

void NetworkStack::receive(size_t nBytes, uintptr_t packet, Network* pCard, uint32_t offset)
{
  if(!packet || !nBytes)
      return;
      
  pCard->gotPacket();
  
  // Some cards might be giving us a DMA address or something, so we copy
  // before passing on to the worker thread...
  uint8_t *safePacket = new uint8_t[nBytes + 1];
  memcpy(safePacket, reinterpret_cast<void*>(packet), nBytes);
  addAsyncRequest(0, reinterpret_cast<uint64_t>(safePacket),
                     static_cast<uint64_t>(nBytes),
                     reinterpret_cast<uint64_t>(pCard),
                     static_cast<uint64_t>(offset));
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
  for(Vector<Network*>::Iterator it = m_Children.begin();
      it != m_Children.end();
      it++, i++)
  if (*it == pDevice)
  {
    m_Children.erase(it);
    break;
  }
}

static void entry()
{
    // Initialise the DNS implementation
    Dns::instance().initialise();
}

static void exit()
{
}

static const char *__mod_deps[] = {"vfs", 0};
MODULE_INFO("network-stack", &entry, &exit, __mod_deps);
