// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// FUZZ-2: image runner, trace/manifest writer and crash recovery entry point.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stateful/generator.h"

namespace xfstest::stateful {

enum class ImageProfile { kDefault, kModern };

bool ParseImageProfile(const std::string& name, ImageProfile* out);
const char* ToString(ImageProfile profile);

struct FuzzOptions {
  uint64_t seed = 1;
  uint64_t steps = 200;
  std::string image;
  std::string trace;
  std::string manifest;
  std::string ops_file;  // replay an explicit operation list instead of seed
  ImageProfile profile = ImageProfile::kModern;
  Workload workload = Workload::kMixed;
  uint64_t size_bytes = 0;  // 0 = profile default
  uint64_t crash_op = 0;
  std::string crash_point;
  uint32_t crash_hit = 1;
  uint64_t consistency_every = 25;
  uint64_t check_every = 1;
  bool keep_image = false;
  bool quiet = false;
  bool recover = false;
  bool require_enospc = false;
  // Generator options, kept here so the reproducer is a single CLI line.
  uint64_t max_file_bytes = 8ull << 20;
};

struct FuzzFailure {
  std::string message;
  uint64_t operation = 0;
};

// Runs a fresh image and, when crash_op is set, terminates at that operation
// through the project's crash injection (the process exits with kCrashExitCode).
int RunFresh(const FuzzOptions& options);

// Re-opens a crashed image, replays the log through the project recovery
// path, verifies the allowed old/new state and closes the log.
int RunRecover(const FuzzOptions& options);

// Reconstructs the deterministic plan and final model without touching an
// image.  Used by the reducer and diagnostics.
void WritePlanForTesting(const FuzzOptions& options, const std::string& path);
// Writes the same plan in the replayable tab-separated operation format.
void WriteOpsForTesting(const FuzzOptions& options, const std::string& path);

}  // namespace xfstest::stateful
