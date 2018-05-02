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

#ifndef __POSIX_SYS_SCHEDULING_h
#define __POSIX_SYS_SCHEDULING_h

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/unistd.h>

#include <sys/types.h>
#include <sys/time.h>

/* Scheduling Policies, P1003.1b-1993, p. 250
   NOTE:  SCHED_SPORADIC added by P1003.4b/D8, p. 34.  */

#define SCHED_OTHER    0
#define SCHED_FIFO     1
#define SCHED_RR       2

#if defined(_POSIX_SPORADIC_SERVER)
#define SCHED_SPORADIC 3 
#endif

/* Scheduling Parameters, P1003.1b-1993, p. 249
   NOTE:  Fields whose name begins with "ss_" added by P1003.4b/D8, p. 33.  */

struct sched_param {
  int sched_priority;           /* Process execution scheduling priority */

#if defined(_POSIX_SPORADIC_SERVER)
  int ss_low_priority;          /* Low scheduling priority for sporadic */
                                /*   server */
  struct timespec ss_replenish_period; 
                                /* Replenishment period for sporadic server */
  struct timespec ss_initial_budget;   /* Initial budget for sporadic server */
#endif
};

#ifdef __cplusplus
}
#endif 

#endif
/* end of include file */

