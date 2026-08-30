/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESSOR_HOSTED_DARWINABI_H
#define PEDIGREE_KERNEL_PROCESSOR_HOSTED_DARWINABI_H
#include <config.h>

#ifdef __APPLE__
// Pedigree's x86-64 ELF ABI uses long for its exact-width 64-bit types. Keep
// public C++ names identical when the hosted kernel is compiled against the
// Darwin SDK, which otherwise uses long long for these typedefs.
#ifndef _INT64_T
#define _INT64_T
typedef signed long int64_t;
#endif

#ifndef _UINT64_T
#define _UINT64_T
typedef unsigned long uint64_t;
#endif
#endif

#endif
