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

#ifndef USBPNP_H
#define USBPNP_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/new"

class Device;
class UsbDevice;

enum UsbPnPConstants {
  VendorIdNone = 0xFFFF,
  ProductIdNone = 0xFFFF,
  ClassNone = 0xFF,
  SubclassNone = 0xFF,
  ProtocolNone = 0xFF,
};

class EXPORTED_PUBLIC UsbPnP {
 private:
  struct CallbackItem;

  /// Callback function type
  typedef UsbDevice* (*callback_t)(UsbDevice*);

 public:
  /**
   * Owns one callback registration.
   *
   * reset() closes admission to the factory/probe callback. Outside callback
   * dispatch it waits for callbacks which were already admitted and returns
   * true once callback-owned state may be destroyed. During dispatch, an active
   * target is closed but retained and false is returned; ownership remains live
   * for a later retry from outside callback context. It does not retire
   * successfully bound driver instances; a module must detach and destroy those
   * objects separately before its code can be unloaded.
   */
  class EXPORTED_PUBLIC Registration {
   public:
    Registration();
    Registration(Registration&& other);
    ~Registration();

    Registration& operator=(Registration&& other);

    bool reset();

    explicit operator bool() const {
      return m_Item != nullptr;
    }

   private:
    friend class UsbPnP;

    void adopt(UsbPnP* owner, CallbackItem* item);

    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

    UsbPnP* m_Owner;
    CallbackItem* m_Item;
  };

  UsbPnP();
  virtual ~UsbPnP();

  /// Singleton design
  static UsbPnP& instance() {
    return m_Instance;
  }

  /// Register a callback for the given vendor and product IDs
  MUST_USE_RESULT bool registerCallback(uint16_t nVendorId, uint16_t nProductId,
                                        callback_t callback, Registration& registration);

  /// Register a callback for the given class, subclass and protocol numbers
  MUST_USE_RESULT bool registerCallback(uint8_t nClass, uint8_t nSubclass, uint8_t nProtocol,
                                        callback_t callback, Registration& registration);

  /// Tries to find a suitable driver for the given USB device
  bool probeDevice(Device* pDeviceBase);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  static bool runHostedRegistrationRegression();
#endif
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  static bool runQemuRegistrationRegression();
#endif
#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
  bool invokeCallbackForTest(size_t callbackIndex = 0);
  size_t callbackCountForTest();
  bool callbackStorageEmptyForTest();
#endif

 private:
  struct ActiveInvocation;

  /// Probes a device and returns the new device if it was successfully
  /// loaded and owned by a driver, or the original pointer otherwise.
  Device* doProbe(Device* pDeviceBase);

  bool registerCallbackItem(CallbackItem* item, Registration& registration, bool reprobe);
  bool unregisterCallback(CallbackItem* item);
  bool acquireCallback(UsbDevice* device, size_t afterSequence, CallbackItem*& item,
                       callback_t& callback, size_t& sequence, ActiveInvocation& invocation);
  void finishCallback(CallbackItem* item, ActiveInvocation& invocation);
  static void* currentInvocationOwner();
  bool isCallbackContext(void* owner) const;
#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
  static bool runRegistrationRegression();
#endif

  /// Static instance
  static UsbPnP m_Instance;

  /// Goes down the device tree, reprobing every USB device
  void reprobeDevices(Device* pParent);

  CallbackItem* m_FirstCallback;
  CallbackItem* m_LastCallback;
  size_t m_CallbackCount;
  Spinlock m_CallbackLock;
  size_t m_NextCallbackSequence;
  ActiveInvocation* m_ActiveInvocations;
};

#endif
