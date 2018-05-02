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

/* timeb.h -- An implementation of the standard Unix <sys/timeb.h> file.
   Written by Ian Lance Taylor <ian@cygnus.com>
   Public domain; no rights reserved.

   <sys/timeb.h> declares the structure used by the ftime function, as
   well as the ftime function itself.  Newlib does not provide an
   implementation of ftime.  */

#ifndef _SYS_TIMEB_H

#ifdef __cplusplus
extern "C" {
#endif

#define _SYS_TIMEB_H

#include <_ansi.h>
#include "pedigree/kernel/machine/types.h"

#ifndef __time_t_defined
typedef _TIME_T_ time_t;
#define __time_t_defined
#endif

struct timeb
{
  time_t time;
  unsigned short millitm;
  short timezone;
  short dstflag;
};

extern int ftime _PARAMS ((struct timeb *));

#ifdef __cplusplus
}
#endif

#endif /* ! defined (_SYS_TIMEB_H) */
