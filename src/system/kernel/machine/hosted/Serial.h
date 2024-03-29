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

#ifndef MACHINE_HOSTED_SERIAL_H
#define MACHINE_HOSTED_SERIAL_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cord.h"

/**
 * Serial device abstraction.
 */
class HostedSerial : public Serial
{
  public:
    HostedSerial();
    virtual void setBase(uintptr_t nBaseAddr);
    virtual ~HostedSerial();

    virtual char read();
    virtual char readNonBlock();
    virtual void write(char c);
    virtual void write_str(const char *c);
    virtual void write_str(const char *c, size_t len);
    virtual void write_str(const Cord &cord);

  private:
    bool isConnected();

    int m_File;
    uintptr_t m_nFileNumber;
};

#endif
