// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// FUZZ-2 stateful filesystem fuzzer (model-based, deterministic, crash-aware).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include "stateful/generator.h"
#include "stateful/runner.h"
#include "xfs/crash.h"

namespace {

using namespace xfstest::stateful;

void Usage() {
  std::printf(
      "usage: stateful_fuzz --seed <n> --steps <n> --image <path> [options]\n"
      "\n"
      "  --seed <n>              deterministic seed (0x... accepted)\n"
      "  --steps <n>             number of operations (default 200)\n"
      "  --image <path>          image to create or recover\n"
      "  --trace <path>          human-readable operation trace\n"
      "  --manifest <path>       JSON semantic manifest for independent checks\n"
      "  --profile default|modern (default: modern)\n"
      "  --workload mixed|dir|xattr|alloc|enospc (default: mixed)\n"
      "  --image-size-mb <n>     override the formatter image size\n"
      "  --crash-op <n>          arm the crash point at operation n\n"
      "  --crash-point <name>    XFSWIN_CRASH_AT point (e.g. after_log_commit)\n"
      "  --crash-hit <n>         occurrence inside the operation (default 1)\n"
      "  --recover               replay/verify a crashed image instead of\n"
      "                          executing a new sequence\n"
      "  --check-every <n>       full semantic check interval (default 5)\n"
      "  --consistency-every <n> CheckConsistency interval (default 50)\n"
      "  --plan-only <path>      write the deterministic operation plan\n"
      "  --list-crash-points     print all crash point names\n"
      "  --quiet                 only print failures\n"
      "\n"
      "A crash run exits with 0x58435348 after the injected termination; run\n"
      "the same command with --recover on the resulting image to validate it.\n");
}

bool ParseU64(const std::string& text, uint64_t* out) {
  if (text.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 0);
  if (errno != 0 || end == text.c_str() || *end != '\0') return false;
  *out = static_cast<uint64_t>(value);
  return true;
}

bool ParseU32(const std::string& text, uint32_t* out) {
  uint64_t value = 0;
  if (!ParseU64(text, &value) || value > 0xffffffffull) return false;
  *out = static_cast<uint32_t>(value);
  return true;
}

void LoadOpsHeader(const std::string& path, FuzzOptions* options) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return;
  std::string line;
  int lines = 0;
  while (std::getline(input, line) && lines++ < 32) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t equals = line.find('=');
    if (equals == std::string::npos || line.find('\t') != std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (key == "seed") {
      uint64_t seed = 0;
      if (ParseU64(value, &seed)) options->seed = seed;
    } else if (key == "profile") {
      ImageProfile profile = ImageProfile::kModern;
      if (ParseImageProfile(value, &profile)) options->profile = profile;
    } else if (key == "workload") {
      Workload workload = Workload::kMixed;
      if (ParseWorkload(value, &workload)) options->workload = workload;
    }
  }
}

std::string Quote(const std::string& value) {
  return "'" + value + "'";
}

std::string Reproducer(const char* argv0, const FuzzOptions& options,
                       bool recover) {
  std::string command = std::string(argv0) + " --seed " +
                        std::to_string(options.seed) + " --steps " +
                        std::to_string(options.steps) + " --profile " +
                        ToString(options.profile) + " --workload " +
                        ToString(options.workload) + " --image " +
                        Quote(options.image) + " --trace " +
                        Quote(options.trace) + " --manifest " +
                        Quote(options.manifest);
  if (!options.ops_file.empty()) {
    command += " --ops-file " + Quote(options.ops_file);
  }
  if (options.crash_op) {
    command += " --crash-op " + std::to_string(options.crash_op) +
               " --crash-point " + options.crash_point;
    if (options.crash_hit != 1) {
      command += " --crash-hit " + std::to_string(options.crash_hit);
    }
  }
  if (recover) command += " --recover";
  return command;
}

