// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "stateful/runner.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "stateful/generator.h"
#include "stateful/verifier.h"
#include "xfs/crash.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/image_device.h"

namespace xfstest::stateful {
namespace {

std::string HexSeed(uint64_t value) {
  char buffer[19];
  std::snprintf(buffer, sizeof(buffer), "0x%016llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

std::string HexHash(uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

void EnsureParent(const std::string& path) {
  std::filesystem::path p(path);
  if (!p.has_parent_path()) return;
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
}

void WriteText(const std::string& path, const std::string& text) {
  EnsureParent(path);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  out.flush();
}

void AppendTrace(const FuzzOptions& options, const std::string& line) {
  if (options.trace.empty()) return;
  std::ofstream out(options.trace, std::ios::binary | std::ios::app);
  out << line << "\n";
  out.flush();
}

void WriteTraceHeader(const FuzzOptions& options) {
  if (options.trace.empty()) return;
  std::ostringstream out;
  out << "FUZZ-2 TRACE\n";
  out << "seed=" << HexSeed(options.seed) << "\n";
  out << "profile=" << ToString(options.profile) << "\n";
  out << "workload=" << ToString(options.workload) << "\n";
  out << "steps=" << options.steps << "\n";
  out << "crash_op=" << options.crash_op << "\n";
  out << "crash_point=" << options.crash_point << "\n";
  out << "image=" << options.image << "\n";
  WriteText(options.trace, out.str());
}

void WriteManifest(const FuzzOptions& options, const std::string& mode,
                   const std::string& state_mode,
                   const std::vector<ModelFs>& states,
                   const std::vector<std::string>& labels,
                   uint64_t target_node = 0,
                   const Operation* range_op = nullptr) {
  if (options.manifest.empty()) return;
  std::string range_data_file;
  const ModelNode* before_target = nullptr;
  const ModelNode* after_target = nullptr;
  if (state_mode == "range") {
    if (!range_op || target_node == 0 || states.size() != 2) {
      throw std::runtime_error("range manifest is missing its operation state");
    }
    before_target = states[0].Find(target_node);
    after_target = states[1].Find(target_node);
    if (!before_target || !after_target ||
        before_target->type != NodeType::kFile ||
        after_target->type != NodeType::kFile) {
      throw std::runtime_error("range manifest target is not a regular file");
    }
    const std::filesystem::path manifest_path(options.manifest);
    range_data_file = manifest_path.filename().string() + ".range.bin";
    std::ofstream data_out(options.manifest + ".range.bin",
                           std::ios::binary | std::ios::trunc);
    if (!data_out) {
      throw std::runtime_error("cannot create range data contract");
    }
    if (!before_target->data.empty()) {
      data_out.write(reinterpret_cast<const char*>(before_target->data.data()),
                     static_cast<std::streamsize>(before_target->data.size()));
    }
    if (!after_target->data.empty()) {
      data_out.write(reinterpret_cast<const char*>(after_target->data.data()),
                     static_cast<std::streamsize>(after_target->data.size()));
    }
    if (!data_out) throw std::runtime_error("cannot write range data contract");
  }
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"xfswin-stateful-v1\",\n";
  out << "  \"mode\": \"" << JsonEscape(mode) << "\",\n";
  out << "  \"state_mode\": \"" << JsonEscape(state_mode) << "\",\n";
  out << "  \"seed\": \"" << HexSeed(options.seed) << "\",\n";
  out << "  \"profile\": \"" << ToString(options.profile) << "\",\n";
  out << "  \"workload\": \"" << ToString(options.workload) << "\",\n";
  out << "  \"steps\": " << options.steps << ",\n";
  out << "  \"crash_op\": " << options.crash_op << ",\n";
  out << "  \"crash_point\": \"" << JsonEscape(options.crash_point) << "\",\n";
  if (target_node != 0) out << "  \"target_node\": " << target_node << ",\n";
  if (!range_data_file.empty()) {
    out << "  \"range_contract\": {\n";
    out << "    \"operation\": \"" << ToString(range_op->type) << "\",\n";
    out << "    \"path\": \""
        << JsonEscape(states[1].PathOf(target_node)) << "\",\n";
    out << "    \"offset\": " << range_op->offset << ",\n";
    out << "    \"length\": " << range_op->length << ",\n";
    out << "    \"size\": " << range_op->size << ",\n";
    out << "    \"data_file\": \"" << JsonEscape(range_data_file)
        << "\",\n";
    out << "    \"before_size\": " << before_target->data.size() << ",\n";
    out << "    \"after_size\": " << after_target->data.size() << ",\n";
    out << "    \"before_hash\": \""
        << HexHash(ModelFs::HashBytes(before_target->data)) << "\",\n";
    out << "    \"after_hash\": \""
        << HexHash(ModelFs::HashBytes(after_target->data)) << "\"\n";
    out << "  },\n";
  }
  out << "  \"labels\": [";
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i) out << ", ";
    out << "\"" << JsonEscape(labels[i]) << "\"";
  }
  out << "],\n";
  out << "  \"states\": [\n";
  for (size_t i = 0; i < states.size(); ++i) {
    if (i) out << ",\n";
    out << states[i].ToJson("    ");
  }
  out << "\n  ]\n";
  out << "}\n";
  WriteText(options.manifest, out.str());
}

struct Plan {
  std::vector<Operation> ops;
  ModelFs final;
  ModelFs before_capture;
  ModelFs after_capture;
  Operation capture_op;
  bool has_capture = false;
};

bool GeneratePlan(const FuzzOptions& options, uint64_t steps,
                  uint64_t capture_op, Plan* plan, std::string* error) {
  ModelFs model;
  GeneratorOptions gen_options;
  gen_options.seed = options.seed;
  gen_options.workload = options.workload;
  gen_options.modern_profile = options.profile == ImageProfile::kModern;
  gen_options.reflink = options.profile == ImageProfile::kModern;
  gen_options.max_file_bytes = options.max_file_bytes;
  Generator generator(model, gen_options);
  plan->ops.clear();
  for (uint64_t i = 1; i <= steps; ++i) {
    std::string why;
    Operation op = generator.Next(i, &why);
    if (!why.empty()) {
      *error = "generator step " + std::to_string(i) + ": " + why;
      return false;
    }
    if (capture_op != 0 && i == capture_op) {
      plan->before_capture = model;
      plan->capture_op = op;
    }
    std::string apply_error;
    if (!model.Apply(op, &apply_error)) {
      *error = "model rejected generated operation " + std::to_string(i) +
               " (" + op.Describe() + "): " + apply_error;
      return false;
    }
    if (capture_op != 0 && i == capture_op) {
      plan->after_capture = model;
      plan->has_capture = true;
    }
    plan->ops.push_back(std::move(op));
  }
  plan->final = model;
  return true;
}

bool GeneratePlanFromFile(const FuzzOptions& options, uint64_t capture_op,
                           Plan* plan, std::string* error) {
  std::ifstream input(options.ops_file, std::ios::binary);
  if (!input) {
    *error = "cannot open operation file " + options.ops_file;
    return false;
  }
  ModelFs model;
  plan->ops.clear();
  std::string line;
  uint64_t index = 0;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line.find('=') != std::string::npos) continue;  // metadata header
    Operation op;
    std::string parse_error;
    if (!OperationFromLine(line, &op, &parse_error)) {
      *error = "operation file line " + std::to_string(index + 1) + ": " +
               parse_error;
      return false;
    }
    ++index;
    if (capture_op != 0 && index == capture_op) {
      plan->before_capture = model;
      plan->capture_op = op;
    }
    std::string apply_error;
    if (!model.Apply(op, &apply_error)) {
      *error = "operation file line " + std::to_string(index) + " (" +
               op.Describe() + "): " + apply_error;
      return false;
    }
    if (capture_op != 0 && index == capture_op) {
      plan->after_capture = model;
      plan->has_capture = true;
    }
    plan->ops.push_back(std::move(op));
  }
  if (index == 0) {
    *error = "operation file is empty";
    return false;
  }
  plan->final = model;
  return true;
}

uint64_t ActualIno(const ModelFs& model, uint64_t id) {
  const ModelNode* node = model.Find(id);
  return node ? node->actual_ino : 0;
}

struct ExecResult {
  bool ok = false;
  bool no_space = false;
  xfs::ErrorContext error;
};

template <typename T>
ExecResult FromResult(const xfs::Result<T>& result) {
  if (result.ok()) return ExecResult{true, false, {}};
  ExecResult out;
  out.error = result.error();
  out.no_space = IsNoSpace(result.error());
  return out;
}

ExecResult FromStatus(const xfs::Status& status) {
  if (status.ok()) return ExecResult{true, false, {}};
  ExecResult out;
  out.error = status.error();
  out.no_space = IsNoSpace(status.error());
  return out;
}

ExecResult ExecuteOperation(xfs::XfsFilesystem& fs, const ModelFs& model,
                            const Operation& op, uint64_t* new_ino) {
  uint64_t parent = ActualIno(model, op.parent);
  uint64_t node = ActualIno(model, op.node);
  uint64_t dst_parent = ActualIno(model, op.dst_parent);
  uint64_t dst_node = ActualIno(model, op.dst_node);

  switch (op.type) {
    case OpType::kMkdir: {
      if (parent == 0) return ExecResult{false, false, {}};
      auto status = fs.Mkdir(parent, op.name, 0755, 0, 0);
      ExecResult result = FromStatus(status);
      if (!result.ok) return result;
      auto found = fs.LookupName(parent, op.name);
      if (!found.ok()) return FromResult(found);
      *new_ino = found.value().ino;
      return result;
    }
    case OpType::kCreate: {
      if (parent == 0) return ExecResult{false, false, {}};
      auto created = fs.CreateRegularFile(parent, op.name, 0644, 0, 0);
      ExecResult result = FromResult(created);
      if (!result.ok) return result;
      *new_ino = created.value();
      return result;
    }
    case OpType::kSymlink: {
      if (parent == 0) return ExecResult{false, false, {}};
      auto created = fs.CreateSymlink(parent, op.name, op.symlink_target, 0777,
                                      0, 0);
      ExecResult result = FromResult(created);
      if (!result.ok) return result;
      *new_ino = created.value();
      return result;
    }
    case OpType::kRename: {
      if (parent == 0 || dst_parent == 0) return ExecResult{false, false, {}};
      auto renamed = fs.Rename(parent, op.name, dst_parent, op.dst_name,
                               op.replace_existing, false);
      return FromResult(renamed);
    }
    case OpType::kHardlink: {
      if (node == 0 || dst_parent == 0) return ExecResult{false, false, {}};
      return FromStatus(fs.Link(node, dst_parent, op.dst_name));
    }
    case OpType::kUnlink: {
      if (parent == 0) return ExecResult{false, false, {}};
      return FromStatus(fs.Unlink(parent, op.name, false));
    }
    case OpType::kRmdir: {
      if (parent == 0) return ExecResult{false, false, {}};
      return FromStatus(fs.RemoveEmptyDirectory(parent, op.name, false));
    }
    case OpType::kSetXattr:
    case OpType::kReplaceXattr:
    case OpType::kRemoveXattr: {
      if (node == 0) return ExecResult{false, false, {}};
      if (op.type == OpType::kRemoveXattr) {
        return FromStatus(fs.RemoveXattr(node, op.xattr_name));
      }
      const std::vector<uint8_t> value =
          PatternBytes(op.xattr_seed, op.xattr_length);
      const auto mode = op.type == OpType::kSetXattr
                            ? xfs::XfsFilesystem::XattrSetMode::kCreate
                            : xfs::XfsFilesystem::XattrSetMode::kReplace;
      auto stored = fs.SetXattr(node, op.xattr_name, value.data(), value.size(),
                                mode);
      return FromResult(stored);
    }
    case OpType::kWrite:
    case OpType::kExtend:
    case OpType::kOverwrite:
    case OpType::kSparseWrite:
    case OpType::kCowOverwrite: {
      if (node == 0 || op.length == 0) return ExecResult{false, false, {}};
      const std::vector<uint8_t> data = PatternBytes(op.data_seed, op.length);
      auto written = fs.WriteFileData(node, op.offset, data.data(), data.size());
      return FromResult(written);
    }
    case OpType::kTruncateSmaller:
    case OpType::kTruncateLarger: {
      if (node == 0) return ExecResult{false, false, {}};
      return FromResult(fs.SetFileSize(node, op.size));
    }
    case OpType::kPreallocate: {
      if (node == 0) return ExecResult{false, false, {}};
      return FromResult(
          fs.AllocateUnwritten(node, op.offset, op.length, op.keep_size));
    }
    case OpType::kConvertUnwritten: {
      if (node == 0) return ExecResult{false, false, {}};
      return FromResult(fs.ConvertUnwritten(node, op.offset, op.length));
    }
    case OpType::kReflink: {
      if (node == 0 || dst_node == 0) return ExecResult{false, false, {}};
      return FromResult(
          fs.ReflinkRange(node, op.offset, dst_node, op.size, op.length));
    }
  }
  return ExecResult{false, false, {}};
}

// The ENOSPC workload must prove more than "one operation returned kNoSpace":
// the failed transaction has to leave the image unchanged, and space must be
// allocatable again after a real release.  Run one deterministic probe at the
// first kNoSpace so a fixed seed always exercises the same release/reuse pair:
// truncate the largest data file to zero (all its extents must disappear),
// then allocate one block on that file (a new extent must appear).
void ProveEnospcReleaseReuse(xfs::XfsFilesystem& fs, ModelFs& model,
                             const FuzzOptions& options, uint64_t step) {
  uint64_t victim = 0;
  uint64_t victim_size = 0;
  for (uint64_t id : model.Files()) {
    const ModelNode* candidate = model.Find(id);
    if (!candidate || candidate->actual_ino == 0 || candidate->data.empty()) {
      continue;
    }
    if (candidate->data.size() > victim_size) {
      victim = id;
      victim_size = candidate->data.size();
    }
  }
  if (victim == 0) {
    throw FuzzFailure{
        "ENOSPC was reached before any data file existed; the workload cannot "
        "demonstrate release and reuse",
        step};
  }
  const ModelNode* node = model.Find(victim);
  const uint64_t ino = node->actual_ino;
  auto before = fs.GetDataExtents(ino);
  if (!before.ok()) throw FuzzFailure{before.error().Format(), step};
  if (before.value().empty()) {
    throw FuzzFailure{"data file has logical content but no allocated extents "
                      "before the ENOSPC release probe",
                      step};
  }

  Operation release;
  release.type = OpType::kTruncateSmaller;
  release.node = victim;
  release.size = 0;
  uint64_t new_ino = 0;
  ExecResult released = ExecuteOperation(fs, model, release, &new_ino);
  if (!released.ok) {
    throw FuzzFailure{"ENOSPC release probe (truncate to zero) failed: " +
                          released.error.Format(),
                      step};
  }
  std::string why;
  if (!model.Apply(release, &why)) {
    throw FuzzFailure{"model rejected the ENOSPC release probe: " + why, step};
  }
  auto freed = fs.GetDataExtents(ino);
  if (!freed.ok()) throw FuzzFailure{freed.error().Format(), step};
  if (!freed.value().empty()) {
    throw FuzzFailure{
        "truncate to zero left data extents allocated after ENOSPC", step};
  }

  Operation reuse;
  reuse.type = OpType::kPreallocate;
  reuse.node = victim;
  reuse.offset = 0;
  reuse.length = fs.geometry().block_size;
  reuse.keep_size = false;
  ExecResult reused = ExecuteOperation(fs, model, reuse, &new_ino);
  if (!reused.ok) {
    throw FuzzFailure{
        "ENOSPC reuse probe (one-block preallocate after release) failed: " +
            reused.error.Format(),
        step};
  }
  if (!model.Apply(reuse, &why)) {
    throw FuzzFailure{"model rejected the ENOSPC reuse probe: " + why, step};
  }
  auto reallocated = fs.GetDataExtents(ino);
  if (!reallocated.ok()) {
    throw FuzzFailure{reallocated.error().Format(), step};
  }
  if (reallocated.value().empty()) {
    throw FuzzFailure{
        "preallocate succeeded after release but produced no data extent",
        step};
  }

  VerifyResult verified = VerifyModel(fs, model);
  if (!verified.ok) {
    throw FuzzFailure{
        "ENOSPC release/reuse probe left a changed filesystem: " +
            verified.error,
        step};
  }
  AppendTrace(options, "reuse-release " + std::to_string(step) + " " +
                           release.Describe() + " freed_extents=" +
                           std::to_string(before.value().size()));
  AppendTrace(options, "reuse-alloc " + std::to_string(step) + " " +
                           reuse.Describe() + " extents=" +
                           std::to_string(reallocated.value().size()));
}

std::string DescribeCheck(const xfs::CheckReport& report) {
  std::ostringstream out;
  out << report.Summary();
  for (const xfs::CheckItem& item : report.items) {
    if (item.status == xfs::CheckStatus::kOk) continue;
    out << "\n  " << xfs::ToString(item.status) << " " << item.name << ": "
        << item.message;
  }
  return out.str();
}

void CheckConsistency(xfs::XfsFilesystem& fs, bool allow_warnings,
                      std::string* error) {
  xfs::CheckOptions check_options;
  check_options.all_allocated_inodes = true;
  xfs::CheckReport report = fs.CheckConsistency(check_options);
  const xfs::CheckStatus status = report.Overall();
  if (status == xfs::CheckStatus::kOk) return;
  if (allow_warnings && status == xfs::CheckStatus::kWarning) return;
  *error = "consistency check failed: " + DescribeCheck(report);
}

bool CrashPointApplicable(const std::string& point, const Operation& op) {
  const OpType type = op.type;
  if (point == "before_log_write" || point == "mid_log_record" ||
      point == "after_log_commit" || point == "before_metadata_write" ||
      point == "mid_metadata_write" || point == "after_metadata_write") {
    return true;
  }
  const bool data_write = type == OpType::kWrite || type == OpType::kExtend ||
                          type == OpType::kOverwrite ||
                          type == OpType::kSparseWrite;
  const bool cow_write = type == OpType::kCowOverwrite ||
                         type == OpType::kTruncateSmaller ||
                         type == OpType::kTruncateLarger;
  const bool xattr_write = type == OpType::kSetXattr ||
                           type == OpType::kReplaceXattr;
  const size_t xattr_entry_size =
      (3 + op.xattr_name.size() + op.xattr_length + 3) & ~size_t{3};
  const bool remote_xattr = xattr_write && xattr_entry_size >= 3072;
  if (point == "before_data_write" || point == "after_data_write") {
    return data_write || remote_xattr || type == OpType::kPreallocate ||
           cow_write;
  }
  if (point == "mid_data_write") return data_write || cow_write ||
                                           type == OpType::kPreallocate;
  if (point == "before_device_flush" || point == "after_device_flush") {
    return remote_xattr || cow_write;
  }
  if (point == "after_bmap_switch") return type == OpType::kReflink;
  if (point == "cow_staged" || point == "before_cow_data_write" ||
      point == "mid_cow_data_write" || point == "after_cow_data_write" ||
      point == "before_cow_remap_commit" || point == "after_cow_remap" ||
      point == "after_refcount_update" || point == "after_rmap_update" ||
      point == "before_old_range_release" ||
      point == "after_old_range_release" || point == "before_intent_done" ||
      point == "after_intent_done") {
    return type == OpType::kCowOverwrite;
  }
  return false;
}

void ArmCrash(const std::string& point, uint32_t hit) {
  std::string spec = point;
  if (hit > 1) spec += ":" + std::to_string(hit);
#ifdef _WIN32
  _putenv_s("XFSWIN_CRASH_AT", spec.c_str());
#else
  setenv("XFSWIN_CRASH_AT", spec.c_str(), 1);
#endif
  xfs::ReloadCrashConfig();
}

void DisarmCrash() {
#ifdef _WIN32
  _putenv_s("XFSWIN_CRASH_AT", "");
#else
  unsetenv("XFSWIN_CRASH_AT");
#endif
  xfs::ReloadCrashConfig();
}

uint64_t DefaultSize(ImageProfile profile) {
  return profile == ImageProfile::kModern ? (512ull << 20) : (320ull << 20);
}

std::unique_ptr<xfs::XfsFilesystem> CreateAndMount(const FuzzOptions& options,
                                                   std::string* error) {
  const uint64_t size = options.size_bytes ? options.size_bytes
                                           : DefaultSize(options.profile);
  std::string backend_error;
  auto device = xfs::ImageBlockDevice::Create(options.image, size, 512,
                                              &backend_error);
  if (!device) {
    *error = "cannot create image: " + backend_error;
    return nullptr;
  }
  xfs::mkfs::FormatOptions format;
  format.block_size = 4096;
  format.inode_size = 512;
  format.quiet = true;
  if (options.profile == ImageProfile::kModern) {
    format.modern_default_profile = true;
  }
  auto report = xfs::mkfs::Format(*device, format);
  if (!report.ok()) {
    *error = "format failed: " + report.error().Format();
    return nullptr;
  }
  xfs::MountOptions mount;
  mount.read_write = true;
  auto mounted =
      xfs::XfsFilesystem::Mount(std::move(device), mount);
  if (!mounted.ok()) {
    *error = "mount failed: " + mounted.error().Format();
    return nullptr;
  }
  return std::move(mounted.value());
}

std::unique_ptr<xfs::XfsFilesystem> OpenAndRecover(const FuzzOptions& options,
                                                   std::string* error) {
  std::string backend_error;
  auto device = xfs::ImageBlockDevice::Open(options.image, true, 512,
                                            &backend_error);
  if (!device) {
    *error = "cannot open image: " + backend_error;
    return nullptr;
  }
  xfs::MountOptions mount;
  mount.read_write = true;
  mount.recover_dirty_log = true;
  mount.clean_log_after_recovery = true;
  auto mounted = xfs::XfsFilesystem::Mount(std::move(device), mount);
  if (!mounted.ok()) {
    *error = "recovery mount failed: " + mounted.error().Format();
    return nullptr;
  }
  return std::move(mounted.value());
}

void FinishClean(xfs::XfsFilesystem& fs, std::string* error) {
  auto flushed = fs.Flush();
  if (!flushed.ok()) {
    *error = "flush failed: " + flushed.error().Format();
    return;
  }
  auto closed = fs.WriteCleanUnmountRecord();
  if (!closed.ok()) {
    *error = "clean unmount failed: " + closed.error().Format();
    return;
  }
  flushed = fs.Flush();
  if (!flushed.ok()) {
    *error = "final flush failed: " + flushed.error().Format();
    return;
  }
}


}  // namespace

