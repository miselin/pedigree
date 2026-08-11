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

#ifndef DISKIMAGE_H
#define DISKIMAGE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Disk.h"

#include <stdio.h>

#if HAS_ADDRESS_SANITIZER
#include <map>
#endif

/** Loads a disk image as a usable disk device. */
class DiskImage : public Disk {
 public:
  DiskImage(const char* path);
  virtual ~DiskImage();

  bool initialise();

  virtual void getName(String& str) {
    str.assign("Hosted disk image", 18);
  }

  virtual void dump(String& str) {
    str.assign("Hosted disk image", 18);
  }

  virtual uintptr_t read(uint64_t location);
  virtual void write(uint64_t location);

  virtual size_t getSize() const;

  virtual size_t getBlockSize() const {
    return 4096;
  }

  virtual bool pin(uint64_t location);

  virtual void unpin(uint64_t location);

 private:
#if HAS_ADDRESS_SANITIZER
  struct BufferMapping {
    void* base;
    size_t length;
    size_t logicalOffset;
  };
#endif

  const char* m_pFileName;
  size_t m_nSize;
  FILE* m_pFile;
  int m_FileNo;

  void* m_pBuffer;
  size_t m_HostPageSize;

#if HAS_ADDRESS_SANITIZER
  std::map<uint64_t, BufferMapping> m_BufferMap;
#endif
};

#endif
