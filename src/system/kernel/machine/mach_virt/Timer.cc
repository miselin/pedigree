/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "Timer.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/String.h"

#include "DeviceTree.h"
#include "GenericTimer.h"

namespace {
constexpr uint64_t NanosecondsPerSecond = 1000000000ULL;

bool leapYear(size_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

uint8_t daysInMonth(size_t year, uint8_t month) {
  static constexpr uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && leapYear(year) ? 29 : days[month - 1];
}

uint64_t addDuration(uint64_t deadline, size_t count, uint64_t multiplier) {
  if (count > (Time::Infinity - deadline) / multiplier) {
    return Time::Infinity;
  }
  return deadline + count * multiplier;
}
}  // namespace

VirtTimer VirtTimer::m_Instance;

VirtTimer& VirtTimer::instance() {
  return m_Instance;
}

VirtTimer::VirtTimer()
    : SplitIrqHandler(MakeConstantString("Virt timer bottom half")),
      m_Frequency(0),
      m_BootCount(0),
      m_LastCount(0),
      m_RtcBaseCount(0),
      m_RtcBaseSeconds(0),
      m_ElapsedSinceSync(0),
      m_IntervalTicks(0),
      m_IrqId(0),
      m_Year(1970),
      m_Month(1),
      m_Day(1),
      m_DayOfWeek(4),
      m_Hour(0),
      m_Minute(0),
      m_Second(0),
      m_Prepared(false),
      m_Initialised(false),
      m_Handlers(),
      m_Alarms(),
      m_AlarmLock(false) {}

VirtTimer::~VirtTimer() {
  uninitialise();
}

uint64_t VirtTimer::counter() {
  return VirtGenericTimer::count();
}

uint64_t VirtTimer::ticksToNanoseconds(uint64_t ticks) const {
  if (!m_Frequency) {
    return 0;
  }
  return (ticks / m_Frequency) * NanosecondsPerSecond +
         (ticks % m_Frequency) * NanosecondsPerSecond / m_Frequency;
}

bool VirtTimer::initialise1() {
  if (m_Prepared) {
    return false;
  }
  m_Frequency = VirtGenericTimer::frequency();
  if (!m_Frequency) {
    return false;
  }
  m_IntervalTicks = static_cast<uint32_t>(m_Frequency / 1000);
  if (!m_IntervalTicks) {
    m_IntervalTicks = 1;
  }
  m_BootCount = m_LastCount = counter();
  m_Handlers.reset();
  synchronise();
  VirtGenericTimer::physicalControl(0);
  m_Prepared = true;
  return true;
}

bool VirtTimer::initialise3() {
  if (!m_Prepared || m_Initialised || !initialiseSplitIrq()) {
    return false;
  }
  IrqManager& manager = *Machine::instance().getIrqManager();
  m_IrqId =
      registerIsaSplitIrq(manager, VirtDeviceTree::physicalTimerIrq(), IrqPolicy::levelHard());
  if (!m_IrqId) {
    shutdownSplitIrq();
    return false;
  }
  m_LastCount = counter();
  rearmIrqSources(1);
  m_Initialised = true;
  return true;
}

void VirtTimer::uninitialise() {
  if (!m_Prepared) {
    return;
  }
  if (m_Initialised) {
    if (!shutdownSplitIrq()) {
      return;
    }
    m_IrqId = 0;
    m_Initialised = false;
  }
  m_Handlers.reset();
  {
    LockGuard<Spinlock> guard(m_AlarmLock);
    for (List<Alarm*>::Iterator it = m_Alarms.begin(); it != m_Alarms.end(); ++it) {
      delete *it;
    }
    m_Alarms.clear();
  }
  m_Prepared = false;
}

VirtTimer::HardStageDisposition VirtTimer::hardIrq(irq_id_t number, InterruptState&, size_t& work) {
  if (number != m_IrqId) {
    return HardStageDisposition::NotHandled;
  }
  VirtGenericTimer::physicalControl(0);
  work = 1;
  return HardStageDisposition::Deferred;
}

void VirtTimer::threadedIrq(size_t work) {
  if (!work) {
    return;
  }
  const uint64_t now = counter();
  const uint64_t delta = ticksToNanoseconds(now - m_LastCount);
  m_LastCount = now;
  processElapsed(delta);
}

bool VirtTimer::quiesceIrqSources() {
  VirtGenericTimer::physicalControl(0);
  return true;
}

void VirtTimer::rearmIrqSources(size_t) {
  VirtGenericTimer::setPhysicalTimer(m_IntervalTicks);
}

uint64_t VirtTimer::getTickCount() {
  return getTickCountNano() / Time::Multiplier::Millisecond;
}

uint64_t VirtTimer::getTickCountNano() {
  return ticksToNanoseconds(counter() - m_BootCount);
}

uint64_t VirtTimer::getTickCountNanoFast() {
  return getTickCountNano();
}

Time::Timestamp VirtTimer::getUnixTimestamp() {
  return m_RtcBaseSeconds + ticksToNanoseconds(counter() - m_RtcBaseCount) / NanosecondsPerSecond;
}

uint64_t VirtTimer::getNanosecond() {
  return ticksToNanoseconds(counter() - m_RtcBaseCount) % NanosecondsPerSecond;
}

void VirtTimer::updateCivilTime(uint64_t seconds) {
  uint64_t days = seconds / 86400;
  uint64_t remainder = seconds % 86400;
  m_DayOfWeek = static_cast<uint8_t>((days + 4) % 7);
  m_Year = 1970;
  while (days >= (leapYear(m_Year) ? 366U : 365U)) {
    days -= leapYear(m_Year) ? 366U : 365U;
    ++m_Year;
  }
  m_Month = 1;
  while (days >= daysInMonth(m_Year, m_Month)) {
    days -= daysInMonth(m_Year, m_Month);
    ++m_Month;
  }
  m_Day = static_cast<uint8_t>(days + 1);
  m_Hour = static_cast<uint8_t>(remainder / 3600);
  remainder %= 3600;
  m_Minute = static_cast<uint8_t>(remainder / 60);
  m_Second = static_cast<uint8_t>(remainder % 60);
}

void VirtTimer::synchronise(bool tohw) {
  if (tohw) {
    return;
  }
  const uintptr_t rtc = VirtDeviceTree::rtcBase();
  const uint64_t count = counter();
  m_RtcBaseSeconds = rtc ? *reinterpret_cast<volatile uint32_t*>(rtc) : 0;
  m_RtcBaseCount = count;
  updateCivilTime(m_RtcBaseSeconds);
}

size_t VirtTimer::getYear() {
  return m_Year;
}

uint8_t VirtTimer::getMonth() {
  return m_Month;
}

uint8_t VirtTimer::getDayOfMonth() {
  return m_Day;
}

uint8_t VirtTimer::getDayOfWeek() {
  return m_DayOfWeek;
}

uint8_t VirtTimer::getHour() {
  return m_Hour;
}

uint8_t VirtTimer::getMinute() {
  return m_Minute;
}

uint8_t VirtTimer::getSecond() {
  return m_Second;
}

bool VirtTimer::registerHandler(TimerHandler* handler) {
  return m_Handlers.registerHandler(handler);
}

bool VirtTimer::unregisterHandler(TimerHandler* handler) {
  return m_Handlers.unregisterHandler(handler);
}

void VirtTimer::addAlarm(Event* event, size_t seconds, size_t microseconds) {
  LockGuard<Spinlock> guard(m_AlarmLock);
  uint64_t deadline = getTickCountNano();
  deadline = addDuration(deadline, seconds, Time::Multiplier::Second);
  deadline = addDuration(deadline, microseconds, Time::Multiplier::Microsecond);
  m_Alarms.pushBack(new Alarm{event, Processor::information().getCurrentThread(), deadline});
}

void VirtTimer::removeAlarm(Event* event) {
  removeAlarm(event, true);
}

size_t VirtTimer::removeAlarm(Event* event, bool returnZero) {
  LockGuard<Spinlock> guard(m_AlarmLock);
  for (List<Alarm*>::Iterator it = m_Alarms.begin(); it != m_Alarms.end(); ++it) {
    Alarm* alarm = *it;
    if (alarm->event != event) {
      continue;
    }
    size_t remaining = 0;
    if (!returnZero && alarm->deadline > getTickCountNano()) {
      const uint64_t delta = alarm->deadline - getTickCountNano();
      remaining = delta / Time::Multiplier::Second + (delta % Time::Multiplier::Second != 0);
    }
    m_Alarms.erase(it);
    delete alarm;
    return remaining;
  }
  return 0;
}

void VirtTimer::processElapsed(uint64_t nanoseconds) {
  m_ElapsedSinceSync += nanoseconds;
  if (m_ElapsedSinceSync >= NanosecondsPerSecond) {
    synchronise();
    m_ElapsedSinceSync %= NanosecondsPerSecond;
  }

  m_AlarmLock.acquire();
  const uint64_t now = getTickCountNano();
  while (true) {
    bool dispatched = false;
    for (List<Alarm*>::Iterator it = m_Alarms.begin(); it != m_Alarms.end(); ++it) {
      Alarm* alarm = *it;
      if (alarm->deadline <= now) {
        alarm->thread->sendEvent(alarm->event);
        m_Alarms.erase(it);
        delete alarm;
        dispatched = true;
        break;
      }
    }
    if (!dispatched) {
      break;
    }
  }
  m_AlarmLock.release();

  Machine::instance().getIrqManager()->tick();
  m_Handlers.dispatch(nanoseconds);
}
