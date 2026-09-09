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
 * ANY DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MODULES_COMMAND_H
#define MODULES_COMMAND_H

#include "pedigree/kernel/debugger/DebuggerCommand.h"
#include "pedigree/kernel/debugger/Scrollable.h"
#include "pedigree/kernel/utilities/StaticString.h"

class ModulesCommand : public DebuggerCommand, public Scrollable {
 public:
  ModulesCommand();
  ~ModulesCommand();

  void autocomplete(const HugeStaticString& input, HugeStaticString& output);
  bool execute(const HugeStaticString& input, HugeStaticString& output, InterruptState& state,
               DebuggerIO* screen);
  const NormalStaticString getString();

  const char* getLine1(size_t index, DebuggerIO::Colour& colour,
                       DebuggerIO::Colour& bgColour);
  const char* getLine2(size_t index, size_t& colOffset, DebuggerIO::Colour& colour,
                       DebuggerIO::Colour& bgColour);
  size_t getLineCount();

 private:
  LargeStaticString m_Line;
};

#endif
