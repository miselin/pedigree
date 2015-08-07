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

#include "TcpStateBlock.h"
#include "TcpEndpoint.h"
#include "Tcp.h"
#include <Log.h>

StateBlock::StateBlock() :
  currentState(Tcp::CLOSED), localPort(0), remoteHost(),
  iss(0), snd_nxt(0), snd_una(0), snd_wnd(0), snd_up(0), snd_wl1(0), snd_wl2(0),
  rcv_nxt(0), rcv_wnd(0), rcv_up(0), irs(0),
  seg_seq(0), seg_ack(0), seg_len(0), seg_wnd(0), seg_up(0), seg_prc(0),
  fin_ack(false), fin_seq(0), tcp_mss(536), // (standard default for MSS)
  numEndpointPackets(0), /// \todo Remove, obsolete
  waitState(0), endpoint(0), connId(0),
  retransmitQueue(), nRemovedFromRetransmit(0),
  waitingForTimeout(false), didTimeout(false), timeoutWait(0), useWaitSem(true),
  m_Nanoseconds(0), m_Seconds(0), m_Timeout(10)
{
  Timer* t = Machine::instance().getTimer();
  if(t)
    t->registerHandler(this);
}

StateBlock::~StateBlock()
{
  Timer* t = Machine::instance().getTimer();
  if(t)
    t->unregisterHandler(this);
}

StateBlock::StateBlock(const StateBlock& s) :
  currentState(Tcp::CLOSED), localPort(0), remoteHost(),
  iss(0), snd_nxt(0), snd_una(0), snd_wnd(0), snd_up(0), snd_wl1(0), snd_wl2(0),
  rcv_nxt(0), rcv_wnd(0), rcv_up(0), irs(0),
  seg_seq(0), seg_ack(0), seg_len(0), seg_wnd(0), seg_up(0), seg_prc(0),
  fin_ack(false), fin_seq(0),
  numEndpointPackets(0), /// \todo Remove, obsolete
  waitState(0), endpoint(0), connId(0),
  retransmitQueue(), nRemovedFromRetransmit(0),
  waitingForTimeout(false), didTimeout(false), timeoutWait(0), useWaitSem(true),
  m_Nanoseconds(0), m_Seconds(0), m_Timeout(10)
{
  // same as TcpEndpoint - the copy constructor should not be called
  ERROR("Tcp: StateBlock copy constructor called");
}

StateBlock& StateBlock::operator = (const StateBlock& s)
{
  // this isn't actually correct EITHER
  ERROR("Tcp: StateBlock copy constructor has been called.");
  return *this;
}

void StateBlock::ackSegment()
{
  // we assume the seg_* variables have been set by the caller (always done in TcpManager::receive)
  uint32_t segAck = seg_ack;
  while(retransmitQueue.count())
  {
    // grab the first segment from the queue
    Segment* seg = reinterpret_cast<Segment*>(retransmitQueue.popFront());
    if((seg->seg_seq + seg->seg_len) <= segAck)
    {
      // this segment is acked, leave it off the queue and free the memory used
      if(seg->payload)
        delete [] (reinterpret_cast<uint8_t*>(seg->payload));

      delete seg;
      continue;
    }
    else
    {
      // check if the ack is within this segment
      if(segAck >= seg->seg_seq)
      {
        // it is, so we need to split the segment payload
        Segment* splitSeg = new Segment;
        *splitSeg = *seg;

        // how many bytes are acked?
        /// \bug This calculation *may* have an off-by-one error
        size_t nBytesAcked = segAck - seg->seg_seq;

        // update the sequence number
        splitSeg->seg_seq = seg->seg_seq + nBytesAcked;
        splitSeg->seg_len -= nBytesAcked;

        // and most importantly, recopy the payload
        if(seg->nBytes && seg->payload)
        {
          uint8_t* newPayload = new uint8_t[splitSeg->seg_len];
          memcpy(newPayload, reinterpret_cast<void*>(seg->payload), seg->nBytes);

          splitSeg->payload = reinterpret_cast<uintptr_t>(newPayload);
        }

        // push on the front, and don't continue (we know there's no potential for further ACKs)
        retransmitQueue.pushFront(reinterpret_cast<void*>(splitSeg));
        if(seg->payload)
          delete [] (reinterpret_cast<uint8_t*>(seg->payload));
        delete seg;
        return;
      }
    }
  }
}

