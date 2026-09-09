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

#include "pedigree/kernel/Version.h"
#include "pedigree/kernel/debugger/commands/CpuInfoCommand.h"
#include "pedigree/kernel/processor/Processor.h"

#if X86_COMMON && !HOSTED
#include "pedigree/kernel/processor/x86_common/Processor.h"
#endif

namespace {
void drawFooter(DebuggerIO* screen) {
  NormalStaticString footer("j: up  k: down  backspace: page up  space: page down  q: quit");
  footer.truncate(screen->getWidth());
  screen->drawString(footer, screen->getHeight() - 1, 0, DebuggerIO::White, DebuggerIO::Green);
}
}  // namespace

CpuInfoCommand::CpuInfoCommand() : DebuggerCommand(), Scrollable(), m_Lines(), m_LineCount(0) {}

CpuInfoCommand::~CpuInfoCommand() {}

void CpuInfoCommand::autocomplete(const HugeStaticString& input, HugeStaticString& output) {}

bool CpuInfoCommand::execute(const HugeStaticString& input, HugeStaticString& output,
                             InterruptState& state, DebuggerIO* screen) {
  m_LineCount = 0;
  m_Lines[m_LineCount++] = "CPU information";

#if X86_COMMON && !HOSTED
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
  X86CommonProcessor::cpuid(0, 0, eax, ebx, ecx, edx);
  const uint32_t maxLeaf = eax;
  char vendor[13];
  MemoryCopy(vendor, &ebx, sizeof(ebx));
  MemoryCopy(vendor + 4, &edx, sizeof(edx));
  MemoryCopy(vendor + 8, &ecx, sizeof(ecx));
  vendor[12] = '\0';
  m_Lines[m_LineCount++] = "Vendor: ";
  m_Lines[m_LineCount - 1] += vendor;

  if (maxLeaf >= 1) {
    X86CommonProcessor::cpuid(1, 0, eax, ebx, ecx, edx);
    const uint32_t baseFamily = (eax >> 8) & 0xf;
    const uint32_t baseModel = (eax >> 4) & 0xf;
    const uint32_t family = baseFamily == 0xf ? baseFamily + ((eax >> 20) & 0xff) : baseFamily;
    const uint32_t model = (baseFamily == 0x6 || baseFamily == 0xf)
                               ? baseModel | (((eax >> 16) & 0xf) << 4)
                               : baseModel;
    m_Lines[m_LineCount++] = "Family/model/stepping: ";
    m_Lines[m_LineCount - 1].append(family, 10);
    m_Lines[m_LineCount - 1] += "/";
    m_Lines[m_LineCount - 1].append(model, 10);
    m_Lines[m_LineCount - 1] += "/";
    m_Lines[m_LineCount - 1].append(eax & 0xf, 10);
    m_Lines[m_LineCount++] = "Logical CPUs reported: ";
    m_Lines[m_LineCount - 1].append((ebx >> 16) & 0xff, 10);
    m_Lines[m_LineCount++] = "Features: ";
    if (edx & (1u << 25))
      m_Lines[m_LineCount - 1] += "SSE ";
    if (edx & (1u << 26))
      m_Lines[m_LineCount - 1] += "SSE2 ";
    if (ecx & (1u << 0))
      m_Lines[m_LineCount - 1] += "SSE3 ";
    if (ecx & (1u << 9))
      m_Lines[m_LineCount - 1] += "SSSE3 ";
    if (ecx & (1u << 19))
      m_Lines[m_LineCount - 1] += "SSE4.1 ";
    if (ecx & (1u << 20))
      m_Lines[m_LineCount - 1] += "SSE4.2 ";
    if (ecx & (1u << 28))
      m_Lines[m_LineCount - 1] += "AVX ";
    if (edx & (1u << 28))
      m_Lines[m_LineCount - 1] += "HTT";
  }

  X86CommonProcessor::cpuid(0x80000000, 0, eax, ebx, ecx, edx);
  if (eax >= 0x80000004) {
    char brand[49];
    for (uint32_t leaf = 0; leaf < 3; ++leaf) {
      X86CommonProcessor::cpuid(0x80000002 + leaf, 0, eax, ebx, ecx, edx);
      MemoryCopy(brand + leaf * 16, &eax, sizeof(eax));
      MemoryCopy(brand + leaf * 16 + 4, &ebx, sizeof(ebx));
      MemoryCopy(brand + leaf * 16 + 8, &ecx, sizeof(ecx));
      MemoryCopy(brand + leaf * 16 + 12, &edx, sizeof(edx));
    }
    brand[48] = '\0';
    for (size_t i = 48; i && brand[i - 1] == ' '; --i)
      brand[i - 1] = '\0';
    m_Lines[m_LineCount++] = "Name: ";
    m_Lines[m_LineCount - 1] += brand;
  }
#else
  HugeStaticString identity;
  Processor::identify(identity);
  m_Lines[m_LineCount++] = "Processor: ";
  m_Lines[m_LineCount - 1] += identity;
#endif

  m_Lines[m_LineCount++] = "Online CPUs: ";
  m_Lines[m_LineCount - 1].append(Processor::getCount(), 10);
  m_Lines[m_LineCount++] = "Build revision: ";
  m_Lines[m_LineCount - 1] += g_pBuildRevision;
  m_Lines[m_LineCount++] = "Built by ";
  m_Lines[m_LineCount - 1] += g_pBuildUser;
  m_Lines[m_LineCount - 1] += " at ";
  m_Lines[m_LineCount - 1] += g_pBuildTime;
  m_Lines[m_LineCount - 1] += " on ";
  m_Lines[m_LineCount - 1] += g_pBuildMachine;
  m_Lines[m_LineCount++] = "Build target: ";
  m_Lines[m_LineCount - 1] += g_pBuildTarget;
  m_Lines[m_LineCount++] = "Build flags: ";
  m_Lines[m_LineCount - 1] += g_pBuildFlags;

  screen->disableCli();
  move(0, 1);
  resize(screen->getWidth(), screen->getHeight() - 2);
  setScrollKeys('j', 'k');
  screen->drawHorizontalLine(' ', 0, 0, screen->getWidth() - 1, DebuggerIO::White,
                             DebuggerIO::Green);
  screen->drawString("Pedigree debugger - CPU/build info", 0, 0, DebuggerIO::White,
                     DebuggerIO::Green);
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

const NormalStaticString CpuInfoCommand::getString() {
  return NormalStaticString("cpuinfo");
}

const char* CpuInfoCommand::getLine1(size_t index, DebuggerIO::Colour& colour,
                                     DebuggerIO::Colour& bgColour) {
  colour = DebuggerIO::White;
  bgColour = DebuggerIO::Black;
  return index < m_LineCount ? m_Lines[index] : nullptr;
}

const char* CpuInfoCommand::getLine2(size_t index, size_t& colOffset, DebuggerIO::Colour& colour,
                                     DebuggerIO::Colour& bgColour) {
  colOffset = 0;
  colour = DebuggerIO::Black;
  bgColour = DebuggerIO::Black;
  return "";
}

size_t CpuInfoCommand::getLineCount() {
  return m_LineCount;
}
