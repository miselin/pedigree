/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_DESCRIPTOR_READ_H
#define POSIX_DESCRIPTOR_READ_H

#include "pedigree/kernel/processor/types.h"

// Copies one complete record, including scatter destinations. Each descriptor
// type owns its reservation and consumption rules around this callback.
using PosixDescriptorReadCopy = bool (*)(void* context, const void* data, size_t size);

#endif
