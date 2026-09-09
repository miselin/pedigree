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

#include "pedigree/kernel/debugger/commands/ModulesCommand.h"
#include "pedigree/kernel/linker/KernelElf.h"

namespace {
const char* statusName(Module::ModuleStatus status) {
  switch (status) {
    case Module::Preloaded:
      return "pending";
    case Module::Executing:
      return "executing";
    case Module::Active:
      return "active";
    case Module::Failed:
      return "failed";
    case Module::Unloading:
      return "unloading";
    case Module::Unloaded:
      return "unloaded";
    case Module::Unknown:
      return "unknown";
  }
  return "unknown";
}

void drawFooter(DebuggerIO* screen) {
  NormalStaticString footer("j: up  k: down  backspace: page up  space: page down  q: quit");
  footer.truncate(screen->getWidth());
  screen->drawString(footer, screen->getHeight() - 1, 0, DebuggerIO::White, DebuggerIO::Green);
}
}  // namespace

ModulesCommand::ModulesCommand() : DebuggerCommand(), Scrollable(), m_Line() {}

ModulesCommand::~ModulesCommand() {}

void ModulesCommand::autocomplete(const HugeStaticString& input, HugeStaticString& output) {}

bool ModulesCommand::execute(const HugeStaticString& input, HugeStaticString& output,
                             InterruptState& state, DebuggerIO* screen) {
  screen->disableCli();
  move(0, 1);
  resize(screen->getWidth(), screen->getHeight() - 2);
  setScrollKeys('j', 'k');

  screen->drawHorizontalLine(' ', 0, 0, screen->getWidth() - 1, DebuggerIO::White,
                             DebuggerIO::Green);
  screen->drawString("Pedigree debugger - Modules", 0, 0, DebuggerIO::White, DebuggerIO::Green);
  screen->drawHorizontalLine(' ', screen->getHeight() - 1, 0, screen->getWidth() - 1,
                             DebuggerIO::White, DebuggerIO::Green);
  drawFooter(screen);

  bool stop = false;
  while (!stop) {
    refresh(screen);
    char c = 0;
    while (!(c = screen->getChar()))
      ;

    if (c == 'j')
      scroll(-1);
    else if (c == 'k')
      scroll(1);
    else if (c == ' ')
      scroll(static_cast<ssize_t>(height()));
    else if (c == 0x08)
      scroll(-static_cast<ssize_t>(height()));
    else if (c == 'q')
      stop = true;
  }

  screen->drawString(" ", 1, 0, DebuggerIO::White, DebuggerIO::Black);
  screen->enableCli();
  return true;
}

const NormalStaticString ModulesCommand::getString() {
  return NormalStaticString("modules");
}

const char* ModulesCommand::getLine1(size_t index, DebuggerIO::Colour& colour,
                                     DebuggerIO::Colour& bgColour) {
  const Module* module = KernelElf::instance().getModule(index);
  m_Line.clear();
  if (!module) {
    return nullptr;
  }

  m_Line += "[";
  m_Line += statusName(module->status);
  m_Line += "] ";
  m_Line += module->name;
  m_Line += "  ";
  m_Line.append(module->loadBase, 16);
  m_Line += "-";
  m_Line.append(module->loadBase + module->loadSize, 16);
  m_Line += "  ";
  m_Line += module->unloadable ? "unloadable" : "pinned";
  if (!module->runtimeUnloadable) {
    m_Line += ", runtime-pinned";
  }

  colour = module->isFailed() ? DebuggerIO::LightRed : DebuggerIO::White;
  bgColour = DebuggerIO::Black;
  return m_Line;
}

const char* ModulesCommand::getLine2(size_t index, size_t& colOffset, DebuggerIO::Colour& colour,
                                     DebuggerIO::Colour& bgColour) {
  colOffset = 0;
  colour = DebuggerIO::Black;
  bgColour = DebuggerIO::Black;
  return "";
}

size_t ModulesCommand::getLineCount() {
  return KernelElf::instance().getModuleCount();
}
