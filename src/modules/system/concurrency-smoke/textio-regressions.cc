/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"

#include "modules/system/console/TextIO.h"

namespace {
struct TextIoCreatorContext {
  explicit TextIoCreatorContext(TextIO* textio) : textio(textio), initialised(false) {}

  TextIO* textio;
  Atomic<bool> initialised;
};

int initialiseTextIo(void* parameter) {
  TextIoCreatorContext* context = reinterpret_cast<TextIoCreatorContext*>(parameter);
  context->initialised = context->textio->initialise(true);
  return 0;
}

int terminateProcess(void* parameter) {
  reinterpret_cast<Process*>(parameter)->kill();
}
}  // namespace

bool runTextIoFlipLifetimeRegression() {
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN textio-flip-kernel-owner");

  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  Process* creatorProcess = new Process(kernelProcess);
  TextIO* textio = new TextIO(String("textio-lifetime-probe"), 0, nullptr, nullptr);
  TextIoCreatorContext context(textio);

  Thread* creator =
      new Thread(creatorProcess, initialiseTextIo, &context, nullptr, false, true, true);
  creator->setName("TextIO disposable creator");
  if (!creator->start() || !creator->joinForCompletion() || !context.initialised) {
    FATAL("QEMU TextIO lifetime probe could not initialise its terminal");
  }

  // The terminal outlives the process which requested it. Its worker must not
  // join that process's exit rendezvous or become owned by its reaper.
  if (creatorProcess->getNumThreads() != 0) {
    delete textio;
    delete creatorProcess;
    FATAL("QEMU TextIO flip worker remained owned by its disposable creator process");
  }

  Thread* terminator =
      new Thread(creatorProcess, terminateProcess, creatorProcess, nullptr, false, true, true);
  terminator->setName("TextIO disposable process terminator");
  if (!terminator->start() || !creatorProcess->waitUntilTerminationReapable()) {
    FATAL("QEMU TextIO lifetime probe could not reap its disposable creator process");
  }
  delete creatorProcess;

  if (!textio->initialise(false)) {
    FATAL("QEMU TextIO lifetime probe could not restart its surviving flip worker");
  }
  textio->writeStr("", 0);
  delete textio;

  NOTICE("QEMU-CONCURRENCY-TEST: PASS textio-flip-kernel-owner");
  return true;
}
