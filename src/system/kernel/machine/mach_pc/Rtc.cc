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

#include <compiler.h>
#include <machine/Machine.h>
#include <process/Thread.h>
#include <process/Scheduler.h>
#include "Rtc.h"

#include <SlamAllocator.h>

#define INITIAL_RTC_HZ 512
#define BCD_TO_BIN8(x) (((((x) & 0xF0) >> 4) * 10) + ((x) & 0x0F))
#define BIN_TO_BCD8(x) ((((x) / 10) * 16) + ((x) % 10))

Rtc::periodicIrqInfo_t Rtc::periodicIrqInfo[6] =
{
  { 256, 0x08, {3906250ULL, 3906250ULL}},
  { 512, 0x07, {1953125ULL, 1953125ULL}},
  {1024, 0x06, { 976562ULL,  976563ULL}},
  {2048, 0x05, { 488281ULL,  488281ULL}},
  {4096, 0x04, { 244140ULL,  244141ULL}},
  {8192, 0x03, { 122070ULL,  122070ULL}},
};

uint8_t daysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

Rtc Rtc::m_Instance;

void Rtc::addAlarm(Event *pEvent, size_t alarmSecs, size_t alarmUsecs)
{
    Alarm *pAlarm = new Alarm(pEvent, alarmSecs*1000000+alarmUsecs+getTickCount(),
                              Processor::information().getCurrentThread());
    m_Alarms.pushBack(pAlarm);
}

void Rtc::removeAlarm(Event *pEvent)
{
    for (List<Alarm*>::Iterator it = m_Alarms.begin();
         it != m_Alarms.end();
         it++)
    {
        if ( (*it)->m_pEvent == pEvent )
        {
            Alarm *pAlarm = *it;
            m_Alarms.erase(it);
            delete pAlarm;
            return;
        }
    }
}

size_t Rtc::removeAlarm(class Event *pEvent, bool bRetZero)
{
    size_t currTime = getTickCount();

    for (List<Alarm*>::Iterator it = m_Alarms.begin();
         it != m_Alarms.end();
         it++)
    {
        if ( (*it)->m_pEvent == pEvent )
        {
            size_t ret = 0;
            if(!bRetZero)
            {
                size_t alarmEndTime = (*it)->m_Time;

                // Is it later than the end of the alarm?
                if(alarmEndTime < currTime)
                    ret = 0;
                else
                {
                    size_t diff = alarmEndTime - currTime;
                    ret = (diff / 1000) + 1;
                }
            }

            Alarm *pAlarm = *it;
            m_Alarms.erase(it);
            delete pAlarm;
            return ret;
        }
    }

    return 0;
}

bool Rtc::registerHandler(TimerHandler *handler)
{
  // find a spare spot and install
  size_t nHandler;
  for(nHandler = 0; nHandler < MAX_TIMER_HANDLERS; nHandler++)
  {
    if(m_Handlers[nHandler] == 0)
    {
      m_Handlers[nHandler] = handler;
      return true;
    }
  }

  // no room!
  return false;
}
bool Rtc::unregisterHandler(TimerHandler *handler)
{
  // find a spare spot and install
  size_t nHandler;
  for(nHandler = 0; nHandler < MAX_TIMER_HANDLERS; nHandler++)
  {
    if(m_Handlers[nHandler] == handler)
    {
      m_Handlers[nHandler] = 0;
      return true;
    }
  }

  // not found
  return false;
}
size_t Rtc::getYear()
{
  return m_Year;
}
uint8_t Rtc::getMonth()
{
  return m_Month;
}
uint8_t Rtc::getDayOfMonth()
{
  return m_DayOfMonth;
}
uint8_t Rtc::getDayOfWeek()
{
  static size_t monthnumbers[] = {0, 3, 3, 6, 1, 4, 6, 2, 5, 0, 3, 5};

  // Calculate day of week
  uint8_t dayOfWeek = m_DayOfMonth % 7;
  dayOfWeek += monthnumbers[m_Month - 1];
  dayOfWeek += ((m_Year % 100) + ((m_Year % 100) / 4)) % 7;
  dayOfWeek -= ((m_Year / 100) % 4 - 3) * 2;
  if (m_Month < 3)dayOfWeek--;
  dayOfWeek %= 7;

  return dayOfWeek;
}
uint8_t Rtc::getHour()
{
  return m_Hour;
}
uint8_t Rtc::getMinute()
{
  return m_Minute;
}
uint8_t Rtc::getSecond()
{
  return m_Second;
}
uint64_t Rtc::getNanosecond()
{
  return m_Nanosecond;
}
uint64_t Rtc::getTickCount()
{
  return m_TickCount / 1000ULL;
}

