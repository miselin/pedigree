/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/process/RelayEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/lib.h"

RelayEvent::RelayEvent(Callback callback, size_t number)
    : Event(reinterpret_cast<uintptr_t>(&RelayEvent::dispatch), false),
      m_Callback(callback),
      m_Number(number) {}

size_t RelayEvent::serialize(uint8_t* buffer) {
  MemoryCopy(buffer, &m_Callback, sizeof(m_Callback));
  return sizeof(m_Callback);
}

size_t RelayEvent::getNumber() {
  return m_Number;
}

void RelayEvent::dispatch(size_t buffer) {
  Callback callback = nullptr;
  MemoryCopy(&callback, reinterpret_cast<void*>(buffer), sizeof(callback));
  Thread* thread = Processor::information().getCurrentThread();
  if (callback && thread) {
    callback(thread);
  }
}
