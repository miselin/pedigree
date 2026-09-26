/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/PciDrivers.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"

namespace {
enum class State { Available, SlotPrepared, UnloadPrepared, Retired };

struct Registration {
  const PciDrivers::Driver* driver;
  bool closing = false;
};

struct Function {
  uint16_t route;
  Registration* owner = nullptr;
  Device* pci = nullptr;
  Device* controller = nullptr;
  State state = State::Available;
};

Mutex g_Lock;
List<Registration*> g_Drivers;
List<Function*> g_Functions;
bool g_Ready = false;

uint16_t routeId(Device* device) {
  return (device->getPciBusPosition() << 8) | (device->getPciDevicePosition() << 3) |
         device->getPciFunctionNumber();
}

bool pciFunction(Device* device) {
  Device* parent = device->getParent();
  return parent && parent->getSpecificType() == "pci" && device->getPciVendorId() &&
         device->getPciVendorId() != 0xffff && device->getPciBusPosition() < 256 &&
         device->getPciDevicePosition() < 32 && device->getPciFunctionNumber() < 8;
}

Function* findFunction(uint16_t route) {
  for (auto* function : g_Functions) {
    if (function->route == route) {
      return function;
    }
  }
  return nullptr;
}

Registration* findDriver(const PciDrivers::Driver* driver) {
  for (auto* registration : g_Drivers) {
    if (registration->driver == driver) {
      return registration;
    }
  }
  return nullptr;
}

Device* resolve(uint16_t route, const PciDrivers::Driver* driver = nullptr) {
  Device* result = nullptr;
  bool duplicate = false;
  auto visit = [&](Device* device) -> Device* {
    if (pciFunction(device) && routeId(device) == route &&
        (!driver || (device->getPciClassCode() == driver->classCode &&
                     device->getPciSubclassCode() == driver->subclass &&
                     device->getPciProgInterface() == driver->progInterface &&
                     device->getType() == Device::Root && !device->getNumChildren()))) {
      duplicate |= result != nullptr;
      result = device;
    }
    return device;
  };
  pedigree_std::Callable<decltype(visit)> callback(visit);
  Device::foreach (callback, nullptr);
  return duplicate ? nullptr : result;
}

bool bind(Function& function, Registration& registration) {
  if (registration.closing || function.owner || function.state != State::Available) {
    return false;
  }
  // Legacy PCI drivers may replace their generic nodes. Resolve unbound BDFs
  // afresh; only this registry's bindings retain the generic node pointer.
  const auto& driver = *registration.driver;
  Device* pci = resolve(function.route, &driver);
  if (!pci) {
    return false;
  }
  Device* controller = driver.attach(pci);
  if (!controller) {
    return false;
  }
  function.pci = pci;
  function.controller = controller;
  function.owner = &registration;
  return true;
}

void unbind(Function& function) {
  function.owner->driver->remove(function.controller);
  function.owner = nullptr;
  function.controller = nullptr;
  function.pci = nullptr;
  function.state = State::Available;
}
}  // namespace

