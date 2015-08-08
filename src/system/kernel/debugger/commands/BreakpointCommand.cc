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

#include "BreakpointCommand.h"
#include <processor/Processor.h>

class DebuggerIO;

BreakpointCommand::BreakpointCommand()
 : DebuggerCommand()
{
}

BreakpointCommand::~BreakpointCommand()
{
}

void BreakpointCommand::autocomplete(const HugeStaticString &input, HugeStaticString &output)
{
  output = "[ {0,1,2,3} {address,trigger,size,enable} [{parameter}] ]";
}

bool BreakpointCommand::execute(const HugeStaticString &input, HugeStaticString &output, InterruptState &state, DebuggerIO *pScreen)
{
  // Did we get any input?
  if (input == "breakpoint")
  {
    // Print out the current breakpoint status.
    output = "Current breakpoint status:\n";
    for(size_t i = 0; i < Processor::getDebugBreakpointCount(); i++)
    {
      DebugFlags::FaultType nFt;
      size_t nLen;
      bool bEnabled;
      uintptr_t nAddress = Processor::getDebugBreakpoint(i, nFt, nLen, bEnabled);
      
      const char *pFaultType = 0;
      switch (nFt)
      {
      case DebugFlags::InstructionFetch:
        pFaultType = "InstructionFetch";
        break;
      case DebugFlags::DataWrite:
        pFaultType = "DataWrite";
        break;
      case DebugFlags::IOReadWrite:
        pFaultType = "IOReadWrite";
        break;
      case DebugFlags::DataReadWrite:
        pFaultType = "DataReadWrite";
        break;
      }
      
      const char *pEnabled = "disabled";
      if (bEnabled) pEnabled = "enabled";
      output += i;
      output += ": 0x"; output.append(nAddress, 16, sizeof(uintptr_t) * 2,'0');
      output += " \t";  output.append(pFaultType);
      output += " \t";  output.append(nLen);
      output += " \t";  output.append(pEnabled);
      output += "\n";
    }
  }
  else
  {
    LargeStaticString inputCopy(input);
    // We expect a number.
    int32_t nBp = input.intValue();
    if (nBp < 0 || nBp >= static_cast<int32_t>(Processor::getDebugBreakpointCount()))
    {
      output = "Invalid breakpoint number.\n";
      return true;
    }
    // We expect a word - find the next space.
    bool bSpaceFound = false;
    for (size_t i = 0; i < inputCopy.length(); i++)
      if (inputCopy[i] == ' ')
      {
        inputCopy.stripFirst(i+1);
        bSpaceFound = true;
        break;
      }

    if (!bSpaceFound)
    {
      output = "Command not recognised\n";
      return true;
    }

    NormalStaticString command;
    // Find another space - end of command.
    bSpaceFound = false;
    for (size_t i = 0; i < inputCopy.length(); i++)
      if (inputCopy[i] == ' ')
      {
        command = inputCopy.left(i);
        inputCopy.stripFirst(i+1);
        bSpaceFound = true;
        break;
      }

    if (!bSpaceFound)
    {
      output = "Command not recognised\n";
      return true;
    }

    NormalStaticString argument;
    // Find another space - end of argument.
    bSpaceFound = false;
    for (size_t i = 0; i < inputCopy.length(); i++)
      if (inputCopy[i] == ' ')
      {
        argument = inputCopy.left(i);
        bSpaceFound = true;
        break;
      }
    if (!bSpaceFound)
      argument = inputCopy;
    
    if (argument.length() == 0)
    {
      output = "Parameter had zero length!\n";
      return true;
    }

    // Get the current breakpoint status.
    DebugFlags::FaultType nFaultType;
    size_t nLength;
    bool bEnabled;
    uintptr_t address = Processor::getDebugBreakpoint(nBp, nFaultType, nLength, bEnabled);

    if (command == "address")
    {
      address = argument.intValue();
      Processor::enableDebugBreakpoint(nBp, address, nFaultType, nLength);
    }
    else if (command == "trigger")
    {
    }
    else if (command == "enabled")
    {
      if (argument == "yes" || argument == "true")
      {
        Processor::enableDebugBreakpoint(nBp, address, nFaultType, nLength);
      }
      else
      {
        Processor::disableDebugBreakpoint(nBp);
      }
    }
    else
    {
      output = "Unrecognised command.\n";
      return true;
    }
    
  }

  return true;
}