bool Rtc::initialise()
{
  NOTICE("Rtc::initialise");

  // Allocate the I/O port range"CMOS"
  if (m_IoPort.allocate(0x70, 2) == false)
    return false;

  // initialise handlers
  memset(m_Handlers, 0, sizeof(TimerHandler*) * MAX_TIMER_HANDLERS);

  // Register the irq
  IrqManager &irqManager = *Machine::instance().getIrqManager();
  m_IrqId = irqManager.registerIsaIrqHandler(8, this);
  if (m_IrqId == 0)
    return false;

  // Are the RTC values in the CMOS encoded in BCD (or binary)?
  m_bBCD = (read(0x0B) & 0x04) != 0x04;

  // Read the time and date
  if (m_bBCD == true)
  {
    m_Second = BCD_TO_BIN8(read(0x00));
    m_Minute = BCD_TO_BIN8(read(0x02));
    m_Hour = BCD_TO_BIN8(read(0x04));
    m_DayOfMonth = BCD_TO_BIN8(read(0x07));
    m_Month = BCD_TO_BIN8(read(0x08));
    m_Year = BCD_TO_BIN8(read(0x32)) * 100 + BCD_TO_BIN8(read(0x09));
  }
  else
  {
    m_Second = read(0x00);
    m_Minute = read(0x02);
    m_Hour = read(0x04);
    m_DayOfMonth = read(0x07);
    m_Month = read(0x08);
    m_Year = read(0x32) * 100 + read(0x09);
  }

  // Find the initial rtc rate
  uint8_t rateBits = 0x06;
  for (size_t i = 0;i < 6;i++)
    if (periodicIrqInfo[i].Hz == INITIAL_RTC_HZ)
    {
      m_PeriodicIrqInfoIndex = i;
      rateBits = periodicIrqInfo[i].rateBits;
      break;
    }

  // Set the Rate for the periodic IRQ
  uint8_t tmp = read(0x0A);
  write(0x0A, (tmp & 0xF0) | rateBits);

  // Activate the IRQ
  uint8_t statusb = read(0x0B);
  write(0x0B, statusb | 0x40);
  read(0x0C); // Some RTC chips need the interrupt status to be cleared after
              // changing the control register.

  return true;
}
void Rtc::synchronise(bool tohw)
{
  enableRtcUpdates(false);

  if (tohw)
  {
    // Write the time and date back
    if (m_bBCD == true)
    {
      write(0x00, BIN_TO_BCD8(m_Second));
      write(0x02, BIN_TO_BCD8(m_Minute));
      write(0x04, BIN_TO_BCD8(m_Hour));
      write(0x07, BIN_TO_BCD8(m_DayOfMonth));
      write(0x08, BIN_TO_BCD8(m_Month));
      write(0x09, BIN_TO_BCD8(m_Year % 100));
      write(0x32, BIN_TO_BCD8(m_Year / 100));
    }
    else
    {
      write(0x00, m_Second);
      write(0x02, m_Minute);
      write(0x04, m_Hour);
      write(0x07, m_DayOfMonth);
      write(0x08, m_Month);
      write(0x09, m_Year % 100);
      write(0x32, m_Year / 100);
    }
  }
  else
  {
    // Read the time and date
    if (m_bBCD == true)
    {
      m_Second = BCD_TO_BIN8(read(0x00));
      m_Minute = BCD_TO_BIN8(read(0x02));
      m_Hour = BCD_TO_BIN8(read(0x04));
      m_DayOfMonth = BCD_TO_BIN8(read(0x07));
      m_Month = BCD_TO_BIN8(read(0x08));
      m_Year = BCD_TO_BIN8(read(0x32)) * 100 + BCD_TO_BIN8(read(0x09));
    }
    else
    {
      m_Second = read(0x00);
      m_Minute = read(0x02);
      m_Hour = read(0x04);
      m_DayOfMonth = read(0x07);
      m_Month = read(0x08);
      m_Year = read(0x32) * 100 + read(0x09);
    }
  }

  enableRtcUpdates(true);
}
void Rtc::uninitialise()
{
  // Deactivate the IRQ
  uint8_t statusb = read(0x0B);
  write(0x0B, statusb & ~0x40);

  synchronise(true);

  // Unregister the irq
  IrqManager &irqManager = *Machine::instance().getIrqManager();
  irqManager.unregisterHandler(m_IrqId, this);

  // Free the I/O port range
  m_IoPort.free();
}

