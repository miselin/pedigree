/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_FILESYSTEM_ACCESS_H
#define PEDIGREE_FILESYSTEM_ACCESS_H
#include "pedigree/kernel/processor/types.h"

namespace FilesystemAccess {
constexpr uint64_t Execute = 1ULL << 0, WriteFile = 1ULL << 1, ReadFile = 1ULL << 2,
                   ReadDir = 1ULL << 3, RemoveDir = 1ULL << 4, RemoveFile = 1ULL << 5,
                   MakeChar = 1ULL << 6, MakeDir = 1ULL << 7, MakeReg = 1ULL << 8,
                   MakeSock = 1ULL << 9, MakeFifo = 1ULL << 10, MakeBlock = 1ULL << 11,
                   MakeSym = 1ULL << 12, Refer = 1ULL << 13, Truncate = 1ULL << 14;
constexpr uint64_t All = (1ULL << 15) - 1;
}  // namespace FilesystemAccess
#endif
