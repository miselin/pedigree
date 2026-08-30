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

#ifndef _PROCESS_IPC_H
#define _PROCESS_IPC_H
#include <config.h>

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"

class MemoryRegion;

namespace Ipc {
class EXPORTED_PUBLIC IpcMessage {
 public:
  /** Maximum payload carried directly through the IPC message pool. */
  static constexpr size_t InlineCapacity = 4096;
  /** Number of fixed-size buffers retained by the IPC message pool. */
  static constexpr size_t InlinePoolBufferCount = 1024;

  IpcMessage();

  /**
   * \brief Constructor for messages with an optional shared-region handle.
   *
   * If you pass an existing region handle, this will link the instance
   * being created to an existing message that may well be in another
   * address space. That handle is obtained via IpcMessage::getHandle,
   * and should be passed via conventional < 4 KiB IPC messages.
   *
   * \param nBytes Number of bytes to allocate for this region.
   * \param regionHandle Handle to a region. Each message allocated with
   *        this method when this parameter is 0 is given a unique handle
   *        to pass to other processes in order to share memory.
   */
  IpcMessage(size_t nBytes, uintptr_t regionHandle = 0);

  virtual ~IpcMessage();

  /// Get the memory buffer for this IPC message. This is typically a
  /// shared memory region to avoid copies where possible.
  void* getBuffer();

  /// Get a handle for the region this message is in. Returns NULL for
  /// messages smaller than 4 KiB, as these are
  /// shared by default.
  void* getHandle();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  /** Hosted-only seam for checking inline-buffer page geometry. */
  static constexpr size_t getHostedInlinePageCount(size_t pageSize) {
    return pageSize ? inlinePageCount(pageSize) : 0;
  }

  static constexpr size_t getHostedInlineSlotSize(size_t pageSize) {
    return pageSize ? inlineSlotSize(pageSize) : 0;
  }

  static constexpr size_t getHostedInlinePoolPageCount(size_t pageSize) {
    return pageSize ? inlinePoolPageCount(pageSize) : 0;
  }
#endif

  /// Copy constructor. Used to create an IpcMessage on the other side
  /// of a send (ie, in the receiving process) when the size is < 4 KiB.
  /// Creating an IpcMessage in another process when the size is at least 4 KiB
  /// should be done with IpcMessage(size_t, uintptr_t).
  IpcMessage(const IpcMessage& src) {
    if (src.m_pMemRegion)
      FATAL("IpcMessage: copy constructor misused.");
    nPages = src.nPages;
    m_vAddr = src.m_vAddr;
    m_pMemRegion = src.m_pMemRegion;
  }

 private:
  static constexpr size_t inlinePageCount(size_t pageSize) {
    return (InlineCapacity / pageSize) + ((InlineCapacity % pageSize) ? 1 : 0);
  }

  static constexpr size_t inlineSlotSize(size_t pageSize) {
    return inlinePageCount(pageSize) * pageSize;
  }

  static constexpr size_t inlinePoolPageCount(size_t pageSize) {
    return InlinePoolBufferCount * inlinePageCount(pageSize);
  }

  void allocatePoolBuffer();

  size_t nPages;

  /// Virtual address of a message when m_pMemRegion is invalid.
  uintptr_t m_vAddr;

  /// This is the memory region we can pass around as a handle IFF we are
  /// working with a region at least 4 KiB in size. Allocated as a result of
  /// the @IpcMessage constructor with regionHandle == 0 once nBytes reaches
  /// the inline capacity.
  MemoryRegion* m_pMemRegion;
};

/// Wraps a message queue and links that to a name. This handles blocking and
/// such, reducing the complexity of the Ipc namespace (and reducing the amount
/// of IPC-related code in ring3 APIs).
class IpcEndpoint {
 public:
  IpcEndpoint(const String& name) : m_Name(name), m_Queue(), m_QueueSize(0), m_QueueLock() {
    NOTICE("Creating endpoint with name " << name);
    NOTICE("Endpoint is at " << reinterpret_cast<uintptr_t>(this));
  }

  ~IpcEndpoint() NORETURN {
    FATAL("IpcEndpoint " << m_Name << " is being destroyed.");
  }

  const String& getName() const {
    return m_Name;
  }

 private:
  class IpcCompletion;

  friend bool send(IpcEndpoint*, IpcMessage*, bool);
  friend bool recv(IpcEndpoint*, IpcMessage**, bool);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  friend bool runHostedIpcInterruptionRegression();
#endif

  IpcCompletion* pushMessage(IpcMessage* pMessage, bool bAsync);
  IpcMessage* getMessage(bool bBlock = false);

  /// A queued message ready for retrieval.
  struct QueuedMessage {
    IpcCompletion* pCompletion;
    IpcMessage* pMessage;
  };

  String m_Name;

  List<QueuedMessage*> m_Queue;
  Semaphore m_QueueSize;

  Mutex m_QueueLock;
};

EXPORTED_PUBLIC bool send(IpcEndpoint* pEndpoint, IpcMessage* pMessage, bool bAsync = false);
EXPORTED_PUBLIC bool recv(IpcEndpoint* pEndpoint, IpcMessage** pMessage, bool bAsync = false);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
EXPORTED_PUBLIC bool runHostedIpcInterruptionRegression();
#endif

EXPORTED_PUBLIC IpcEndpoint* getEndpoint(String& name);

EXPORTED_PUBLIC void createEndpoint(String& name);
EXPORTED_PUBLIC void removeEndpoint(String& name);
};  // namespace Ipc

#endif
