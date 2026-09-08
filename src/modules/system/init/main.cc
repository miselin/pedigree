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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/core/BootIO.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include "modules/Module.h"
#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/ResolvedPath.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/VFS.h"
#if HOSTED
#include "pedigree/kernel/processor/hosted/smoke.h"
#endif
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/new"

#include <fcntl.h>

class File;

static Thread* g_pStage2Thread = 0;

static void error(const char* s) {
  extern BootIO bootIO;
  static HugeStaticString str;
  str += s;
  str += "\n";
  bootIO.write(str, BootIO::Red, BootIO::Black);
  str.clear();
}

static File* resolveBootPath(const String& name, ResolvedPath& result) {
  auto context =
      Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  FilesystemPathRef selected;
  VfsMountView::ResolveOptions options;
  if (!view || !context || !view->resolve(context, FilesystemPathRef(), name, options, selected))
    return nullptr;
  result.retain(selected);
  return result.get();
}

static int init_stage2(void* param) {
  EMIT_IF(HOSTED) {
    if (!HOSTED_SMOKE_TESTS) {
      extern void system_reset();
      NOTICE("Hosted build has no smoke-test command; shutting down.");
      system_reset();
      return 0;
    }

#if HOSTED
    if (g_HostedSmokeStage == HostedSmokeRoot) {
      extern void system_reset();
      NOTICE("HOSTED-SMOKE: root mounted");
      system_reset();
      return 0;
    }
#endif
  }

  bool tryingLinux = false;
  bool directHostedSmokeCommand = false;

  ResolvedPath executable;
  File* file = 0;

  String init_path;
#if HOSTED
  directHostedSmokeCommand = HOSTED_SMOKE_TESTS && ((g_HostedSmokeStage == HostedSmokeCommand) ||
                                                    (g_HostedSmokeStage == HostedSmokeShutdown));
#endif
  init_path.assign(directHostedSmokeCommand ? "/usr/bin/hosted-smoke-command" : "/usr/bin/init");
  NOTICE("Searching for userspace program at " << init_path);
  file = resolveBootPath(init_path, executable);
  if (!file && !directHostedSmokeCommand) {
    WARNING("Did not find " << init_path << ", trying for a Linux userspace...");
    init_path.assign("/sbin/init");
    tryingLinux = true;

    NOTICE("Searching for Linux init at " << init_path);
    file = resolveBootPath(init_path, executable);
  }

  if (!file) {
    error(directHostedSmokeCommand
              ? "failed to find hosted smoke command"
              : "failed to find init program (tried /usr/bin/init and /sbin/init)");
    return 1;
  }

  NOTICE("Found a userspace program at " << init_path);

  Vector<String> argv, env;
  argv.pushBack(init_path);

#if HOSTED
  if (!tryingLinux && HOSTED_SMOKE_TESTS) {
    switch (g_HostedSmokeStage) {
      case HostedSmokeInit:
        argv.pushBack(String("init"));
        break;
      case HostedSmokeCommand:
        argv.pushBack(String("command"));
        break;
      case HostedSmokeShutdown:
        argv.pushBack(String("shutdown"));
        break;
      default:
        break;
    }
  }
#endif

  if (tryingLinux) {
    // Jump to runlevel 5
    argv.pushBack(String("5"));
  }

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  Process::setInit(pProcess);

  NOTICE("Invoking userspace program at " << init_path);
  if (!pProcess->getSubsystem()->invoke(init_path.cstr(), argv, env)) {
    error("failed to load userspace program");
  }

  return 0;
}

