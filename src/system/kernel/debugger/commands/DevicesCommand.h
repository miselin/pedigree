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

#ifndef DEVICESCOMMAND_H
#define DEVICESCOMMAND_H

#include <DebuggerCommand.h>
#include <Scrollable.h>
#include <utilities/Vector.h>

class Device;

/** @addtogroup kerneldebuggercommands
 * @{ */

/**
 * Allows the tracing of an execution path, single stepping and continuing to breakpoints,
 * while displaying a disassembly, the target CPU state and a stack backtrace.
 */
class DevicesCommand : public DebuggerCommand
{
public:
  /**
   * Default constructor - zero's stuff.
   */
  DevicesCommand();

  /**
   * Default destructor - does nothing.
   */
  ~DevicesCommand();

  /**
   * Return an autocomplete string, given an input string.
   */
  void autocomplete(const HugeStaticString &input, HugeStaticString &output);
  
  /**
   * Execute the command with the given screen.
   */
  bool execute(const HugeStaticString &input, HugeStaticString &output, InterruptState &state, DebuggerIO *screen);
  
  /**
   * Returns the string representation of this command.
   */
  const NormalStaticString getString()
  {
    return NormalStaticString("devices");
  }
  
private:
  class DeviceTree : public Scrollable
  {
  public:
    DeviceTree();
    ~DeviceTree() {}
    const char *getLine1(size_t index, DebuggerIO::Colour &colour, DebuggerIO::Colour &bgColour);
    const char *getLine2(size_t index, size_t &colOffset, DebuggerIO::Colour &colour, DebuggerIO::Colour &bgColour);
    size_t getLineCount();
    Device *getDevForIndex(size_t index);
    size_t m_Line;
  private:
    void probeDev(Device *pDev);
    Vector<Device*> m_LinearTree;
  };
  
  class DeviceInfo : public Scrollable
  {
    public:
      DeviceInfo();
      ~DeviceInfo() {}
      void setDevice(Device *dev);
      Device *getDevice() {return m_pDev;}
      const char *getLine1(size_t index, DebuggerIO::Colour &colour, DebuggerIO::Colour &bgColour);
      const char *getLine2(size_t index, size_t &colOffset, DebuggerIO::Colour &colour, DebuggerIO::Colour &bgColour);
      size_t getLineCount();
    private:
      Device *m_pDev;
      DeviceInfo(const DeviceInfo &);
      void operator =(const DeviceInfo &);
  };
  
  void drawBackground(size_t nLines, DebuggerIO *pScreen);
};

/** @} */

#endif
