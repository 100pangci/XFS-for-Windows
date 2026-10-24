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
#include "xfs/filesystem.h"
#include "xfs/image_device.h"
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
                                        bool writable, std::string* error) {
  if (StartsWith(target, "\\\\") || StartsWith(target, "//")) {
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

int RunMountHost(const std::string& target, const std::string& drive,
                 bool force_unsafe_ro, bool verbose, bool read_write,
                 bool allow_rmapbt_qualification) {
  // Delay-loaded WinFsp runtime: fail cleanly if it is not available.
  HMODULE winfsp_dll = LoadLibraryW(L"winfsp-x64.dll");
  if (!winfsp_dll) {
    std::cerr
        << "error: the WinFsp runtime (winfsp-x64.dll) was not found.\n"
           "       WinFsp is required for mounting. Install WinFsp from "
           "https://winfsp.dev and\n"
           "       make sure the drive is free. Formatting and reading "
           "images work without it.\n";
    return 1;
  }
  FreeLibrary(winfsp_dll);

  std::string error;
  auto device = OpenTarget(target, /*writable=*/read_write, &error);
  if (!device) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  MountOptions options;
  options.force_unsafe_ro = force_unsafe_ro;
  options.read_write = read_write;
  options.allow_rmapbt_qualification = allow_rmapbt_qualification;
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
  Status status = RunMountedFilesystem(std::move(filesystem.value()), config);
  if (!status.ok()) {
    std::cerr << "error: " << status.error().Format() << "\n";
    return 1;
  }
  return 0;
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
int RunMountHost(const std::string&, const std::string&, bool, bool, bool,
                 bool) {
  return 1;
}
int StopMountHost(const std::string&) { return 1; }
}  // namespace xfs::winfsp

#endif
