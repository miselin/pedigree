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

#ifndef _WORDEXP_H_
#define _WORDEXP_H_

#include <sys/types.h>

struct _wordexp_t
{
  size_t we_wordc;	/* Count of words matched by words. */
  char **we_wordv;	/* Pointer to list of expanded words. */
  size_t we_offs;	/* Slots to reserve at the beginning of we_wordv. */
};

typedef struct _wordexp_t wordexp_t;

#define	WRDE_DOOFFS	0x0001	/* Use we_offs. */
#define	WRDE_APPEND	0x0002	/* Append to output from previous call. */
#define	WRDE_NOCMD	0x0004	/* Don't perform command substitution. */
#define	WRDE_REUSE	0x0008	/* pwordexp points to a wordexp_t struct returned from
                                   a previous successful call to wordexp. */
#define	WRDE_SHOWERR	0x0010	/* Print error messages to stderr. */
#define	WRDE_UNDEF	0x0020	/* Report attempt to expand undefined shell variable. */

enum {
  WRDE_SUCCESS,
  WRDE_NOSPACE,
  WRDE_BADCHAR,
  WRDE_BADVAL,
  WRDE_CMDSUB,
  WRDE_SYNTAX,
  WRDE_NOSYS
};

/* Note: This implementation of wordexp requires a version of bash
   that supports the --wordexp and --protected arguments to be present
   on the system.  It does not support the WRDE_UNDEF flag. */
int wordexp(const char *, wordexp_t *, int);
void wordfree(wordexp_t *);

#endif /* _WORDEXP_H_  */
