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

#include "modules/subsys/posix/VirtualTerminal.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Vga.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"

#include "modules/subsys/posix/DevFs.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/system/console/Console.h"

extern DevFs* g_pDevFs;

VirtualTerminalManager::VirtualTerminalManager(DevFsDirectory* parentDir)
    : m_pTty(nullptr),
      m_CurrentTty(0),
      m_WantedTty(~size_t(0)),
      m_NumTtys(0),
      m_ParentDir(parentDir),
      m_bSwitchingLocked(false) {
  for (size_t i = 0; i < MAX_VT; ++i) {
    m_Terminals[i].textio = nullptr;
    m_Terminals[i].file = nullptr;
#if THREADS
    m_Terminals[i].owner = ~size_t(0);
    m_Terminals[i].ownerThread = ~size_t(0);
#endif

    ByteSet(&m_Terminals[i].mode, 0, sizeof(m_Terminals[i].mode));
    m_Terminals[i].mode.mode = VT_AUTO;
    m_Terminals[i].systemMode = Text;
  }
}

VirtualTerminalManager::~VirtualTerminalManager() {
  // The DevFs directory owns textui and every ConsolePhysicalFile wrapper.
  // Secondary TextIO backends are not directory entries, so retire them
  // here while their workers and input callbacks can still drain.
  for (size_t i = 1; i < MAX_VT; ++i) {
    delete m_Terminals[i].textio;
    m_Terminals[i].textio = nullptr;
    m_Terminals[i].file = nullptr;
#if THREADS
    m_Terminals[i].owner = ~size_t(0);
    m_Terminals[i].ownerThread = ~size_t(0);
#endif
  }

  m_Terminals[0].textio = nullptr;
  m_Terminals[0].file = nullptr;
  m_pTty = nullptr;
}

bool VirtualTerminalManager::initialise() {
  // Create /dev/textui for the text-only UI device.
  m_pTty = new TextIO(String("textui"), g_pDevFs->getNextInode(), g_pDevFs, m_ParentDir);
  m_pTty->markPrimary();
  if (m_pTty->initialise(false)) {
    m_ParentDir->addEntry(m_pTty->getName(), m_pTty);
  } else {
    WARNING(
        "POSIX: no /dev/tty - VirtualTerminalManager failed to "
        "initialise.");
    g_pDevFs->revertInode();
    delete m_pTty;
    m_pTty = nullptr;

    return false;
  }

  // set up tty1
  ConsolePhysicalFile* pTty1 = new ConsolePhysicalFile(0, m_pTty, String("tty1"), g_pDevFs);
  m_ParentDir->addEntry(pTty1->getName(), pTty1);

  m_Terminals[0].textio = m_pTty;
  m_Terminals[0].file = pTty1;

  // create tty2-6 as non-overloaded TextIO instances
  for (size_t i = 1; i < 8; ++i) {
    String ttyname;
    ttyname.Format("tty%u", i + 1);

    TextIO* tio = new TextIO(ttyname, g_pDevFs->getNextInode(), g_pDevFs, m_ParentDir);
    if (tio->initialise(true)) {
      ConsolePhysicalFile* file = new ConsolePhysicalFile(i, tio, ttyname, g_pDevFs);
      m_ParentDir->addEntry(ttyname, file);

      m_Terminals[i].textio = tio;
      m_Terminals[i].file = file;

      // activate the terminal by performing an empty write, which will
      // ensure users switching to the terminal see a blank screen if
      // nothing has actually opened it - this is better than seeing the
      // previous tty's output...
      tio->writeStr("", 0);
    } else {
      WARNING("POSIX: failed to create " << ttyname);
      g_pDevFs->revertInode();
      delete tio;
    }
  }

  return true;
}

bool VirtualTerminalManager::isTerminal(size_t n) const {
  return n < MAX_VT && m_Terminals[n].textio;
}

