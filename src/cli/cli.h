// XFS for Windows - command line interface.
#pragma once

#include <string>
#include <vector>

namespace xfs::cli {

struct Options {
  bool verbose = false;
  bool debug = false;
  bool force_unsafe_ro = false;
  bool yes_i_really_mean_it = false;
};

// Runs the CLI. argv excludes the program name.
int Run(const std::vector<std::string>& args);

}  // namespace xfs::cli
