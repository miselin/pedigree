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

#ifndef MACHINE_UDP_H
#define MACHINE_UDP_H

#include <processor/types.h>
#include <network/IpAddress.h>

class Network;
class IpBase;

/**
 * The Pedigree network stack - UDP layer
 */
class Udp
{
private:

  static Udp udpInstance;

  struct udpHeader
  {
    uint16_t  src_port;
    uint16_t  dest_port;
    uint16_t  len;
    uint16_t  checksum;
  } __attribute__ ((packed));

public:
  Udp();
  virtual ~Udp();

  /** For access to the stack without declaring an instance of it */
  static Udp& instance()
  {
    return udpInstance;
  }

  /** Packet arrival callback */
  void receive(IpAddress from, IpAddress to, uintptr_t packet, size_t nBytes, IpBase *pIp, Network* pCard);

  /** Sends a UDP packet */
  static bool send(IpAddress dest, uint16_t srcPort, uint16_t destPort, size_t nBytes, uintptr_t payload, bool broadcast = false, Network *pCard = 0);
};

#endif
