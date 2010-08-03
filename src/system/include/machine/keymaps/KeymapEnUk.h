/*
 * Copyright (c) 2010 James Molloy, Burtescu Eduard
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

#ifndef __MACHINE_KEYMAPS_KEYMAPENUK_H
#define __MACHINE_KEYMAPS_KEYMAPENUK_H

static char sparseBuff[613] =
"\x04\x00\x00\x00\x08\x00\x00\x00\x0c\x00\x00\x00\x10\x00\x00\x00\x14\x00\x00\x00\x18\
\x00\x04\x02\x1c\x00\x68\x01\x20\x00\x1c\x01\x24\x00\xf4\x00\x28\x00\x00\x00\x2c\
\x00\x8c\x00\x30\x00\x00\x00\x34\x00\x58\x00\x38\x00\x44\x00\x3c\x00\x60\x80\x40\
\x00\x20\x80\x00\x00\x00\x80\xe0\x80\x48\x00\x60\x81\x4c\x00\x50\x00\x00\x00\x54\
\x00\x00\x00\xa0\x81\x00\x00\x5c\x00\x80\x00\x60\x00\x70\x00\x00\x00\x64\x00\x68\
\x00\xc0\x81\x6c\x00\xb0\x81\x00\x00\xa8\x81\x74\x00\x18\x82\x78\x00\xf8\x81\xe0\
\x81\x7c\x00\xf0\x81\x00\x00\x84\x00\x00\x00\x88\x00\x00\x00\x58\x82\x00\x00\x90\
\x00\x00\x00\x94\x00\xc8\x00\x98\x00\xa4\x00\x9c\x00\xd8\x82\xa0\x00\x98\x82\x00\
\x00\x78\x82\xa8\x00\xb8\x00\x58\x83\xac\x00\x00\x00\xb0\x00\xb4\x00\xa0\x83\x00\
\x00\x98\x83\xb0\x83\xbc\x00\xc0\x00\x00\x00\xc4\x00\x00\x00\xf0\x83\x00\x00\xcc\
\x00\xe8\x00\x00\x00\xd0\x00\x00\x00\xd4\x00\xd8\x00\xe0\x00\xdc\x00\x00\x84\x00\
\x00\xf8\x83\xe4\x00\x18\x84\x10\x84\x00\x00\xec\x00\x00\x00\xf0\x00\x00\x00\x28\
\x84\x00\x00\xf8\x00\x00\x00\xfc\x00\x00\x00\x00\x01\x00\x00\x04\x01\x00\x00\x00\
\x00\x08\x01\x00\x00\x0c\x01\x10\x01\x00\x00\x14\x01\x50\x84\x00\x00\x18\x01\x00\
\x00\x48\x84\x20\x01\x00\x00\x24\x01\x00\x00\x28\x01\x48\x01\x2c\x01\x00\x00\x30\
\x01\x00\x00\x34\x01\x00\x00\x38\x01\x00\x00\x00\x00\x3c\x01\x40\x01\x00\x00\x44\
\x01\x00\x00\x70\x84\x00\x00\x4c\x01\x00\x00\x50\x01\x00\x00\x54\x01\x00\x00\x58\
\x01\x00\x00\x00\x00\x5c\x01\x60\x01\x00\x00\x64\x01\x00\x00\x78\x84\x00\x00\x6c\
\x01\xb8\x01\x70\x01\x00\x00\x74\x01\x00\x00\x78\x01\x98\x01\x7c\x01\x00\x00\x80\
\x01\x00\x00\x84\x01\x00\x00\x88\x01\x00\x00\x00\x00\x8c\x01\x90\x01\x00\x00\x94\
\x01\x00\x00\x80\x84\x00\x00\x9c\x01\x00\x00\xa0\x01\x00\x00\xa4\x01\x00\x00\xa8\
\x01\x00\x00\x00\x00\xac\x01\xb0\x01\x00\x00\xb4\x01\x00\x00\x88\x84\x00\x00\xbc\
\x01\x00\x00\xc0\x01\x00\x00\xc4\x01\xe4\x01\xc8\x01\x00\x00\xcc\x01\x00\x00\xd0\
\x01\x00\x00\xd4\x01\x00\x00\x00\x00\xd8\x01\xdc\x01\x00\x00\xe0\x01\x00\x00\x90\
\x84\x00\x00\xe8\x01\x00\x00\xec\x01\x00\x00\xf0\x01\x00\x00\xf4\x01\x00\x00\x00\
\x00\xf8\x01\xfc\x01\x00\x00\x00\x02\x00\x00\x98\x84\x00\x00\x08\x02\x00\x00\x00\
\x00\x0c\x02\x10\x02\x00\x00\x14\x02\x00\x00\x18\x02\x38\x02\x1c\x02\x00\x00\x20\
\x02\x00\x00\x24\x02\x00\x00\x28\x02\x00\x00\x00\x00\x2c\x02\x30\x02\x00\x00\x34\
\x02\x00\x00\xa0\x84\x00\x00\x3c\x02\x00\x00\x40\x02\x00\x00\x44\x02\x00\x00\x48\
\x02\x00\x00\x00\x00\x4c\x02\x50\x02\x00\x00\x54\x02\x00\x00\xa8\x84\x00\x00\x00\
\x00\x00\x00\xf0\x00\x00\x00\x2c\x00\x00\x07";

static char dataBuff[1225] =
"\x00\x00\x00\x00\x61\x00\x00\x00\x00\x00\x00\x00\x62\x00\x00\x00\x00\x00\x00\x00\x63\
\x00\x00\x00\x00\x00\x00\x00\x64\x00\x00\x00\x00\x00\x00\x00\x65\x00\x00\x00\x00\
\x00\x00\x00\x66\x00\x00\x00\x00\x00\x00\x00\x67\x00\x00\x00\x00\x00\x00\x00\x68\
\x00\x00\x00\x00\x00\x00\x00\x69\x00\x00\x00\x00\x00\x00\x00\x6a\x00\x00\x00\x00\
\x00\x00\x00\x6b\x00\x00\x00\x00\x00\x00\x00\x6c\x00\x00\x00\x00\x00\x00\x00\x6d\
\x00\x00\x00\x00\x00\x00\x00\x6e\x00\x00\x00\x00\x00\x00\x00\x6f\x00\x00\x00\x00\
\x00\x00\x00\x70\x00\x00\x00\x00\x00\x00\x00\x71\x00\x00\x00\x00\x00\x00\x00\x72\
\x00\x00\x00\x00\x00\x00\x00\x73\x00\x00\x00\x00\x00\x00\x00\x74\x00\x00\x00\x00\
\x00\x00\x00\x75\x00\x00\x00\x00\x00\x00\x00\x76\x00\x00\x00\x00\x00\x00\x00\x77\
\x00\x00\x00\x00\x00\x00\x00\x78\x00\x00\x00\x00\x00\x00\x00\x79\x00\x00\x00\x00\
\x00\x00\x00\x7a\x00\x00\x00\x00\x00\x00\x00\x31\x00\x00\x00\x00\x00\x00\x00\x32\
\x00\x00\x00\x00\x00\x00\x00\x33\x00\x00\x00\x00\x00\x00\x00\x34\x00\x00\x00\x00\
\x00\x00\x00\x35\x00\x00\x00\x00\x00\x00\x00\x36\x00\x00\x00\x00\x00\x00\x00\x37\
\x00\x00\x00\x00\x00\x00\x00\x38\x00\x00\x00\x00\x00\x00\x00\x39\x00\x00\x00\x00\
\x00\x00\x00\x30\x00\x00\x00\x00\x00\x00\x00\x0a\x00\x00\x00\x00\x00\x00\x00\x1b\
\x00\x00\x00\x00\x00\x00\x00\x08\x00\x00\x00\x00\x00\x00\x00\x09\x00\x00\x00\x00\
\x00\x00\x00\x20\x00\x00\x00\x00\x00\x00\x00\x2d\x00\x00\x00\x00\x00\x00\x00\x3d\
\x00\x00\x00\x00\x00\x00\x00\x5b\x00\x00\x00\x00\x00\x00\x00\x5d\x00\x00\x00\x00\
\x00\x00\x00\x5c\x00\x00\x00\x00\x00\x00\x00\x23\x00\x00\x00\x00\x00\x00\x00\x3b\
\x00\x00\x00\x00\x00\x00\x00\x27\x00\x00\x00\x00\x00\x00\x00\x60\x00\x00\x00\x00\
\x00\x00\x00\x2c\x00\x00\x00\x00\x00\x00\x00\x2e\x00\x00\x00\x00\x00\x00\x00\x2f\
\x00\x00\x00\x00\x00\x00\x80\x69\x6e\x73\x00\x00\x00\x00\x80\x68\x6f\x6d\x65\x00\
\x00\x00\x80\x70\x67\x75\x70\x00\x00\x00\x80\x64\x65\x6c\x00\x00\x00\x00\x80\x65\
\x6e\x64\x00\x00\x00\x00\x80\x70\x67\x64\x6e\x00\x00\x00\x80\x72\x69\x67\x68\x00\
\x00\x00\x80\x6c\x65\x66\x74\x00\x00\x00\x80\x64\x6f\x77\x6e\x00\x00\x00\x80\x75\
\x70\x00\xf3\x00\x00\x00\x00\x2f\x00\x00\x00\x00\x00\x00\x00\x2a\x00\x00\x00\x00\
\x00\x00\x00\x2d\x00\x00\x00\x00\x00\x00\x00\x2b\x00\x00\x00\x00\x00\x00\x00\x0a\
\x00\x00\x00\x00\x00\x00\x00\x31\x00\x00\x00\x00\x00\x00\x00\x32\x00\x00\x00\x00\
\x00\x00\x00\x33\x00\x00\x00\x00\x00\x00\x00\x34\x00\x00\x00\x00\x00\x00\x00\x35\
\x00\x00\x00\x00\x00\x00\x00\x36\x00\x00\x00\x00\x00\x00\x00\x37\x00\x00\x00\x00\
\x00\x00\x00\x38\x00\x00\x00\x00\x00\x00\x00\x39\x00\x00\x00\x00\x00\x00\x00\x30\
\x00\x00\x00\x00\x00\x00\x00\x2e\x00\x00\x00\x00\x00\x00\x00\x41\x00\x00\x00\x00\
\x00\x00\x00\x42\x00\x00\x00\x00\x00\x00\x00\x43\x00\x00\x00\x00\x00\x00\x00\x44\
\x00\x00\x00\x00\x00\x00\x00\x45\x00\x00\x00\x00\x00\x00\x00\x46\x00\x00\x00\x00\
\x00\x00\x00\x47\x00\x00\x00\x00\x00\x00\x00\x48\x00\x00\x00\x00\x00\x00\x00\x49\
\x00\x00\x00\x00\x00\x00\x00\x4a\x00\x00\x00\x00\x00\x00\x00\x4b\x00\x00\x00\x00\
\x00\x00\x00\x4c\x00\x00\x00\x00\x00\x00\x00\x4d\x00\x00\x00\x00\x00\x00\x00\x4e\
\x00\x00\x00\x00\x00\x00\x00\x4f\x00\x00\x00\x00\x00\x00\x00\x41\x00\x00\x00\x00\
\x00\x00\x00\x51\x00\x00\x00\x00\x00\x00\x00\x52\x00\x00\x00\x00\x00\x00\x00\x53\
\x00\x00\x00\x00\x00\x00\x00\x54\x00\x00\x00\x00\x00\x00\x00\x55\x00\x00\x00\x00\
\x00\x00\x00\x56\x00\x00\x00\x00\x00\x00\x00\x57\x00\x00\x00\x00\x00\x00\x00\x58\
\x00\x00\x00\x00\x00\x00\x00\x59\x00\x00\x00\x00\x00\x00\x00\x5a\x00\x00\x00\x00\
\x00\x00\x00\x21\x00\x00\x00\x00\x00\x00\x00\x22\x00\x00\x00\x00\x00\x00\x00\xa3\
\x00\x00\x00\x00\x00\x00\x00\x24\x00\x00\x00\x00\x00\x00\x00\x25\x00\x00\x00\x00\
\x00\x00\x00\x5e\x00\x00\x00\x00\x00\x00\x00\x26\x00\x00\x00\x00\x00\x00\x00\x2a\
\x00\x00\x00\x00\x00\x00\x00\x28\x00\x00\x00\x00\x00\x00\x00\x29\x00\x00\x00\x00\
\x00\x00\x00\x5f\x00\x00\x00\x00\x00\x00\x00\x2b\x00\x00\x00\x00\x00\x00\x00\x7b\
\x00\x00\x00\x00\x00\x00\x00\x7d\x00\x00\x00\x00\x00\x00\x00\x7c\x00\x00\x00\x00\
\x00\x00\x00\x7e\x00\x00\x00\x00\x00\x00\x00\x3a\x00\x00\x00\x00\x00\x00\x00\x40\
\x00\x00\x00\x00\x00\x00\x00\xac\x00\x00\x00\x00\x00\x00\x00\x3c\x00\x00\x00\x00\
\x00\x00\x00\x3e\x00\x00\x00\x00\x00\x00\x00\x3f\x00\x00\x00\x00\x00\x00\x80\x65\
\x6e\x64\x00\x00\x00\x00\x80\x64\x6f\x77\x6e\x00\x00\x00\x80\x70\x67\x64\x6e\x00\
\x00\x00\x80\x6c\x65\x66\x74\x00\x00\x00\x80\x72\x69\x67\x68\x00\x00\x00\x80\x68\
\x6f\x6d\x65\x00\x00\x00\x80\x75\x70\x00\x55\x00\x00\x00\x80\x70\x67\x75\x70\x00\
\x00\x00\x80\x69\x6e\x73\x00\x00\x00\x00\x80\x64\x65\x6c\x00\x01\x00\x00\x00\x27\
\x00\x00\x00\x03\x00\x00\x00\x5e\x00\x00\x00\x02\x00\x00\x00\x60\x00\x00\x00\x00\
\x00\x00\x00\xab\x00\x00\x00\x00\x00\x00\x00\xbb\x00\x00\x00\x00\x00\x00\x00\xe9\
\x00\x00\x00\x00\x00\x00\x00\xc9\x00\x00\x00\x00\x00\x00\x00\xe8\x00\x00\x00\x00\
\x00\x00\x00\xc8\x00\x00\x00\x00\x00\x00\x00\xea\x00\x00\x00\x00\x00\x00\x00\xca\
\x00\x00\x00\x00\x00\x00\x00\xeb\x00\x00\x00\x00\x00\x00\x00\xcb\x00\x00\x00\x00\
\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\
\x00\x00\x00";

#endif
