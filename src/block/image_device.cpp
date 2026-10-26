// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/image_device.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "xfs/perf_stats.h"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xfs {
namespace {

#ifdef _WIN32

std::string WinErrorString(DWORD code) {
  LPSTR buffer = nullptr;
  DWORD len = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string message;
  if (len && buffer) {
    message.assign(buffer, len);
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' ||
            message.back() == ' ')) {
      message.pop_back();
    }
  } else {
    message = "Windows error " + std::to_string(code);
  }
  if (buffer) LocalFree(buffer);
  return " (error " + std::to_string(code) + ": " + message + ")";
}

// A per-thread manual-reset event, reused by positioned transfers. The image
// handle is opened with FILE_FLAG_OVERLAPPED, so every request must carry its
// own OVERLAPPED structure and event: this is what allows independent ranges to
// be in flight at the same time instead of sharing the synchronous file
// pointer.
struct ThreadEvent {
  HANDLE handle = INVALID_HANDLE_VALUE;
  ThreadEvent() { handle = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~ThreadEvent() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};

HANDLE TransferEvent() {
  static thread_local ThreadEvent event;
  return event.handle;
}

// One raw positioned transfer. Returns the number of bytes transferred; on
// failure returns 0 and fills `error`. A short transfer is not an error here:
// the caller loops until the request is complete.
template <bool kWrite, typename T>
size_t PositionedTransfer(HANDLE handle, uint64_t offset, T buffer, size_t size,
                          std::string* error) {
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffu);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  HANDLE event = TransferEvent();
  if (event != INVALID_HANDLE_VALUE) {
    ResetEvent(event);
    overlapped.hEvent = event;
  }
  constexpr size_t kMaxTransfer = 16u * 1024 * 1024;
  DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, kMaxTransfer));
  DWORD transferred = 0;
  BOOL ok;
  if constexpr (kWrite) {
    ok = WriteFile(handle, buffer, chunk, &transferred, &overlapped);
  } else {
    ok = ReadFile(handle, buffer, chunk, &transferred, &overlapped);
  }
  if (!ok) {
    DWORD code = GetLastError();
    if (code == ERROR_IO_PENDING) {
      if (!GetOverlappedResult(handle, &overlapped, &transferred, TRUE)) {
        code = GetLastError();
        if constexpr (!kWrite) {
          if (code == ERROR_HANDLE_EOF) return 0;
        }
        if (error) *error = "image I/O failed" + WinErrorString(code);
        return 0;
      }
    } else {
      // A read that runs into the end of the file reports ERROR_HANDLE_EOF
      // instead of returning a short count; both mean "short transfer" to the
      // caller, which then fails the request with an explicit message.
      if constexpr (!kWrite) {
        if (code == ERROR_HANDLE_EOF) return 0;
      }
      if (error) *error = "image I/O failed" + WinErrorString(code);
      return 0;
    }
  }
  if (transferred == 0) {
    if (error) *error = "image I/O made no progress";
    return 0;
  }
  return transferred;
}

#else

std::string ErrnoString(int code) {
  return " (errno " + std::to_string(code) + ": " + std::strerror(code) + ")";
}

#endif  // _WIN32

}  // namespace

struct ImageBlockDevice::Impl {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
#else
  int fd = -1;
#endif
};

ImageBlockDevice::ImageBlockDevice() : impl_(std::make_unique<Impl>()) {}

ImageBlockDevice::~ImageBlockDevice() { Close(); }

std::unique_ptr<ImageBlockDevice> ImageBlockDevice::Open(
    const std::filesystem::path& path, bool writable, uint32_t sector_size,
    std::string* error) {
  std::error_code ec;
  uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    if (error) *error = "cannot stat " + path.string() + ": " + ec.message();
    return nullptr;
  }

  auto dev = std::unique_ptr<ImageBlockDevice>(new ImageBlockDevice());
  dev->path_ = path;