/// Sends a segment over the network
bool StateBlock::sendSegment(Segment* seg)
{
  if(seg)
  {
    if((seg->flags & Tcp::ACK) == 0)
    {
      // If no ACK flag, don't transmit an ACK number.
      seg->seg_ack = 0;
    }
    return Tcp::send(remoteHost.ip, localPort, remoteHost.remotePort, seg->seg_seq, seg->seg_ack, seg->flags, seg->seg_wnd, seg->nBytes, seg->payload);
  }
  return false;
}

/// Sends a segment over the network
bool StateBlock::sendSegment(uint8_t flags, size_t nBytes, uintptr_t payload, bool addToRetransmitQueue)
{
  // split the passed buffer up into segments based on the MSS and send each
  size_t offset;
  for(offset = 0; offset < (nBytes == 0 ? 1 : nBytes); offset += tcp_mss)
  {
    Segment* seg = new Segment;

    size_t segmentSize = tcp_mss;
    if((offset + segmentSize) >= nBytes)
    {
      segmentSize = nBytes - offset;
      if(nBytes)
      {
         flags |= Tcp::PSH;
      }
    }

    seg_seq = snd_nxt;
    snd_nxt += segmentSize;
    snd_wnd = endpoint->m_ShadowDataStream.getRemainingSize();

    seg->seg_seq = seg_seq;
    seg->seg_ack = rcv_nxt;
    seg->seg_len = segmentSize;
    seg->seg_wnd = snd_wnd;
    seg->seg_up = 0;
    seg->flags = flags;

    if(nBytes && payload)
    {
      uint8_t* newPayload = new uint8_t[segmentSize];
      memcpy(newPayload, reinterpret_cast<void*>(payload + offset), segmentSize);

      seg->payload = reinterpret_cast<uintptr_t>(newPayload);
    }
    else
      seg->payload = 0;
    seg->nBytes = seg->seg_len;

    sendSegment(seg);

    if(addToRetransmitQueue)
      retransmitQueue.pushBack(reinterpret_cast<void*>(seg));
    else
      delete seg;
  }

  return true;
}

// timer for all retransmissions (and state changes such as TIME_WAIT)
void StateBlock::timer(uint64_t delta, InterruptState& state)
{
  if(!waitingForTimeout)
    return;

  if(UNLIKELY(m_Seconds < m_Timeout))
  {
    m_Nanoseconds += delta;
    if(UNLIKELY(m_Nanoseconds >= 1000000000ULL))
    {
      ++m_Seconds;
      m_Nanoseconds -= 1000000000ULL;
    }

    if(UNLIKELY(m_Seconds >= m_Timeout))
    {
      // timeout is hit!
      waitingForTimeout = false;
      didTimeout = true;
      if(useWaitSem)
        timeoutWait.release();

      // check to see if there's data on the retransmission queue to send
      if(retransmitQueue.count())
      {
        NOTICE("Remote TCP did not ack all the data!");

        // still more data unacked - grab the first segment and transmit it
        // note that we don't pop it off the queue permanently, as we are still
        // waiting for an ack for the segment
        Segment* seg = reinterpret_cast<Segment*>(retransmitQueue.popFront());
        sendSegment(seg);
        retransmitQueue.pushFront(reinterpret_cast<void*>(seg));

        // reset the timeout
        resetTimer();
      }
      else if(currentState == Tcp::TIME_WAIT)
      {
        // timer has fired, we need to close the connection
        NOTICE("TIME_WAIT timeout complete");
        currentState = Tcp::CLOSED;

        // create the cleanup thread
        Thread *pThread = new Thread(Processor::information().getCurrentThread()->getParent(),
                                     reinterpret_cast<Thread::ThreadStartFunc> (&stateBlockFree),
                                     reinterpret_cast<void*> (this));
        pThread->detach();
      }
    }
  }
}

// resets the timer (to restart a timeout)
void StateBlock::resetTimer(uint32_t timeout)
{
  m_Seconds = m_Nanoseconds = 0;
  m_Timeout = timeout;
  didTimeout = false;
}