Rtc::Rtc()
  : m_IoPort("CMOS"), m_IrqId(0), m_PeriodicIrqInfoIndex(0), m_bBCD(true), m_Year(0), m_Month(0),
    m_DayOfMonth(0), m_Hour(0), m_Minute(0), m_Second(0), m_Nanosecond(0), m_TickCount(0), m_Alarms()
{
}
extern size_t g_FreePages;
extern size_t g_AllocedPages;
bool Rtc::irq(irq_id_t number, InterruptState &state)
{
  static size_t index = 0;
  // Update the Tick Count
  uint64_t delta = periodicIrqInfo[m_PeriodicIrqInfoIndex].ns[index];
  index = (index == 0) ? 1 : 0;
  m_TickCount += delta;

  // Calculate the new time/date
  m_Nanosecond += delta;

  // Check for alarms.
  while (true)
  {
      bool bDispatched = false;
      for (List<Alarm*>::Iterator it = m_Alarms.begin();
           it != m_Alarms.end();
           it++)
      {
          Alarm *pA = *it;
          if ( pA->m_Time <= getTickCount() )
          {
              pA->m_pThread->sendEvent(pA->m_pEvent);
              m_Alarms.erase(it);
              bDispatched = true;
              delete pA;
              break;
          }
      }
      if (!bDispatched)
          break;
  }

  if (UNLIKELY(m_Nanosecond >= 1000000ULL))
  {
    // Every millisecond, unblock any interrupts which were halted and halt any
    // which need to be halted.
    Machine::instance().getIrqManager()->tick();
  }

  if (UNLIKELY(m_Nanosecond >= 1000000000ULL))
  {
    ++m_Second;
    m_Nanosecond -= 1000000000ULL;

#ifndef MEMORY_TRACING  // Memory tracing uses serial line 1.
#ifdef MEMORY_LOGGING_ENABLED
    Serial *pSerial = Machine::instance().getSerial(1);
    NormalStaticString str;
    str += "Heap: ";
    str += SlamAllocator::instance().heapPageCount() * 4;
    str += "K\tPages: ";
    str += (g_AllocedPages * 4096) / 1024;
    str += "K\t Free: ";
    str += (g_FreePages * 4096) / 1024;
    str += "K\n";

    pSerial->write(str);

    // Memory snapshot of current processes.
    for(size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i)
    {
        Process *pProcess = Scheduler::instance().getProcess(i);
        LargeStaticString str;

        ssize_t virtK = (pProcess->getVirtualPageCount() * 0x1000) / 1024;
        ssize_t physK = (pProcess->getPhysicalPageCount() * 0x1000) / 1024;
        ssize_t shrK = (pProcess->getSharedPageCount() * 0x1000) / 1024;

        str.append("\tProcess ");
        str.append(pProcess->description());
        str.append(" V=");
        str.append(virtK, 10);
        str.append("K P=");
        str.append(physK, 10);
        str.append("K S=");
        str.append(shrK, 10);
        str.append("\n");
        pSerial->write(str);
    }
#endif
#endif

    if (UNLIKELY(m_Second == 60))
    {
      ++m_Minute;
      m_Second = 0;

      if (UNLIKELY(m_Minute == 60))
      {
        ++m_Hour;
        m_Minute = 0;

        if (UNLIKELY(m_Hour == 24))
        {
          ++m_DayOfMonth;
          m_Hour = 0;

          // Are we in a leap year
          bool isLeap = ((m_Year % 4) == 0) & (((m_Year % 100) != 0) | ((m_Year % 400) == 0));

          if (UNLIKELY(((m_DayOfMonth > daysPerMonth[m_Month - 1]) && ((m_Month != 2) || isLeap == false)) ||
                       (m_DayOfMonth > (daysPerMonth[m_Month - 1] + 1))))
          {
            ++m_Month;
            m_DayOfMonth = 1;

            if (UNLIKELY(m_Month > 12))
            {
              ++m_Year;
              m_Month = 1;
            }
          }
        }
      }
    }
  }

  // Acknowledging the IRQ (within the CMOS)
  read(0x0C);

  // call handlers
  size_t nHandler;
  for(nHandler = 0; nHandler < MAX_TIMER_HANDLERS; nHandler++)
  {
    // timer delta is in nanoseconds
    if(m_Handlers[nHandler])
      m_Handlers[nHandler]->timer(delta, state);
  }

  return true;
}

void Rtc::setIndex(uint8_t index)
{
  uint8_t idx = m_IoPort.read8(0);
  m_IoPort.write8((idx & 0x80) | (index & 0x7F), 0);
}
void Rtc::waitForUpdateCompletion(uint8_t index)
{
  if (index <= 9 || index == 50)
  {
    setIndex(0x0A);
    while ((m_IoPort.read8(1) & 0x80) == 0x80);
  }
}
void Rtc::enableRtcUpdates(bool enable)
{
  // Write the index
  setIndex(0x0B);

  // Update the status register
  uint8_t statusA = m_IoPort.read8(1);
  m_IoPort.write8((statusA & 0x80) | (enable ? 0 : (1 << 7)), 1);
}
uint8_t Rtc::read(uint8_t index)
{
  // Wait until the RTC Update is completed
  waitForUpdateCompletion(index);

  // Write the index
  setIndex(index);

  // Read the data at that index
  return m_IoPort.read8(1);
}
void Rtc::write(uint8_t index, uint8_t value)
{
  // Wait until the RTC Update is completed
  waitForUpdateCompletion(index);

  // Write the index
  setIndex(index);

  // Write the data to that index
  m_IoPort.write8(value, 1);
}
