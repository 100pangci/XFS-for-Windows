// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "cli/cli.h"
#include "xfs/log.h"

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 1; i < argc; ++i) {
    int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr,
                                  nullptr);
    std::string utf8(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
    if (len > 1) {
      WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, utf8.data(), len, nullptr,
                          nullptr);
    }
    args.push_back(std::move(utf8));
  }
  return xfs::cli::Run(args);
}
