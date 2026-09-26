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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"

ServiceManager ServiceManager::m_Instance;

ServiceManager::ServiceManager() : m_Lock(), m_Services() {}

ServiceManager::~ServiceManager() {
  List<InternalService*> services;
  {
    LockGuard<Mutex> guard(m_Lock);
    for (auto* service : m_Services) {
      service->operations.close();
      services.pushBack(service);
    }
    m_Services.clear();
  }
  for (auto* service : services) {
    service->operations.wait();
    delete service;
  }
}

ServiceFeatures* ServiceManager::enumerateOperations(const String& serviceName) {
  LockGuard<Mutex> guard(m_Lock);
  RadixTree<InternalService*>::LookupType result = m_Services.lookup(serviceName);
  if (result.hasValue()) {
    return result.value()->pFeatures;
  } else {
    return 0;
  }
}

void ServiceManager::addService(const String& serviceName, Service* s, ServiceFeatures* feats) {
  if (!s || !feats) {
    return;
  }
  InternalService* p = new InternalService;
  if (!p) {
    return;
  }
  p->pService = s;
  p->pFeatures = feats;
  InternalService* displaced = nullptr;
  {
    LockGuard<Mutex> guard(m_Lock);
    auto old = m_Services.lookup(serviceName);
    if (old.hasValue()) {
      displaced = old.value();
      displaced->operations.close();
    }
    m_Services.insert(serviceName, p);
  }
  if (displaced) {
    displaced->operations.wait();
    delete displaced;
  }
}

void ServiceManager::removeService(const String& serviceName) {
  InternalService* service = nullptr;
  {
    LockGuard<Mutex> guard(m_Lock);
    auto result = m_Services.lookup(serviceName);
    if (!result.hasValue()) {
      return;
    }
    service = result.value();
    m_Services.remove(serviceName);
    service->operations.close();
  }
  service->operations.wait();
  delete service;
}

Service* ServiceManager::getService(const String& serviceName) {
  LockGuard<Mutex> guard(m_Lock);
  RadixTree<InternalService*>::LookupType result = m_Services.lookup(serviceName);
  if (result.hasValue()) {
    return result.value()->pService;
  } else {
    return 0;
  }
}

bool ServiceManager::tryGetService(const String& serviceName, Service*& service,
                                   ServiceFeatures*& features) {
  service = nullptr;
  features = nullptr;
  TerminationDeferral lifetime(Processor::getInterrupts());
  if (!m_Lock.tryAcquire()) {
    return false;
  }
  auto result = m_Services.lookup(serviceName);
  if (result.hasValue()) {
    service = result.value()->pService;
    features = result.value()->pFeatures;
  }
  m_Lock.release();
  return service && features;
}

bool ServiceManager::serve(const String& serviceName, ServiceFeatures::Type type, void* data,
                           size_t dataLen) {
  OperationBarrier::Lease operation;
  Service* service = nullptr;
  ServiceFeatures* features = nullptr;
  {
    LockGuard<Mutex> guard(m_Lock);
    auto result = m_Services.lookup(serviceName);
    if (!result.hasValue() || !result.value()->operations.tryAcquire(operation)) {
      return false;
    }
    service = result.value()->pService;
    features = result.value()->pFeatures;
  }
  return features->provides(type) && service->serve(type, data, dataLen);
}
