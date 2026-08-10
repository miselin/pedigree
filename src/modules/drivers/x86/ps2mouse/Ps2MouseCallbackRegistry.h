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

  class Registration {
   private:
    using UnregisterThunk = void (*)(void*, void*, Registration*);

   public:
    Registration() : m_pOwner(nullptr), m_pSlot(nullptr), m_Unregister(nullptr) {}

    ~Registration() {
      reset();
    }

    void reset() {
      if (!m_pSlot) {
        return;
      }

      void* owner = m_pOwner;
      void* slot = m_pSlot;
      UnregisterThunk unregister = m_Unregister;
      m_pOwner = nullptr;
      m_pSlot = nullptr;
      m_Unregister = nullptr;
      unregister(owner, slot, this);
    }

    explicit operator bool() const {
      return m_pSlot != nullptr;
    }

   private:
    friend class Ps2MouseCallbackRegistry;

    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

    void adopt(void* owner, void* slot, UnregisterThunk unregister) {
      m_pOwner = owner;
      m_pSlot = slot;
      m_Unregister = unregister;
    }

    void releaseFromOwner(void* owner, void* slot) {
      if (m_pOwner == owner && m_pSlot == slot) {
        m_pOwner = nullptr;
        m_pSlot = nullptr;
        m_Unregister = nullptr;
      }
    }

    void* m_pOwner;
    void* m_pSlot;
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
    size_t inFlight;
    bool enabled;
    bool draining;
    bool deferredRemoval;
    CallbackDispatch* dispatches;
    WaitQueue drainWaiters;
  };

  static void* currentDispatchOwner();
  static void unregisterThunk(void* owner, void* slot, Registration* registration);
  void clearSlot(CallbackSlot& slot);

  void unregister(CallbackSlot* slot, Registration* registration);
  void releaseCallback(CallbackSlot& slot, CallbackDispatch& dispatch);

  CallbackSlot m_Callbacks[MaxCallbacks];
  Spinlock m_CallbackLock;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  CallbackPinHook m_CallbackPinHook;
#endif
};

#endif
