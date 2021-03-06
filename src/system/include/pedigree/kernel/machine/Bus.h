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

#ifndef MACHINE_BUS_H
#define MACHINE_BUS_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"

class String;

/**
 * A bus is a device which provides a view onto other devices.
 */
class EXPORTED_PUBLIC Bus : public Device
{
  public:
    Bus(const char *pName);
    virtual ~Bus();

    virtual Type getType();

    virtual void getName(String &str);

    virtual void dump(String &str);

  private:
    Bus(const Bus &);
    void operator=(const Bus &);

    const char *m_pName;
};

#endif
