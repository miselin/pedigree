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

#ifndef TCPSTATEBLOCK_H
#define TCPSTATEBLOCK_H

#include <compiler.h>
#include <processor/types.h>
#include <machine/Network.h>
#include <process/Semaphore.h>
#include <processor/Processor.h>

#include "NetworkStack.h"
#include "Endpoint.h"
#include "TcpMisc.h"
#include "Tcp.h"

class TcpEndpoint;

/// \todo Eventify.

/// This is passed a given StateBlock and its sole purpose is to remove it
/// from the system. It's called as a thread when the TIME_WAIT timeout expires
/// to enable the block to be freed without requiring intervention.
int stateBlockFree(void* p);

// TCP is based on connections, so we need to keep track of them
// before we even think about depositing into Endpoints. These state blocks
// keep track of important information relating to the connection state.
class StateBlock
{
  private:

    struct Segment
    {
      uint32_t  seg_seq; // Segment sequence number
      uint32_t  seg_ack; // Ack number
      uint32_t  seg_len; // Segment length
      uint32_t  seg_wnd; // Segment window
      uint32_t  seg_up; // Urgent pointer
      uint8_t   flags;

      uintptr_t payload;
      size_t    nBytes;
    };

  public:
    StateBlock();
    ~StateBlock();

    Tcp::TcpState currentState;

    uint16_t localPort;

    Endpoint::RemoteEndpoint remoteHost;

    // Send sequence variables
    uint32_t iss; // initial sender sequence number (CLIENT)
    uint32_t snd_nxt; // next send sequence number
    uint32_t snd_una; // send unack
    uint32_t snd_wnd; // send window ----> How much they can receive max
    uint32_t snd_up; // urgent pointer?
    uint32_t snd_wl1; // segment sequence number for last WND update
    uint32_t snd_wl2; // segment ack number for last WND update

    // Receive sequence variables
    uint32_t rcv_nxt; // receive next - what we're expecting perhaps?
    uint32_t rcv_wnd; // receive window ----> How much we want to receive methinks...
    uint32_t rcv_up; // receive urgent pointer
    uint32_t irs; // initial receiver sequence number (SERVER)

    // Segment variables
    uint32_t seg_seq; // segment sequence number
    uint32_t seg_ack; // ack number
    uint32_t seg_len; // segment length
    uint32_t seg_wnd; // segment window
    uint32_t seg_up; // urgent pointer
    uint32_t seg_prc; // precedence

    // FIN information
    bool     fin_ack; // is ACK already set (for use with FIN bit checks)
    uint32_t fin_seq; // last FIN we sent had this sequence number

    // Connection information
    uint32_t tcp_mss; // maximum segment size

    // Number of packets we've deposited into our Endpoint
    // (decremented when a packet is picked up by the receiver)
    uint32_t numEndpointPackets;

    // Waiting for something?
    Mutex lock;
    ConditionVariable cond;

    // The endpoint applications use for this TCP connection
    TcpEndpoint* endpoint;

    // the id of this specific connection
    size_t connId;

    // Retransmission queue
    //TcpBuffer retransmitQueue;
    List<void*> retransmitQueue;

    // Number of bytes removed from the retransmit queue
    size_t nRemovedFromRetransmit;

    /// Handles a segment ack
    /// \note This will remove acked segments, however if there is only a partial ack on a segment
    ///       it will split it into two, remove the first, and leave a partial segment on the queue.
    ///       This behaviour does not affect anything internally as long as this function is always
    ///       used to acknowledge segments.
    void ackSegment();

    /// Sends a segment over the network
    bool sendSegment(Segment* seg);

    /// Sends a segment over the network
    bool sendSegment(uint8_t flags, size_t nBytes, uintptr_t payload, bool addToRetransmitQueue);

    // timer for all retransmissions (and state changes such as TIME_WAIT)
    virtual void timer(uint64_t delta, InterruptState& state);

    // resets the timer (to restart a timeout)
    void resetTimer(uint32_t timeout = 10);

    // starts a timer to destroy this state block
    void startCleanup();

    // are we waiting on a timeout?
    bool waitingForTimeout;

    // did the action time out or not?
    /// \note This ensures that, if we end up releasing the timeout wait semaphore
    ///       via a non-timeout source (such as a data ack) we know where the release
    ///       actually came from.
    bool didTimeout;

    // timeout wait semaphore (in case)
    bool useWaitSem;

  private:
    static int performCleanupTrampoline(void *param);

    void performCleanup();

    // number of nanoseconds & seconds for the timer
    uint64_t m_Nanoseconds;
    uint64_t m_Seconds;
    uint32_t m_Timeout;

    NOT_COPYABLE_OR_ASSIGNABLE(StateBlock);
};

#endif
