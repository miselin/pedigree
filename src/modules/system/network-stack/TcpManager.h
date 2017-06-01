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

#ifndef MACHINE_TCPMANAGER_H
#define MACHINE_TCPMANAGER_H

#include <LockGuard.h>
#include <machine/Network.h>
#include <machine/TimerHandler.h>
#include <process/Mutex.h>
#include <process/Semaphore.h>
#include <processor/types.h>
#include <utilities/List.h>
#include <utilities/String.h>
#include <utilities/Tree.h>

#include <Log.h>

#include "Manager.h"

#include "Endpoint.h"
#include "NetworkStack.h"
#include "Tcp.h"
#include "TcpEndpoint.h"
#include "TcpMisc.h"
#include "TcpStateBlock.h"

#define BASE_EPHEMERAL_PORT 32768

/**
 * The Pedigree network stack - TCP Protocol Manager
 */
class TcpManager : public ProtocolManager
{
    public:
    TcpManager();
    virtual ~TcpManager();

    /** For access to the manager without declaring an instance of it */
    static TcpManager &instance()
    {
        return *manager;
    }

    /** Connects to a remote host (blocks until connected) */
    size_t Connect(
        Endpoint::RemoteEndpoint remoteHost, uint16_t localPort,
        TcpEndpoint *endpoint, bool bBlock = true);

    /** Starts listening for connections */
    size_t Listen(Endpoint *e, uint16_t port, Network *pCard = 0);

    /** In TCP terms - sends FIN. */
    void Shutdown(size_t connectionId, bool bOnlyStopReceive = false);

    /** Disconnects from a remote host (blocks until disconnected). Totally
     * tears down the connection, don't call unless you absolutely must! Use
     * Shutdown to begin a standard disconnect without blocking.
     */
    void Disconnect(size_t connectionId);

    /** Gets a new Endpoint for a connection */
    Endpoint *getEndpoint(uint16_t localPort = 0, Network *pCard = 0);

    /** Returns an Endpoint */
    void returnEndpoint(Endpoint *e);

    /** A new packet has arrived! */
    void receive(
        IpAddress from, uint16_t sourcePort, uint16_t destPort,
        Tcp::tcpHeader *header, uintptr_t payload, size_t payloadSize,
        Network *pCard);

    /** Sends a TCP packet over the given connection ID */
    int send(
        size_t connId, uintptr_t payload, bool push, size_t nBytes,
        bool addToRetransmitQueue = true);

    /** Removes a given (closed) connection from the system */
    void removeConn(size_t connId);

    /** Grabs the current state of a given connection */
    Tcp::TcpState getState(size_t connId);

    /** Gets the next sequence number to use */
    uint32_t getNextSequenceNumber();

    /** Gets a unique connection ID */
    size_t getConnId();

    /** Grabs the number of packets that have been queued for a given connection
     */
    uint32_t getNumQueuedPackets(size_t connId);

    /** Reduces the number of queued packets by the specified amount */
    void removeQueuedPackets(size_t connId, uint32_t n = 1);

    /** Allocates a unique local port for a connection with a server */
    uint16_t allocatePort();

    private:
    static int sequenceIncrementer(void *param);

    static TcpManager *manager;

    // next TCP sequence number to allocate
    uint32_t m_NextTcpSequence;

    // this keeps track of the next valid connection ID
    size_t m_NextConnId;

    // standard state blocks
    Tree<StateBlockHandle, StateBlock *> m_StateBlocks;

    // server state blocks (separated from standard blocks in the list)
    Tree<StateBlockHandle, StateBlock *> m_ListeningStateBlocks;

    /** Current connections - basically a map between connection IDs and handles
     */
    Tree<size_t, StateBlockHandle *> m_CurrentConnections;

    /** Currently known endpoints (all actually TcpEndpoints). */
    Tree<size_t, Endpoint *> m_Endpoints;

    /** Listen port availability */
    ExtensibleBitmap m_ListenPorts;
    /** Ephemeral ports. */
    ExtensibleBitmap m_EphemeralPorts;

    /** Lock to control access to state blocks. */
    Mutex m_TcpMutex;

    /**
     * Lock to control access to the sequence number, as it increments every
     * half a second.
     */
    Mutex m_SequenceMutex;

    /** Count of milliseconds, used for timer handler. */
    uint64_t m_Nanoseconds;

    /** Handle to our manager thread. */
    void *m_ThreadHandle;

    // Is the manager still alive?
    bool m_bAlive;
};

#endif
