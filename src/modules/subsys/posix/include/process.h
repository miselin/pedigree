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

/* process.h.  This file comes with MSDOS and WIN32 systems.  */

#ifndef __PROCESS_H_
#define __PROCESS_H_

#ifdef __cplusplus
extern "C" {
#endif

int execl(const char *path, const char *argv0, ...);
int execle(const char *path, const char *argv0, ... /*, char * const *envp */);
int execlp(const char *path, const char *argv0, ...);
int execlpe(const char *path, const char *argv0, ... /*, char * const *envp */);

int execv(const char *path, char * const *argv);
int execve(const char *path, char * const *argv, char * const *envp);
int execvp(const char *path, char * const *argv);
int execvpe(const char *path, char * const *argv, char * const *envp);

int spawnl(int mode, const char *path, const char *argv0, ...);
int spawnle(int mode, const char *path, const char *argv0, ... /*, char * const *envp */);
int spawnlp(int mode, const char *path, const char *argv0, ...);
int spawnlpe(int mode, const char *path, const char *argv0, ... /*, char * const *envp */);

int spawnv(int mode, const char *path, const char * const *argv);
int spawnve(int mode, const char *path, const char * const *argv, const char * const *envp);
int spawnvp(int mode, const char *path, const char * const *argv);
int spawnvpe(int mode, const char *path, const char * const *argv, const char * const *envp);

int cwait(int *, int, int);

#define _P_WAIT		1
#define _P_NOWAIT	2	/* always generates error */
#define _P_OVERLAY	3
#define _P_NOWAITO	4
#define _P_DETACH	5

#define WAIT_CHILD 1

#ifdef __cplusplus
}
#endif

#endif
