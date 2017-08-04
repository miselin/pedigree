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

#ifndef MACHINE_ETHERNET_H
#define MACHINE_ETHERNET_H

#include "NetworkStack.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/ProducerConsumer.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"

#define ETH_ARP 0x0806
#define ETH_RARP 0x8035
#define ETH_IPV4 0x0800
#define ETH_IPV6 0x86DD

/**
 * The Pedigree network stack - Ethernet layer
 */
class Ethernet : public ProducerConsumer
{
  public:
    Ethernet();
    virtual ~Ethernet();

    /** For access to the stack without declaring an instance of it */
    static Ethernet &instance()
    {
        return ethernetInstance;
    }

    /** Packet arrival callback */
    void receive(NetworkStack::Packet *packet);

    /** Sends an ethernet packet */
    static void send(
        size_t nBytes, uintptr_t packet, Network *pCard, MacAddress dest,
        uint16_t type);

    /** Injects an Ethernet header into a given buffer and returns the size
     * of the header. */
    size_t injectHeader(
        uintptr_t packet, MacAddress destMac, MacAddress sourceMac,
        uint16_t type);

    /** Gets the MAC address from a given packet */
    void getMacFromPacket(uintptr_t packet, MacAddress *mac);

    inline size_t ethHeaderSize()
    {
        return sizeof(ethernetHeader);
    }

  private:
    static Ethernet ethernetInstance;

    void consume(uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8);

    struct ethernetHeader
    {
        uint16_t destMac[3];
        uint16_t sourceMac[3];
        uint16_t type;
    } __attribute__((packed));
};

#endif
