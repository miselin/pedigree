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

#ifndef USBHUB_H
#define USBHUB_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/List.h"

#include "modules/system/usb/Usb.h"

class UsbHub;
class UsbDeviceContainer;

/** Identifies one controller-owned recurring interrupt-IN subscription. */
struct UsbInterruptInToken {
  uintptr_t transaction;
  size_t generation;
};

/**
 * Owns one recurring interrupt-IN subscription.
 *
 * reset() removes the controller producer and drains every callback captured
 * for this exact subscription generation before returning.
 */
class EXPORTED_PUBLIC UsbInterruptInHandle {
 public:
  UsbInterruptInHandle();
  UsbInterruptInHandle(UsbInterruptInHandle&& other);
  ~UsbInterruptInHandle();

  UsbInterruptInHandle& operator=(UsbInterruptInHandle&& other);

  /** Returns false when callback context cannot safely wait for cancellation. */
  MUST_USE_RESULT bool reset();

  explicit operator bool() const {
    return static_cast<UsbHub*>(m_Owner) != nullptr;
  }

 private:
  friend class UsbHub;

  void adopt(UsbHub* owner, const UsbInterruptInToken& token, void (*callback)(uintptr_t, ssize_t),
             uintptr_t parameter);

  UsbInterruptInHandle(const UsbInterruptInHandle&) = delete;
  UsbInterruptInHandle& operator=(const UsbInterruptInHandle&) = delete;

  Atomic<UsbHub*> m_Owner;
  Atomic<bool> m_Resetting;
  UsbInterruptInToken m_Token;
  void (*m_Callback)(uintptr_t, ssize_t);
  uintptr_t m_Parameter;
  bool m_CancellationStarted;
};

class EXPORTED_PUBLIC UsbHub : public Device {
 public:
  /**
   * Suppresses root-port connection-change handling for one lexical scope.
   *
   * A suppressed hardware observation is remembered by UsbHub and replayed
   * once when the outermost lease is released. The lease is move-only so an
   * early return cannot leave a root port permanently suppressed.
   */
  class EXPORTED_PUBLIC ConnectionChangeSuppression {
   public:
    ConnectionChangeSuppression();
    ConnectionChangeSuppression(ConnectionChangeSuppression&& other);
    ~ConnectionChangeSuppression();

    ConnectionChangeSuppression& operator=(ConnectionChangeSuppression&& other);

    explicit operator bool() const {
      return m_Hub != nullptr;
    }

    void reset();

   private:
    friend class UsbHub;

    ConnectionChangeSuppression(UsbHub* hub, size_t port);

    ConnectionChangeSuppression(const ConnectionChangeSuppression&) = delete;
    ConnectionChangeSuppression& operator=(const ConnectionChangeSuppression&) = delete;

    UsbHub* m_Hub;
    size_t m_Port;
  };

  UsbHub();
  UsbHub(Device* p);

  virtual ~UsbHub();

  struct RootConnection {
    uint8_t port;
    size_t generation;
  };

  /** Resolves a child port to its root-controller connection generation. */
  RootConnection rootConnectionForChild(uint8_t childPort) const;

  virtual Type getType();

  /// Adds a new transfer to an existent transaction
  virtual void addTransferToTransaction(uintptr_t pTransaction, bool bToggle, UsbPid pid,
                                        uintptr_t pBuffer, size_t nBytes) = 0;

  /// Creates a new transaction with the given endpoint data
  virtual uintptr_t createTransaction(UsbEndpoint endpointInfo) = 0;

  /**
   * Performs a transaction asynchronously.
   *
   * Returning true transfers one completion obligation to the controller:
   * it must call the callback exactly once, and only after it can no longer
   * access any transfer buffer. Returning false guarantees that the callback
   * was not and will not be called, and that the rejected transaction no
   * longer owns controller or transfer-buffer state.
   */
  MUST_USE_RESULT virtual bool doAsync(uintptr_t pTransaction,
                                       void (*pCallback)(uintptr_t, ssize_t) = 0,
                                       uintptr_t pParam = 0) = 0;

  /**
   * Cancels an accepted transaction or drains a completion which won the
   * race. On return, the matching callback has run exactly once and the
   * controller can no longer access any transfer buffer.
   *
   * The callback and parameter identify the original transaction generation,
   * preventing a stale numeric transaction handle from cancelling a reused
   * controller slot.
   */
  virtual void cancelAsyncAndDrain(uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
                                   uintptr_t pParam) = 0;

  /// Adds an owned recurring interrupt-IN transaction.
  MUST_USE_RESULT virtual bool addInterruptInHandler(UsbEndpoint endpointInfo, uintptr_t pBuffer,
                                                     uint16_t nBytes,
                                                     void (*pCallback)(uintptr_t, ssize_t),
                                                     UsbInterruptInHandle& handle,
                                                     uintptr_t pParam = 0) = 0;

  /// Called when a device is connected to a port on the hub
  bool deviceConnected(uint8_t nPort, UsbSpeed speed);

  /// Called when a device is disconnected from a port on the hub
  void deviceDisconnected(uint8_t nPort);

  /// Performs a transaction, blocks until it's completed and returns the
  /// result
  ssize_t doSync(uintptr_t nTransaction, uint32_t timeout = 5000);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  /** Exercises callback ownership at the synchronous timeout boundary. */
  static bool runHostedSyncOwnershipRegression();

  /** Exercises recurring callback cancellation and handle retention. */
  static bool runHostedInterruptOwnershipRegression();

