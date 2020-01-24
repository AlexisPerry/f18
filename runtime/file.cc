//===-- runtime/file.cc -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "file.h"
#include "magic-numbers.h"
#include "memory.h"
#include <errno.h>
#include <unistd.h>

namespace Fortran::runtime::io {

std::size_t OpenFile::Read(Offset at, char *buffer, std::size_t minBytes,
    std::size_t maxBytes, IoErrorHandler &handler) {
  LockHolder locked{lock_};
  if (maxBytes == 0 || !Seek(at, handler)) {
    return 0;
  }
  if (maxBytes < minBytes) {
    minBytes = maxBytes;
  }
  std::size_t got{0};
  while (got < minBytes) {
    auto chunk{::read(fd_, buffer + got, maxBytes - got)};
    if (chunk == 0) {
      handler.SignalEnd();
      break;
    }
    if (chunk < 0) {
      auto err{errno};
      if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
        handler.SignalError(err);
        break;
      }
    } else {
      position_ += chunk;
      got += chunk;
    }
  }
  return got;
}

std::size_t OpenFile::Write(
    Offset at, const char *buffer, std::size_t bytes, IoErrorHandler &handler) {
  LockHolder locked{lock_};
  if (bytes == 0 || !Seek(at, handler)) {
    return 0;
  }
  std::size_t put{0};
  while (put < bytes) {
    auto chunk{::write(fd_, buffer + put, bytes - put)};
    if (chunk >= 0) {
      position_ += chunk;
      put += chunk;
    } else {
      auto err{errno};
      if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
        handler.SignalError(err);
        break;
      }
    }
  }
  if (knownSize_ && position_ > *knownSize_) {
    knownSize_ = position_;
  }
  return put;
}

void OpenFile::Truncate(Offset at, IoErrorHandler &handler) {
  LockHolder locked{lock_};
  if (!knownSize_ || *knownSize_ != at) {
    if (::ftruncate(fd_, at) != 0) {
      handler.SignalError(errno);
    }
    knownSize_ = at;
  }
}

int OpenFile::ReadAsynchronously(
    Offset at, char *buffer, std::size_t bytes, IoErrorHandler &handler) {
  LockHolder locked{lock_};
  int iostat{0};
  for (std::size_t got{0}; got < bytes;) {
#if _XOPEN_SOURCE >= 500 || _POSIX_C_SOURCE >= 200809L
    auto chunk{::pread(fd_, buffer + got, bytes - got, at)};
#else
    auto chunk{RawSeek(at) ? ::read(fd_, buffer + got, bytes - got) : -1};
#endif
    if (chunk == 0) {
      iostat = FORTRAN_RUNTIME_IOSTAT_END;
      break;
    }
    if (chunk < 0) {
      auto err{errno};
      if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
        iostat = err;
        break;
      }
    } else {
      at += chunk;
      got += chunk;
    }
  }
  return PendingResult(handler, iostat);
}

int OpenFile::WriteAsynchronously(
    Offset at, const char *buffer, std::size_t bytes, IoErrorHandler &handler) {
  LockHolder locked{lock_};
  int iostat{0};
  for (std::size_t put{0}; put < bytes;) {
#if _XOPEN_SOURCE >= 500 || _POSIX_C_SOURCE >= 200809L
    auto chunk{::pwrite(fd_, buffer + put, bytes - put, at)};
#else
    auto chunk{RawSeek(at) ? ::write(fd_, buffer + put, bytes - put) : -1};
#endif
    if (chunk >= 0) {
      at += chunk;
      put += chunk;
    } else {
      auto err{errno};
      if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
        iostat = err;
        break;
      }
    }
  }
  return PendingResult(handler, iostat);
}

void OpenFile::Wait(int id, IoErrorHandler &handler) {
  Pending *p{nullptr};
  {
    LockHolder locked{lock_};
    Pending *prev{nullptr};
    for (p = pending_; p; prev = p, p = p->next) {
      if (p->id == id) {
        if (prev) {
          prev->next = p->next;
        } else {
          pending_ = p->next;
        }
        p->next = nullptr;
        break;
      }
    }
  }
  if (p) {
    handler.SignalError(p->ioStat);
    FreeMemory(p);
  }
}

void OpenFile::WaitAll(IoErrorHandler &handler) {
  while (true) {
    Pending *p{nullptr};
    {
      LockHolder locked{lock_};
      p = pending_;
      if (!p) {
        return;
      }
      pending_ = p->next;
      p->next = nullptr;
    }
    handler.SignalError(p->ioStat);
    FreeMemory(p);
  }
}

bool OpenFile::Seek(Offset at, IoErrorHandler &handler) {
  if (at == position_) {
    return true;
  } else if (RawSeek(at)) {
    position_ = at;
    return true;
  } else {
    handler.SignalError(errno);
    return false;
  }
}

bool OpenFile::RawSeek(Offset at) {
#ifdef _LARGEFILE64_SOURCE
  return ::lseek64(fd_, at, SEEK_SET) == 0;
#else
  return ::lseek(fd_, at, SEEK_SET) == 0;
#endif
}

int OpenFile::PendingResult(Terminator &terminator, int iostat) {
  int id{nextId_++};
  pending_ = &New<Pending>{}(terminator, id, pending_, iostat);
  return id;
}
}
