/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_PS2_MOUSE_CALLBACK_REGISTRY_H
#define PEDIGREE_PS2_MOUSE_CALLBACK_REGISTRY_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/types.h"

class Ps2MouseCallbackRegistry {
 private:
  struct CallbackSlot;

 public:
  using Handler = void (*)(void*, const void*, size_t);

  class EXPORTED_PUBLIC Registration {
   private:
    using UnregisterThunk = bool (*)(void*, void*, size_t, Registration*);

   public:
    Registration();
    ~Registration();

    /**
     * Closes admission and retires this callback registration.
     *
     * External callers drain admitted callbacks before true is returned. A
     * callback cannot drain a live peer, so false preserves ownership for a
     * later retry outside callback context.
     */
    bool reset();

    explicit operator bool() const {
      return m_pSlot != nullptr;
    }

   private:
    friend class Ps2MouseCallbackRegistry;

    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

    void adopt(void* owner, void* slot, size_t generation, UnregisterThunk unregister) {
      m_pOwner = owner;
      m_pSlot = slot;
      m_Generation = generation;
      m_Unregister = unregister;
    }

    void releaseFromOwner(void* owner, void* slot, size_t generation) {
      if (m_pOwner == owner && m_pSlot == slot && m_Generation == generation) {
        m_pOwner = nullptr;
        m_pSlot = nullptr;
        m_Generation = 0;
        m_Unregister = nullptr;
      }
    }

    void* m_pOwner;
    void* m_pSlot;
    size_t m_Generation;
    UnregisterThunk m_Unregister;
  };

  Ps2MouseCallbackRegistry();
  ~Ps2MouseCallbackRegistry();

  MUST_USE_RESULT bool subscribe(Handler handler, void* parameter, Registration& registration);

  void dispatch(const void* buffer, size_t length);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  using CallbackPinHook = void (*)(Handler, void*);

  void setCallbackPinHook(CallbackPinHook hook);
#endif

 private:
  static constexpr size_t MaxCallbacks = 32;

  struct CallbackDispatch {
    void* owner;
    CallbackDispatch* next;
  };

  struct CallbackSlot {
    CallbackSlot();

    Handler handler;
    void* parameter;
    Registration* registration;
    size_t generation;
    size_t inFlight;
    bool enabled;
    bool draining;
    bool deferredRemoval;
    CallbackDispatch* dispatches;
    WaitQueue drainWaiters;
  };

  static void* currentDispatchOwner();
  static bool unregisterThunk(void* owner, void* slot, size_t generation,
                              Registration* registration);
  void clearSlot(CallbackSlot& slot);

  bool unregister(CallbackSlot* slot, size_t generation, Registration* registration);
  bool isCallbackContext(void* owner) const;
  void releaseCallback(CallbackSlot& slot, CallbackDispatch& dispatch);

  CallbackSlot m_Callbacks[MaxCallbacks];
  Spinlock m_CallbackLock;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  CallbackPinHook m_CallbackPinHook;
#endif
};

#endif
