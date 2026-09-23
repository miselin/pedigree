/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_VIRT_TIMER_H
#define KERNEL_MACHINE_VIRT_TIMER_H

#include "pedigree/kernel/machine/SplitIrqHandler.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/machine/TimerHandlerRegistry.h"
#include "pedigree/kernel/utilities/List.h"

class Event;
class Thread;

/** Monotonic architectural counter, PL031 wall clock, and 1 ms timer worker. */
class VirtTimer : public Timer, private SplitIrqHandler {
 public:
  static VirtTimer& instance();

  bool initialise1();
  bool initialise3();
  void uninitialise();

  size_t getYear() override;
  uint8_t getMonth() override;
  uint8_t getDayOfMonth() override;
  uint8_t getDayOfWeek() override;
  uint8_t getHour() override;
  uint8_t getMinute() override;
  uint8_t getSecond() override;
  uint64_t getNanosecond() override;
  uint64_t getTickCount() override;
  uint64_t getTickCountNano() override;
  uint64_t getTickCountNanoFast() override;
  Time::Timestamp getUnixTimestamp() override;
  void synchronise(bool tohw = false) override;

  bool registerHandler(TimerHandler* handler) override;
  bool unregisterHandler(TimerHandler* handler) override;
  void addAlarm(Event* event, size_t seconds, size_t microseconds = 0) override;
  void removeAlarm(Event* event) override;
  size_t removeAlarm(Event* event, bool returnZero) override;

 private:
  struct Alarm {
    Event* event;
    Thread* thread;
    uint64_t deadline;
  };

  VirtTimer();
  ~VirtTimer() override;

  HardStageDisposition hardIrq(irq_id_t number, InterruptState& state, size_t& work) override;
  void threadedIrq(size_t work) override;
  bool quiesceIrqSources() override;
  void rearmIrqSources(size_t work) override;

  static uint64_t counter();
  uint64_t ticksToNanoseconds(uint64_t ticks) const;
  void processElapsed(uint64_t nanoseconds);
  void updateCivilTime(uint64_t seconds);

  uint64_t m_Frequency;
  uint64_t m_BootCount;
  uint64_t m_LastCount;
  uint64_t m_RtcBaseCount;
  uint64_t m_RtcBaseSeconds;
  uint64_t m_ElapsedSinceSync;
  uint32_t m_IntervalTicks;
  irq_id_t m_IrqId;
  size_t m_Year;
  uint8_t m_Month;
  uint8_t m_Day;
  uint8_t m_DayOfWeek;
  uint8_t m_Hour;
  uint8_t m_Minute;
  uint8_t m_Second;
  bool m_Prepared;
  bool m_Initialised;
  TimerHandlerRegistry m_Handlers;
  List<Alarm*> m_Alarms;
  Spinlock m_AlarmLock;

  static VirtTimer m_Instance;
};

#endif
