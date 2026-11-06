// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "winfsp/mount_host.h"

#ifdef XFSWIN_HAS_WINFSP

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <memory>

#include "winfsp/winfsp_adapter.h"
#include "xfs/deployment.h"
#include "xfs/filesystem.h"
#include "xfs/image_device.h"
#include "xfs/partition_device.h"
#include "xfs/windows_raw_device.h"

namespace xfs::winfsp {
namespace {

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), len);
  return out;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}

std::unique_ptr<BlockDevice> OpenTarget(const std::string& target,
                                        bool writable,
                                        const DeviceOpenPolicy* device_policy,
                                        std::string* error) {
  if (StartsWith(target, "\\\\") || StartsWith(target, "//")) {
    if (device_policy) {
      return OpenRawDevice(Utf8ToWide(target), *device_policy, error);
    }
    return OpenRawDevice(Utf8ToWide(target), /*writable=*/writable,
                        /*exclusive=*/writable, error);
  }
  const auto* p = reinterpret_cast<const char8_t*>(target.data());
  std::filesystem::path path = std::filesystem::path(std::u8string(p, target.size()));
  std::string open_error;
  auto dev = ImageBlockDevice::Open(path, writable, 512, &open_error);
  if (!dev && error) {
    *error = "cannot open image: " + target +
             (open_error.empty() ? "" : " (" + open_error + ")");
  }
  return dev;
}

}  // namespace

int RunMountHost(const MountTarget& target, const std::string& drive,
                 bool force_unsafe_ro, bool verbose, bool read_write,
                 bool perf_stats, bool allow_rmapbt_qualification,
                 bool allow_reflink_qualification,
                 const DeviceOpenPolicy* device_policy) {
  // Delay-loaded WinFsp runtime: resolve the architecture-matching official
  // 2.x DLL and validate it before any imported call.  This replaces the old
  // hard-coded winfsp-x64.dll lookup.
  WinFspRuntimeInfo runtime;
  Status runtime_status = PreloadWinFspRuntime(&runtime);
  if (!runtime_status.ok()) {
    std::cerr << "error: " << runtime_status.error().Format() << "\n"
              << "       WinFsp 2.x is required for mounting. Install WinFsp "
                 "from https://winfsp.dev and\n"
                 "       make sure the drive is free. Formatting and reading "
                 "images work without it.\n";
    return 1;
  }

  std::string error;
  auto device = OpenTarget(target.path, /*writable=*/read_write, device_policy,
                           &error);
  if (!device) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  if (target.offset_bytes != 0 || target.length_bytes != 0) {
    uint64_t length = target.length_bytes;
    if (length == 0) {
      if (target.offset_bytes >= device->Size()) {
        std::cerr << "error: the partition starts outside the device\n";
        return 1;
      }
      length = device->Size() - target.offset_bytes;
    }
    auto view = OffsetBlockDevice::Create(std::move(device),
                                          target.offset_bytes, length);
    if (!view.ok()) {
      std::cerr << "error: " << view.error().Format() << "\n";
      return 1;
    }
    device = std::move(view.value());
  }
  MountOptions options;
  options.force_unsafe_ro = force_unsafe_ro;
  options.read_write = read_write;
  options.allow_rmapbt_qualification = allow_rmapbt_qualification;
  options.allow_reflink_qualification = allow_reflink_qualification;
  options.perf_stats = perf_stats;
  auto filesystem = XfsFilesystem::Mount(std::move(device), options);
  if (!filesystem.ok()) {
    std::cerr << "error: " << filesystem.error().Format() << "\n";
    return 1;
  }

  MountConfig config;
  config.mount_point = Utf8ToWide(drive);
  config.force_unsafe_ro = force_unsafe_ro;
  config.verbose = verbose;
  config.read_write = read_write;
  config.perf_stats = perf_stats;
  Status status = RunMountedFilesystem(std::move(filesystem.value()), config);
  if (!status.ok()) {
    std::cerr << "error: " << status.error().Format() << "\n";
    return 1;
  }
  return 0;
}

int RunMountHost(const std::string& target, const std::string& drive,
                 bool force_unsafe_ro, bool verbose, bool read_write,
                 bool perf_stats, bool allow_rmapbt_qualification,
                 bool allow_reflink_qualification,
                 const DeviceOpenPolicy* device_policy) {
  MountTarget whole;
  whole.path = target;
  return RunMountHost(whole, drive, force_unsafe_ro, verbose, read_write,
                      perf_stats, allow_rmapbt_qualification,
                      allow_reflink_qualification, device_policy);
}

int StopMountHost(const std::string& drive) {
  Status status = RequestUnmount(Utf8ToWide(drive));
  if (!status.ok()) {
    std::cerr << "error: " << status.error().Format() << "\n";
    return 1;
  }
  std::cout << "unmount requested for " << drive << "\n";
  return 0;
}

}  // namespace xfs::winfsp

#else

namespace xfs::winfsp {
int RunMountHost(const MountTarget&, const std::string&, bool, bool, bool,
                 bool, bool, bool, const DeviceOpenPolicy*) {
  return 1;
}
int RunMountHost(const std::string&, const std::string&, bool, bool, bool,
                 bool, bool, bool, const DeviceOpenPolicy*) {
  return 1;
}
int StopMountHost(const std::string&) { return 1; }
}  // namespace xfs::winfsp

#endif