void VirtualTerminalManager::switchTerminal(size_t n) {
  m_Terminals[m_CurrentTty].textio->unmarkPrimary();
  m_CurrentTty = n;
  m_WantedTty = ~size_t(0);
  if (m_Terminals[n].systemMode == Text) {
    if (Machine::instance().getNumVga()) {
      Machine::instance().getVga(0)->setLargestTextMode();
    }
    m_Terminals[n].textio->markPrimary();
  }
  if (!sendSignal(n, true) && m_Terminals[n].mode.mode == VT_PROCESS) {
    m_Terminals[n].mode.mode = VT_AUTO;
    m_Terminals[n].systemMode = Text;
    if (Machine::instance().getNumVga()) {
      Machine::instance().getVga(0)->setLargestTextMode();
    }
    m_Terminals[n].textio->markPrimary();
  }
}

bool VirtualTerminalManager::activate(size_t n) {
  LockGuard<Mutex> guard(m_Lock);
  if (!isTerminal(n) || m_bSwitchingLocked) {
    return false;
  }
  if (n == m_CurrentTty) {
    return true;
  }
  if (m_WantedTty != ~size_t(0)) {
    if (sendSignal(m_CurrentTty, false)) {
      return false;
    }
    m_WantedTty = ~size_t(0);
  }
  if (m_Terminals[m_CurrentTty].mode.mode == VT_PROCESS) {
    m_WantedTty = n;
    if (sendSignal(m_CurrentTty, false)) {
      return true;
    }
    // An exited controller cannot acknowledge release.
    m_Terminals[m_CurrentTty].mode.mode = VT_AUTO;
    m_Terminals[m_CurrentTty].systemMode = Text;
  }
  switchTerminal(n);
  return true;
}

bool VirtualTerminalManager::reportPermission(size_t n, SwitchPermission perm) {
  LockGuard<Mutex> guard(m_Lock);
  if (n != m_CurrentTty || m_WantedTty == ~size_t(0)) {
    return false;
  }
#if THREADS
  if (Processor::information().getCurrentThread()->getParent()->getId() != m_Terminals[n].owner) {
    return false;
  }
#endif
  if (perm == Disallowed) {
    m_WantedTty = ~size_t(0);
  } else {
    switchTerminal(m_WantedTty);
  }
  return true;
}

bool VirtualTerminalManager::acknowledgeAcquire(size_t n) {
  LockGuard<Mutex> guard(m_Lock);
  if (n != m_CurrentTty || m_Terminals[n].mode.mode != VT_PROCESS) {
    return false;
  }
#if THREADS
  return Processor::information().getCurrentThread()->getParent()->getId() == m_Terminals[n].owner;
#else
  return true;
#endif
}

size_t VirtualTerminalManager::openInactive() {
  LockGuard<Mutex> guard(m_Lock);
  for (size_t i = 0; i < MAX_VT; ++i) {
    if (m_Terminals[i].textio == nullptr) {
      NOTICE("VirtualTerminalManager: opening inactive VT #" << i);

      String ttyname;
      ttyname.Format("tty%u", i + 1);

      TextIO* tio = new TextIO(ttyname, g_pDevFs->getNextInode(), g_pDevFs, m_ParentDir);
      if (tio->initialise(true)) {
        ConsolePhysicalFile* file = new ConsolePhysicalFile(i, tio, ttyname, g_pDevFs);
        m_ParentDir->addEntry(tio->getName(), file);

        m_Terminals[i].textio = tio;
        m_Terminals[i].file = file;

        // activate the terminal by performing an empty write, which
        // will ensure users switching to the terminal see a blank
        // screen if nothing has actually opened it - this is better
        // than seeing the previous tty's output...
        tio->writeStr("", 0);

        return i;
      } else {
        WARNING("POSIX: failed to create " << ttyname);
        g_pDevFs->revertInode();
        delete tio;
      }
    }
  }

  return ~0;
}

void VirtualTerminalManager::lockSwitching(bool locked) {
  LockGuard<Mutex> guard(m_Lock);
  m_bSwitchingLocked = locked;
}

size_t VirtualTerminalManager::getCurrentTerminalNumber() const {
  return m_CurrentTty;
}

TextIO* VirtualTerminalManager::getCurrentTerminal() const {
  return m_Terminals[m_CurrentTty].textio;
}

File* VirtualTerminalManager::getCurrentTerminalFile() const {
  return m_Terminals[m_CurrentTty].file;
}