static bool init() {
#if THREADS
  EMIT_IF(HOSTED) {
    if (!HOSTED_SMOKE_TESTS) {
      extern void system_reset();
      NOTICE("Hosted build has no smoke-test command; shutting down.");
      system_reset();
      return true;
    }
  }

  // Resolve the only fallible prerequisite before a PosixProcess constructor
  // registers its hardware IntervalTimer callback.
  ResolvedPath nullPath;
  auto context =
      Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  FilesystemPathRef selectedNull;
  VfsMountView::ResolveOptions options;
  if (!view || !context ||
      !view->resolve(context, FilesystemPathRef(), String("/dev/null"), options, selectedNull)) {
    error("/dev/null does not exist");
    return false;
  }

  nullPath.retain(selectedNull);

  // Create a new process for the init process.
  PosixProcess* pProcess =
      new PosixProcess(Processor::information().getCurrentThread()->getParent());

  if (!pProcess || !pProcess->jobControlReady() || !pProcess->filesystemContextReady()) {
    delete pProcess;
    error("Unable to initialise the process session");
    return false;
  }

  Process* bootstrap = Processor::information().getCurrentThread()->getParent();
  if (!pProcess->installUserIdentity(bootstrap->getUser(), bootstrap->getGroup(), nullptr, 0)) {
    delete pProcess;
    error("Unable to initialise the process identity");
    return false;
  }

  pProcess->description() = "init";
  pProcess->setCttyContext(SharedPointer<Process::ControllingTerminal>());

  PosixSubsystem* pSubsystem = new PosixSubsystem;
  if (!pSubsystem) {
    delete pProcess;
    error("Unable to initialise the process subsystem");
    return false;
  }
  pProcess->setSubsystem(pSubsystem);

  // add an empty stdout, stdin
  FileDescriptor* stdinDescriptor = new FileDescriptor(nullPath.path(), 0, 0, 0, O_RDONLY);
  FileDescriptor* stdoutDescriptor = new FileDescriptor(nullPath.path(), 0, 1, 0, O_WRONLY);

  pSubsystem->addFileDescriptor(0, stdinDescriptor);
  pSubsystem->addFileDescriptor(1, stdoutDescriptor);

  UniquePointer<PreparedTraceTask> preparedTrace;
  if (pSubsystem->traceContext().prepareTask(preparedTrace) != TraceStatus::Success) {
    delete pProcess;
    error("Unable to initialise the process task identity");
    return false;
  }
  UtsRef initialUts;
  UniquePointer<PreparedUtsThread> preparedUts;
  if (!pSubsystem->namespaceContext() || !pSubsystem->namespaceContext()->valid() ||
      posix_uts_initial(initialUts) != UtsStatus::Success ||
      posix_uts_prepare_thread(initialUts, false, preparedUts) != UtsStatus::Success) {
    delete pProcess;
    error("Unable to initialise the process namespace");
    return false;
  }

  g_pStage2Thread = new Thread(pProcess, init_stage2, 0, 0, false, false, true);
  if (!g_pStage2Thread) {
    delete pProcess;
    error("Unable to initialise the process thread");
    return false;
  }
  pSubsystem->namespaceContext()->publishThread(preparedUts, *g_pStage2Thread, true);
  if (pSubsystem->traceContext().publishTask(preparedTrace, *g_pStage2Thread) !=
      TraceStatus::Success)
    FATAL("Initial trace task publication failed");
  g_pStage2Thread->setName("init");
  pProcess->publish();
  if (!g_pStage2Thread->start()) {
    FATAL("init: delayed initial thread could not be started.");
  }

  // wait for the other process to start before we move on with startup
  g_pStage2Thread->join();
#endif

  return true;
}

static void destroy() {}

#if X86_COMMON
#define __MOD_DEPS "vfs", "posix", "linker", "users"
#define __MOD_DEPS_OPT "gfx-deps", "mountroot", "confignics"
#else
#define __MOD_DEPS "vfs", "posix", "linker", "users"
#define __MOD_DEPS_OPT "mountroot", "confignics"
#endif
MODULE_INFO("init", &init, &destroy, __MOD_DEPS);
#ifdef __MOD_DEPS_OPT
MODULE_OPTIONAL_DEPENDS(__MOD_DEPS_OPT);
#endif
