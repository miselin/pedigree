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

#ifndef _WCTYPE_H_
#define _WCTYPE_H_

#include <_ansi.h>
#include <sys/_types.h>

#define __need_wint_t
#include <stddef.h>

#ifndef WEOF
# define WEOF ((wint_t)-1)
#endif

_BEGIN_STD_C

#ifndef _WCTYPE_T
#define _WCTYPE_T
typedef int wctype_t;
#endif

#ifndef _WCTRANS_T
#define _WCTRANS_T
typedef int wctrans_t;
#endif

int	_EXFUN(iswalpha, (wint_t));
int	_EXFUN(iswalnum, (wint_t));
int	_EXFUN(iswblank, (wint_t));
int	_EXFUN(iswcntrl, (wint_t));
int	_EXFUN(iswctype, (wint_t, wctype_t));
int	_EXFUN(iswdigit, (wint_t));
int	_EXFUN(iswgraph, (wint_t));
int	_EXFUN(iswlower, (wint_t));
int	_EXFUN(iswprint, (wint_t));
int	_EXFUN(iswpunct, (wint_t));
int	_EXFUN(iswspace, (wint_t));
int	_EXFUN(iswupper, (wint_t));
int	_EXFUN(iswxdigit, (wint_t));
wint_t	_EXFUN(towctrans, (wint_t, wctrans_t));
wint_t	_EXFUN(towupper, (wint_t));
wint_t	_EXFUN(towlower, (wint_t));
wctrans_t _EXFUN(wctrans, (const char *));
wctype_t _EXFUN(wctype, (const char *));

_END_STD_C

#endif /* _WCTYPE_H_ */
