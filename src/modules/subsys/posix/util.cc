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

#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/processor/Processor.h"

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"

#if UTILITY_LINUX
#include <mutex>
#include <vector>

std::vector<SharedPointer<FileDescriptor>> g_Descriptors;
std::mutex g_DescriptorsLock;

bool acquireDescriptor(int fd, DescriptorLease& descriptor) {
  descriptor.reset();
  std::lock_guard<std::mutex> guard(g_DescriptorsLock);
  if (fd < 0 || static_cast<size_t>(fd) >= g_Descriptors.size()) {
    return false;
  }

  descriptor.retain(g_Descriptors[fd]);
  return static_cast<bool>(descriptor);
}

void addDescriptor(int fd, FileDescriptor* f) {
  if (fd < 0) {
    delete f;
    return;
  }

  SharedPointer<FileDescriptor> replacement(f);
  SharedPointer<FileDescriptor> retiring;
  {
    std::lock_guard<std::mutex> guard(g_DescriptorsLock);
    if (static_cast<size_t>(fd) >= g_Descriptors.size()) {
      g_Descriptors.resize(fd + 1);
    }

    retiring = pedigree_std::move(g_Descriptors[fd]);
    g_Descriptors[fd] = pedigree_std::move(replacement);
  }
  retiring.reset();
}

bool removeDescriptor(int fd, const DescriptorLease& descriptor) {
  SharedPointer<FileDescriptor> retiring;
  {
    std::lock_guard<std::mutex> guard(g_DescriptorsLock);
    if (fd < 0 || static_cast<size_t>(fd) >= g_Descriptors.size() ||
        g_Descriptors[fd] != descriptor.m_Descriptor) {
      return false;
    }

    retiring = pedigree_std::move(g_Descriptors[fd]);
  }

  retiring.reset();
  return true;
}

size_t getAvailableDescriptor() {
  std::lock_guard<std::mutex> guard(g_DescriptorsLock);
  const size_t descriptor = g_Descriptors.size();
  g_Descriptors.resize(descriptor + 1);
  return descriptor;
}
#else
/// \todo move these into a common area, this code is duplicated EVERYWHERE
PosixSubsystem* getSubsystem() {
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return nullptr;
  }

  return pSubsystem;
}

bool acquireDescriptor(int fd, DescriptorLease& descriptor) {
  PosixSubsystem* pSubsystem = getSubsystem();
  if (!pSubsystem) {
    descriptor.reset();
    return false;
  }
  return pSubsystem->acquireFileDescriptor(fd, descriptor);
}

void addDescriptor(int fd, FileDescriptor* f) {
  PosixSubsystem* pSubsystem = getSubsystem();
  pSubsystem->addFileDescriptor(fd, f);
}

bool removeDescriptor(int fd, const DescriptorLease& descriptor) {
  PosixSubsystem* pSubsystem = getSubsystem();
  return pSubsystem && pSubsystem->closeFileDescriptor(fd, descriptor);
}

size_t getAvailableDescriptor() {
  PosixSubsystem* pSubsystem = getSubsystem();
  return pSubsystem->getFd();
}
#endif
