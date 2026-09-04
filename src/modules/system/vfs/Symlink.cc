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

#include "Symlink.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Filesystem.h"
#include "VFS.h"

Symlink::Symlink() : File(), m_sTarget(), m_TargetLock() {}

Symlink::Symlink(const String& name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
                 Time::Timestamp creationTime, uintptr_t inode, Filesystem* pFs, size_t size,
                 File* pParent)
    : File(name, accessedTime, modifiedTime, creationTime, inode, pFs, size, pParent),
      m_sTarget(),
      m_TargetLock() {}

Symlink::~Symlink() {}

void Symlink::initialise(bool bForce) {
  if (m_sTarget.length() && !bForce)
    return;

  size_t sz = getSize();
  if (sz > 0x1000)
    sz = 0x1000;
  if (!sz) {
    m_sTarget.clear();
    return;
  }

  // Read symlink target.
  char* pBuffer = new char[sz];
  const size_t bytesRead = read(0ULL, sz, reinterpret_cast<uintptr_t>(pBuffer));

  // Convert to String object, wipe out whitespace.
  m_sTarget.assign(pBuffer, bytesRead);
  m_sTarget.rstrip();
  delete[] pBuffer;
}

File* Symlink::followLinkRetained(Directory::ChildLease& result) {
  Directory::ChildLease parentLease;
  File* parent = getParent();
  if (parent && VFS::instance().retainTrackedFile(parent)) {
    parentLease.adopt(parent);
  } else if (parent && parent != m_pFilesystem->getRoot()) {
    return nullptr;
  }

  String target;
  {
    LockGuard<Mutex> guard(m_TargetLock);
    initialise();
    target = m_sTarget;
  }
  return m_pFilesystem->findRetained(target.view(), result, parent);
}

int Symlink::followLink(char* pBuffer, size_t bufLen) {
  LockGuard<Mutex> guard(m_TargetLock);
  initialise();

  if (m_sTarget.length() < bufLen)
    bufLen = m_sTarget.length();

  StringCopyN(pBuffer, static_cast<const char*>(m_sTarget), bufLen);

  return bufLen;
}

bool Symlink::isBytewise() const {
  return true;
}
