/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_FILE_METADATA_H
#define POSIX_FILE_METADATA_H
#include <sys/types.h>
class File;
struct stat;
// The caller retains the selected target for the entire operation.
bool posix_stat_file(const char* name, File* file, struct stat* output);
bool posix_chmod_file(File* file, mode_t mode);
bool posix_chown_file(File* file, uid_t owner, gid_t group);
#endif