void PrintFailure(const char* argv0, const FuzzOptions& options,
                  const FuzzFailure& failure) {
  std::fprintf(stderr,
               "\nFUZZ-2 FAILURE\n"
               "seed: %llu (0x%llx)\n"
               "steps: %llu\n"
               "operation: %llu\n"
               "crash_op: %llu\n"
               "crash point: %s\n"
               "image profile: %s\n"
               "workload: %s\n"
               "image: %s\n"
               "trace: %s\n"
               "manifest: %s\n"
               "\nmessage:\n%s\n\nreproduce:\n%s\n",
               static_cast<unsigned long long>(options.seed),
               static_cast<unsigned long long>(options.seed),
               static_cast<unsigned long long>(options.steps),
               static_cast<unsigned long long>(failure.operation),
               static_cast<unsigned long long>(options.crash_op),
               options.crash_point.empty() ? "(none)"
                                           : options.crash_point.c_str(),
               ToString(options.profile), ToString(options.workload),
               options.image.c_str(), options.trace.c_str(),
               options.manifest.c_str(), failure.message.c_str(),
               Reproducer(argv0, options, false).c_str());
  if (options.crash_op != 0) {
    std::fprintf(stderr, "\nrecover and verify:\n%s\n",
                 Reproducer(argv0, options, true).c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  FuzzOptions options;
  std::string plan_only;
  std::string ops_out;
  bool list_points = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      Usage();
      return 0;
    }
    if (arg == "--list-crash-points") {
      list_points = true;
      continue;
    }
    if (arg == "--seed") {
      if (!ParseU64(value("--seed"), &options.seed)) {
        std::fprintf(stderr, "invalid --seed\n");
        return 2;
      }
    } else if (arg == "--steps") {
      if (!ParseU64(value("--steps"), &options.steps)) {
        std::fprintf(stderr, "invalid --steps\n");
        return 2;
      }
    } else if (arg == "--image") {
      options.image = value("--image");
    } else if (arg == "--trace") {
      options.trace = value("--trace");
    } else if (arg == "--manifest") {
      options.manifest = value("--manifest");
    } else if (arg == "--profile") {
      if (!ParseImageProfile(value("--profile"), &options.profile)) {
        std::fprintf(stderr, "invalid --profile (default|modern)\n");
        return 2;
      }
    } else if (arg == "--workload") {
      if (!ParseWorkload(value("--workload"), &options.workload)) {
        std::fprintf(stderr, "invalid --workload\n");
        return 2;
      }
    } else if (arg == "--image-size-mb") {
      uint64_t mb = 0;
      if (!ParseU64(value("--image-size-mb"), &mb) || mb == 0) {
        std::fprintf(stderr, "invalid --image-size-mb\n");
        return 2;
      }
      options.size_bytes = mb << 20;
    } else if (arg == "--crash-op") {
      if (!ParseU64(value("--crash-op"), &options.crash_op)) {
        std::fprintf(stderr, "invalid --crash-op\n");
        return 2;
      }
    } else if (arg == "--crash-point") {
      options.crash_point = value("--crash-point");
      xfs::CrashPoint point = xfs::CrashPoint::kCount;
      if (!xfs::ParseCrashPoint(options.crash_point, &point)) {
        std::fprintf(stderr, "unknown --crash-point '%s'\n",
                     options.crash_point.c_str());
        return 2;
      }
    } else if (arg == "--crash-hit") {
      if (!ParseU32(value("--crash-hit"), &options.crash_hit) ||
          options.crash_hit == 0) {
        std::fprintf(stderr, "invalid --crash-hit\n");
        return 2;
      }
    } else if (arg == "--check-every") {
      if (!ParseU64(value("--check-every"), &options.check_every)) {
        std::fprintf(stderr, "invalid --check-every\n");
        return 2;
      }
    } else if (arg == "--consistency-every") {
      if (!ParseU64(value("--consistency-every"), &options.consistency_every)) {
        std::fprintf(stderr, "invalid --consistency-every\n");
        return 2;
      }
    } else if (arg == "--plan-only") {
      plan_only = value("--plan-only");
    } else if (arg == "--ops-out") {
      ops_out = value("--ops-out");
    } else if (arg == "--ops-file") {
      options.ops_file = value("--ops-file");
    } else if (arg == "--recover") {
      options.recover = true;
    } else if (arg == "--require-enospc") {
      options.require_enospc = true;
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
      Usage();
      return 2;
    }
  }

  if (list_points) {
    for (int i = 0; i < static_cast<int>(xfs::CrashPoint::kCount); ++i) {
      std::printf("%s\n",
                  xfs::ToString(static_cast<xfs::CrashPoint>(i)));
    }
    return 0;
  }

  if (options.image.empty()) {
    std::fprintf(stderr, "--image is required\n");
    return 2;
  }
  if (options.trace.empty()) options.trace = options.image + ".trace.txt";
  if (options.manifest.empty()) {
    options.manifest = options.image + ".manifest.json";
  }
  if (options.crash_op != 0 && options.crash_point.empty()) {
    std::fprintf(stderr, "--crash-op requires --crash-point\n");
    return 2;
  }
  if (!options.ops_file.empty()) LoadOpsHeader(options.ops_file, &options);

  if (!plan_only.empty() || !ops_out.empty()) {
    try {
      if (!plan_only.empty()) WritePlanForTesting(options, plan_only);
      if (!ops_out.empty()) WriteOpsForTesting(options, ops_out);
      return 0;
    } catch (const FuzzFailure& failure) {
      PrintFailure(argv[0], options, failure);
      return 1;
    }
  }

  try {
    return options.recover ? RunRecover(options) : RunFresh(options);
  } catch (const FuzzFailure& failure) {
    PrintFailure(argv[0], options, failure);
    return 1;
  } catch (const std::exception& error) {
    FuzzFailure failure{error.what(), 0};
    PrintFailure(argv[0], options, failure);
    return 1;
  }
}
