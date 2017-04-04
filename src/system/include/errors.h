/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef ERRORS_H
#define ERRORS_H

// This namespace is mapped to posix errno's where possible. Keep it that way!
namespace Error
{
  enum PosixError
  {
    NoError              =0,
    NotEnoughPermissions =1,  // EPERM
    DoesNotExist         =2,  // ENOENT
    NoSuchProcess        =3,  // ESRCH
    Interrupted          =4,  // EINTR
    IoError              =5,  // EIO
    NoSuchDevice         =6,  // ENXIO
    TooBig               =7,  // E2BIG
    ExecFormatError      =8,  // ENOEXEC
    BadFileDescriptor    =9,  // EBADF
    NoChildren           =10,  // ECHILD
    NoMoreProcesses      =11,  // EAGAIN
    OutOfMemory          =12,  // ENOMEM
    PermissionDenied     =13,  // EACCES
    BadAddress           =14,  // EFAULT
    DeviceBusy           =16,  // EBUSY
    FileExists           =17,  // EEXIST
    CrossDeviceLink      =18,  // EXDEV
    DeviceDoesNotExist   =19,  // ENODEV
    NotADirectory        =20,  // ENOTDIR
    IsADirectory         =21,  // EISDIR
    InvalidArgument      =22,  // EINVAL
    TooManyOpenFiles     =23,  // ENFILE
    NotAConsole          =25,  // ENOTTY
    FileTooLarge         =27,  // EFBIG
    NoSpaceLeftOnDevice  =28,  // ENOSPC
    IllegalSeek          =29,  // ESPIPE  (??)
    ReadOnlyFilesystem   =30,  // EROFS
    BrokenPipe           =32,  // EPIPE
    BadRange             =34,  // ERANGE
    Deadlock             =35,  // EDEADLK
    NameTooLong          =36,  // ENAMETOOLONG
    Unimplemented        =38,  // ENOSYS
    NotEmpty             =39,  // ENOTEMPTY
    LoopExists           =40,  // ELOOP
    ProtocolNotAvailable =92,  // ENOPROTOOPT
    OperationNotSupported=95,  // ENOTSUP
    ConnectionAborted    =103,  // ECONNABORTED
    IsConnected          =106,  // EISCONN
    TimedOut             =110,  // ETIMEDOUT
    ConnectionRefused    =111,  // ECONNREFUSED
    Already              =114,  // EALREADY
    InProgress           =115,  // EINPROGRESS
  };
}

#endif
