/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/errors.h"

#include "Filesystem.h"
#include "Quota.h"

QuotaStatus Filesystem::quotaControl(const QuotaRequest&, QuotaResponse&, File*) {
  return QuotaStatus::Unsupported;
}

int quotaError(QuotaStatus status) {
  switch (status) {
    case QuotaStatus::Success:
      return 0;
    case QuotaStatus::Unsupported:
      return Error::OperationNotSupported;
    case QuotaStatus::Invalid:
      return Error::InvalidArgument;
    case QuotaStatus::NotEnabled:
      return Error::NoSuchProcess;
    case QuotaStatus::Busy:
      return Error::DeviceBusy;
    case QuotaStatus::ReadOnly:
      return Error::ReadOnlyFilesystem;
    case QuotaStatus::NoMemory:
      return Error::OutOfMemory;
    case QuotaStatus::IoError:
      return Error::IoError;
    case QuotaStatus::Limit:
      return Error::QuotaExceeded;
    case QuotaStatus::Overflow:
      return Error::ValueTooLarge;
    case QuotaStatus::Permission:
      return Error::NotEnoughPermissions;
    case QuotaStatus::NoSpace:
      return Error::NoSpaceLeftOnDevice;
    case QuotaStatus::TooLarge:
      return Error::FileTooLarge;
  }
  return Error::IoError;
}
