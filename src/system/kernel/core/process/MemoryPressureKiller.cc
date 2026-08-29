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
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/process/MemoryPressureKiller.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"

static size_t mb(size_t pages) {
  return (pages * TargetInfo::getPageSize()) / 0x100000;
}

bool MemoryPressureProcessKiller::compact() {
  Scheduler::ProcessLease candidate;
  for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i) {
    Scheduler::ProcessLease process;
    if (!Scheduler::instance().acquireProcess(process, i)) {
      continue;
    }

    // Requires a subsystem to kill.
    if (!process->getSubsystem())
      continue;

    if (!candidate)
      candidate = pedigree_std::move(process);
    else {
      if (process->getPhysicalPageCount() > candidate->getPhysicalPageCount()) {
        candidate = pedigree_std::move(process);
      }
    }
  }

  if (!candidate)
    return false;

  NOTICE_NOLOCK("MemoryPressureProcessKiller will kill pid=" << Dec << candidate->getId() << Hex);
  NOTICE_NOLOCK("virt=" << Dec << mb(candidate->getVirtualPageCount())
                        << "m phys=" << mb(candidate->getPhysicalPageCount())
                        << "m shared=" << mb(candidate->getSharedPageCount()) << "m" << Hex);

  // Hard kill the process (SIGKILL, in POSIX terms).
  // We cannot afford to let the thread do anything else.
  Subsystem* pSubsystem = candidate->getSubsystem();
  Process::ThreadLease target;
  if (!candidate->acquireThread(target, static_cast<size_t>(0))) {
    return false;
  }
  pSubsystem->kill(Subsystem::Unknown, target.get());

  // Success means a victim accepted termination, not that its pages are
  // already reclaimed. The pressure manager may make another pass later.
  return true;
}