bool ParseImageProfile(const std::string& name, ImageProfile* out) {
  if (name == "default") {
    *out = ImageProfile::kDefault;
    return true;
  }
  if (name == "modern") {
    *out = ImageProfile::kModern;
    return true;
  }
  return false;
}

const char* ToString(ImageProfile profile) {
  return profile == ImageProfile::kModern ? "modern" : "default";
}

int RunFresh(const FuzzOptions& options) {
  if (options.image.empty()) {
    throw FuzzFailure{"--image is required", 0};
  }
  if (options.steps == 0) throw FuzzFailure{"--steps must be > 0", 0};
  if (options.crash_op != 0 && options.crash_op > options.steps) {
    throw FuzzFailure{"--crash-op must be <= --steps", options.crash_op};
  }
  if (options.workload == Workload::kEnospc && options.crash_op != 0) {
    throw FuzzFailure{"ENOSPC workload does not support crash injection", 0};
  }

  WriteTraceHeader(options);

  Plan crash_plan;
  if (options.crash_op != 0) {
    std::string error;
    const bool loaded = !options.ops_file.empty()
                            ? GeneratePlanFromFile(options, options.crash_op,
                                                   &crash_plan, &error)
                            : GeneratePlan(options, options.crash_op,
                                           options.crash_op, &crash_plan,
                                           &error);
    if (!loaded) {
      throw FuzzFailure{error, options.crash_op};
    }
    if (crash_plan.ops.size() > options.crash_op) {
      crash_plan.ops.resize(static_cast<size_t>(options.crash_op));
    }
    if (!crash_plan.has_capture) {
      throw FuzzFailure{"internal: crash plan has no capture", options.crash_op};
    }
    if (!CrashPointApplicable(options.crash_point, crash_plan.capture_op)) {
      throw FuzzFailure{"crash point '" + options.crash_point +
                            "' is not applicable to operation " +
                            std::to_string(options.crash_op) + " (" +
                            ToString(crash_plan.capture_op.type) + ")",
                        options.crash_op};
    }
    WriteManifest(options, "crash-plan",
                  IsRangeCrashOperation(crash_plan.capture_op.type) ? "range"
                                                                   : "exact",
                  {crash_plan.before_capture, crash_plan.after_capture},
                  {"before", "after"},
                  crash_plan.capture_op.type == OpType::kReflink
                      ? crash_plan.capture_op.dst_node
                      : crash_plan.capture_op.node,
                  IsRangeCrashOperation(crash_plan.capture_op.type)
                      ? &crash_plan.capture_op
                      : nullptr);
    for (size_t i = 0; i < crash_plan.ops.size(); ++i) {
      AppendTrace(options, "plan " + std::to_string(i + 1) + " " +
                               crash_plan.ops[i].Describe());
    }
  }

  std::string error;
  auto mounted = CreateAndMount(options, &error);
  if (!mounted) throw FuzzFailure{error, 0};
  xfs::XfsFilesystem& fs = *mounted;
  ModelFs model;
  model.mutable_nodes()[model.root()].actual_ino = fs.RootInode();

  uint64_t no_space = 0;
  uint64_t executed = 0;
  bool reuse_probe_done = false;

  if (options.crash_op != 0) {
    for (uint64_t i = 0; i < crash_plan.ops.size(); ++i) {
      const Operation& op = crash_plan.ops[i];
      const bool target = (i + 1 == options.crash_op);
      if (target) {
        AppendTrace(options, "begin " + std::to_string(i + 1) + " " +
                                 op.Describe());
        ArmCrash(options.crash_point, options.crash_hit);
      }
      uint64_t new_ino = 0;
      ExecResult result = ExecuteOperation(fs, model, op, &new_ino);
      if (!result.ok) {
        throw FuzzFailure{"operation " + std::to_string(i + 1) + " (" +
                              op.Describe() + ") failed: " +
                              result.error.Format(),
                          i + 1};
      }
      std::string why;
      if (!model.Apply(op, &why)) {
        throw FuzzFailure{"model rejected executed operation " +
                              std::to_string(i + 1) + ": " + why,
                          i + 1};
      }
      if (new_ino != 0) model.mutable_nodes()[op.node].actual_ino = new_ino;
      executed = i + 1;
      AppendTrace(options, "ok " + std::to_string(i + 1) + " " +
                               op.Describe());
      if (target) {
        DisarmCrash();
        throw FuzzFailure{"crash point '" + options.crash_point +
                              "' was not reached at operation " +
                              std::to_string(i + 1),
                          i + 1};
      }
      if (options.check_every &&
          (i + 1) % options.check_every == 0) {
        VerifyResult verified = VerifyModel(fs, model);
        if (!verified.ok) {
          throw FuzzFailure{verified.error, i + 1};
        }
      }
    }
    throw FuzzFailure{"crash injection ended without terminating", executed};
  }

  if (options.workload == Workload::kEnospc) {
    if (!options.ops_file.empty()) {
      throw FuzzFailure{"--ops-file is not supported by the ENOSPC workload", 0};
    }
    GeneratorOptions gen_options;
    gen_options.seed = options.seed;
    gen_options.workload = options.workload;
    gen_options.modern_profile = options.profile == ImageProfile::kModern;
    gen_options.reflink = options.profile == ImageProfile::kModern;
    gen_options.max_file_bytes = options.max_file_bytes;
    Generator generator(model, gen_options);
    for (uint64_t i = 1; i <= options.steps; ++i) {
      std::string why;
      Operation op = generator.Next(i, &why);
      if (!why.empty()) throw FuzzFailure{why, i};
      uint64_t new_ino = 0;
      ExecResult result = ExecuteOperation(fs, model, op, &new_ino);
      if (!result.ok) {
        if (!result.no_space) {
          throw FuzzFailure{"operation " + std::to_string(i) + " (" +
                                op.Describe() + ") failed: " +
                                result.error.Format(),
                            i};
        }
        ++no_space;
        // The operation must have failed before writing any metadata.  This
        // is the ENOSPC contract: allocation rollback, then the filesystem
        // stays usable.
        VerifyResult verified = VerifyModel(fs, model);
        if (!verified.ok) {
          throw FuzzFailure{"ENOSPC left a changed filesystem: " +
                                verified.error,
                            i};
        }
        AppendTrace(options, "enospc " + std::to_string(i) + " " +
                                 op.Describe());
        if (!reuse_probe_done) {
          ProveEnospcReleaseReuse(fs, model, options, i);
          reuse_probe_done = true;
        }
      } else {
        if (!model.Apply(op, &why)) {
          throw FuzzFailure{"model rejected executed operation " +
                                std::to_string(i) + ": " + why,
                            i};
        }
        if (new_ino != 0) model.mutable_nodes()[op.node].actual_ino = new_ino;
        executed = i;
        AppendTrace(options, "ok " + std::to_string(i) + " " + op.Describe());
      }
      if (options.check_every && i % options.check_every == 0) {
        VerifyResult verified = VerifyModel(fs, model);
        if (!verified.ok) throw FuzzFailure{verified.error, i};
      }
    }
    if (options.require_enospc && no_space == 0) {
      throw FuzzFailure{"ENOSPC workload completed without a kNoSpace result",
                        options.steps};
    }
  } else {
    Plan plan;
    const bool loaded = !options.ops_file.empty()
                            ? GeneratePlanFromFile(options, 0, &plan, &error)
                            : GeneratePlan(options, options.steps, 0, &plan,
                                           &error);
    if (!loaded) throw FuzzFailure{error, 0};
    for (uint64_t i = 0; i < plan.ops.size(); ++i) {
      const Operation& op = plan.ops[i];
      uint64_t new_ino = 0;
      ExecResult result = ExecuteOperation(fs, model, op, &new_ino);
      if (!result.ok) {
        throw FuzzFailure{"operation " + std::to_string(i + 1) + " (" +
                              op.Describe() + ") failed: " +
                              result.error.Format(),
                          i + 1};
      }
      std::string why;
      if (!model.Apply(op, &why)) {
        throw FuzzFailure{"model rejected executed operation " +
                              std::to_string(i + 1) + ": " + why,
                          i + 1};
      }
      if (new_ino != 0) model.mutable_nodes()[op.node].actual_ino = new_ino;
      executed = i + 1;
      AppendTrace(options, "ok " + std::to_string(i + 1) + " " + op.Describe());
      if (options.check_every && (i + 1) % options.check_every == 0) {
        VerifyResult verified = VerifyModel(fs, model);
        if (!verified.ok) {
          throw FuzzFailure{verified.error, i + 1};
        }
      }
    }
    if (model.ToJson("") != plan.final.ToJson("")) {
      throw FuzzFailure{"execution model diverged from the generated plan",
                        executed};
    }
  }

  VerifyResult final_verify = VerifyModel(fs, model);
  if (!final_verify.ok) throw FuzzFailure{final_verify.error, executed};
  FinishClean(fs, &error);
  if (!error.empty()) throw FuzzFailure{error, executed};
  std::string check_error;
  CheckConsistency(fs, false, &check_error);
  if (!check_error.empty()) throw FuzzFailure{check_error, executed};

  WriteManifest(options, "final", "exact", {model}, {"final"});
  if (!options.quiet) {
    std::printf("FUZZ-2 PASS: seed=%s steps=%llu profile=%s workload=%s "
                "enospc=%llu image=%s\n",
                HexSeed(options.seed).c_str(),
                static_cast<unsigned long long>(options.steps),
                ToString(options.profile), ToString(options.workload),
                static_cast<unsigned long long>(no_space),
                options.image.c_str());
  }
  return 0;
}

