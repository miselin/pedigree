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

#ifndef _CONNECTION_BASED_ENDPOINT_H
#define _CONNECTION_BASED_ENDPOINT_H

#include <compiler.h>

#include "Endpoint.h"

/**
 * ConnectionBasedEndpoint: Endpoint specialisation for connection-based
 * protocols (such as TCP).
 */
class ConnectionBasedEndpoint : public Endpoint
{
    public:
    ConnectionBasedEndpoint();
    ConnectionBasedEndpoint(uint16_t local, uint16_t remote);
    ConnectionBasedEndpoint(
        IpAddress remoteIp, uint16_t local = 0, uint16_t remote = 0);
    virtual ~ConnectionBasedEndpoint();

    enum EndpointState
    {
        LISTENING,
        CONNECTING,
        TRANSFER,
        CLOSING,
        CLOSED,
        UNKNOWN,
    };

    virtual EndpointType getType() const;

    /** Grabs an integer representation of the connection state */
    virtual EndpointState state() const;

    /** Are we in a connected state? */
    virtual bool isConnected() const;

    /** Connects to the given remote host */
    virtual bool
    connect(const Endpoint::RemoteEndpoint &remoteHost, bool bBlock = true);

    /** Closes the connection */
    virtual void close();

    /**
     * Puts the connection into the listening state, waiting for incoming
     * connections.
     */
    virtual bool listen();

    /**
     * Blocks until an incoming connection is available, then accepts it
     * and returns an Endpoint for that connection.
     */
    virtual Endpoint *accept();

    /**
     * Sends nBytes of buffer
     * \param nBytes the number of bytes to send
     * \param buffer the buffer to send
     * \returns -1 on failure, the number of bytes sent otherwise
     */
    virtual int send(size_t nBytes, uintptr_t buffer);

    /**
     * Receives from the network into the given buffer
     * \param buffer the buffer to receive into
     * \param maxSize the size of the buffer
     * \param block whether or not to block
     * \param bPeek whether or not to keep messages in the data buffer
     * \returns -1 on failure, the number of bytes received otherwise
     */
    virtual int recv(uintptr_t buffer, size_t maxSize, bool block, bool bPeek);

    /** Retrieves the connection ID for this connection. */
    virtual uint32_t getConnId() const;

    /**
     * Because TCP works with RemoteEndpoints a lot, it's easier to set our
     * internal state using this kind of function rather than several calls
     * to the setXyz functions.
     */
    virtual void setRemoteHost(const RemoteEndpoint &host);

    private:
    NOT_COPYABLE_OR_ASSIGNABLE(ConnectionBasedEndpoint);
};

#endif
