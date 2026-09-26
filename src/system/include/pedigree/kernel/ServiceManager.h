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

#ifndef SERVICE_MANAGER_H
#define SERVICE_MANAGER_H

#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/String.h"

class Service;

/// \todo Integrate with the Event system somehow

/** Service Manager
 *
 *  The service manager controls Services in a central location, by allowing
 *  them to be referred to by name.
 *
 *  It also provides services such as enumeration of Service operations.
 */
class EXPORTED_PUBLIC ServiceManager {
 public:
  ServiceManager();
  virtual ~ServiceManager();

  static ServiceManager& instance() {
    return m_Instance;
  }

  /**
   *  Enumerates all possible operations that can be performed for a
   *  given Service. The caller must externally pin the service lifetime.
   */
  ServiceFeatures* enumerateOperations(const String& serviceName);

  /** Adds a service to the manager */
  void addService(const String& serviceName, Service* s, ServiceFeatures* feats);

  /** Removes a service and drains admitted calls before its owner deletes it.
   * A service callback must not remove or replace its own registration.
   */
  void removeService(const String& serviceName);

  /** Gets a Service pointer whose lifetime the caller must externally pin. */
  Service* getService(const String& serviceName);

  /** Nonblocking lookup for diagnostics. Returned objects require an external
   * lifetime guarantee; unavailable or contended lookup resets both outputs.
   */
  bool tryGetService(const String& serviceName, Service*& service, ServiceFeatures*& features);

  /** Invokes a supported operation while retaining the registered service. */
  bool serve(const String& serviceName, ServiceFeatures::Type type, void* data, size_t dataLen);

 private:
  static ServiceManager m_Instance;

  /** Internal representation of a Service */
  class InternalService {
   public:
    /// The Service itself
    Service* pService;

    /// Service operation enumeration
    ServiceFeatures* pFeatures;

    OperationBarrier operations;
  };

  /** Services we know about */
  Mutex m_Lock;
  RadixTree<InternalService*> m_Services;
};
#endif
