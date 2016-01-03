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

#include <Log.h>
#include <Debugger.h>
#include <DebuggerIO.h>
#include <LocalIO.h>
#include <SerialIO.h>
#include <DisassembleCommand.h>
#include <QuitCommand.h>
#include <PanicCommand.h>
#include <BreakpointCommand.h>
#include <CpuInfoCommand.h>
#include <DumpCommand.h>
#include <LogViewer.h>
#include <Backtracer.h>
#include <AllocationCommand.h>
#include <SlamCommand.h>
#include <utilities/utility.h>
#include <StepCommand.h>
#include <TraceCommand.h>
#include <MemoryInspector.h>
#include <IoCommand.h>
#include <ThreadsCommand.h>
#include <DevicesCommand.h>
#include <SyscallTracerCommand.h>
#include <LookupCommand.h>
#include <HelpCommand.h>
#include <LocksCommand.h>
#include <MappingCommand.h>
#include <machine/Machine.h>
#include <graphics/GraphicsService.h>
#include <processor/InterruptManager.h>
#include <ServiceManager.h>
#include <machine/Display.h>

Debugger Debugger::m_Instance;

TraceCommand g_Trace;
/// Helper function. Returns the index of a command in pCommands that matches prefix. Starts searching
/// through pCommands at index start. Returns -1 if none found.
static int getCommandMatchingPrefix(char *prefix, DebuggerCommand **pCommands, size_t nCmds, size_t start)
{
  for (size_t i = start; i < nCmds; i++)
  {
    if (!strncmp(pCommands[i]->getString(), prefix, strlen(prefix)))
      return i;
  }
  return -1;
}

/// Helper function. Returns true if the string in pStr matches pCommand. If so, pStr is changed
/// so that on return, it contains the parameters to that command (everything after the first space).
static bool matchesCommand(char *pStr, DebuggerCommand *pCommand)
{
  if (!strncmp(pCommand->getString(), pStr, strlen(pCommand->getString())))
  {
    size_t n = strlen(pCommand->getString());
    memcpy(pStr, pStr+n+1, strlen(pStr)-n);
    return true;
  }
  else
  {
    return false;
  }
}

Debugger::Debugger() :
  m_pTempState(0), m_nIoType(DEBUGGER)
{
}

Debugger::~Debugger()
{
}

void Debugger::initialise()
{
#ifndef ARM_COMMON /// \todo Figure out a way of getting similar functionality on ARM
  if (!InterruptManager::instance().registerInterruptHandlerDebugger(InterruptManager::instance().getBreakpointInterruptNumber(), this))
    ERROR_NOLOCK("Debugger: breakpoint interrupt registration failed!");
  if (!InterruptManager::instance().registerInterruptHandlerDebugger(InterruptManager::instance().getDebugInterruptNumber(), this))
    ERROR_NOLOCK("Debugger: debug interrupt registration failed!");
#endif
}