#ifdef _WIN32
  // Sharing rules: a writable image is opened with FILE_SHARE_READ only, so
  // two read-write mounts can never race on the same file and no external
  // writer can modify it behind the mounted filesystem's back. Read-only opens
  // tolerate other readers and a writer.
  const DWORD share =
      writable ? FILE_SHARE_READ : (FILE_SHARE_READ | FILE_SHARE_WRITE);
  dev->impl_->handle = CreateFileW(
      path.c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0), share,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (dev->impl_->handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "cannot open " + path.string() +
               WinErrorString(GetLastError());
    }
    return nullptr;
  }
#else
  const int flags = writable ? O_RDWR : O_RDONLY;
  dev->impl_->fd = ::open(path.c_str(), flags);
  if (dev->impl_->fd < 0) {
    if (error) *error = "cannot open " + path.string() + ErrnoString(errno);
    return nullptr;
  }
#endif

  dev->info_.size_bytes = size;
  dev->info_.logical_sector_size = sector_size;
  dev->info_.physical_sector_size = sector_size;
  dev->info_.writable = writable;
  dev->info_.is_raw_device = false;
  dev->info_.description = path.string();
  return dev;
}

std::unique_ptr<ImageBlockDevice> ImageBlockDevice::Create(
    const std::filesystem::path& path, uint64_t size_bytes,
    uint32_t sector_size, std::string* error) {
  if (size_bytes == 0) {
    if (error) *error = "image size must not be zero";
    return nullptr;
  }

  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::filesystem::remove(path, ec);
    if (ec) {
      if (error) *error = "cannot remove existing " + path.string() + ": " +
                          ec.message();
      return nullptr;
    }
  }

  auto dev = std::unique_ptr<ImageBlockDevice>(new ImageBlockDevice());
  dev->path_ = path;
#ifdef _WIN32
  dev->impl_->handle = CreateFileW(
      path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (dev->impl_->handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "cannot create " + path.string() +
               WinErrorString(GetLastError());
    }
    return nullptr;
  }
  // Best effort: keep the image sparse so test images do not consume their
  // nominal size. Failure only means the file is fully allocated.
  DWORD returned = 0;
  DeviceIoControl(dev->impl_->handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                  &returned, nullptr);
  LARGE_INTEGER distance{};
  distance.QuadPart = static_cast<LONGLONG>(size_bytes);
  if (!SetFilePointerEx(dev->impl_->handle, distance, nullptr, FILE_BEGIN) ||
      !SetEndOfFile(dev->impl_->handle) ||
      !SetFilePointerEx(dev->impl_->handle, LARGE_INTEGER{}, nullptr,
                        FILE_BEGIN)) {
    if (error) {
      *error = "cannot size " + path.string() + WinErrorString(GetLastError());
    }
    dev->Close();
    std::filesystem::remove(path, ec);
    return nullptr;
  }
#else
  dev->impl_->fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC,
                          0644);
  if (dev->impl_->fd < 0) {
    if (error) *error = "cannot create " + path.string() + ErrnoString(errno);
    return nullptr;
  }
  if (::ftruncate(dev->impl_->fd, static_cast<off_t>(size_bytes)) != 0) {
    if (error) {
      *error = "cannot size " + path.string() + ErrnoString(errno);
    }
    dev->Close();
    std::filesystem::remove(path, ec);
    return nullptr;
  }
#endif

  dev->info_.size_bytes = size_bytes;
  dev->info_.logical_sector_size = sector_size;
  dev->info_.physical_sector_size = sector_size;
  dev->info_.writable = true;
  dev->info_.is_raw_device = false;
  dev->info_.description = path.string();
  return dev;
}