struct vt_mode VirtualTerminalManager::getTerminalMode(size_t n) const {
  LockGuard<Mutex> guard(m_Lock);
  return m_Terminals[n].mode;
}

bool VirtualTerminalManager::setTerminalMode(size_t n, struct vt_mode mode) {
  LockGuard<Mutex> guard(m_Lock);
  if (!isTerminal(n) || (mode.mode != VT_AUTO && mode.mode != VT_PROCESS) ||
      (mode.mode == VT_PROCESS &&
       (mode.relsig <= 0 || mode.acqsig <= 0 ||
        static_cast<size_t>(mode.relsig) > PosixSubsystem::MaximumSupportedSignal ||
        static_cast<size_t>(mode.acqsig) > PosixSubsystem::MaximumSupportedSignal))) {
    return false;
  }
  m_Terminals[n].mode = mode;
#if THREADS
  Thread* thread = Processor::information().getCurrentThread();
  m_Terminals[n].owner = mode.mode == VT_PROCESS ? thread->getParent()->getId() : ~size_t(0);
  m_Terminals[n].ownerThread = mode.mode == VT_PROCESS ? thread->getId() : ~size_t(0);
#endif
  if (n == m_CurrentTty && mode.mode == VT_AUTO && m_WantedTty != ~size_t(0)) {
    switchTerminal(m_WantedTty);
  }
  return true;
}

struct vt_stat VirtualTerminalManager::getState() const {
  LockGuard<Mutex> guard(m_Lock);
  struct vt_stat state;

  state.v_active = m_CurrentTty + 1;
  state.v_signal = 0;
  state.v_state = 1;  // VT 0 == current, always
  for (size_t i = 0; i < 15; ++i) {
    if (m_Terminals[i].textio != nullptr) {
      state.v_state |= (1 << (i + 1));
    }
  }

  NOTICE("getState:");
  NOTICE(" -> active = " << state.v_active);
  NOTICE(" -> state = " << Hex << state.v_state);

  return state;
}

void VirtualTerminalManager::setSystemMode(size_t n, SystemMode mode) {
  LockGuard<Mutex> guard(m_Lock);
  m_Terminals[n].systemMode = mode;
  if (n != m_CurrentTty) {
    return;
  }
  if (mode == Graphics) {
    m_Terminals[n].textio->unmarkPrimary();
  } else {
    if (Machine::instance().getNumVga()) {
      Machine::instance().getVga(0)->setLargestTextMode();
    }
    m_Terminals[n].textio->markPrimary();
  }
}

VirtualTerminalManager::SystemMode VirtualTerminalManager::getSystemMode(size_t n) const {
  LockGuard<Mutex> guard(m_Lock);
  return m_Terminals[n].systemMode;
}

void VirtualTerminalManager::setInputMode(size_t n, TextIO::InputMode newMode) {
  if (!m_Terminals[n].textio) {
    NOTICE("VirtualTerminalManager: can't set mode of VT #" << n << " as it is inactive");
    return;
  }

  m_Terminals[n].textio->setMode(newMode);
}

TextIO::InputMode VirtualTerminalManager::getInputMode(size_t n) const {
  if (!m_Terminals[n].textio) {
    NOTICE("VirtualTerminalManager: can't get mode of VT #" << n << " as it is inactive");
    return TextIO::Standard;
  }

  return m_Terminals[n].textio->getMode();
}

bool VirtualTerminalManager::sendSignal(size_t n, bool acq) {
  auto& terminal = m_Terminals[n];
  if (terminal.mode.mode != VT_PROCESS) {
    return false;
  }
#if THREADS
  Scheduler::ProcessLease process;
  if (!Scheduler::instance().acquireProcessById(process, terminal.owner) ||
      process->getState() >= Process::Terminating) {
    return false;
  }
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  Process::ThreadLease target;
  if (!subsystem || !process->acquireThreadById(target, terminal.ownerThread) ||
      !target->acceptingEvents()) {
    return false;
  }
  // The controller masks these signals while presenting. Another thread must
  // not acknowledge release while that presentation is still writing scanout.
  subsystem->sendSignal(target.get(), acq ? terminal.mode.acqsig : terminal.mode.relsig, false,
                        false);
  return true;
#else
  return false;
#endif
}
