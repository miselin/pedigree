/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_DESCRIPTOR_PATH_H
#define POSIX_DESCRIPTOR_PATH_H

#include "pedigree/kernel/utilities/SharedPointer.h"

#include <stddef.h>

class File;
class ProcFs;
class DevFs;
class PosixNamespaceContext;

File* posix_make_descriptor_directory(ProcFs& filesystem, File* parent, size_t pid,
                                      const SharedPointer<PosixNamespaceContext>& identity);
File* posix_make_dev_fd_link(DevFs& filesystem, File* parent);

#endif