namespace PciDrivers {
bool initialize() {
  LockGuard<Mutex> guard(g_Lock);
  if (g_Ready) {
    return true;
  }
  bool valid = true;
  auto visit = [&](Device* device) -> Device* {
    if (valid && pciFunction(device)) {
      const uint16_t route = routeId(device);
      if (findFunction(route)) {
        valid = false;
      } else {
        auto* function = new Function{route};
        if (!function || !g_Functions.tryPushBack(function)) {
          delete function;
          valid = false;
        }
      }
    }
    return device;
  };
  pedigree_std::Callable<decltype(visit)> callback(visit);
  Device::foreach (callback, nullptr);
  if (!valid) {
    while (g_Functions.count()) {
      delete g_Functions.popFront();
    }
    return false;
  }
  g_Ready = true;
  return true;
}

bool registerDriver(const Driver* driver) {
  if (!driver || !driver->attach || !driver->prepareRemove || !driver->cancelRemove ||
      !driver->remove) {
    return false;
  }
  LockGuard<Mutex> guard(g_Lock);
  if (!g_Ready || findDriver(driver)) {
    return false;
  }
  auto* registration = new Registration{driver};
  if (!registration || !g_Drivers.tryPushBack(registration)) {
    delete registration;
    return false;
  }
  for (auto* function : g_Functions) {
    bind(*function, *registration);
  }
  return true;
}

bool prepareUnregisterDriver(const Driver* driver) {
  LockGuard<Mutex> guard(g_Lock);
  Registration* registration = findDriver(driver);
  if (!registration || registration->closing) {
    return true;
  }
  for (auto* function : g_Functions) {
    if (function->owner == registration && function->state != State::Available) {
      return false;
    }
  }
  registration->closing = true;
  for (auto* function : g_Functions) {
    if (function->owner != registration) {
      continue;
    }
    if (!driver->prepareRemove(function->controller)) {
      for (auto* prepared : g_Functions) {
        if (prepared->owner == registration && prepared->state == State::UnloadPrepared) {
          driver->cancelRemove(prepared->controller);
          prepared->state = State::Available;
        }
      }
      registration->closing = false;
      return false;
    }
    function->state = State::UnloadPrepared;
  }
  return true;
}

bool unregisterDriver(const Driver* driver) {
  LockGuard<Mutex> guard(g_Lock);
  Registration* registration = findDriver(driver);
  if (!registration) {
    return true;
  }
  if (!registration->closing) {
    return false;
  }
  for (auto* function : g_Functions) {
    if (function->owner == registration && function->state != State::UnloadPrepared) {
      return false;
    }
  }
  for (auto* function : g_Functions) {
    if (function->owner == registration) {
      unbind(*function);
    }
  }
  for (auto it = g_Drivers.begin(); it != g_Drivers.end(); ++it) {
    if (*it == registration) {
      g_Drivers.erase(it);
      break;
    }
  }
  delete registration;
  return true;
}

bool attach(Device* pci) {
  if (!pci) {
    return false;
  }
  LockGuard<Mutex> guard(g_Lock);
  if (!g_Ready || resolve(routeId(pci)) != pci) {
    return false;
  }
  Function* function = findFunction(routeId(pci));
  if (!function) {
    function = new Function{routeId(pci)};
    if (!function || !g_Functions.tryPushBack(function)) {
      delete function;
      return false;
    }
  }
  if (function->owner) {
    return function->pci == pci && function->state == State::Available;
  }
  if (function->state == State::Retired) {
    function->state = State::Available;
  }
  for (auto* registration : g_Drivers) {
    if (bind(*function, *registration)) {
      return true;
    }
  }
  return false;
}

bool prepareRemove(Device* pci) {
  if (!pci) {
    return false;
  }
  LockGuard<Mutex> guard(g_Lock);
  Function* function = findFunction(routeId(pci));
  if (!function || function->state != State::Available ||
      (function->owner && function->pci != pci)) {
    return false;
  }
  // Legacy remapping domains retain the requester pointer and cannot yet be
  // detached safely; module unload may keep the node, physical removal cannot.
  auto& bus = PciBus::instance();
  if (bus.hasDmaRemapping(pci) && !bus.hasDmaIsolation(pci)) {
    return false;
  }
  if (function->owner) {
    if (function->owner->closing || !function->owner->driver->prepareRemove(function->controller)) {
      return false;
    }
  } else {
    if (resolve(function->route) != pci || bus.hasDmaRemapping(pci)) {
      return false;
    }
    {
      Device::TreeLockGuard tree;
      if (pci->getType() != Device::Root || pci->getNumChildren()) {
        return false;
      }
    }
    uint16_t command = 0;
    if (!PciBus::instance().readConfig16(pci, 4, command) || (command & 4)) {
      return false;
    }
    function->pci = pci;
  }
  function->state = State::SlotPrepared;
  return true;
}

void cancelRemove(Device* pci) {
  if (!pci) {
    return;
  }
  LockGuard<Mutex> guard(g_Lock);
  Function* function = findFunction(routeId(pci));
  if (!function || function->state != State::SlotPrepared || function->pci != pci) {
    return;
  }
  if (function->owner) {
    function->owner->driver->cancelRemove(function->controller);
  } else {
    function->pci = nullptr;
  }
  function->state = State::Available;
}

bool remove(Device* pci) {
  if (!pci) {
    return false;
  }
  LockGuard<Mutex> guard(g_Lock);
  Function* function = findFunction(routeId(pci));
  if (!function || function->state != State::SlotPrepared || function->pci != pci) {
    return false;
  }
  if (function->owner) {
    unbind(*function);
  }
  function->pci = nullptr;
  function->state = State::Retired;
  return true;
}
}  // namespace PciDrivers