int RunRecover(const FuzzOptions& options) {
  if (options.image.empty()) throw FuzzFailure{"--image is required", 0};
  if (options.crash_op == 0) {
    throw FuzzFailure{"--recover requires --crash-op", 0};
  }
  Plan plan;
  std::string error;
  const bool loaded = !options.ops_file.empty()
                          ? GeneratePlanFromFile(options, options.crash_op,
                                                 &plan, &error)
                          : GeneratePlan(options, options.crash_op,
                                         options.crash_op, &plan, &error);
  if (!loaded) throw FuzzFailure{error, options.crash_op};
  if (!plan.has_capture) {
    throw FuzzFailure{"internal: recovery plan has no capture",
                      options.crash_op};
  }
  auto mounted = OpenAndRecover(options, &error);
  if (!mounted) throw FuzzFailure{error, options.crash_op};
  xfs::XfsFilesystem& fs = *mounted;

  const Operation& op = plan.capture_op;
  const uint64_t target = op.type == OpType::kReflink ? op.dst_node : op.node;
  bool range = false;
  std::string before_error;
  std::string after_error;
  bool matches_before = false;
  bool matches_after = false;

  if (IsRangeCrashOperation(op.type)) {
    range = true;
    VerifyResult verified =
        VerifyRangeCrash(fs, plan.before_capture, plan.after_capture, op);
    if (!verified.ok) {
      throw FuzzFailure{"recovered image violates the crash data contract: " +
                            verified.error,
                        options.crash_op};
    }
  } else {
    VerifyResult before = VerifyModel(fs, plan.before_capture);
    matches_before = before.ok;
    if (!before.ok) before_error = before.error;
    VerifyResult after = VerifyModel(fs, plan.after_capture);
    matches_after = after.ok;
    if (!after.ok) after_error = after.error;
    if (!matches_before && !matches_after) {
      throw FuzzFailure{"recovered image matches neither the old nor the new "
                        "state (before: " +
                            before_error + "; after: " + after_error + ")",
                        options.crash_op};
    }
  }

  std::string check_error;
  FinishClean(fs, &error);
  if (!error.empty()) throw FuzzFailure{error, options.crash_op};
  CheckConsistency(fs, false, &check_error);
  if (!check_error.empty()) throw FuzzFailure{check_error, options.crash_op};

  if (range) {
    WriteManifest(options, "recovered", "range",
                  {plan.before_capture, plan.after_capture},
                  {"before", "after"}, target, &op);
  } else if (matches_after) {
    WriteManifest(options, "recovered", "exact", {plan.after_capture},
                  {"after"});
  } else {
    WriteManifest(options, "recovered", "exact", {plan.before_capture},
                  {"before"});
  }
  if (!options.quiet) {
    std::printf("FUZZ-2 RECOVER PASS: seed=%s crash_op=%llu point=%s "
                "contract=%s image=%s\n",
                HexSeed(options.seed).c_str(),
                static_cast<unsigned long long>(options.crash_op),
                options.crash_point.c_str(),
                range ? "old-or-new-data" : (matches_after ? "new" : "old"),
                options.image.c_str());
  }
  return 0;
}

