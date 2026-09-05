/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_DESCRIPTOR_READ_H
#define POSIX_DESCRIPTOR_READ_H

#include "pedigree/kernel/processor/types.h"

// A record is consumed only after its entire user copy succeeds, including
// when readv splits it across several user buffers.
using PosixDescriptorReadCopy = bool (*)(void* context, const void* data, size_t size);

#endif
