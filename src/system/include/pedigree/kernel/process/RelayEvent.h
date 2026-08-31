/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_RELAYEVENT_H
#define PEDIGREE_KERNEL_PROCESS_RELAYEVENT_H
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Event.h"

#include <config.h>

/**
 * A stable Event that resolves work through a callback when it is delivered.
 *
 * The callback value is serialized before the Event delivery lease is
 * released, so the handler never dereferences the RelayEvent object.
 */
class EXPORTED_PUBLIC RelayEvent : public Event {
 public:
  using Callback = void (*)(Thread*);

  RelayEvent(Callback callback, size_t number);

  size_t serialize(uint8_t* buffer) override;
  size_t getNumber() override;

 private:
  static void dispatch(size_t buffer);

  Callback m_Callback;
  size_t m_Number;
};

#endif
