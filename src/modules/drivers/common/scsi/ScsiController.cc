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

#include "ScsiController.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/new"

#include "ScsiDisk.h"

ScsiController::ScsiController(Controller* pDev)
    : Controller(pDev), RequestQueue(MakeConstantString("ScsiController")) {
  initialise();
}

ScsiController::ScsiController() : RequestQueue(MakeConstantString("ScsiController")) {
  // Start the RequestQueue
  initialise();
}

ScsiController::~ScsiController() {
  // Known controllers close this before retiring their disk caches. Keep the
  // base teardown safe if a controller never reached that phase.
  m_DiskOperations.closeAndWait();
  RequestQueue::destroy();
}

bool ScsiController::acquireDiskOperation(OperationBarrier::Lease& operation) {
  return m_DiskOperations.tryAcquire(operation);
}

void ScsiController::shutdownDiskCaches() {
  // No new client may publish queue work or acquire cache ownership after
  // this point. Cache callbacks deliberately bypass this gate below.
  m_DiskOperations.closeAndWait();

  if (!RequestQueue::drain()) {
    FATAL("SCSI controller could not drain work before cache shutdown");
  }

  for (size_t i = 0; i < getNumChildren(); ++i) {
    static_cast<ScsiDisk*>(getChild(i))->shutdownCache();
  }

  if (!RequestQueue::drain()) {
    FATAL("SCSI controller cache shutdown left queued work behind");
  }
}

void ScsiController::searchDisks() {
  for (size_t i = 0; i < getNumUnits(); i++) {
    ScsiDisk* pDisk = new ScsiDisk();
    if (pDisk->initialise(this, i))
      addChild(pDisk);
    else
      delete pDisk;
  }
}

uint64_t ScsiController::executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
                                        uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8) {
  ScsiDisk* pDisk = reinterpret_cast<ScsiDisk*>(p2);
  if (p1 == SCSI_REQUEST_READ)
    return pDisk->doRead(p3);
  else if (p1 == SCSI_REQUEST_WRITE) {
    uint64_t result = pDisk->doWrite(p3);
    // write() transfers its cache pin to the request. Cancellation and
    // successful execution must release that ownership symmetrically.
    pDisk->unpin(p3);
    return result;
  } else if (p1 == SCSI_REQUEST_SYNC)
    return pDisk->doSync(p3);
  else if (p1 == SCSI_REQUEST_WRITE_DIRECT)
    return pDisk->doWriteDirect(p3, static_cast<uintptr_t>(p4));
  else
    return 0;
}

void ScsiController::cancelRequest(const Request& request) {
  if (request.p1 == SCSI_REQUEST_WRITE && request.p2) {
    ScsiDisk* disk = reinterpret_cast<ScsiDisk*>(request.p2);
    disk->unpin(request.p3);
  }
}
