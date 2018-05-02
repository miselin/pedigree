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

/* internal use only -- mapping of "system calls" for libraries that lose
   and only provide C names, so that we end up in violation of ANSI */
#ifndef __SYSLIST_H
#define __SYSLIST_H

#ifdef MISSING_SYSCALL_NAMES
#define _close close
#define _execve execve
#define _fcntl fcntl
#define _fork fork
#define _fstat fstat
#define _getpid getpid
#define _gettimeofday gettimeofday
#define _isatty isatty
#define _kill kill
#define _link link
#define _lseek lseek
#define _open open
#define _read read
#define _sbrk sbrk
#define _stat stat
#define _times times
#define _unlink unlink
#define _wait wait
#define _write write
#endif /* MISSING_SYSCALL_NAMES */

#if defined MISSING_SYSCALL_NAMES || !defined HAVE_OPENDIR
/* If the system call interface is missing opendir, readdir, and
   closedir, there is an implementation of these functions in
   libc/posix that is implemented using open, getdents, and close. 
   Note, these functions are currently not in the libc/syscalls
   directory.  */
#define _opendir opendir
#define _readdir readdir
#define _closedir closedir
#endif /* MISSING_SYSCALL_NAMES || !HAVE_OPENDIR */

#endif /* !__SYSLIST_H_ */
