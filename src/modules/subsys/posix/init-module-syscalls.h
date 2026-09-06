/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_INIT_MODULE_SYSCALLS_H
#define POSIX_INIT_MODULE_SYSCALLS_H
#include "pedigree/kernel/processor/types.h"
int posix_init_module(const void* image, size_t length, const char* parameters);
#endif
