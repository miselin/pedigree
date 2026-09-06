/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_NAMESPACE_FILE_H
#define POSIX_NAMESPACE_FILE_H
#include "modules/system/vfs/FileHandle.h"
#include "uts-namespace.h"
UtsStatus posix_uts_make_file(const UtsRef& space, RetainedFile& result);
bool posix_uts_file_namespace(File* file, UtsRef& result);
#endif
