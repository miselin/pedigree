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

#include "TcpManager.h"
#include "RoutingTable.h"
#include <Log.h>
#include <processor/Processor.h>
#include <utilities/pocketknife.h>

TcpManager *TcpManager::manager = 0;

int TcpManager::sequenceIncrementer(void *param)
{
  /// \todo figure out when to terminate this thread
  TcpManager *m = reinterpret_cast<TcpManager *>(param);
  while (true)
  {
    {
      LockGuard<Mutex> guard(m->m_SequenceMutex);
      m->m_NextTcpSequence += 64000;
    }
    Time::delay(500 * Time::Multiplier::MILLISECOND);
  }
  return 0;
}

TcpManager::TcpManager() :
  m_NextTcpSequence(1), m_NextConnId(1), m_StateBlocks(), m_ListeningStateBlocks(),
  m_CurrentConnections(), m_Endpoints(), m_ListenPorts(), m_EphemeralPorts(),
  m_TcpMutex(false), m_SequenceMutex(false), m_Nanoseconds(0)
{
  manager = this;

  // Ports 32768 -> 65535 are ephemeral ports for client->server connections.
  for(size_t n = 0; n < BASE_EPHEMERAL_PORT; ++n)
  {
    m_EphemeralPorts.set(n);
  }

  pocketknife::runConcurrently(sequenceIncrementer, this);
}

TcpManager::~TcpManager()
{
  /// \todo figure out how to remove the sequence incrementer now
  manager = 0;
}

size_t TcpManager::Listen(Endpoint* e, uint16_t port, Network* pCard)
{
  // all callers should have chosen a card based on their bound address
  if(!pCard)
    pCard = RoutingTable::instance().DefaultRoute();

  if(!e || !pCard || !port)
    return 0;

  StateBlockHandle* handle = new StateBlockHandle;
  if(!handle)
    return 0;
  handle->localPort = port;
  handle->remotePort = 0;
  handle->remoteHost.ip.setIp(static_cast<uint32_t>(0));
  handle->listen = true;
  StateBlock* stateBlock;
  {
    LockGuard<Mutex> guard(m_TcpMutex);
    if((stateBlock = m_StateBlocks.lookup(*handle)) != 0)
    {
      delete handle;
      return 0;
    }
  }

  // build a state block for it
  size_t connId = getConnId();

  stateBlock = new StateBlock;
  if(!stateBlock)
  {
    delete handle;
    return 0;
  }

  stateBlock->localPort = port;
  stateBlock->remoteHost = handle->remoteHost;

  stateBlock->connId = connId;

  stateBlock->currentState = Tcp::LISTEN;

  stateBlock->endpoint = static_cast<TcpEndpoint*>(e);

  stateBlock->numEndpointPackets = 0;

  // Allocate the port now - just about to register the connection.
  {
    LockGuard<Mutex> guard(m_TcpMutex);
    if((port >= BASE_EPHEMERAL_PORT) && m_EphemeralPorts.test(port))
    {
      ERROR("Ephemeral port " << Dec << port << Hex << " cannot be listened on!");
      delete handle;
      delete stateBlock;
      return 0;
    }

    if(m_ListenPorts.test(port))
    {
      ERROR("Can't listen on already-used port " << Dec << port << Hex << "!");
      delete handle;
      delete stateBlock;
      return 0;
    }

    m_ListenPorts.set(port);
  }

  {
    LockGuard<Mutex> guard(m_TcpMutex);
    m_ListeningStateBlocks.insert(*handle, stateBlock);
    m_CurrentConnections.insert(connId, handle);
  }

  return connId;
}