void WriteOpsForTesting(const FuzzOptions& options, const std::string& path) {
  Plan plan;
  std::string error;
  const bool loaded = !options.ops_file.empty()
                          ? GeneratePlanFromFile(options, 0, &plan, &error)
                          : GeneratePlan(options, options.steps, 0, &plan,
                                         &error);
  if (!loaded) throw FuzzFailure{error, 0};
  std::ostringstream out;
  out << "# FUZZ-2 replayable operation file\n";
  out << "seed=" << HexSeed(options.seed) << "\n";
  out << "profile=" << ToString(options.profile) << "\n";
  out << "workload=" << ToString(options.workload) << "\n";
  for (const Operation& op : plan.ops) {
    out << OperationToLine(op) << "\n";
  }
  WriteText(path, out.str());
}

void WritePlanForTesting(const FuzzOptions& options, const std::string& path) {
  Plan plan;
  std::string error;
  const bool loaded = !options.ops_file.empty()
                          ? GeneratePlanFromFile(options, 0, &plan, &error)
                          : GeneratePlan(options, options.steps, 0, &plan,
                                         &error);
  if (!loaded) throw FuzzFailure{error, 0};
  std::ostringstream out;
  out << "seed=" << HexSeed(options.seed) << "\n";
  out << "profile=" << ToString(options.profile) << "\n";
  out << "workload=" << ToString(options.workload) << "\n";
  out << "steps=" << options.steps << "\n";
  for (size_t i = 0; i < plan.ops.size(); ++i) {
    out << (i + 1) << " " << plan.ops[i].Describe() << "\n";
  }
  WriteText(path, out.str());
}

}  // namespace xfstest::stateful