  using ConnectionChangePendingHook = void (*)(UsbHub* hub, size_t port, size_t observedState);
  using ConnectionChangeReplayWaitHook = void (*)(UsbHub* hub, size_t port);
  static void setConnectionChangePendingHookForTest(ConnectionChangePendingHook hook);
  static void setConnectionChangeReplayWaitHookForTest(ConnectionChangeReplayWaitHook hook);
#endif

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  /** Exercises recurring cancellation/drain across SMP callback threads. */
  static bool runQemuInterruptOwnershipRegression();
#endif

  /// Gets a UsbDevice from a given vendor:product pair
  // void getDeviceByIds(size_t vendor, size_t product, void
  // (*pCallback)(class UsbDevice *));

  /// Performs a port reset for the given port. Should only be used in
  /// situations where a device cannot recover from an error without a
  /// complete reset.
  /// \note Assumes the port is at CONNECTED with a VALID DEVICE attached.
  /// \param bErrorResponse true if this is a reset as a response to an error.
  ///                       Error responses are allowed to use significantly
  ///                       longer delays in their reset logic.
  virtual bool portReset(uint8_t nPort, bool bErrorResponse = false) = 0;

 private:
#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
  static bool runInterruptOwnershipRegression();
  static int retireProbeSubtreeForTest(void* parameter);
#endif

  /// Structure used synchronous transactions
  struct SyncParam {
    inline SyncParam() : semaphore(0), nResult(-1), owners(2) {}

    ~SyncParam();
    void releaseOwner();

    Semaphore semaphore;
    ssize_t nResult;
    Atomic<size_t> owners;
  };

  /// Callback used by synchronous transactions
  static void syncCallback(uintptr_t pParam, ssize_t ret);

  void deviceDisconnectedLocked(uint8_t nPort);
  void retirePortContainersLocked(uint8_t nPort, bool* retiredAddresses,
                                  bool* pinnedSubtreeAddresses = nullptr);
  void releaseRetiredAddressesLocked(uint8_t nPort, const bool* retiredAddresses);
  void retainAddressLocked(size_t address);
  void releaseAddressLocked(size_t address);
  void retainAddressesLocked(const bool* addresses);
  void releaseAddressesLocked(const bool* addresses);
  static void collectUsbAddresses(Device* device, bool* addresses);
  static void collectImmediateUsbContainers(Device* device, List<UsbDeviceContainer*>& containers);
  static void drainSubtreeProbeAdmissions(UsbDeviceContainer* container);
  void releaseConnectionChangeSuppression(size_t port);

  /// Bitmap of used addresses under this hub
  /// \note valid only for root hubs
  ExtensibleBitmap m_UsedAddresses;

  /** Live owners plus temporary subtree-retirement pins for each address. */
  size_t m_AddressReferences[128] = {};

  /** Serializes default-address enumeration and address ownership. */
  Mutex m_EnumerationLock;

  /** Serializes connection generations published by this hub. */
  Mutex m_TopologyLock;

  static constexpr size_t ConnectionChangePortCount = 16;
  static constexpr size_t ConnectionChangePending =
      static_cast<size_t>(1) << ((sizeof(size_t) * 8) - static_cast<size_t>(1));
  static constexpr size_t ConnectionChangeCountMask = ~ConnectionChangePending;

  /** One atomic publication state per USB 2 root-port slot. */
  Atomic<size_t> m_ConnectionChangeStates[ConnectionChangePortCount];

  /** Shared address-allocation owner for this hub hierarchy. */
  UsbHub* m_RootHub;

  /** Device-taking construction identifies an HCD rather than a USB hub. */
  bool m_IsRootHub;

  /** Root-controller port through which this downstream hub is reached. */
  uint8_t m_RootPort;

  /** Root connection generation captured by the downstream hub device. */
  size_t m_RootPortGeneration;

  /** Driver-only unbind cannot prove downstream devices stopped answering. */
  bool m_RetainDisconnectedAddresses = false;

 protected:
  friend class UsbInterruptInHandle;

  /** Publishes a controller token into an empty caller-owned handle. */
  MUST_USE_RESULT bool publishInterruptInHandle(UsbInterruptInHandle& handle,
                                                const UsbInterruptInToken& token,
                                                void (*callback)(uintptr_t, ssize_t),
                                                uintptr_t parameter);

  /** Stops and drains one exact recurring interrupt-IN subscription. */
  MUST_USE_RESULT virtual bool cancelInterruptInAndDrain(const UsbInterruptInToken& token,
                                                         void (*callback)(uintptr_t, ssize_t),
                                                         uintptr_t parameter,
                                                         bool producerAlreadyStopped) = 0;

  /** True when this execution context must not wait for a USB callback. */
  virtual bool inInterruptCallbackContext() const {
    return false;
  }

  /** Destroys every USB child while the most-derived hub is still live. */
  void disconnectAllDevices();

  /** Keeps descendant addresses reserved when hardware cannot be reset. */
  void retainDisconnectedAddressesUntilControllerTeardown();

  /** Associates a downstream hub with its already-initialised parent hub. */
  void attachToUpstreamHub(UsbHub* upstream, const RootConnection& connection);

  /** Returns the HCD which owns address allocation for this hierarchy. */
  UsbHub* rootHub() const {
    return m_RootHub;
  }

  /** Begins one nested root-port suppression scope. */
  MUST_USE_RESULT bool suppressConnectionChanges(size_t port,
                                                 ConnectionChangeSuppression& suppression);

  /**
   * Atomically records an observation only if its suppression scope is
   * still active. False means the caller must process the observation now.
   */
  MUST_USE_RESULT bool deferConnectionChangeIfSuppressed(size_t port);

  /** Re-publishes one coalesced observation in ordinary thread context. */
  virtual void replaySuppressedConnectionChange(size_t port);

  /** Current observed connection generation for one HCD root port. */
  virtual size_t currentRootPortGeneration(size_t port) const {
    (void)port;
    return 0;
  }
};

#endif
