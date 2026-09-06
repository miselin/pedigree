/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_ADVISORY_LOCK_SYSCALLS_H
#define POSIX_ADVISORY_LOCK_SYSCALLS_H

class DescriptorLease;
class PosixSubsystem;

int posix_advisory_flock(const DescriptorLease& descriptor, int operation);
int posix_advisory_fcntl(PosixSubsystem& subsystem, int fd, const DescriptorLease& descriptor,
                         int command, void* userLock);

#endif
