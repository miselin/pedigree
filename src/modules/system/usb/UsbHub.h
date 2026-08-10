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
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"

#include "modules/system/usb/Usb.h"

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

  /// Adds a new handler for an interrupt IN transaction
  virtual void addInterruptInHandler(UsbEndpoint endpointInfo, uintptr_t pBuffer, uint16_t nBytes,
                                     void (*pCallback)(uintptr_t, ssize_t),
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

  using ConnectionChangePendingHook = void (*)(UsbHub* hub, size_t port, size_t observedState);
  using ConnectionChangeReplayWaitHook = void (*)(UsbHub* hub, size_t port);
  static void setConnectionChangePendingHookForTest(ConnectionChangePendingHook hook);
  static void setConnectionChangeReplayWaitHookForTest(ConnectionChangeReplayWaitHook hook);
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

  void releaseConnectionChangeSuppression(size_t port);

  /// Bitmap of used addresses under this hub
  /// \note valid only for root hubs
  ExtensibleBitmap m_UsedAddresses;

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

 protected:
  /** Associates a downstream hub with its already-initialised parent hub. */
  void attachToUpstreamHub(UsbHub* upstream);

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
};

#endif
