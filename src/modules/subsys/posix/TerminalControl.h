/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_TERMINALCONTROL_H
#define POSIX_TERMINALCONTROL_H

#include "modules/system/console/Console.h"

class PosixProcess;

class TerminalControl final : public ConsoleControlState {
 public:
  TerminalControl(size_t session, size_t foreground);
  bool active() const;
  void controlCharacter(Character character) override;
  static Mutex& lock();
  static int attach(ConsoleFile& console, bool steal, bool automatic = false,
                    const SharedPointer<ConsoleIoState>& opened = SharedPointer<ConsoleIoState>());
  static int setForeground(ConsoleFile& console, int group,
                           const SharedPointer<ConsoleIoState>& opened);
  static int foreground(ConsoleFile& console, const SharedPointer<ConsoleIoState>& opened);
  static int hangup();
  static void processTerminated(PosixProcess& process);

 private:
  void invalidate();
  bool m_Active;
  size_t m_Session;
  size_t m_Foreground;
};
#endif
