// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/image_device.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <cstdio>
#include <fstream>
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

// One raw positioned transfer. Returns the number of bytes transferred; on
// failure returns 0 and fills `error`. A short transfer is not an error here:
// the caller loops until the request is complete.
template <bool kWrite, typename T>
size_t PositionedTransfer(HANDLE handle, uint64_t offset, T buffer, size_t size,
                          std::string* error) {
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffu);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
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
    // A read that runs into the end of the file reports ERROR_HANDLE_EOF
    // instead of returning a short count; both mean "short transfer" to the
    // caller, which then fails the request with an explicit message.
    if constexpr (!kWrite) {
      if (code == ERROR_HANDLE_EOF) return 0;
    }
    if (error) *error = "image I/O failed" + WinErrorString(code);
    return 0;
  }
  if (transferred == 0) {
    if (error) *error = "image I/O made no progress";
    return 0;
  }
  return transferred;
}

#endif  // _WIN32

}  // namespace

struct ImageBlockDevice::Impl {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
#else
  std::fstream file;
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
  // two read-write mounts can never race on the same file. Read-only opens
  // tolerate other readers and a writer.
  const DWORD share =
      writable ? FILE_SHARE_READ : (FILE_SHARE_READ | FILE_SHARE_WRITE);
  dev->impl_->handle =
      CreateFileW(path.c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0),
                  share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (dev->impl_->handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "cannot open " + path.string() +
               WinErrorString(GetLastError());
    }
    return nullptr;
  }
#else
  std::ios::openmode mode = std::ios::binary | std::ios::in;
  if (writable) mode |= std::ios::out;
  dev->impl_->file.open(path, mode);
  if (!dev->impl_->file.is_open()) {
    if (error) *error = "cannot open " + path.string();
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
  dev->impl_->handle =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
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
  dev->impl_->file.open(path, std::ios::binary | std::ios::in |
                                  std::ios::out | std::ios::trunc);
  if (!dev->impl_->file.is_open()) {
    if (error) *error = "cannot create " + path.string();
    return nullptr;
  }
  // Size the file by writing at the last byte (sparse where supported).
  dev->impl_->file.seekp(static_cast<std::streamoff>(size_bytes - 1));
  const char zero = 0;
  dev->impl_->file.write(&zero, 1);
  dev->impl_->file.flush();
  if (!dev->impl_->file.good()) {
    if (error) *error = "cannot size " + path.string();
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
  std::shared_lock<std::shared_mutex> lock(io_mutex_);
  if (size == 0) return true;
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
    if (!impl_->file.is_open()) {
      last_error_ = "image not open";
      return false;
    }
    impl_->file.clear();
    impl_->file.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    impl_->file.read(reinterpret_cast<char*>(out),
                     static_cast<std::streamsize>(remaining));
    size_t got = static_cast<size_t>(impl_->file.gcount());
    if (!impl_->file || got == 0) {
      last_error_ = "image short read";
      return false;
    }
#endif
    out += got;
    pos += got;
    remaining -= got;
  }
  return true;
}

bool ImageBlockDevice::Write(uint64_t offset, const void* buffer, size_t size) {
  std::unique_lock<std::shared_mutex> lock(io_mutex_);
  if (!info_.writable) {
    last_error_ = "image opened read-only";
    return false;
  }
  if (size == 0) return true;
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
    if (!impl_->file.is_open()) {
      last_error_ = "image not open";
      return false;
    }
    impl_->file.clear();
    impl_->file.seekp(static_cast<std::streamoff>(pos), std::ios::beg);
    impl_->file.write(reinterpret_cast<const char*>(in),
                      static_cast<std::streamsize>(remaining));
    if (!impl_->file) {
      last_error_ = "image short write";
      return false;
    }
    size_t put = remaining;
#endif
    in += put;
    pos += put;
    remaining -= put;
  }
  return true;
}

bool ImageBlockDevice::Flush() {
  std::unique_lock<std::shared_mutex> lock(io_mutex_);
#ifdef _WIN32
  if (impl_->handle == INVALID_HANDLE_VALUE) {
    last_error_ = "image not open";
    return false;
  }
  // Nothing can be dirty on a read-only image; flushing it would fail with
  // ACCESS_DENIED and turn a successful read-only session into an error.
  if (!info_.writable) return true;
  if (!FlushFileBuffers(impl_->handle)) {
    last_error_ = "FlushFileBuffers failed" + WinErrorString(GetLastError());
    return false;
  }
  return true;
#else
  if (!impl_->file.is_open()) {
    last_error_ = "image not open";
    return false;
  }
  if (!info_.writable) return true;
  impl_->file.flush();
  if (!impl_->file.good()) {
    last_error_ = "image flush failed";
    return false;
  }
  return true;
#endif
}

void ImageBlockDevice::Close() {
  std::unique_lock<std::shared_mutex> lock(io_mutex_);
#ifdef _WIN32
  if (impl_ && impl_->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->handle);
    impl_->handle = INVALID_HANDLE_VALUE;
  }
#else
  if (impl_ && impl_->file.is_open()) impl_->file.close();
#endif
}

}  // namespace xfs