size_t TcpManager::Connect(Endpoint::RemoteEndpoint remoteHost, uint16_t localPort, TcpEndpoint* endpoint, bool bBlock)
{
  if(!endpoint)
    return 0;

  StateBlockHandle* handle = new StateBlockHandle;
  if(!handle)
    return 0;

  handle->localPort = localPort;
  handle->remotePort = remoteHost.remotePort;
  handle->remoteHost = remoteHost;
  handle->listen = false;
  StateBlock* stateBlock;
  {
    LockGuard<Mutex> guard(m_TcpMutex);
    if((stateBlock = m_StateBlocks.lookup(*handle)) != 0)
    {
      delete handle;
      return 0;
    }
  }

  // build a state block for it
  size_t connId = getConnId();

  stateBlock = new StateBlock;
  if(!stateBlock)
  {
    delete handle;
    return 0;
  }

  stateBlock->localPort = localPort;
  stateBlock->remoteHost = remoteHost;

  stateBlock->connId = connId;

  stateBlock->iss = getNextSequenceNumber();
  stateBlock->snd_nxt = stateBlock->iss + 1;
  stateBlock->snd_una = stateBlock->iss;
  stateBlock->snd_wnd = endpoint->m_ShadowDataStream.getSize();
  stateBlock->snd_up = 0;
  stateBlock->snd_wl1 = stateBlock->snd_wl2 = 0;

  stateBlock->currentState = Tcp::SYN_SENT;

  stateBlock->endpoint = endpoint;

  stateBlock->numEndpointPackets = 0;

  stateBlock->tcp_mss = 1460; /// \todo Base this on the MTU of the link, or PMTU Discovery.

  {
    LockGuard<Mutex> guard(m_TcpMutex);
    m_StateBlocks.insert(*handle, stateBlock);
    m_CurrentConnections.insert(connId, handle);
  }

  Tcp::send(stateBlock->remoteHost.ip, stateBlock->localPort, stateBlock->remoteHost.remotePort, stateBlock->iss, 0, Tcp::SYN, stateBlock->snd_wnd, 0, 0);
  endpoint->reportError(Error::InProgress);

  if(!bBlock)
    return connId; // connection in progress - assume it works

  bool timedOut = false;
  stateBlock->lock.acquire();
  while (true)
  {
    if (stateBlock->currentState != Tcp::ESTABLISHED)
    {
      if (!stateBlock->cond.wait(stateBlock->lock, 15 * Time::Multiplier::SECOND))
      {
        timedOut = true;
        break;
      }
    }
  }
  stateBlock->lock.release();

  if((stateBlock->currentState != Tcp::ESTABLISHED) || timedOut)
  {
    /// \todo record a proper error somehow
    endpoint->reportError(Error::ConnectionRefused);
    return 0; /// \todo Keep track of an error number somewhere in StateBlock
  }
  else
  {
    endpoint->resetError();
    return connId;
  }
}

void TcpManager::Shutdown(size_t connectionId, bool bOnlyStopReceive)
{
  LockGuard<Mutex> guard(m_TcpMutex);

  StateBlockHandle* handle;
  if((handle = m_CurrentConnections.lookup(connectionId)) == 0)
    return;

  StateBlock* stateBlock;
  if((stateBlock = m_StateBlocks.lookup(*handle)) == 0)
    return;
    
  if(bOnlyStopReceive)
  {
    return;
  }

  IpAddress dest;
  dest = stateBlock->remoteHost.ip;

  // These two checks will end up closing our writing end of the connection

  /** ESTABLISHED: No FIN received - send our own **/
  if(stateBlock->currentState == Tcp::ESTABLISHED)
  {
    stateBlock->fin_seq = stateBlock->snd_nxt;

    stateBlock->currentState = Tcp::FIN_WAIT_1;
    stateBlock->seg_wnd = 0;
    stateBlock->sendSegment(Tcp::FIN | Tcp::ACK, 0, 0, true);
    stateBlock->snd_nxt++;
  }
  /** CLOSE_WAIT: FIN received - reply **/
  else if(stateBlock->currentState == Tcp::CLOSE_WAIT)
  {
    stateBlock->fin_seq = stateBlock->snd_nxt;

    stateBlock->currentState = Tcp::LAST_ACK;
    stateBlock->seg_wnd = 0;
    stateBlock->sendSegment(Tcp::FIN | Tcp::ACK, 0, 0, true);
    stateBlock->snd_nxt++;
  }
}

