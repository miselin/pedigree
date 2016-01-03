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

#include "LogViewer.h"
#include <Log.h>
#include <DebuggerIO.h>

LogViewer::LogViewer()
 : DebuggerCommand(), Scrollable()
{
}

LogViewer::~LogViewer()
{
}

void LogViewer::autocomplete(const HugeStaticString &input, HugeStaticString &output)
{
}

bool LogViewer::execute(const HugeStaticString &input, HugeStaticString &output, InterruptState &state, DebuggerIO *pScreen)
{
  // Let's enter 'raw' screen mode.
  pScreen->disableCli();

  // Initialise the Scrollable class
  move(0, 1);
  resize(pScreen->getWidth(), pScreen->getHeight() - 2);
  setScrollKeys('j', 'k');

  // Clear the top status lines.
  pScreen->drawHorizontalLine(' ',
                              0,
                              0,
                              pScreen->getWidth() - 1,
                              DebuggerIO::White,
                              DebuggerIO::Green);
  
  // Write the correct text in the upper status line.
  pScreen->drawString("Pedigree debugger - Log viewer",
                      0,
                      0,
                      DebuggerIO::White,
                      DebuggerIO::Green);
  
  // Clear the bottom status lines.
  // TODO: If we use arrow keys and page up/down keys we actually can remove the status line
  //       because the interface is then intuitive enough imho.
  pScreen->drawHorizontalLine(' ',
                              pScreen->getHeight() - 1,
                              0,
                              pScreen->getWidth() - 1,
                              DebuggerIO::White,
                              DebuggerIO::Green);

  // Write some helper text in the lower status line.
  // TODO FIXME: Drawing this might screw the top status bar
  pScreen->drawString("j: Up one line. k: Down one line. backspace: Page up. space: Page down. q: Quit", 
                      pScreen->getHeight()-1, 0, DebuggerIO::White, DebuggerIO::Green);
  pScreen->drawString("j", pScreen->getHeight()-1, 0, DebuggerIO::Yellow, DebuggerIO::Green);
  pScreen->drawString("k", pScreen->getHeight()-1, 16, DebuggerIO::Yellow, DebuggerIO::Green);
  pScreen->drawString("backspace", pScreen->getHeight()-1, 34, DebuggerIO::Yellow, DebuggerIO::Green);
  pScreen->drawString("space", pScreen->getHeight()-1, 54, DebuggerIO::Yellow, DebuggerIO::Green);
  pScreen->drawString("q", pScreen->getHeight()-1, 72, DebuggerIO::Yellow, DebuggerIO::Green);

  // Main loop.
  bool bStop = false;
  while(!bStop)
  {
    refresh(pScreen);

    // Wait for input.
    char c = 0;
    while( !(c=pScreen->getChar()) )
      ;

    // TODO: Use arrow keys and page up/down someday
    if (c == 'j')
      scroll(-1);
    else if (c == 'k')
      scroll(1);
    else if (c == ' ')
      scroll(static_cast<ssize_t>(height()));
    else if (c == 0x08)
      scroll(-static_cast<ssize_t>(height()));
    else if (c == 'q')
      bStop = true;
  }

  // HACK:: Serial connections will fill the screen with the last background colour used.
  //        Here we write a space with black background so the CLI screen doesn't get filled
  //        by some random colour!
  pScreen->drawString(" ", 1, 0, DebuggerIO::White, DebuggerIO::Black);
  pScreen->enableCli();
  return true;
}

const char *LogViewer::getLine1(size_t index, DebuggerIO::Colour &colour, DebuggerIO::Colour &bgColour)
{
  Log::SeverityLevel level;
  static NormalStaticString Line;
  Line.clear();
  Line.append("[");

  Log &log = Log::instance();
  if (index < log.getStaticEntryCount())
  {
    const Log::StaticLogEntry &entry = log.getStaticEntry(index);
    Line.append(entry.timestamp, 10, 8, '0');
    level = entry.type;
  }
  else
  {
    const Log::DynamicLogEntry &entry = log.getDynamicEntry(index);
    Line.append(entry.timestamp, 10, 8, '0');
    level = entry.type;
  }

  Line.append("] ");

  colour = DebuggerIO::White;
  switch (level)
  {
    case Log::Debug:
      colour = DebuggerIO::LightBlue;
      break;
    case Log::Notice:
      colour = DebuggerIO::Green;
      break;
    case Log::Warning:
      colour = DebuggerIO::Yellow;
      break;
    case Log::Error:
      colour = DebuggerIO::Magenta;
      break;
    case Log::Fatal:
      colour = DebuggerIO::Red;
      break;
  }

  return Line;
}
const char *LogViewer::getLine2(size_t index, size_t &colOffset, DebuggerIO::Colour &colour, DebuggerIO::Colour &bgColour)
{
  Log &log = Log::instance();
  static LargeStaticString Line;
  Line.clear();

  if (index < log.getStaticEntryCount())
  {
    const Log::StaticLogEntry &entry = log.getStaticEntry(index);
    Line.append(entry.str);
  }
  else
  {
    const Log::DynamicLogEntry &entry = log.getDynamicEntry(index);
    Line.append(entry.str);
  }

  colour = DebuggerIO::White;
  colOffset = 11;
  return Line;
}
size_t LogViewer::getLineCount()
{
  Log &log = Log::instance();
  return log.getStaticEntryCount() + log.getDynamicEntryCount();
}