/// \todo OZMFGBARBIE, this needs major cleanup. Look at the state of it!! :O
void Debugger::start(InterruptState &state, LargeStaticString &description)
{
  Log::instance() << " << Flushing log content >>" << Flush;
#if defined(VALGRIND) || defined(HAS_SANITIZERS)
  Processor::halt();
#endif
  static String graphicsService("graphics");
  
  // Drop out of whatever graphics mode we were in
  GraphicsService::GraphicsProvider provider;
  memset(&provider, 0, sizeof(provider));
  provider.bTextModes = true;
  
  ServiceFeatures *pFeatures = ServiceManager::instance().enumerateOperations(graphicsService);
  Service         *pService  = ServiceManager::instance().getService(graphicsService);
  bool bSuccess = false;
  if(pFeatures && pFeatures->provides(ServiceFeatures::probe))
    if(pService)
      bSuccess = pService->serve(ServiceFeatures::probe, reinterpret_cast<void*>(&provider), sizeof(provider));
  
  if(bSuccess && !provider.bTextModes)
    provider.pDisplay->setScreenMode(0);

  // We take a copy of the interrupt state here so that we can replace it with another thread's interrupt state should we
  // decide to switch threads.
#ifdef MULTIPROCESSOR
  Machine::instance().stopAllOtherProcessors();
#endif
  // The current thread, in case we decide to switch.
#if defined(THREADS)
  Thread *pThread = Processor::information().getCurrentThread();
#endif

  bool debugState = Machine::instance().getKeyboard()->getDebugState();
  Machine::instance().getKeyboard()->setDebugState(true);

  DebuggerIO *pInterfaces[2] = {0};
  
#ifndef DONT_LOG_TO_SERIAL
  static SerialIO serialIO(Machine::instance().getSerial(0));
  serialIO.initialise();
#endif

  /*
   * I/O implementations.
   */
  int nInterfaces = 0;
  if(Machine::instance().getNumVga()) // Not all machines have "VGA", so handle that
  {
    static LocalIO localIO(Machine::instance().getVga(0), Machine::instance().getKeyboard());
#ifdef DONT_LOG_TO_SERIAL
    pInterfaces[0] = &localIO;
    nInterfaces = 1;
#else
    pInterfaces[0] = &localIO;
    pInterfaces[1] = &serialIO;
    nInterfaces = 2;
#endif
  }
#ifndef DONT_LOG_TO_SERIAL
  else
  {
    pInterfaces[0] = &serialIO;
    nInterfaces = 1;
  }
#endif

  if(!nInterfaces)
  {
    // Oops, system doesn't support any output mechanisms!
    ERROR_NOLOCK("This machine/CPU combination doesn't support any output methods for the debugger!");
  }

  // IO interface.
  DebuggerIO *pIo = 0;
  int nChosenInterface = -1;

  // Commands.
  static DisassembleCommand disassembler;
  static LogViewer logViewer;
  static Backtracer backtracer;
  static QuitCommand quit;
  static BreakpointCommand breakpoint;
  static DumpCommand dump;
  static StepCommand step;
  static MemoryInspector memory;
  static PanicCommand panic;
  static CpuInfoCommand cpuInfo;
  static IoCommand io;
  static DevicesCommand devices;
  static SyscallTracerCommand syscallTracer;
  static LookupCommand lookup;
  static HelpCommand help;
  static MappingCommand mapping;

#if defined(THREADS)
  static ThreadsCommand threads;

  threads.setPointers(&pThread, &state);
#endif

#if defined(THREADS)
  size_t nCommands = 21;
#else
  size_t nCommands = 20;
#endif
  DebuggerCommand *pCommands[] = {&syscallTracer,
                                  &disassembler,
                                  &logViewer,
                                  &backtracer,
                                  &quit,
                                  &breakpoint,
                                  &dump,
                                  &step,
                                  &memory,
                                  &g_Trace,
                                  &panic,
                                  &cpuInfo,
                                  &devices,
#if defined(THREADS)
                                  &threads,
#endif
                                  &io,
                                  &g_AllocationCommand,
                                  &g_SlamCommand,
                                  &lookup,
                                  &help,
                                  &g_LocksCommand,
                                  &mapping};

  // Are we going to jump directly into the tracer? In which case bypass device detection.
  int n = g_Trace.execTrace();
  if (n == -1)
  {
    // Write a "Press any key..." message to each device, then poll each device.
    // The first one with data waiting becomes the active device, all others are locked out.
    for (int i = 0; i < nInterfaces; i++)
    {
      pInterfaces[i]->disableCli();
      pInterfaces[i]->drawString("Press any key to enter the debugger...", 0, 0, DebuggerIO::LightBlue, DebuggerIO::Black);
      NormalStaticString str;
      str += description;
      pInterfaces[i]->drawString(str, 2, 0, DebuggerIO::LightBlue, DebuggerIO::Black);
    }
    // Poll each device.
    while (pIo == 0)
      for (int i = 0; i < nInterfaces; i++)
      {
        char c = pInterfaces[i]->getCharNonBlock();
        if ((c >= 32 && static_cast<unsigned char>(c) <= 127) || c == '\n' ||
            c == 0x08 || c == '\r' || c == 0x09)
        {
          pIo = pInterfaces[i];
          nChosenInterface = i;
          break;
        }
      }
  }
  else
  {
    pIo = pInterfaces[n];
    nChosenInterface = n;
  }
  pIo->readDimensions();

  // Say sorry to the losers...
  for (int i = 0; i < nInterfaces; i++)
    if (pIo != pInterfaces[i])
      pInterfaces[i]->drawString("Locked by another device.", 1, 0, DebuggerIO::LightRed, DebuggerIO::Black);

  pIo->setCliUpperLimit(1); // Give us room for a status bar on top.
  pIo->setCliLowerLimit(1); // And a status bar on the bottom.
  pIo->enableCli(); // Start CLI mode.

  description += "\n";
  pIo->writeCli(description, DebuggerIO::Yellow, DebuggerIO::Black);

  description.clear();
  description += "Kernel heap ends at ";
  description.append(reinterpret_cast<uintptr_t>(VirtualAddressSpace::getKernelAddressSpace().m_HeapEnd), 16);
  description += "\n";
  pIo->writeCli(description, DebuggerIO::Yellow, DebuggerIO::Black);

  // Main CLI loop.
  bool bKeepGoing = false;
  do
  {
    HugeStaticString command;
    HugeStaticString output;
    // Should we jump directly in to the tracer?
    if (g_Trace.execTrace() != -1)
    {
      bKeepGoing = g_Trace.execute(command, output, state, pIo);
      continue;
    }
    else
      g_Trace.setInterface(nChosenInterface);
    // Clear the top and bottom status lines.
    pIo->drawHorizontalLine(' ', 0, 0, pIo->getWidth()-1, DebuggerIO::White, DebuggerIO::Green);
    pIo->drawHorizontalLine(' ', pIo->getHeight()-1, 0, pIo->getWidth()-1, DebuggerIO::White, DebuggerIO::Green);
    // Write the correct text in the upper status line.
    pIo->drawString("Pedigree debugger", 0, 0, DebuggerIO::White, DebuggerIO::Green);

    bool matchedCommand = false;
    DebuggerCommand *pAutoComplete = 0;
    while(1)
    {
      // Try and get a character from the CLI, passing in a buffer to populate and an
      // autocomplete command for if the user presses TAB (if one is defined).
      if (pIo->readCli(command, pAutoComplete))
        break; // Command complete, try and parse it.

      // The command wasn't complete - let's parse it and try and get an autocomplete string.
      HugeStaticString str;
      NormalStaticString str2;
      matchedCommand = false;
      for (size_t i = 0; i < nCommands; i++)
      {
        // TODO: This cast is completly wrong. As I said, don't touch (as in 'write') StaticString's
        //       internal string. The const is not there because I don't like you :-), it is there
        //       because directly writing is not garantueed to work (and it actually will break our code).
        if (matchesCommand(const_cast<char*>(static_cast<const char*>(command)), pCommands[i]))
        {
          str2 = static_cast<const char *>(pCommands[i]->getString());
          str2 += ' ';
          pCommands[i]->autocomplete(command, str);
          matchedCommand = true;
          break;
        }
      }

      pAutoComplete = 0;
      if (!matchedCommand)
      {
        int i = -1;
        while ( (i = getCommandMatchingPrefix(const_cast<char*>(static_cast<const char*>(command)), pCommands, nCommands, i+1)) != -1)
        {
          if (!pAutoComplete)
            pAutoComplete = pCommands[i];
          str += static_cast<const char*> (pCommands[i]->getString());
          str += " ";
        }
      }

      pIo->drawHorizontalLine(' ', pIo->getHeight()-1, 0, pIo->getWidth()-1, DebuggerIO::White, DebuggerIO::Green);
      pIo->drawString(str2, pIo->getHeight()-1, 0, DebuggerIO::Yellow, DebuggerIO::Green);
      pIo->drawString(str, pIo->getHeight()-1, str2.length(), DebuggerIO::White, DebuggerIO::Green);
    }

    // A command was entered.
    bool bValidCommand = false;
    for (size_t i = 0; i < nCommands; i++)
    {
      if (matchesCommand(const_cast<char*>(static_cast<const char*>(command)), pCommands[i]))
      {
        bKeepGoing = pCommands[i]->execute(command, output, state, pIo);
        pIo->writeCli(output, DebuggerIO::LightGrey, DebuggerIO::Black);
        bValidCommand = true;
      }
    }

    if (!bValidCommand)
    {
      pIo->writeCli("Unrecognised command.\n", DebuggerIO::LightGrey, DebuggerIO::Black);
      bKeepGoing = true;
    }

  }
  while (bKeepGoing);
  if(Machine::instance().getNumVga())
    pInterfaces[0]->destroy(); // Causes rememberMode to be called twice.
#ifndef DONT_LOG_TO_SERIAL
  serialIO.destroy();
#endif

  Machine::instance().getKeyboard()->setDebugState(debugState);
}

void Debugger::interrupt(size_t interruptNumber, InterruptState &state)
{
  LargeStaticString description;
  // We switch here on the interrupt number, and dispatch accordingly.
  if (interruptNumber == InterruptManager::instance().getBreakpointInterruptNumber())
  {
    // Here we check to see if the breakpoint was caused by an assertion, or a fatal error.
    if (state.getRegister(0) == ASSERT_FAILED_SENTINEL)
    {
      // As it's an assert or fatal, we assume state.getRegister(1) is a pointer to a descriptive string.
      const char *pDescription = reinterpret_cast<const char*> (state.getRegister(1));
      description += pDescription;
    }
    else
    {
      description += "Breakpoint exception.";
    }
    start(state, description);
  }
  else if (interruptNumber == InterruptManager::instance().getDebugInterruptNumber())
  {
    Processor::setSingleStep(false, state);
    description = "Debug/trap exception";
    start(state, description);
  }
}