void TcpManager::Disconnect(size_t connectionId)
{
  LockGuard<Mutex> guard(m_TcpMutex);

  StateBlockHandle* handle;
  if((handle = m_CurrentConnections.lookup(connectionId)) == 0)
    return;

  StateBlock* stateBlock;
  if((stateBlock = m_StateBlocks.lookup(*handle)) == 0)
    return;

  IpAddress dest;
  dest = stateBlock->remoteHost.ip;

  // no FIN received yet
  if(stateBlock->currentState == Tcp::ESTABLISHED)
  {
    stateBlock->fin_seq = stateBlock->snd_nxt;

    stateBlock->currentState = Tcp::FIN_WAIT_1;
    stateBlock->seg_wnd = 0;
    stateBlock->sendSegment(Tcp::FIN | Tcp::ACK, 0, 0, true);
    stateBlock->snd_nxt++;
  }
  // received a FIN already
  else if(stateBlock->currentState == Tcp::CLOSE_WAIT)
  {
    stateBlock->fin_seq = stateBlock->snd_nxt;

    stateBlock->currentState = Tcp::LAST_ACK;
    stateBlock->seg_wnd = 0;
    stateBlock->sendSegment(Tcp::FIN | Tcp::ACK, 0, 0, true);
    stateBlock->snd_nxt++;
  }
  // LISTEN socket closing
  else if(stateBlock->currentState == Tcp::LISTEN)
  {
    NOTICE("Disconnect called on a LISTEN socket\n");
    stateBlock->currentState = Tcp::CLOSED;
    removeConn(stateBlock->connId);
  }
  else if(stateBlock->currentState == Tcp::LAST_ACK)
  {
    // Waiting on final ACK from remote, no need to do anything here.
  }
  else if(stateBlock->currentState == Tcp::SYN_SENT)
  {
    // Sent SYN but need to close now. Possible on non-blocking sockets.
    // Send an RST to ensure we don't get a late SYN/ACK and close.
    stateBlock->sendSegment(Tcp::RST, 0, 0, true);
    stateBlock->currentState = Tcp::CLOSED;
    removeConn(stateBlock->connId);
  }
  else
  {
      NOTICE("Connection Id " << Dec << connectionId << Hex << " is trying to close but isn't valid state [" << Tcp::stateString(stateBlock->currentState) << "]!");
  }
}

int TcpManager::send(size_t connId, uintptr_t payload, bool push, size_t nBytes, bool addToRetransmitQueue)
{
  LockGuard<Mutex> guard(m_TcpMutex);

  if(!payload || !nBytes)
    return -1;

  StateBlockHandle* handle;
  if((handle = m_CurrentConnections.lookup(connId)) == 0)
    return -1;

  StateBlock* stateBlock;
  if((stateBlock = m_StateBlocks.lookup(*handle)) == 0)
    return -1;

  if(stateBlock->currentState != Tcp::ESTABLISHED &&
        stateBlock->currentState != Tcp::CLOSE_WAIT)
     /*
     &&
     stateBlock->currentState != Tcp::FIN_WAIT_1 &&
     stateBlock->currentState != Tcp::FIN_WAIT_2
     */
    return -1; // When we SHUT_WR, we send FIN meaning no more data from us.

  stateBlock->sendSegment(Tcp::ACK | (push ? Tcp::PSH : 0), nBytes, payload, addToRetransmitQueue);

  // success!
  return nBytes;
}

void TcpManager::removeConn(size_t connId)
{
  LockGuard<Mutex> guard(m_TcpMutex);

  //return;
  StateBlockHandle* handle;
  if((handle = m_CurrentConnections.lookup(connId)) == 0)
    return;

  StateBlock* stateBlock;
  if(handle->listen)
    stateBlock = m_ListeningStateBlocks.lookup(*handle);
  else
    stateBlock = m_StateBlocks.lookup(*handle);
  if(stateBlock == 0)
    return;

  // only remove closed connections!
  if(stateBlock->currentState != Tcp::CLOSED)
  {
    return;
  }

  // remove from the lists
  if(handle->listen)
    m_ListeningStateBlocks.remove(*handle);
  else
    m_StateBlocks.remove(*handle);
  m_CurrentConnections.remove(connId);

  // destroy the state block (and its internals)
  stateBlock->cond.broadcast();
  delete stateBlock;

  // stateBlock->endpoint is what applications are using right now, so
  // we can't really delete it yet. They will do that with returnEndpoint().
}

