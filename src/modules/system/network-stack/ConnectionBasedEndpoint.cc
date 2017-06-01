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

#include "ConnectionBasedEndpoint.h"
#include "Endpoint.h"

ConnectionBasedEndpoint::ConnectionBasedEndpoint() : Endpoint()
{
}

ConnectionBasedEndpoint::ConnectionBasedEndpoint(
    uint16_t local, uint16_t remote)
    : Endpoint(local, remote)
{
}

ConnectionBasedEndpoint::ConnectionBasedEndpoint(
    IpAddress remoteIp, uint16_t local, uint16_t remote)
    : Endpoint(remoteIp, local, remote)
{
}

ConnectionBasedEndpoint::~ConnectionBasedEndpoint()
{
}

Endpoint::EndpointType ConnectionBasedEndpoint::getType() const
{
    return Endpoint::ConnectionBased;
}

ConnectionBasedEndpoint::EndpointState ConnectionBasedEndpoint::state() const
{
    return UNKNOWN;
}

bool ConnectionBasedEndpoint::isConnected() const
{
    auto currState = state();
    return (currState == TRANSFER) || (currState == CLOSING);
}

bool ConnectionBasedEndpoint::connect(
    const Endpoint::RemoteEndpoint &remoteHost, bool bBlock)
{
    return false;
}

void ConnectionBasedEndpoint::close()
{
}

bool ConnectionBasedEndpoint::listen()
{
    return false;
}

Endpoint *ConnectionBasedEndpoint::accept()
{
    return 0;
}

int ConnectionBasedEndpoint::send(size_t nBytes, uintptr_t buffer)
{
    return -1;
}

int ConnectionBasedEndpoint::recv(
    uintptr_t buffer, size_t maxSize, bool block, bool bPeek)
{
    return -1;
}

uint32_t ConnectionBasedEndpoint::getConnId() const
{
    return 0;
}

void ConnectionBasedEndpoint::setRemoteHost(const RemoteEndpoint &host)
{
}