bool ImageBlockDevice::Read(uint64_t offset, void* buffer, size_t size) {
  if (size == 0) return true;
  PerfStats& stats = PerfStats::Get();
  const bool perf = stats.enabled();
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mutex_);
  if (offset > info_.size_bytes || size > info_.size_bytes - offset) {
    last_error_ = "read out of bounds";
    return false;
  }
  auto* out = static_cast<uint8_t*>(buffer);
  uint64_t pos = offset;
  size_t remaining = size;
  while (remaining > 0) {
#ifdef _WIN32
    if (impl_->handle == INVALID_HANDLE_VALUE) {
      last_error_ = "image not open";
      return false;
    }
    std::string error;
    size_t got = PositionedTransfer<false>(impl_->handle, pos, out, remaining,
                                           &error);
    if (got == 0) {
      last_error_ = error.empty() ? "image read made no progress" : error;
      return false;
    }
#else
    if (impl_->fd < 0) {
      last_error_ = "image not open";
      return false;
    }
    ssize_t got;
    do {
      got = ::pread(impl_->fd, out, remaining, static_cast<off_t>(pos));
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
      last_error_ = "image read failed" + ErrnoString(errno);
      return false;
    }
    if (got == 0) {
      last_error_ = "image short read";
      return false;
    }
#endif
    out += got;
    pos += got;
    remaining -= got;
  }
  if (perf) {
    stats.device_read_calls.fetch_add(1, std::memory_order_relaxed);
    stats.device_read_bytes.fetch_add(size, std::memory_order_relaxed);
  }
  return true;
}

bool ImageBlockDevice::Write(uint64_t offset, const void* buffer, size_t size) {
  if (size == 0) return true;
  PerfStats& stats = PerfStats::Get();
  const bool perf = stats.enabled();
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mutex_);
  std::shared_lock<std::shared_mutex> barrier(flush_barrier_);
  if (!info_.writable) {
    last_error_ = "image opened read-only";
    return false;
  }
  if (offset > info_.size_bytes || size > info_.size_bytes - offset) {
    last_error_ = "write out of bounds";
    return false;
  }
  const auto* in = static_cast<const uint8_t*>(buffer);
  uint64_t pos = offset;
  size_t remaining = size;
  while (remaining > 0) {
#ifdef _WIN32
    if (impl_->handle == INVALID_HANDLE_VALUE) {
      last_error_ = "image not open";
      return false;
    }
    std::string error;
    size_t put = PositionedTransfer<true>(impl_->handle, pos, in, remaining,
                                          &error);
    if (put == 0) {
      last_error_ = error.empty() ? "image write made no progress" : error;
      return false;
    }
#else
    if (impl_->fd < 0) {
      last_error_ = "image not open";
      return false;
    }
    ssize_t put;
    do {
      put = ::pwrite(impl_->fd, in, remaining, static_cast<off_t>(pos));
    } while (put < 0 && errno == EINTR);
    if (put < 0) {
      last_error_ = "image write failed" + ErrnoString(errno);
      return false;
    }
    if (put == 0) {
      last_error_ = "image write made no progress";
      return false;
    }
#endif
    in += put;
    pos += put;
    remaining -= put;
  }
  if (perf) {
    stats.device_write_calls.fetch_add(1, std::memory_order_relaxed);
    stats.device_write_bytes.fetch_add(size, std::memory_order_relaxed);
  }
  return true;
}

bool ImageBlockDevice::Flush() {
  PerfStats& stats = PerfStats::Get();
  const bool perf = stats.enabled();
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mutex_);
  // A flush is a durability barrier: it waits for every write that started
  // before it. Reads are not part of the barrier and may continue.
  std::unique_lock<std::shared_mutex> barrier(flush_barrier_);
#ifdef _WIN32
  if (impl_->handle == INVALID_HANDLE_VALUE) {
    last_error_ = "image not open";
    return false;
  }
  // Nothing can be dirty on a read-only image; flushing it would fail with
  // ACCESS_DENIED and turn a successful read-only session into an error.
  if (info_.writable && !FlushFileBuffers(impl_->handle)) {
    last_error_ = "FlushFileBuffers failed" + WinErrorString(GetLastError());
    return false;
  }
#else
  if (impl_->fd < 0) {
    last_error_ = "image not open";
    return false;
  }
  if (info_.writable) {
    int rc;
    do {
      rc = ::fsync(impl_->fd);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
      last_error_ = "image flush failed" + ErrnoString(errno);
      return false;
    }
  }
#endif
  if (perf) {
    stats.device_flush_calls.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

void ImageBlockDevice::Close() {
  std::unique_lock<std::shared_mutex> lifecycle(lifecycle_mutex_);
#ifdef _WIN32
  if (impl_ && impl_->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->handle);
    impl_->handle = INVALID_HANDLE_VALUE;
  }
#else
  if (impl_ && impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
#endif
}

}  // namespace xfs