void TcpManager::returnEndpoint(Endpoint* e)
{
  // The connection may be still closing when this is called!
  // It is well-defined behaviour that the endpoint will be returned
  // and the connection released once it hits the CLOSED state.

  TcpEndpoint *endpoint = static_cast<TcpEndpoint *>(e);

  // Is this a LISTEN socket?
  if (endpoint->m_Listening)
  {
    // Safe to destroy.
    delete endpoint;
  }
}

Endpoint* TcpManager::getEndpoint(uint16_t localPort, Network* pCard)
{
    if(!pCard)
        pCard = RoutingTable::instance().DefaultRoute();
    if(!pCard)
        return 0;

    Endpoint* e;

    if(localPort == 0)
    {
      localPort = allocatePort();
      if(localPort == 0)
          return 0; // Couldn't allocate port.
    }

    TcpEndpoint* tmp = new TcpEndpoint(localPort, 0);
    if(!tmp)
      return 0;

    tmp->setCard(pCard);
    tmp->setManager(this);

    e = static_cast<Endpoint*>(tmp);

    return e;
}

Tcp::TcpState TcpManager::getState(size_t connId)
{
  LockGuard<Mutex> guard(m_TcpMutex);

  StateBlockHandle* handle;
  if((handle = m_CurrentConnections.lookup(connId)) == 0)
  {
    WARNING("getState couldn't find a connection for ID " << connId);
    return Tcp::UNKNOWN;
  }

  StateBlock* stateBlock;
  if((stateBlock = m_ListeningStateBlocks.lookup(*handle)) == 0)
  {
    if((stateBlock = m_StateBlocks.lookup(*handle)) == 0)
    {
      WARNING("getState couldn't find a state block for ID " << connId);
      return Tcp::UNKNOWN;
    }
  }

  return stateBlock->currentState;
}

uint32_t TcpManager::getNextSequenceNumber()
{
  LockGuard<Mutex> guard(m_SequenceMutex);

  /// \todo This needs to be randomised to avoid sequence attacks
  size_t retSeq = m_NextTcpSequence;
  m_NextTcpSequence += 64000;
  return retSeq;
}

size_t TcpManager::getConnId()
{
  /// \todo Need recursive mutexes!

  size_t ret = m_NextConnId;
  while(m_CurrentConnections.lookup(ret) != 0) // ensure it's unique
    ret++;
  m_NextConnId = ret + 1;
  return ret;
}

uint32_t TcpManager::getNumQueuedPackets(size_t connId)
{
  LockGuard<Mutex> guard(m_TcpMutex);

  StateBlockHandle* handle;
  if((handle = m_CurrentConnections.lookup(connId)) == 0)
    return 0;

  StateBlock* stateBlock;
  if((stateBlock = m_StateBlocks.lookup(*handle)) == 0)
    return 0;

  return stateBlock->numEndpointPackets;
}

void TcpManager::removeQueuedPackets(size_t connId, uint32_t n)
{
  LockGuard<Mutex> guard(m_TcpMutex);

  StateBlockHandle* handle;
  if((handle = m_CurrentConnections.lookup(connId)) == 0)
    return;

  StateBlock* stateBlock;
  if((stateBlock = m_StateBlocks.lookup(*handle)) == 0)
    return;

  stateBlock->numEndpointPackets -= n;
}

uint16_t TcpManager::allocatePort()
{
  LockGuard<Mutex> guard(m_TcpMutex);

  /// \todo Handle cleaning up these ports when connections terminate!!!
  size_t bit = m_EphemeralPorts.getFirstClear();
  if(bit > 0xFFFF)
  {
    WARNING("No ports available!");
    return 0;
  }
  m_EphemeralPorts.set(bit);

  return bit;
}
