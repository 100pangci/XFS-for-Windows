// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// CONC-1: deterministic concurrency and lock-model verification.
//
// Every test in this file drives real operations through the public
// XfsFilesystem API on a real (fixture) image. The scheduler is cooperative:
// exactly one operation thread is runnable at a time, so a WaitFor/Resume
// script is a reproducible interleaving, not a timing guess.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "support/fixture_image.h"
#include "test_util.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"

#ifdef XFSWIN_CONCURRENCY_HOOKS

#include "xfs/concurrency_test.h"

namespace {

using xfstest::FixtureImage;
using xfs::conc::Controller;
using xfs::conc::EventKind;

void ConcFailure(const std::string& message) {
  throw xfstest::TestFailure(message);
}

Controller& C() { return Controller::Get(); }

// A failed scenario must not leave a scheduler-blocked operation alive into
// the next test. The guard disables scheduling and joins every spawned thread
// during unwinding; operations complete through the normal (now unscheduled)
// code path.
struct ScenarioGuard {
  ScenarioGuard() {
    C().SetFailureHandler(&ConcFailure);
    C().Enable();
  }
  ~ScenarioGuard() {
    try {
      C().JoinAll();
    } catch (...) {
    }
    C().Disable();
  }
  void Finish() {
    C().JoinAll();
    C().Disable();
  }
  ScenarioGuard(const ScenarioGuard&) = delete;
  ScenarioGuard& operator=(const ScenarioGuard&) = delete;
};

void BeginScenario() {
  C().SetFailureHandler(&ConcFailure);
  C().Enable();
}

void EndScenario() {
  C().JoinAll();
  C().Disable();
}

// RAII form of BeginScenario/EndScenario for tests that pause operations at
// intermediate sync points: a failed wait throws while operations are still
// parked, so the threads must be joined and scheduling disabled during
// unwinding (or the next test would inherit live threads and a dangling
// filesystem pointer).
struct ScenarioScope {
  ScenarioScope() { BeginScenario(); }
  ~ScenarioScope() {
    if (!finished_) {
      try {
        C().JoinAll();
      } catch (...) {
      }
      C().Disable();
    }
  }
  void Finish() {
    finished_ = true;
    EndScenario();
  }
  ScenarioScope(const ScenarioScope&) = delete;
  ScenarioScope& operator=(const ScenarioScope&) = delete;

 private:
  bool finished_ = false;
};

void Require(bool condition, const std::string& what) {
  if (!condition) {
    throw xfstest::TestFailure(what + "\n" + C().Dump());
  }
}

void WaitSync(uint64_t op, const char* point) {
  Require(C().WaitFor(op, EventKind::kSync, point),
          std::string("timed out waiting for sync point ") + point);
}

void WaitLockBlock(uint64_t op, const char* lock) {
  Require(C().WaitFor(op, EventKind::kLockBlock, lock),
          std::string("timed out waiting for lock block on ") + lock);
}

void RunToEnd(uint64_t op) {
  Require(C().RunToCompletion(op), "operation did not complete");
}

void ResumeAndFinish(uint64_t op) {
  C().Resume(op);
  RunToEnd(op);
}

// Steps `op` through sync points until it parks on a wait for `lock`.
void RunUntil(uint64_t op, const char* target);

void ContinueToLock(uint64_t op, const char* lock) {
  for (;;) {
    xfs::conc::Event event;
    Require(C().WaitNext(op, &event),
            std::string("operation stopped before lock ") + lock);
    if (event.kind == EventKind::kSync) {
      C().Resume(op);
      continue;
    }
    if (event.kind == EventKind::kLockBlock) {
      Require(event.point == lock,
              std::string("unexpected lock wait on ") + event.point + "\n" +
                  C().Dump());
      return;
    }
    if (event.kind == EventKind::kOpExit) {
      throw xfstest::TestFailure(
          std::string("operation exited before lock wait ") + lock + "\n" +
          C().Dump());
    }
  }
}

// Starts `op` and leaves it parked on the named sync point, resuming every
// intermediate sync point on the way.
void PauseAt(uint64_t op, const char* point) {
  C().Start(op);
  RunUntil(op, point);
}

// Starts `op` and leaves it parked on a lock wait for `lock`.
void StartToLock(uint64_t op, const char* lock) {
  C().Start(op);
  ContinueToLock(op, lock);
}

// Resumes a parked operation and lets it finish. Lock waits still stop the
// operation, so callers must only use this when the operation can make
// progress; a stall shows up as a timeout with the full event dump.
void FinishOp(uint64_t op) {
  C().Resume(op);
  RunToEnd(op);
}

// Waits for a sync point and resumes the operation past it. Used to step a
// reader through the intermediate sync points that precede the one under
// test.
void AdvanceSync(uint64_t op, const char* point) {
  WaitSync(op, point);
  C().Resume(op);
}

// Resumes `op` through however many sync points it hits until `target` is
// reached; the operation is left blocked at that target. A lock wait on the
// way is a script error (the controller must resolve it explicitly).
void RunUntil(uint64_t op, const char* target) {
  for (;;) {
    xfs::conc::Event event;
    Require(C().WaitNext(op, &event),
            std::string("operation stopped before reaching ") + target);
    if (event.kind == EventKind::kSync && event.point == target) return;
    if (event.kind == EventKind::kSync) {
      C().Resume(op);
      continue;
    }
    if (event.kind == EventKind::kLockBlock) {
      throw xfstest::TestFailure(
          std::string("unexpected lock wait before ") + target + "\n" +
          C().Dump());
    }
    if (event.kind == EventKind::kOpExit) {
      throw xfstest::TestFailure(
          std::string("operation exited before reaching ") + target + "\n" +
          C().Dump());
    }
  }
}

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((i * 197u + seed) ^ (i >> 7));
  }
  return data;
}

}  // namespace

TEST(conc_scheduler_orders_lock_waiters_deterministically) {
  ScenarioGuard scenario;
  xfs::conc::Mutex mutex;
  std::vector<std::string> order;
  uint64_t a = C().Spawn("A", [&] {
    mutex.lock();
    order.push_back("A");
    xfs::conc::Sync("a.locked");
    mutex.unlock();
    xfs::conc::Sync("a.done");
  });
  uint64_t b = C().Spawn("B", [&] {
    mutex.lock();
    order.push_back("B");
    xfs::conc::Sync("b.locked");
    mutex.unlock();
  });

  C().Start(a);
  WaitSync(a, "a.locked");
  C().Start(b);
  WaitLockBlock(b, "mutex");
  C().Resume(a);
  WaitSync(a, "a.done");
  RunToEnd(a);
  C().Resume(b);
  WaitSync(b, "b.locked");
  RunToEnd(b);
  scenario.Finish();

  CHECK_EQ(order.size(), 2u);
  CHECK_STR_EQ(order[0], "A");
  CHECK_STR_EQ(order[1], "B");
  CHECK(!C().deadlocked());
}

TEST(conc_inode_cache_never_publishes_stale_parse) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  auto created = fs.CreateRegularFile(fs.RootInode(), "cold.bin", 0644, 0, 0);
  CHECK(created.ok());
  const uint64_t cold_ino = created.value();
  auto payload = Pattern(8192, 0x5a);
  auto wrote = fs.WriteFileData(cold_ino, 0, payload.data(), payload.size());
  CHECK(wrote.ok());
  const uint64_t ino = cold_ino;
  const uint64_t old_size = payload.size();

  ScenarioGuard scenario;
  xfs::XfsFileInfo observed;
  bool reader_ok = false;
  uint64_t reader = C().Spawn("reader", [&] {
    auto info = fs.GetFileInfo(ino);
    reader_ok = info.ok();
    if (info.ok()) observed = info.value();
  });
  uint64_t truncater = C().Spawn("truncate", [&] {
    auto status = fs.SetFileSize(ino, 0);
    CHECK(status.ok());
  });

  C().Start(reader);
  RunUntil(reader, "inode.read.after_device");
  // The reader holds write_barrier_ shared from GetFileInfo until it returns,
  // so the truncate cannot publish a new inode while the old chunk is in
  // flight. The reader finishes with the complete pre-truncate snapshot, and
  // only then may the truncate invalidate and replace it.
  StartToLock(truncater, "write_barrier_");
  ResumeAndFinish(reader);
  FinishOp(truncater);
  scenario.Finish();

  CHECK(reader_ok);
  CHECK_EQ(observed.size, old_size);
  auto info = fs.GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, 0u);
  CHECK_EQ(fs.OpenInode(ino).value().size, 0u);
}

TEST(conc_info_cache_never_publishes_stale_snapshot) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t ino = FixtureImage::Instance().info().file_ino;
  const uint64_t old_size = FixtureImage::Instance().info().file_size;

  ScenarioGuard scenario;
  xfs::XfsFileInfo observed;
  bool reader_ok = false;
  uint64_t reader = C().Spawn("reader", [&] {
    auto info = fs.GetFileInfo(ino);
    reader_ok = info.ok();
    if (info.ok()) observed = info.value();
  });
  uint64_t truncater = C().Spawn("truncate", [&] {
    auto status = fs.SetFileSize(ino, 0);
    CHECK(status.ok());
  });

  C().Start(reader);
  RunUntil(reader, "info.cache.insert.before");
  // The reader is about to publish the pre-truncate info while holding the
  // barrier shared. The exclusive truncate waits for that publication; the
  // generation check then replaces the entry after invalidation.
  StartToLock(truncater, "write_barrier_");
  ResumeAndFinish(reader);
  FinishOp(truncater);
  scenario.Finish();

  CHECK(reader_ok);
  CHECK_EQ(observed.size, old_size);
  auto info = fs.GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, 0u);
}

TEST(conc_directory_cache_never_publishes_stale_listing) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();

  ScenarioGuard scenario;
  std::vector<xfs::XfsDirEntry> listed_before;
  bool listed_ok = false;
  uint64_t reader = C().Spawn("readdir", [&] {
    auto entries = fs.ListDirectory(root);
    listed_ok = entries.ok();
    if (entries.ok()) listed_before = entries.value();
  });
  uint64_t creator = C().Spawn("create", [&] {
    auto created = fs.CreateRegularFile(root, "raced.bin", 0644, 0, 0);
    CHECK(created.ok());
  });

  C().Start(reader);
  AdvanceSync(reader, "dir.read.before_device");
  WaitSync(reader, "dir.read.after_device");
  // The reader holds the barrier shared through the parse and publication.
  // The exclusive create waits, so its dirent update cannot invalidate the
  // listing while the reader is still building it.
  StartToLock(creator, "write_barrier_");
  ResumeAndFinish(reader);
  FinishOp(creator);
  scenario.Finish();

  CHECK(listed_ok);
  CHECK(std::none_of(listed_before.begin(), listed_before.end(),
                     [](const xfs::XfsDirEntry& entry) {
                       return entry.name == "raced.bin";
                     }));
  auto looked_up = fs.LookupName(root, "raced.bin");
  CHECK(looked_up.ok());
  auto entries = fs.ListDirectory(root);
  CHECK(entries.ok());
  size_t found = 0;
  for (const auto& entry : entries.value()) {
    if (entry.name == "raced.bin") ++found;
  }
  CHECK_EQ(found, 1u);
}

TEST(conc_namespace_create_create_serializes_through_barrier) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  xfs::Status first;
  xfs::Status second;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("create-A", [&] {
    auto created = fs.CreateRegularFile(root, "same.bin", 0644, 0, 0);
    first = created.ok() ? xfs::OkStatus() : created.error();
  });
  uint64_t b = C().Spawn("create-B", [&] {
    auto created = fs.CreateRegularFile(root, "same.bin", 0644, 0, 0);
    second = created.ok() ? xfs::OkStatus() : created.error();
  });

  C().Start(a);
  WaitSync(a, "namespace.barrier.before");
  C().Resume(a);
  AdvanceSync(a, "namespace.barrier.after");
  WaitSync(a, "txn.begin");
  C().Start(b);
  WaitSync(b, "namespace.barrier.before");
  C().Resume(b);
  WaitLockBlock(b, "write_barrier_");
  RunToEnd(a);
  C().Resume(b);
  RunToEnd(b);
  scenario.Finish();

  const bool a_ok = first.ok();
  const bool b_ok = second.ok();
  CHECK(a_ok != b_ok);
  const xfs::Status& failed = a_ok ? second : first;
  CHECK_EQ(failed.error().code, xfs::XfsError::kAlreadyExists);
  auto entries = fs.ListDirectory(root);
  CHECK(entries.ok());
  size_t found = 0;
  for (const auto& entry : entries.value()) {
    if (entry.name == "same.bin") ++found;
  }
  CHECK_EQ(found, 1u);
}

TEST(conc_namespace_create_unlink_order_is_serializable) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  xfs::Status created;
  xfs::Status removed;

  ScenarioScope scenario;
  uint64_t creator = C().Spawn("create", [&] {
    auto result = fs.CreateRegularFile(root, "lifetime.bin", 0644, 0, 0);
    created = result.ok() ? xfs::OkStatus() : result.error();
  });
  uint64_t remover = C().Spawn("unlink", [&] {
    auto result = fs.Unlink(root, "lifetime.bin");
    removed = result.ok() ? xfs::OkStatus() : result.error();
  });

  C().Start(creator);
  WaitSync(creator, "namespace.barrier.before");
  C().Resume(creator);
  AdvanceSync(creator, "namespace.barrier.after");
  WaitSync(creator, "txn.begin");
  C().Start(remover);
  WaitSync(remover, "namespace.barrier.before");
  C().Resume(remover);
  WaitLockBlock(remover, "write_barrier_");
  RunToEnd(creator);
  C().Resume(remover);
  RunToEnd(remover);
  scenario.Finish();

  // Serial order create -> unlink: both succeed and the name is gone. That is
  // the only legal history for this interleaving.
  CHECK(created.ok());
  CHECK(removed.ok());
  auto lookup = fs.LookupName(root, "lifetime.bin");
  CHECK(!lookup.ok());
  CHECK_EQ(lookup.error().code, xfs::XfsError::kNotFound);
}

TEST(conc_same_inode_writes_serialize_on_the_inode_stripe) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t ino = FixtureImage::Instance().info().file_ino;
  auto first_data = Pattern(4096, 0x11);
  auto second_data = Pattern(4096, 0x22);

  ScenarioGuard scenario;
  uint64_t first = C().Spawn("write-A", [&] {
    auto wrote =
        fs.WriteFileData(ino, 0, first_data.data(), first_data.size());
    CHECK(wrote.ok());
  });
  uint64_t second = C().Spawn("write-B", [&] {
    auto wrote =
        fs.WriteFileData(ino, 0, second_data.data(), second_data.size());
    CHECK(wrote.ok());
  });

  C().Start(first);
  RunUntil(first, "data.write.before");
  C().Start(second);
  WaitSync(second, "barrier.before");
  C().Resume(second);
  // Shared barrier holders are compatible, so B reaches the stripe attempt
  // where A (a different operation) still holds the inode lock.
  AdvanceSync(second, "inode.lock.before");
  WaitLockBlock(second, "inode_stripe");
  RunToEnd(first);
  C().Resume(second);
  RunToEnd(second);
  scenario.Finish();

  std::vector<uint8_t> actual(second_data.size());
  auto read = fs.ReadFile(ino, 0, actual.data(), actual.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), actual.size());
  // B acquired the stripe after A completed, so B's bytes are final. A mixed
  // result would mean the two data transfers overlapped.
  CHECK(actual == second_data);
}

TEST(conc_rename_opposite_directions_do_not_deadlock) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  for (const char* name : {"a.bin", "b.bin"}) {
    auto created = fs.CreateRegularFile(root, name, 0644, 0, 0);
    CHECK(created.ok());
  }
  xfs::Status ab;
  xfs::Status ba;

  ScenarioScope scenario;
  uint64_t a_to_b = C().Spawn("rename-a-to-b", [&] {
    auto result = fs.Rename(root, "a.bin", root, "b.bin", /*replace_existing=*/true);
    ab = result.ok() ? xfs::OkStatus() : result.error();
  });
  uint64_t b_to_a = C().Spawn("rename-b-to-a", [&] {
    auto result = fs.Rename(root, "b.bin", root, "a.bin", /*replace_existing=*/true);
    ba = result.ok() ? xfs::OkStatus() : result.error();
  });

  C().Start(a_to_b);
  WaitSync(a_to_b, "namespace.barrier.before");
  C().Resume(a_to_b);
  AdvanceSync(a_to_b, "namespace.barrier.after");
  WaitSync(a_to_b, "txn.begin");
  C().Start(b_to_a);
  WaitSync(b_to_a, "namespace.barrier.before");
  C().Resume(b_to_a);
  WaitLockBlock(b_to_a, "write_barrier_");
  RunToEnd(a_to_b);
  C().Resume(b_to_a);
  RunToEnd(b_to_a);
  scenario.Finish();

  CHECK(ab.ok());
  CHECK(ba.ok());
  // Serial a->b then b->a: the inode formerly named a ends up at a, b is
  // gone. The reverse history would leave only b. Either way exactly one of
  // the two names survives, never both or neither.
  const bool has_a = fs.LookupName(root, "a.bin").ok();
  const bool has_b = fs.LookupName(root, "b.bin").ok();
  CHECK(has_a != has_b);
}

// ---------------------------------------------------------------------------
// CONC-1 scenario building blocks and the operation matrix.
// ---------------------------------------------------------------------------

namespace {

struct ConcImage {
  std::unique_ptr<xfstest::MemoryBlockDevice> device;
  std::unique_ptr<xfs::XfsFilesystem> fs;
};

ConcImage MakeConcImage(bool reflink = false,
                        uint64_t size_bytes = 300ull << 20) {
  ConcImage image;
  image.device = std::make_unique<xfstest::MemoryBlockDevice>(size_bytes);
  xfs::mkfs::FormatOptions options;
  options.quiet = true;
  options.reflink = reflink;
  options.rmapbt = reflink;  // exercise owned allocation together with CoW
  auto formatted = xfs::mkfs::Format(*image.device, options);
  if (!formatted.ok()) {
    throw xfstest::TestFailure("format failed: " +
                               formatted.error().Format());
  }
  xfs::MountOptions mount_options;
  mount_options.read_write = true;
  auto mounted = xfs::XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get()),
      mount_options);
  if (!mounted.ok()) {
    throw xfstest::TestFailure("mount failed: " + mounted.error().Format());
  }
  image.fs = std::move(mounted.value());
  return image;
}

void RequireOk(const xfs::Status& status, const char* what) {
  if (!status.ok()) {
    throw xfstest::TestFailure(std::string(what) + ": " +
                               status.error().Format());
  }
}

template <typename T>
T Take(xfs::Result<T> result, const char* what) {
  if (!result.ok()) {
    throw xfstest::TestFailure(std::string(what) + ": " +
                               result.error().Format());
  }
  return std::move(result.value());
}

uint64_t MakeFile(xfs::XfsFilesystem& fs, uint64_t parent, const char* name) {
  auto created = fs.CreateRegularFile(parent, name, 0644, 0, 0);
  return Take(std::move(created), name);
}

void RequireConsistent(xfs::XfsFilesystem& fs, const char* what) {
  // A mounted filesystem writes metadata buffers ahead of the superblock LSN
  // (that is the durable-log contract); the LSN check only accepts an image
  // whose log was closed and whose superblock covers every metadata LSN. End
  // the scenario with the same clean-unmount boundary the external oracles
  // use before judging the image.
  RequireOk(fs.WriteCleanUnmountRecord(), "clean unmount before check");
  auto report = fs.CheckConsistency(xfs::CheckOptions{});
  if (report.Overall() != xfs::CheckStatus::kOk) {
    std::string detail = std::string(what) +
                         ": consistency check failed: " + report.Summary();
    for (const auto& item : report.items) {
      if (item.status != xfs::CheckStatus::kOk) {
        detail += "\n  " + item.name + ": " + item.message;
      }
    }
    throw xfstest::TestFailure(detail);
  }
}

bool DirectoryContains(xfs::XfsFilesystem& fs, uint64_t dir,
                       const char* name) {
  auto listed = fs.ListDirectory(dir);
  if (!listed.ok()) {
    throw xfstest::TestFailure("ListDirectory failed: " +
                               listed.error().Format());
  }
  for (const auto& entry : listed.value()) {
    if (entry.name == name) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Namespace operations: every namespace mutation takes write_barrier_
// exclusively, so opposite directions serialize instead of overlapping. The
// tests still drive real operations through the scheduler and verify the
// resulting history is one of the legal serial orders.
// ---------------------------------------------------------------------------

TEST(conc_namespace_unlink_unlink_serializes) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  MakeFile(fs, root, "gone.bin");
  xfs::Status first;
  xfs::Status second;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("unlink-A", [&] {
    first = fs.Unlink(root, "gone.bin");
  });
  uint64_t b = C().Spawn("unlink-B", [&] {
    second = fs.Unlink(root, "gone.bin");
  });

  PauseAt(a, "txn.begin");
  C().Start(b);
  ContinueToLock(b, "write_barrier_");
  RunToEnd(a);
  FinishOp(b);
  scenario.Finish();

  CHECK(first.ok());
  CHECK(!second.ok());
  CHECK_EQ(second.error().code, xfs::XfsError::kNotFound);
  CHECK(!fs.LookupName(root, "gone.bin").ok());
  RequireConsistent(fs, "unlink+unlink");
}

TEST(conc_namespace_rename_lookup_sees_old_then_new_name) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "old.bin");
  const bool old_before = fs.LookupName(root, "old.bin").ok();
  const bool new_before = fs.LookupName(root, "new.bin").ok();
  xfs::Status renamed;

  ScenarioScope scenario;
  uint64_t writer = C().Spawn("rename", [&] {
    auto result = fs.Rename(root, "old.bin", root, "new.bin",
                            /*replace_existing=*/false);
    renamed = result.ok() ? xfs::OkStatus() : result.error();
  });
  bool old_during = false;
  bool new_during = false;
  uint64_t reader = C().Spawn("lookup", [&] {
    old_during = fs.LookupName(root, "old.bin").ok();
    new_during = fs.LookupName(root, "new.bin").ok();
  });

  PauseAt(writer, "txn.begin");
  // The rename holds the barrier exclusively. The lookup waits until the
  // whole transaction has committed, so it can only observe one of the two
  // legal names, never the in-transaction dirent update.
  StartToLock(reader, "write_barrier_");
  RunToEnd(writer);
  FinishOp(reader);
  scenario.Finish();

  CHECK(renamed.ok());
  CHECK(old_before);
  CHECK(!new_before);
  CHECK(!old_during);
  CHECK(new_during);
  CHECK(!fs.LookupName(root, "old.bin").ok());
  auto found = fs.LookupName(root, "new.bin");
  CHECK(found.ok());
  CHECK_EQ(found.value().ino, ino);
  RequireConsistent(fs, "rename+lookup");
}

TEST(conc_namespace_rename_replace_with_unlink) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t source = MakeFile(fs, root, "source.bin");
  MakeFile(fs, root, "victim.bin");
  xfs::Status renamed;
  xfs::Status removed;

  ScenarioScope scenario;
  uint64_t renamer = C().Spawn("rename-replace", [&] {
    auto result = fs.Rename(root, "source.bin", root, "victim.bin",
                            /*replace_existing=*/true);
    renamed = result.ok() ? xfs::OkStatus() : result.error();
  });
  uint64_t unlinker = C().Spawn("unlink-victim", [&] {
    removed = fs.Unlink(root, "victim.bin");
  });

  PauseAt(renamer, "txn.begin");
  C().Start(unlinker);
  ContinueToLock(unlinker, "write_barrier_");
  RunToEnd(renamer);
  FinishOp(unlinker);
  scenario.Finish();

  CHECK(renamed.ok());
  CHECK(removed.ok());
  CHECK(!fs.LookupName(root, "source.bin").ok());
  CHECK(!fs.LookupName(root, "victim.bin").ok());
  CHECK(!fs.LookupName(root, "victim.bin").ok());
  // The renamed inode was the one the unlink removed.
  auto inode = fs.OpenInode(source);
  CHECK(!inode.ok() || inode.value().nlink == 0);
  RequireConsistent(fs, "rename replace+unlink");
}

TEST(conc_namespace_hardlink_unlink_nlink_lifecycle) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "link-a.bin");
  RequireOk(fs.Link(ino, root, "link-b.bin"), "hardlink");
  auto before = fs.OpenInode(ino);
  CHECK(before.ok());
  CHECK_EQ(before.value().nlink, 2u);
  xfs::Status first;
  xfs::Status second;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("unlink-a", [&] {
    first = fs.Unlink(root, "link-a.bin");
  });
  uint64_t b = C().Spawn("unlink-b", [&] {
    second = fs.Unlink(root, "link-b.bin");
  });

  PauseAt(a, "txn.begin");
  C().Start(b);
  ContinueToLock(b, "write_barrier_");
  RunToEnd(a);
  FinishOp(b);
  scenario.Finish();

  CHECK(first.ok());
  CHECK(second.ok());
  CHECK(!fs.LookupName(root, "link-a.bin").ok());
  CHECK(!fs.LookupName(root, "link-b.bin").ok());
  RequireConsistent(fs, "hardlink unlink+unlink");
}

TEST(conc_namespace_hardlink_rename_both_names_survive) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "ln-a.bin");
  RequireOk(fs.Link(ino, root, "ln-b.bin"), "hardlink");
  xfs::Status first;
  xfs::Status second;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("rename-a", [&] {
    auto r = fs.Rename(root, "ln-a.bin", root, "ln-x.bin", false);
    first = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t b = C().Spawn("rename-b", [&] {
    auto r = fs.Rename(root, "ln-b.bin", root, "ln-y.bin", false);
    second = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(a, "txn.begin");
  C().Start(b);
  ContinueToLock(b, "write_barrier_");
  RunToEnd(a);
  FinishOp(b);
  scenario.Finish();

  CHECK(first.ok());
  CHECK(second.ok());
  auto x = fs.LookupName(root, "ln-x.bin");
  auto y = fs.LookupName(root, "ln-y.bin");
  CHECK(x.ok());
  CHECK(y.ok());
  CHECK_EQ(x.value().ino, ino);
  CHECK_EQ(y.value().ino, ino);
  auto inode = fs.OpenInode(ino);
  CHECK(inode.ok());
  CHECK_EQ(inode.value().nlink, 2u);
  RequireConsistent(fs, "hardlink rename+rename");
}

TEST(conc_namespace_rename_opposite_directions_cross_directory) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  RequireOk(fs.Mkdir(root, "d1", 0755, 0, 0), "mkdir d1");
  RequireOk(fs.Mkdir(root, "d2", 0755, 0, 0), "mkdir d2");
  const uint64_t d1 = Take(fs.LookupName(root, "d1"), "lookup d1").ino;
  const uint64_t d2 = Take(fs.LookupName(root, "d2"), "lookup d2").ino;
  const uint64_t ino = MakeFile(fs, d1, "move.bin");
  xfs::Status forward;
  xfs::Status backward;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("rename-d1-to-d2", [&] {
    auto r = fs.Rename(d1, "move.bin", d2, "move.bin", false);
    forward = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t b = C().Spawn("rename-d2-to-d1", [&] {
    auto r = fs.Rename(d2, "move.bin", d1, "move.bin", false);
    backward = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(a, "txn.begin");
  C().Start(b);
  ContinueToLock(b, "write_barrier_");
  RunToEnd(a);
  FinishOp(b);
  scenario.Finish();

  CHECK(forward.ok());
  CHECK(backward.ok());
  auto d1_entry = fs.LookupName(d1, "move.bin");
  CHECK(d1_entry.ok());
  CHECK_EQ(d1_entry.value().ino, ino);
  CHECK(!fs.LookupName(d2, "move.bin").ok());
  RequireConsistent(fs, "cross-directory rename both directions");
}

TEST(conc_namespace_rmdir_races_child_create) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  RequireOk(fs.Mkdir(root, "doomed", 0755, 0, 0), "mkdir doomed");
  const uint64_t dir = Take(fs.LookupName(root, "doomed"), "lookup dir").ino;
  xfs::Status created;
  xfs::Status removed;

  ScenarioScope scenario;
  uint64_t creator = C().Spawn("create-child", [&] {
    auto r = fs.CreateRegularFile(dir, "child.bin", 0644, 0, 0);
    created = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t remover = C().Spawn("rmdir", [&] {
    removed = fs.RemoveEmptyDirectory(root, "doomed");
  });

  PauseAt(creator, "txn.begin");
  C().Start(remover);
  ContinueToLock(remover, "write_barrier_");
  RunToEnd(creator);
  FinishOp(remover);
  scenario.Finish();

  CHECK(created.ok());
  CHECK(!removed.ok());
  CHECK_EQ(removed.error().code, xfs::XfsError::kDirectoryNotEmpty);
  CHECK(fs.LookupName(root, "doomed").ok());
  CHECK(fs.LookupName(dir, "child.bin").ok());
  RequireConsistent(fs, "rmdir+create");
}

TEST(conc_namespace_rmdir_then_child_remove) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  RequireOk(fs.Mkdir(root, "empty", 0755, 0, 0), "mkdir empty");
  const uint64_t dir = Take(fs.LookupName(root, "empty"), "lookup dir").ino;
  MakeFile(fs, dir, "child.bin");
  xfs::Status unlinked;
  xfs::Status removed;

  ScenarioScope scenario;
  uint64_t unlinker = C().Spawn("unlink-child", [&] {
    unlinked = fs.Unlink(dir, "child.bin");
  });
  uint64_t remover = C().Spawn("rmdir", [&] {
    removed = fs.RemoveEmptyDirectory(root, "empty");
  });

  PauseAt(unlinker, "txn.begin");
  C().Start(remover);
  ContinueToLock(remover, "write_barrier_");
  RunToEnd(unlinker);
  FinishOp(remover);
  scenario.Finish();

  CHECK(unlinked.ok());
  CHECK(removed.ok());
  CHECK(!fs.LookupName(root, "empty").ok());
  RequireConsistent(fs, "rmdir+child unlink");
}

TEST(conc_namespace_lookup_and_readdir_during_create) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const bool missing_before = !fs.LookupName(root, "fresh.bin").ok();
  xfs::Status created;

  ScenarioScope scenario;
  uint64_t creator = C().Spawn("create", [&] {
    auto r = fs.CreateRegularFile(root, "fresh.bin", 0644, 0, 0);
    created = r.ok() ? xfs::OkStatus() : r.error();
  });
  bool lookup_during = false;
  bool listed_during = false;
  uint64_t reader = C().Spawn("lookup+readdir", [&] {
    lookup_during = fs.LookupName(root, "fresh.bin").ok();
    listed_during = DirectoryContains(fs, root, "fresh.bin");
  });

  PauseAt(creator, "dirent.update.after");
  // The dirent update is inside the create transaction; the reader cannot
  // enter the namespace while the exclusive barrier is held, so it starts
  // after the commit and sees the complete new entry.
  StartToLock(reader, "write_barrier_");
  RunToEnd(creator);
  FinishOp(reader);
  scenario.Finish();

  CHECK(missing_before);
  CHECK(created.ok());
  CHECK(lookup_during);
  CHECK(listed_during);
  CHECK(fs.LookupName(root, "fresh.bin").ok());
  CHECK(DirectoryContains(fs, root, "fresh.bin"));
  RequireConsistent(fs, "lookup/readdir serialized with create");
}

TEST(conc_namespace_readdir_during_unlink) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  MakeFile(fs, root, "leaving.bin");
  const bool present_before = DirectoryContains(fs, root, "leaving.bin");
  xfs::Status removed;

  ScenarioScope scenario;
  uint64_t remover = C().Spawn("unlink", [&] {
    removed = fs.Unlink(root, "leaving.bin");
  });
  bool listed_during = true;
  uint64_t reader = C().Spawn("readdir", [&] {
    listed_during = DirectoryContains(fs, root, "leaving.bin");
  });

  PauseAt(remover, "dirent.update.after");
  // The unlink owns the barrier exclusively. The readdir waits for the whole
  // unlink, so it cannot see the half-applied dirent state.
  StartToLock(reader, "write_barrier_");
  RunToEnd(remover);
  FinishOp(reader);
  scenario.Finish();

  CHECK(present_before);
  CHECK(removed.ok());
  CHECK(!listed_during);
  CHECK(!DirectoryContains(fs, root, "leaving.bin"));
  RequireConsistent(fs, "readdir serialized with unlink");
}

// ---------------------------------------------------------------------------
// File data: ordinary writes share write_barrier_ and serialize per inode on
// the inode stripe; truncate/preallocate/convert take the barrier exclusively.
// ---------------------------------------------------------------------------

TEST(conc_two_nonoverlapping_writes_both_land) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "two-writes.bin");
  auto first_data = Pattern(4096, 0x31);
  auto second_data = Pattern(4096, 0x72);

  ScenarioScope scenario;
  uint64_t first = C().Spawn("write-low", [&] {
    auto r = fs.WriteFileData(ino, 0, first_data.data(), first_data.size());
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "write low");
  });
  uint64_t second = C().Spawn("write-high", [&] {
    auto r = fs.WriteFileData(ino, 4096, second_data.data(),
                              second_data.size());
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "write high");
  });

  PauseAt(first, "data.write.before");
  C().Start(second);
  WaitSync(second, "barrier.before");
  C().Resume(second);
  AdvanceSync(second, "inode.lock.before");
  ContinueToLock(second, "inode_stripe");
  RunToEnd(first);
  FinishOp(second);
  scenario.Finish();

  std::vector<uint8_t> actual(8192);
  auto read = fs.ReadFile(ino, 0, actual.data(), actual.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), actual.size());
  std::vector<uint8_t> expected = first_data;
  expected.insert(expected.end(), second_data.begin(), second_data.end());
  CHECK(actual == expected);
  RequireConsistent(fs, "two non-overlapping writes");
}

TEST(conc_read_during_write_sees_old_then_new_bytes) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "rw-race.bin");
  auto old_data = Pattern(4096, 0x10);
  auto new_data = Pattern(4096, 0x99);
  auto wrote = fs.WriteFileData(ino, 0, old_data.data(), old_data.size());
  CHECK(wrote.ok());

  ScenarioScope scenario;
  uint64_t writer = C().Spawn("write", [&] {
    auto r = fs.WriteFileData(ino, 0, new_data.data(), new_data.size());
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "write");
  });
  std::vector<uint8_t> during(4096);
  size_t during_len = 0;

  PauseAt(writer, "data.write.before");
  auto during_read = fs.ReadFile(ino, 0, during.data(), during.size());
  if (during_read.ok()) during_len = during_read.value();
  RunToEnd(writer);
  scenario.Finish();

  CHECK_EQ(during_len, during.size());
  CHECK(during == old_data);
  std::vector<uint8_t> after(4096);
  auto after_read = fs.ReadFile(ino, 0, after.data(), after.size());
  CHECK(after_read.ok());
  CHECK_EQ(after_read.value(), after.size());
  CHECK(after == new_data);
  RequireConsistent(fs, "read during write");
}

TEST(conc_write_blocks_until_truncate_finishes) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "trunc-vs-write.bin");
  auto data = Pattern(4096, 0x42);
  CHECK(fs.WriteFileData(ino, 0, data.data(), data.size()).ok());
  xfs::Status wrote;

  ScenarioScope scenario;
  uint64_t truncater = C().Spawn("truncate", [&] {
    auto r = fs.SetFileSize(ino, 0);
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "truncate");
  });
  uint64_t writer = C().Spawn("write", [&] {
    auto r = fs.WriteFileData(ino, 0, data.data(), data.size());
    wrote = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(truncater, "inode.lock.after");
  C().Start(writer);
  WaitSync(writer, "barrier.before");
  C().Resume(writer);
  ContinueToLock(writer, "write_barrier_");
  RunToEnd(truncater);
  FinishOp(writer);
  scenario.Finish();

  CHECK(wrote.ok());
  auto info = fs.GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, data.size());
  RequireConsistent(fs, "truncate then write");
}

TEST(conc_extent_cache_never_publishes_stale_parse) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "stale-extent.bin");
  auto data = Pattern(8192, 0x77);
  CHECK(fs.WriteFileData(ino, 0, data.data(), data.size()).ok());

  ScenarioScope scenario;
  std::vector<xfs::XfsExtent> loaded_before;
  bool reader_ok = false;
  uint64_t reader = C().Spawn("extents", [&] {
    auto loaded = fs.GetDataExtents(ino);
    reader_ok = loaded.ok();
    if (loaded.ok()) loaded_before = loaded.value();
  });
  uint64_t truncater = C().Spawn("truncate", [&] {
    auto r = fs.SetFileSize(ino, 0);
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "truncate");
  });

  PauseAt(reader, "extent.read.after_device");
  // GetDataExtents holds write_barrier_ shared until it returns. The
  // truncate waits for that publication instead of freeing the blocks the
  // reader just parsed.
  StartToLock(truncater, "write_barrier_");
  ResumeAndFinish(reader);
  FinishOp(truncater);
  scenario.Finish();

  CHECK(reader_ok);
  CHECK(!loaded_before.empty());
  auto extents = fs.GetDataExtents(ino);
  CHECK(extents.ok());
  CHECK(extents.value().empty());
  auto info = fs.GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, 0u);
  RequireConsistent(fs, "extent cache serialized with truncate");
}

TEST(conc_ordinary_write_waits_for_extent_map_reader) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t ino = MakeFile(fs, fs.RootInode(), "map-reader.bin");
  auto first = Pattern(4096, 0x31);
  auto second = Pattern(4096, 0x72);
  CHECK(fs.WriteFileData(ino, 0, first.data(), first.size()).ok());

  ScenarioScope scenario;
  std::vector<uint8_t> observed(4096);
  size_t read_len = 0;
  bool reader_ok = false;
  uint64_t reader = C().Spawn("reader", [&] {
    auto r = fs.ReadFile(ino, 0, observed.data(), observed.size());
    reader_ok = r.ok();
    if (r.ok()) read_len = r.value();
  });
  uint64_t writer = C().Spawn("extend", [&] {
    auto r = fs.WriteFileData(ino, first.size(), second.data(), second.size());
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "extend");
  });

  // The reader holds write_barrier_ shared and the inode's extent-map lock
  // shared for the whole parse and data read. The ordinary extending write
  // has to change the extent map (Step 1 allocation); it must wait for the
  // parse to finish before it can insert into the BMBT.
  PauseAt(reader, "extent.read.before_device");
  StartToLock(writer, "extent_map");
  ResumeAndFinish(reader);
  FinishOp(writer);
  scenario.Finish();

  CHECK(reader_ok);
  CHECK_EQ(read_len, first.size());
  CHECK(observed == first);

  std::vector<uint8_t> final_data(first.size() + second.size());
  auto read = fs.ReadFile(ino, 0, final_data.data(), final_data.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), final_data.size());
  CHECK(std::equal(first.begin(), first.end(), final_data.begin()));
  CHECK(std::equal(second.begin(), second.end(),
                   final_data.begin() + first.size()));
  auto info = fs.GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, first.size() + second.size());
  RequireConsistent(fs, "ordinary write serialized with extent-map reader");
}

TEST(conc_conversion_waits_for_extent_map_reader) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t ino = MakeFile(fs, fs.RootInode(), "map-convert.bin");
  auto first = Pattern(4096, 0x41);
  auto second = Pattern(4096, 0x52);
  CHECK(fs.WriteFileData(ino, 0, first.data(), first.size()).ok());

  ScenarioScope scenario;
  std::vector<uint8_t> observed(4096);
  size_t read_len = 0;
  bool reader_ok = false;
  uint64_t reader = C().Spawn("reader", [&] {
    auto r = fs.ReadFile(ino, 0, observed.data(), observed.size());
    reader_ok = r.ok();
    if (r.ok()) read_len = r.value();
  });
  uint64_t writer = C().Spawn("convert", [&] {
    auto r = fs.WriteFileData(ino, 4096, second.data(), second.size());
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "convert");
  });

  // Step 1 allocates the hole, commits and invalidates the inode/extent
  // caches, then releases the extent-map lock. Pause the writer after that
  // point, then let a cold reader start parsing the fork. Step 3 is the
  // transaction that removes the unwritten record and can free bmbt blocks;
  // it must wait for the reader's parse.
  PauseAt(writer, "data.write.prepare");
  PauseAt(reader, "extent.read.before_device");
  C().Resume(writer);
  ContinueToLock(writer, "extent_map");
  ResumeAndFinish(reader);
  FinishOp(writer);
  scenario.Finish();

  CHECK(reader_ok);
  CHECK_EQ(read_len, first.size());
  CHECK(observed == first);

  std::vector<uint8_t> final_data(first.size() + second.size());
  auto read = fs.ReadFile(ino, 0, final_data.data(), final_data.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), final_data.size());
  CHECK(std::equal(first.begin(), first.end(), final_data.begin()));
  CHECK(std::equal(second.begin(), second.end(),
                   final_data.begin() + first.size()));
  auto info = fs.GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, first.size() + second.size());
  RequireConsistent(fs, "conversion serialized with extent-map reader");
}

TEST(conc_maintenance_check_waits_for_writer) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t ino = MakeFile(fs, fs.RootInode(), "check-race.bin");
  auto data = Pattern(4096, 0x63);

  ScenarioScope scenario;
  bool checked = false;
  uint64_t writer = C().Spawn("write", [&] {
    auto r = fs.WriteFileData(ino, 0, data.data(), data.size());
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "write");
  });
  uint64_t checker = C().Spawn("check", [&] {
    auto report = fs.CheckConsistency(xfs::CheckOptions{});
    (void)report;
    checked = true;
  });

  // The writer holds write_barrier_ shared for the whole write. The
  // maintenance check takes it exclusively, so it must wait for the write
  // (including its metadata commit) to finish instead of walking AGF/btree
  // state while the transaction is writing it back.
  PauseAt(writer, "data.write.before");
  StartToLock(checker, "write_barrier_");
  ResumeAndFinish(writer);
  FinishOp(checker);
  scenario.Finish();

  CHECK(checked);
  RequireConsistent(fs, "maintenance check serialized with writer");
}

TEST(conc_preallocate_then_write_serializes) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "prealloc.bin");
  auto data = Pattern(4096, 0x23);
  xfs::Status allocated;
  xfs::Status wrote;

  ScenarioScope scenario;
  uint64_t prealloc = C().Spawn("preallocate", [&] {
    auto r = fs.AllocateUnwritten(ino, 0, 8192);
    allocated = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t writer = C().Spawn("write", [&] {
    auto r = fs.WriteFileData(ino, 0, data.data(), data.size());
    wrote = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(prealloc, "inode.lock.after");
  C().Start(writer);
  WaitSync(writer, "barrier.before");
  C().Resume(writer);
  ContinueToLock(writer, "write_barrier_");
  RunToEnd(prealloc);
  FinishOp(writer);
  scenario.Finish();

  CHECK(allocated.ok());
  CHECK(wrote.ok());
  std::vector<uint8_t> actual(4096);
  auto read = fs.ReadFile(ino, 0, actual.data(), actual.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), actual.size());
  CHECK(actual == data);
  RequireConsistent(fs, "preallocate+write");
}

TEST(conc_convert_unwritten_then_write_serializes) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "convert.bin");
  CHECK(fs.AllocateUnwritten(ino, 0, 4096).ok());
  auto data = Pattern(4096, 0x64);
  xfs::Status converted;
  xfs::Status wrote;

  ScenarioScope scenario;
  uint64_t convert = C().Spawn("convert", [&] {
    auto r = fs.ConvertUnwritten(ino, 0, 4096);
    converted = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t writer = C().Spawn("write", [&] {
    auto r = fs.WriteFileData(ino, 0, data.data(), data.size());
    wrote = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(convert, "inode.lock.after");
  C().Start(writer);
  WaitSync(writer, "barrier.before");
  C().Resume(writer);
  ContinueToLock(writer, "write_barrier_");
  RunToEnd(convert);
  FinishOp(writer);
  scenario.Finish();

  CHECK(converted.ok());
  CHECK(wrote.ok());
  std::vector<uint8_t> actual(4096);
  auto read = fs.ReadFile(ino, 0, actual.data(), actual.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), actual.size());
  CHECK(actual == data);
  RequireConsistent(fs, "convert+write");
}

TEST(conc_extend_then_truncate_leaves_a_legal_state) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "extend-truncate.bin");
  auto first = Pattern(4096, 0x71);
  auto second = Pattern(4096, 0x72);
  CHECK(fs.WriteFileData(ino, 0, first.data(), first.size()).ok());
  xfs::Status extended;
  xfs::Status truncated;

  ScenarioScope scenario;
  uint64_t extender = C().Spawn("extend", [&] {
    auto r = fs.WriteFileData(ino, 4096, second.data(), second.size());
    extended = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t truncater = C().Spawn("truncate", [&] {
    auto r = fs.SetFileSize(ino, 4096);
    truncated = r.ok() ? xfs::OkStatus() : r.error();
  });

  // The extending write holds the shared barrier and the inode stripe. The
  // truncate must wait for it (exclusive barrier) and then shrink the file the
  // extend produced; the final state is exactly "extend, then truncate".
  PauseAt(extender, "inode.lock.after");
  C().Start(truncater);
  WaitSync(truncater, "truncate.barrier.before");
  C().Resume(truncater);
  ContinueToLock(truncater, "write_barrier_");
  RunToEnd(extender);
  FinishOp(truncater);
  scenario.Finish();

  CHECK(extended.ok());
  CHECK(truncated.ok());
  auto info = fs.GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, 4096u);
  std::vector<uint8_t> actual(4096);
  auto read = fs.ReadFile(ino, 0, actual.data(), actual.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), actual.size());
  CHECK(actual == first);
  std::vector<uint8_t> past(4096);
  auto eof = fs.ReadFile(ino, 4096, past.data(), past.size());
  CHECK(eof.ok());
  CHECK_EQ(eof.value(), 0u);
  RequireConsistent(fs, "extend then truncate");
}

TEST(conc_truncate_grow_then_write_serializes) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "grow-write.bin");
  auto first = Pattern(4096, 0x73);
  auto second = Pattern(4096, 0x74);
  CHECK(fs.WriteFileData(ino, 0, first.data(), first.size()).ok());
  xfs::Status grown;
  xfs::Status wrote;

  ScenarioScope scenario;
  uint64_t grower = C().Spawn("grow", [&] {
    auto r = fs.SetFileSize(ino, 8192);
    grown = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t writer = C().Spawn("write", [&] {
    auto r = fs.WriteFileData(ino, 4096, second.data(), second.size());
    wrote = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(grower, "inode.lock.after");
  C().Start(writer);
  WaitSync(writer, "barrier.before");
  C().Resume(writer);
  ContinueToLock(writer, "write_barrier_");
  RunToEnd(grower);
  FinishOp(writer);
  scenario.Finish();

  CHECK(grown.ok());
  CHECK(wrote.ok());
  auto info = fs.GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, 8192u);
  std::vector<uint8_t> actual(8192);
  auto read = fs.ReadFile(ino, 0, actual.data(), actual.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), actual.size());
  CHECK(std::equal(first.begin(), first.end(), actual.begin()));
  CHECK(std::equal(second.begin(), second.end(), actual.begin() + 4096));
  RequireConsistent(fs, "truncate grow then write");
}

TEST(conc_read_while_truncate_smaller_is_stable) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "shrink-read.bin");
  auto first = Pattern(4096, 0x75);
  auto second = Pattern(4096, 0x76);
  CHECK(fs.WriteFileData(ino, 0, first.data(), first.size()).ok());
  CHECK(fs.WriteFileData(ino, 4096, second.data(), second.size()).ok());

  ScenarioScope scenario;
  uint64_t truncater = C().Spawn("truncate", [&] {
    auto r = fs.SetFileSize(ino, 4096);
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "truncate");
  });
  PauseAt(truncater, "inode.lock.after");

  // The truncate holds the barrier exclusively and is parked before it has
  // changed the inode. The reader waits on the shared side, so it can never
  // observe a half-shrunk extent map or touch a block the truncate frees.
  std::vector<uint8_t> during(8192);
  size_t during_len = 0;
  bool during_ok = false;
  uint64_t reader = C().Spawn("read", [&] {
    auto read = fs.ReadFile(ino, 0, during.data(), during.size());
    during_ok = read.ok();
    if (read.ok()) during_len = read.value();
  });
  StartToLock(reader, "write_barrier_");
  RunToEnd(truncater);
  FinishOp(reader);
  scenario.Finish();

  CHECK(during_ok);
  CHECK_EQ(during_len, 4096u);
  CHECK(std::equal(first.begin(), first.end(), during.begin()));

  auto info = fs.GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, 4096u);
  std::vector<uint8_t> after(8192);
  auto after_read = fs.ReadFile(ino, 0, after.data(), after.size());
  CHECK(after_read.ok());
  CHECK_EQ(after_read.value(), 4096u);
  CHECK(std::equal(first.begin(), first.end(), after.begin()));
  std::vector<uint8_t> past(4096);
  auto eof = fs.ReadFile(ino, 4096, past.data(), past.size());
  CHECK(eof.ok());
  CHECK_EQ(eof.value(), 0u);
  RequireConsistent(fs, "read serialized with truncate");
}

TEST(conc_stale_inode_snapshot_never_mixes_with_new_extent_cache) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "stale-snapshot.bin");
  auto first = Pattern(4096, 0x75);
  auto second = Pattern(4096, 0x76);
  CHECK(fs.WriteFileData(ino, 0, first.data(), first.size()).ok());
  CHECK(fs.WriteFileData(ino, 4096, second.data(), second.size()).ok());

  std::vector<uint8_t> observed;
  size_t observed_len = 0;
  bool read_ok = false;

  ScenarioScope scenario;
  uint64_t opener = C().Spawn("snapshot", [&] {
    auto inode = fs.OpenInode(ino);
    RequireOk(inode.ok() ? xfs::OkStatus() : inode.error(), "open snapshot");
    std::vector<uint8_t> buffer(8192);
    auto read = fs.ReadFile(inode.value(), nullptr, 0, buffer.data(),
                            buffer.size());
    read_ok = read.ok();
    if (read.ok()) {
      observed_len = read.value();
      observed.assign(buffer.begin(), buffer.begin() + observed_len);
    }
  });
  uint64_t truncater = C().Spawn("truncate", [&] {
    auto r = fs.SetFileSize(ino, 4096);
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "truncate");
  });
  uint64_t refresher = C().Spawn("fresh-read", [&] {
    std::vector<uint8_t> buffer(4096);
    auto r = fs.ReadFile(ino, 0, buffer.data(), buffer.size());
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "fresh read");
  });

  // Hold an inode parsed before the truncate, let the truncate commit and a
  // fresh reader repopulate the extent cache, then resume the old snapshot.
  PauseAt(opener, "inode.cache.insert.after");
  C().Start(truncater);
  RunToEnd(truncater);
  C().Start(refresher);
  RunToEnd(refresher);
  ResumeAndFinish(opener);
  scenario.Finish();

  CHECK(read_ok);
  const bool old_view =
      observed_len == 8192 &&
      std::equal(first.begin(), first.end(), observed.begin()) &&
      std::equal(second.begin(), second.end(), observed.begin() + 4096);
  const bool new_view =
      observed_len == 4096 &&
      std::equal(first.begin(), first.end(), observed.begin());
  if (!old_view && !new_view) {
    throw xfstest::TestFailure(
        "read from a pre-truncate inode mixed with the post-truncate extent "
        "cache: length " + std::to_string(observed_len) +
        " (expected the complete old file or the new 4096-byte file)\n" +
        C().Dump());
  }
  RequireConsistent(fs, "stale inode snapshot read");
}

TEST(conc_read_snapshot_never_reads_reallocated_blocks) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t source = MakeFile(fs, root, "reuse-src.bin");
  const uint64_t target = MakeFile(fs, root, "reuse-dst.bin");
  auto first = Pattern(4096, 0x75);
  auto second = Pattern(4096, 0x76);
  auto target_data = Pattern(4096, 0x99);
  CHECK(fs.WriteFileData(source, 0, first.data(), first.size()).ok());
  CHECK(fs.WriteFileData(source, 4096, second.data(), second.size()).ok());

  std::vector<uint8_t> observed(8192);
  size_t observed_len = 0;
  bool read_ok = false;

  ScenarioScope scenario;
  uint64_t reader = C().Spawn("snapshot-read", [&] {
    auto inode = fs.OpenInode(source);
    RequireOk(inode.ok() ? xfs::OkStatus() : inode.error(), "open snapshot");
    auto read = fs.ReadFile(inode.value(), nullptr, 0, observed.data(),
                            observed.size());
    read_ok = read.ok();
    if (read.ok()) observed_len = read.value();
  });
  uint64_t truncater = C().Spawn("truncate", [&] {
    auto r = fs.SetFileSize(source, 4096);
    RequireOk(r.ok() ? xfs::OkStatus() : r.error(), "truncate source");
  });

  PauseAt(reader, "extent.read.before_device");
  // The read holds write_barrier_ shared through the extent parse and the
  // data transfer. The truncate (and therefore the free) must wait; without
  // the read barrier the freed block could be reallocated to `target` and
  // this old snapshot would return target's bytes.
  StartToLock(truncater, "write_barrier_");
  ResumeAndFinish(reader);
  auto old_extents = Take(fs.GetDataExtents(source), "source extents");
  CHECK(!old_extents.empty());
  FinishOp(truncater);

  CHECK(read_ok);
  CHECK_EQ(observed_len, 8192u);
  CHECK(std::equal(first.begin(), first.end(), observed.begin()));
  CHECK(std::equal(second.begin(), second.end(), observed.begin() + 4096));

  auto allocated = fs.AllocateUnwritten(target, 0, 4096);
  RequireOk(allocated.ok() ? xfs::OkStatus() : allocated.error(),
            "allocate target");
  auto wrote = fs.WriteFileData(target, 0, target_data.data(),
                                target_data.size());
  RequireOk(wrote.ok() ? xfs::OkStatus() : wrote.error(), "write target");
  auto target_extents = Take(fs.GetDataExtents(target), "target extents");
  bool reused = false;
  for (const xfs::XfsExtent& before : old_extents) {
    for (const xfs::XfsExtent& after : target_extents) {
      if (after.startblock < before.startblock + before.blockcount &&
          before.startblock < after.startblock + after.blockcount) {
        reused = true;
      }
    }
  }
  CHECK(reused);
  scenario.Finish();

  RequireConsistent(fs, "read snapshot vs freed block reuse");
}

// A COW write replaces its block range and frees the private blocks it
// removes. A reader holds write_barrier_ shared across the device read, so the
// free must wait for every in-flight read: otherwise the freed block can be
// reallocated to another file and the reader returns that file's bytes.
TEST(conc_cow_remap_waits_for_read_snapshot) {
  auto image = MakeConcImage(/*reflink=*/true);
  auto& fs = *image.fs;
  const uint64_t root = fs.RootInode();
  const uint64_t src = MakeFile(fs, root, "cow-snap-src.bin");
  const uint64_t dst = MakeFile(fs, root, "cow-snap-dst.bin");
  const uint64_t target = MakeFile(fs, root, "cow-snap-target.bin");
  auto original = Pattern(8192, 0x3f);
  auto first_cow = Pattern(4096, 0xa1);
  auto second_cow = Pattern(8192, 0xb2);
  auto target_data = Pattern(4096, 0x99);
  CHECK(fs.WriteFileData(src, 0, original.data(), original.size()).ok());
  CHECK(fs.ReflinkRange(src, 0, dst, 0, 8192).ok());
  // Block 0 becomes private to dst; block 1 stays shared with src. A write
  // over both blocks must COW, and its remap frees dst's private block 0.
  CHECK(fs.WriteFileData(dst, 0, first_cow.data(), first_cow.size()).ok());
  auto old_extents = Take(fs.GetDataExtents(dst), "dst extents");
  uint64_t private_fsb = 0;
  for (const xfs::XfsExtent& e : old_extents) {
    if (e.startoff == 0) private_fsb = e.startblock;
  }
  CHECK(private_fsb != 0);

  std::vector<uint8_t> observed(8192);
  bool read_ok = false;
  size_t observed_len = 0;

  ScenarioScope scenario;
  uint64_t reader = C().Spawn("cow-read", [&] {
    auto read = fs.ReadFile(dst, 0, observed.data(), observed.size());
    read_ok = read.ok();
    if (read.ok()) observed_len = read.value();
  });
  PauseAt(reader, "data.read.before_device");

  xfs::Status wrote;
  uint64_t writer = C().Spawn("cow-write", [&] {
    auto r = fs.WriteFileData(dst, 0, second_cow.data(), second_cow.size());
    wrote = r.ok() ? xfs::OkStatus() : r.error();
  });
  // The COW remap frees the blocks it replaces, so the writer must drop its
  // shared barrier and wait for the paused reader before remapping.
  StartToLock(writer, "write_barrier_");
  ResumeAndFinish(reader);
  FinishOp(writer);
  scenario.Finish();

  CHECK(wrote.ok());
  CHECK(read_ok);
  CHECK_EQ(observed_len, 8192u);
  // The reader mapped dst before the remap: it must see the snapshot's data,
  // not blocks that the remap freed and `target` reused.
  CHECK(std::equal(first_cow.begin(), first_cow.end(), observed.begin()));
  CHECK(std::equal(original.begin() + 4096, original.end(),
                   observed.begin() + 4096));

  // Prove the freed private block really was available for reuse: write the
  // target only after the reader drained.
  auto target_wrote = fs.WriteFileData(target, 0, target_data.data(),
                                       target_data.size());
  RequireOk(target_wrote.ok() ? xfs::OkStatus() : target_wrote.error(),
            "write target");
  auto target_extents = Take(fs.GetDataExtents(target), "target extents");
  bool reused = false;
  for (const xfs::XfsExtent& e : target_extents) {
    if (e.startblock <= private_fsb &&
        private_fsb < e.startblock + e.blockcount) {
      reused = true;
    }
  }
  CHECK(reused);

  std::vector<uint8_t> dst_actual(8192);
  auto dst_read = fs.ReadFile(dst, 0, dst_actual.data(), dst_actual.size());
  RequireOk(dst_read.ok() ? xfs::OkStatus() : dst_read.error(), "read dst");
  CHECK(dst_actual == second_cow);
  std::vector<uint8_t> src_actual(8192);
  auto src_read = fs.ReadFile(src, 0, src_actual.data(), src_actual.size());
  RequireOk(src_read.ok() ? xfs::OkStatus() : src_read.error(), "read src");
  CHECK(src_actual == original);
  RequireConsistent(fs, "cow remap vs read snapshot");
}

// The exclusive retry re-validates the inode. The detection pass drops its
// shared barrier before the retry, so the inode can be unlinked and its number
// reused in between; the write must fail instead of landing in the new file.
TEST(conc_write_retry_refuses_reused_inode) {
  auto image = MakeConcImage(/*reflink=*/true);
  auto& fs = *image.fs;
  const uint64_t root = fs.RootInode();
  const uint64_t src = MakeFile(fs, root, "retry-src.bin");
  const uint64_t dst = MakeFile(fs, root, "retry-dst.bin");
  auto original = Pattern(8192, 0x51);
  auto cow = Pattern(4096, 0x52);
  auto fresh = Pattern(4096, 0x53);
  CHECK(fs.WriteFileData(src, 0, original.data(), original.size()).ok());
  CHECK(fs.ReflinkRange(src, 0, dst, 0, 8192).ok());

  xfs::Status wrote;
  ScenarioScope scenario;
  uint64_t writer = C().Spawn("retry-write", [&] {
    auto r = fs.WriteFileData(dst, 0, cow.data(), cow.size());
    wrote = r.ok() ? xfs::OkStatus() : r.error();
  });
  PauseAt(writer, "write.retry.before");

  // Between the two passes: free dst and let a new file take its number.
  // Generation values are random 32-bit values, so a direct comparison could
  // collide with probability 2^-32. If it does, free and re-create until the
  // generation differs; the retry is supposed to reject a reused number by
  // di_gen, and the scenario must not be flaky.
  const uint32_t old_gen = Take(fs.OpenInode(dst), "dst inode").gen;
  Require(fs.Unlink(root, "retry-dst.bin", /*defer_inode_free=*/false).ok(),
          "unlink dst");
  std::string reused_name = "retry-new-0.bin";
  uint64_t reused = MakeFile(fs, root, reused_name.c_str());
  CHECK_EQ(reused, dst);
  for (int attempt = 0;
       Take(fs.OpenInode(reused), "reused inode").gen == old_gen; ++attempt) {
    CHECK(attempt < 8);
    Require(fs.Unlink(root, reused_name.c_str(),
                      /*defer_inode_free=*/false)
                .ok(),
            "unlink reused inode");
    reused_name = "retry-new-" + std::to_string(attempt + 1) + ".bin";
    reused = MakeFile(fs, root, reused_name.c_str());
    CHECK_EQ(reused, dst);
  }
  auto fresh_wrote = fs.WriteFileData(reused, 4096, fresh.data(),
                                      fresh.size());
  Require(fresh_wrote.ok(), "write new file");

  FinishOp(writer);
  scenario.Finish();

  CHECK(!wrote.ok());
  CHECK_EQ(static_cast<uint32_t>(wrote.error().code),
           static_cast<uint32_t>(xfs::XfsError::kNotFound));

  std::vector<uint8_t> actual(8192, 0);
  auto read = fs.ReadFile(reused, 0, actual.data(), actual.size());
  Require(read.ok(), "read reused inode");
  CHECK_EQ(read.value(), 8192u);
  // The rejected retry did not touch the new file: its first block is still a
  // hole and only its own bytes are visible.
  for (size_t i = 0; i < 4096; ++i) CHECK_EQ(actual[i], 0u);
  CHECK(std::equal(fresh.begin(), fresh.end(), actual.begin() + 4096));
  RequireConsistent(fs, "write retry vs reused inode");
}

TEST(conc_different_inodes_serialize_on_mutation_mutex) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t a_ino = MakeFile(fs, root, "mutex-a.bin");
  const uint64_t b_ino = MakeFile(fs, root, "mutex-b.bin");
  auto a_data = Pattern(4096, 0x0a);
  auto b_data = Pattern(4096, 0x0b);
  xfs::Status a_status;
  xfs::Status b_status;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("write-a", [&] {
    auto r = fs.WriteFileData(a_ino, 0, a_data.data(), a_data.size());
    a_status = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t b = C().Spawn("write-b", [&] {
    auto r = fs.WriteFileData(b_ino, 0, b_data.data(), b_data.size());
    b_status = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(a, "txn.begin");
  C().Start(b);
  WaitSync(b, "barrier.before");
  C().Resume(b);
  AdvanceSync(b, "inode.lock.before");
  ContinueToLock(b, "mutation_mutex_");
  RunToEnd(a);
  FinishOp(b);
  scenario.Finish();

  CHECK(a_status.ok());
  CHECK(b_status.ok());
  std::vector<uint8_t> actual_a(4096);
  std::vector<uint8_t> actual_b(4096);
  CHECK(fs.ReadFile(a_ino, 0, actual_a.data(), actual_a.size()).ok());
  CHECK(fs.ReadFile(b_ino, 0, actual_b.data(), actual_b.size()).ok());
  CHECK(actual_a == a_data);
  CHECK(actual_b == b_data);
  RequireConsistent(fs, "two inode writes");
}

TEST(conc_flush_waits_for_in_flight_write) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "flush-vs-write.bin");
  auto data = Pattern(4096, 0x51);
  xfs::Status wrote;
  xfs::Status flushed;

  ScenarioScope scenario;
  uint64_t writer = C().Spawn("write", [&] {
    auto r = fs.WriteFileData(ino, 0, data.data(), data.size());
    wrote = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t flusher = C().Spawn("flush", [&] {
    flushed = fs.Flush();
  });

  PauseAt(writer, "data.write.before");
  StartToLock(flusher, "write_barrier_");
  RunToEnd(writer);
  FinishOp(flusher);
  scenario.Finish();

  CHECK(wrote.ok());
  CHECK(flushed.ok());
  std::vector<uint8_t> actual(4096);
  CHECK(fs.ReadFile(ino, 0, actual.data(), actual.size()).ok());
  CHECK(actual == data);
  RequireConsistent(fs, "flush+write");
}

// ---------------------------------------------------------------------------
// Extended attributes. set/replace/remove take write_barrier_ exclusively,
// so two mutations serialize and a read concurrent with one sees the old
// value until it completes.
// ---------------------------------------------------------------------------

TEST(conc_xattr_set_set_last_writer_wins) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "xattr-set.bin");
  const std::string first_value = "first-value";
  const std::string second_value = "second-value-different";
  xfs::Status first;
  xfs::Status second;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("xattr-a", [&] {
    auto r = fs.SetXattr(ino, "user.conc", first_value.data(),
                         first_value.size(), xfs::XfsFilesystem::XattrSetMode::kUpsert);
    first = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t b = C().Spawn("xattr-b", [&] {
    auto r = fs.SetXattr(ino, "user.conc", second_value.data(),
                         second_value.size(), xfs::XfsFilesystem::XattrSetMode::kUpsert);
    second = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(a, "txn.begin");
  C().Start(b);
  ContinueToLock(b, "write_barrier_");
  RunToEnd(a);
  FinishOp(b);
  scenario.Finish();

  CHECK(first.ok());
  CHECK(second.ok());
  auto stored = fs.GetXattr(ino, "user.conc");
  CHECK(stored.ok());
  CHECK_EQ(stored.value().value.size(), second_value.size());
  CHECK(std::string(stored.value().value.begin(), stored.value().value.end()) ==
        second_value);
  RequireConsistent(fs, "xattr set+set");
}

TEST(conc_xattr_set_then_remove) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "xattr-remove.bin");
  const std::string value = "to-be-removed";
  xfs::Status set_status;
  xfs::Status remove_status;

  ScenarioScope scenario;
  uint64_t setter = C().Spawn("set", [&] {
    auto r = fs.SetXattr(ino, "user.gone", value.data(), value.size(),
                         xfs::XfsFilesystem::XattrSetMode::kUpsert);
    set_status = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t remover = C().Spawn("remove", [&] {
    remove_status = fs.RemoveXattr(ino, "user.gone");
  });

  PauseAt(setter, "txn.begin");
  C().Start(remover);
  ContinueToLock(remover, "write_barrier_");
  RunToEnd(setter);
  FinishOp(remover);
  scenario.Finish();

  CHECK(set_status.ok());
  CHECK(remove_status.ok());
  CHECK(!fs.GetXattr(ino, "user.gone").ok());
  RequireConsistent(fs, "xattr set+remove");
}

TEST(conc_xattr_replace_vs_remove) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "xattr-replace.bin");
  const std::string initial = "initial";
  const std::string replaced = "replaced-value";
  CHECK(fs.SetXattr(ino, "user.r", initial.data(), initial.size(),
                    xfs::XfsFilesystem::XattrSetMode::kUpsert)
            .ok());
  xfs::Status replace_status;
  xfs::Status remove_status;

  ScenarioScope scenario;
  uint64_t replacer = C().Spawn("replace", [&] {
    auto r = fs.SetXattr(ino, "user.r", replaced.data(), replaced.size(),
                         xfs::XfsFilesystem::XattrSetMode::kReplace);
    replace_status = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t remover = C().Spawn("remove", [&] {
    remove_status = fs.RemoveXattr(ino, "user.r");
  });

  PauseAt(replacer, "txn.begin");
  C().Start(remover);
  ContinueToLock(remover, "write_barrier_");
  RunToEnd(replacer);
  FinishOp(remover);
  scenario.Finish();

  CHECK(replace_status.ok());
  CHECK(remove_status.ok());
  CHECK(!fs.GetXattr(ino, "user.r").ok());
  RequireConsistent(fs, "xattr replace+remove");
}

TEST(conc_xattr_read_during_set) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "xattr-read.bin");
  CHECK(fs.SetXattr(ino, "user.r", "old", 3,
                    xfs::XfsFilesystem::XattrSetMode::kUpsert)
            .ok());
  std::string before;
  {
    auto old = fs.GetXattr(ino, "user.r");
    CHECK(old.ok());
    before.assign(old.value().value.begin(), old.value().value.end());
  }
  std::string during;
  bool during_ok = false;
  xfs::Status set_status;

  ScenarioScope scenario;
  uint64_t setter = C().Spawn("set", [&] {
    auto r = fs.SetXattr(ino, "user.r", "new-value", 9,
                         xfs::XfsFilesystem::XattrSetMode::kUpsert);
    set_status = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t reader = C().Spawn("get", [&] {
    auto value = fs.GetXattr(ino, "user.r");
    during_ok = value.ok();
    if (value.ok()) {
      during.assign(value.value().value.begin(), value.value().value.end());
    }
  });

  PauseAt(setter, "txn.begin");
  // GetXattr takes the barrier shared for the whole read. The setter owns it
  // exclusively, so the reader waits for the committed new value instead of
  // parsing the inode/attr fork while the transaction is half-built.
  StartToLock(reader, "write_barrier_");
  RunToEnd(setter);
  FinishOp(reader);
  scenario.Finish();

  CHECK(before == "old");
  CHECK(set_status.ok());
  CHECK(during_ok);
  CHECK(during == "new-value");
  auto now = fs.GetXattr(ino, "user.r");
  CHECK(now.ok());
  CHECK(std::string(now.value().value.begin(), now.value().value.end()) ==
        "new-value");
  RequireConsistent(fs, "xattr read serialized with set");
}

TEST(conc_xattr_fork_expand_then_contract) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "xattr-fork.bin");
  std::vector<uint8_t> big(3072, 0x5c);
  std::vector<uint8_t> small(16, 0x11);
  xfs::Status expanded;
  xfs::Status contracted;

  ScenarioScope scenario;
  uint64_t big_set = C().Spawn("set-big", [&] {
    auto r = fs.SetXattr(ino, "user.big", big.data(), big.size(),
                         xfs::XfsFilesystem::XattrSetMode::kUpsert);
    expanded = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t small_set = C().Spawn("set-small", [&] {
    auto r = fs.SetXattr(ino, "user.big", small.data(), small.size(),
                         xfs::XfsFilesystem::XattrSetMode::kUpsert);
    contracted = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(big_set, "txn.begin");
  C().Start(small_set);
  ContinueToLock(small_set, "write_barrier_");
  RunToEnd(big_set);
  FinishOp(small_set);
  scenario.Finish();

  CHECK(expanded.ok());
  CHECK(contracted.ok());
  auto stored = fs.GetXattr(ino, "user.big");
  CHECK(stored.ok());
  CHECK(stored.value().value == small);
  RequireConsistent(fs, "xattr fork expansion/contraction");
}

// ---------------------------------------------------------------------------
// Reflink / CoW. ReflinkRange takes write_barrier_ exclusively and both inode
// locks in ascending order. A COW write also takes the barrier exclusively (its
// remap frees the blocks it replaces, and readers hold the barrier shared), so
// two COW writes serialize on the barrier while the inode stripes order the
// two-inode clone against them.
// ---------------------------------------------------------------------------

TEST(conc_reflink_then_both_sides_write) {
  auto image = MakeConcImage(/*reflink=*/true);
  auto& fs = *image.fs;
  const uint64_t root = fs.RootInode();
  const uint64_t src = MakeFile(fs, root, "src.bin");
  const uint64_t dst = MakeFile(fs, root, "dst.bin");
  auto original = Pattern(8192, 0x0d);
  CHECK(fs.WriteFileData(src, 0, original.data(), original.size()).ok());
  auto cloned = fs.ReflinkRange(src, 0, dst, 0, 8192);
  CHECK(cloned.ok());
  CHECK_EQ(cloned.value(), 8192u);
  auto src_data = Pattern(4096, 0xa1);
  auto dst_data = Pattern(4096, 0xb2);
  xfs::Status src_status;
  xfs::Status dst_status;

  ScenarioScope scenario;
  uint64_t src_writer = C().Spawn("cow-src", [&] {
    auto r = fs.WriteFileData(src, 0, src_data.data(), src_data.size());
    src_status = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t dst_writer = C().Spawn("cow-dst", [&] {
    auto r = fs.WriteFileData(dst, 0, dst_data.data(), dst_data.size());
    dst_status = r.ok() ? xfs::OkStatus() : r.error();
  });

  // The first writer has detected sharing and holds the exclusive barrier
  // inside its staging transaction; the second one must wait on that barrier
  // instead of remapping concurrently.
  PauseAt(src_writer, "txn.begin");
  C().Start(dst_writer);
  ContinueToLock(dst_writer, "write_barrier_");
  RunToEnd(src_writer);
  FinishOp(dst_writer);
  scenario.Finish();

  CHECK(src_status.ok());
  CHECK(dst_status.ok());
  std::vector<uint8_t> src_actual(8192);
  std::vector<uint8_t> dst_actual(8192);
  CHECK(fs.ReadFile(src, 0, src_actual.data(), src_actual.size()).ok());
  CHECK(fs.ReadFile(dst, 0, dst_actual.data(), dst_actual.size()).ok());
  for (size_t i = 0; i < 4096; ++i) {
    CHECK_EQ(src_actual[i], src_data[i]);
    CHECK_EQ(dst_actual[i], dst_data[i]);
  }
  for (size_t i = 4096; i < 8192; ++i) {
    CHECK_EQ(src_actual[i], original[i]);
    CHECK_EQ(dst_actual[i], original[i]);
  }
  RequireConsistent(fs, "reflink both sides write");
}

TEST(conc_reflink_clone_then_unlink_both_names) {
  auto image = MakeConcImage(/*reflink=*/true);
  auto& fs = *image.fs;
  const uint64_t root = fs.RootInode();
  const uint64_t src = MakeFile(fs, root, "unlink-src.bin");
  const uint64_t dst = MakeFile(fs, root, "unlink-dst.bin");
  auto data = Pattern(8192, 0x6e);
  CHECK(fs.WriteFileData(src, 0, data.data(), data.size()).ok());
  CHECK(fs.ReflinkRange(src, 0, dst, 0, 8192).ok());
  xfs::Status first;
  xfs::Status second;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("unlink-src", [&] {
    first = fs.Unlink(root, "unlink-src.bin");
  });
  uint64_t b = C().Spawn("unlink-dst", [&] {
    second = fs.Unlink(root, "unlink-dst.bin");
  });

  PauseAt(a, "txn.begin");
  C().Start(b);
  ContinueToLock(b, "write_barrier_");
  RunToEnd(a);
  FinishOp(b);
  scenario.Finish();

  CHECK(first.ok());
  CHECK(second.ok());
  CHECK(!fs.LookupName(root, "unlink-src.bin").ok());
  CHECK(!fs.LookupName(root, "unlink-dst.bin").ok());
  RequireConsistent(fs, "reflink unlink both sides");
}

TEST(conc_cow_write_survives_source_truncate) {
  auto image = MakeConcImage(/*reflink=*/true);
  auto& fs = *image.fs;
  const uint64_t root = fs.RootInode();
  const uint64_t src = MakeFile(fs, root, "cow-src.bin");
  const uint64_t dst = MakeFile(fs, root, "cow-dst.bin");
  auto original = Pattern(8192, 0x3f);
  CHECK(fs.WriteFileData(src, 0, original.data(), original.size()).ok());
  CHECK(fs.ReflinkRange(src, 0, dst, 0, 8192).ok());
  auto overwrite = Pattern(4096, 0x88);
  xfs::Status truncated;
  xfs::Status wrote;

  ScenarioScope scenario;
  uint64_t truncater = C().Spawn("truncate-src", [&] {
    auto r = fs.SetFileSize(src, 0);
    truncated = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t writer = C().Spawn("write-dst", [&] {
    auto r = fs.WriteFileData(dst, 0, overwrite.data(), overwrite.size());
    wrote = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(truncater, "inode.lock.after");
  C().Start(writer);
  WaitSync(writer, "barrier.before");
  C().Resume(writer);
  ContinueToLock(writer, "write_barrier_");
  RunToEnd(truncater);
  FinishOp(writer);
  scenario.Finish();

  CHECK(truncated.ok());
  CHECK(wrote.ok());
  std::vector<uint8_t> actual(8192);
  auto read = fs.ReadFile(dst, 0, actual.data(), actual.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), actual.size());
  for (size_t i = 0; i < 4096; ++i) CHECK_EQ(actual[i], overwrite[i]);
  for (size_t i = 4096; i < 8192; ++i) CHECK_EQ(actual[i], original[i]);
  RequireConsistent(fs, "CoW write after source truncate");
}

TEST(conc_reflink_opposite_pairs_do_not_deadlock) {
  auto image = MakeConcImage(/*reflink=*/true);
  auto& fs = *image.fs;
  const uint64_t root = fs.RootInode();
  const uint64_t a_ino = MakeFile(fs, root, "pair-a.bin");
  const uint64_t b_ino = MakeFile(fs, root, "pair-b.bin");
  auto a_data = Pattern(8192, 0x14);
  auto b_data = Pattern(8192, 0x15);
  CHECK(fs.WriteFileData(a_ino, 0, a_data.data(), a_data.size()).ok());
  CHECK(fs.WriteFileData(b_ino, 0, b_data.data(), b_data.size()).ok());
  xfs::Status forward;
  xfs::Status backward;

  ScenarioScope scenario;
  uint64_t a_to_b = C().Spawn("clone-a-to-b", [&] {
    auto r = fs.ReflinkRange(a_ino, 0, b_ino, 0, 8192);
    forward = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t b_to_a = C().Spawn("clone-b-to-a", [&] {
    auto r = fs.ReflinkRange(b_ino, 0, a_ino, 0, 8192);
    backward = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(a_to_b, "reflink.first_lock");
  C().Start(b_to_a);
  WaitSync(b_to_a, "reflink.barrier.before");
  C().Resume(b_to_a);
  ContinueToLock(b_to_a, "write_barrier_");
  RunToEnd(a_to_b);
  FinishOp(b_to_a);
  scenario.Finish();

  CHECK(!C().deadlocked());
  CHECK(forward.ok());
  CHECK(backward.ok());
  std::vector<uint8_t> final_a(8192);
  std::vector<uint8_t> final_b(8192);
  CHECK(fs.ReadFile(a_ino, 0, final_a.data(), final_a.size()).ok());
  CHECK(fs.ReadFile(b_ino, 0, final_b.data(), final_b.size()).ok());
  // Serial a->b then b->a: b now holds a's bytes and a is a clone of b, so
  // both sides must agree.
  CHECK(final_a == final_b);
  RequireConsistent(fs, "reflink opposite directions");
}

// ---------------------------------------------------------------------------
// Journal ordering and allocator ownership.
// ---------------------------------------------------------------------------

TEST(conc_journal_records_do_not_cross_between_transactions) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t a_ino = MakeFile(fs, root, "wal-a.bin");
  const uint64_t b_ino = MakeFile(fs, root, "wal-b.bin");
  auto a_data = Pattern(4096, 0x0c);
  auto b_data = Pattern(4096, 0x0e);
  xfs::Status a_status;
  xfs::Status b_status;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("wal-a", [&] {
    auto r = fs.WriteFileData(a_ino, 0, a_data.data(), a_data.size());
    a_status = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t b = C().Spawn("wal-b", [&] {
    auto r = fs.WriteFileData(b_ino, 0, b_data.data(), b_data.size());
    b_status = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(a, "txn.commit.before_metadata");
  C().Start(b);
  WaitSync(b, "barrier.before");
  C().Resume(b);
  AdvanceSync(b, "inode.lock.before");
  ContinueToLock(b, "mutation_mutex_");
  RunToEnd(a);
  FinishOp(b);
  scenario.Finish();

  CHECK(a_status.ok());
  CHECK(b_status.ok());
  auto events = C().Events();
  uint64_t a_txn = 0;
  uint64_t b_txn = 0;
  uint64_t a_log_durable_seq = 0;
  uint64_t a_metadata_seq = 0;
  uint64_t a_done_seq = 0;
  uint64_t b_begin_seq = 0;
  for (const auto& event : events) {
    if (event.op_label == "wal-a") {
      if (event.kind == EventKind::kSync && event.point == "txn.begin") {
        a_txn = event.txn;
      }
      if (event.kind == EventKind::kSync &&
          event.point == "txn.commit.log_durable") {
        a_log_durable_seq = event.seq;
      }
      if (event.kind == EventKind::kSync &&
          event.point == "txn.commit.before_metadata") {
        a_metadata_seq = event.seq;
      }
      if (event.kind == EventKind::kSync &&
          event.point == "txn.commit.done") {
        a_done_seq = event.seq;
      }
    }
    if (event.op_label == "wal-b" &&
        event.kind == EventKind::kSync && event.point == "txn.begin") {
      b_txn = event.txn;
      b_begin_seq = event.seq;
    }
  }
  CHECK(a_txn != 0);
  CHECK(b_txn != 0);
  CHECK(a_txn != b_txn);
  // Log durable before metadata write-back, and the second transaction can
  // only begin after the first one finished applying its metadata.
  CHECK(a_log_durable_seq != 0);
  CHECK(a_metadata_seq != 0);
  CHECK(a_log_durable_seq < a_metadata_seq);
  CHECK(a_done_seq != 0);
  CHECK(b_begin_seq > a_done_seq);
  RequireConsistent(fs, "journal ordering");
}

TEST(conc_same_ag_allocations_serialize_through_the_allocator) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t a_ino = MakeFile(fs, root, "alloc-a.bin");
  const uint64_t b_ino = MakeFile(fs, root, "alloc-b.bin");
  auto a_data = Pattern(4096, 0x7a);
  auto b_data = Pattern(4096, 0x7b);
  xfs::Status a_status;
  xfs::Status b_status;

  ScenarioScope scenario;
  uint64_t a = C().Spawn("alloc-a", [&] {
    auto r = fs.WriteFileData(a_ino, 0, a_data.data(), a_data.size());
    a_status = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t b = C().Spawn("alloc-b", [&] {
    auto r = fs.WriteFileData(b_ino, 0, b_data.data(), b_data.size());
    b_status = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(a, "alloc.allocate.owned");
  C().Start(b);
  WaitSync(b, "barrier.before");
  C().Resume(b);
  AdvanceSync(b, "inode.lock.before");
  ContinueToLock(b, "mutation_mutex_");
  RunToEnd(a);
  FinishOp(b);
  scenario.Finish();

  CHECK(a_status.ok());
  CHECK(b_status.ok());
  std::vector<uint8_t> actual_a(4096);
  std::vector<uint8_t> actual_b(4096);
  CHECK(fs.ReadFile(a_ino, 0, actual_a.data(), actual_a.size()).ok());
  CHECK(fs.ReadFile(b_ino, 0, actual_b.data(), actual_b.size()).ok());
  CHECK(actual_a == a_data);
  CHECK(actual_b == b_data);
  RequireConsistent(fs, "same-AG allocation");
}

TEST(conc_free_then_reallocate_serializes) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t victim = MakeFile(fs, root, "free-me.bin");
  auto victim_data = Pattern(16384, 0x2b);
  CHECK(fs.WriteFileData(victim, 0, victim_data.data(),
                         victim_data.size())
            .ok());
  xfs::Status removed;
  uint64_t created_ino = 0;
  xfs::Status created;
  auto new_data = Pattern(4096, 0x2c);

  ScenarioScope scenario;
  uint64_t remover = C().Spawn("unlink", [&] {
    removed = fs.Unlink(root, "free-me.bin");
  });
  uint64_t creator = C().Spawn("create-write", [&] {
    auto r = fs.CreateRegularFile(root, "reuse.bin", 0644, 0, 0);
    if (!r.ok()) {
      created = r.error();
      return;
    }
    created_ino = r.value();
    auto w = fs.WriteFileData(created_ino, 0, new_data.data(),
                              new_data.size());
    created = w.ok() ? xfs::OkStatus() : w.error();
  });

  // The inode free releases its data extents through the allocator
  // (alloc.release) while the exclusive namespace barrier is still held.
  PauseAt(remover, "alloc.release");
  C().Start(creator);
  WaitSync(creator, "namespace.barrier.before");
  C().Resume(creator);
  ContinueToLock(creator, "write_barrier_");
  RunToEnd(remover);
  FinishOp(creator);
  scenario.Finish();

  CHECK(removed.ok());
  CHECK(created.ok());
  CHECK(created_ino != 0);
  std::vector<uint8_t> actual(4096);
  auto read = fs.ReadFile(created_ino, 0, actual.data(), actual.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), actual.size());
  CHECK(actual == new_data);
  RequireConsistent(fs, "free+reallocate");
}

// ---------------------------------------------------------------------------
// Diagnostics: the scheduler's wait-for graph must report a real cycle, not
// wait for a timeout. This is the mechanism the operation tests rely on.
// ---------------------------------------------------------------------------

TEST(conc_deadlock_detector_reports_lock_cycle) {
  ScenarioScope scenario;
  xfs::conc::Mutex first;
  xfs::conc::Mutex second;
  first.SetName("cycle-first");
  second.SetName("cycle-second");

  uint64_t a = C().Spawn("cycle-a", [&] {
    first.lock();
    xfs::conc::Sync("a-holds-first");
    second.lock();
    second.unlock();
    first.unlock();
  });
  uint64_t b = C().Spawn("cycle-b", [&] {
    second.lock();
    xfs::conc::Sync("b-holds-second");
    first.lock();
    first.unlock();
    second.unlock();
  });

  C().Start(a);
  WaitSync(a, "a-holds-first");
  C().Start(b);
  WaitSync(b, "b-holds-second");
  C().Resume(a);
  WaitLockBlock(a, "cycle-second");
  // A now waits for `second` (held by B); let B ask for `first` (held by A)
  // so the wait-for graph closes into a cycle.
  C().Resume(b);
  Require(C().WaitForAny(EventKind::kDeadlock, "", 10000),
          "deadlock was not detected\n" + C().Dump());
  CHECK(C().deadlocked());
  const std::string dump = C().Dump();
  CHECK(dump.find("lock-order deadlock") != std::string::npos);
  CHECK(dump.find("cycle-a") != std::string::npos);
  CHECK(dump.find("cycle-b") != std::string::npos);
  // JoinAll disables scheduling and releases the parked operations.
  scenario.Finish();
}

namespace {
void MaybeExportImage(xfs::XfsFilesystem& fs, const char* name);
}  // namespace

// ---------------------------------------------------------------------------
// Seeded deterministic stress. The scheduler runs exactly one operation at a
// time; a driver picks the next runnable operation from a PRNG seeded from a
// recorded value. The same seed and the same initial image therefore replay
// the same interleaving, and the recorded index sequence can be replayed with
// XFSWIN_CONC_REPLAY to minimize a failure.
// ---------------------------------------------------------------------------

namespace {

uint64_t EnvUnsigned(const char* name, uint64_t fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 0);
  if (end == value) return fallback;
  return static_cast<uint64_t>(parsed);
}

std::vector<uint32_t> EnvSchedule() {
  std::vector<uint32_t> out;
  const char* value = std::getenv("XFSWIN_CONC_REPLAY");
  if (value == nullptr || *value == '\0') return out;
  std::string text(value);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t comma = text.find(',', pos);
    if (comma == std::string::npos) comma = text.size();
    if (comma > pos) {
      out.push_back(static_cast<uint32_t>(
          std::strtoul(text.substr(pos, comma - pos).c_str(), nullptr, 10)));
    }
    pos = comma + 1;
  }
  return out;
}

// Spawns `bodies`, starts each one deterministically, then drives the
// workload. Returns the sequence of spawned-operation indices it chose (one
// per scheduler step).
std::vector<uint32_t> RunScheduled(
    uint64_t seed, const std::vector<std::function<void()>>& bodies,
    const std::vector<std::string>& labels, const char* what,
    const std::vector<uint32_t>& replay_override = {}) {
  std::vector<uint64_t> ids;
  ids.reserve(bodies.size());
  for (size_t i = 0; i < bodies.size(); ++i) {
    ids.push_back(C().Spawn(labels[i], bodies[i]));
  }
  // Start each operation in spawn order and let it park on its first sync
  // point. This fixes the initial state without a wall-clock race.
  for (uint64_t id : ids) {
    if (!C().Step(id)) {
      throw xfstest::TestFailure(std::string(what) +
                                 ": operation failed to start\n" + C().Dump());
    }
  }

  const std::vector<uint32_t> replay =
      replay_override.empty() ? EnvSchedule() : replay_override;
  const bool strict_replay = !replay.empty();
  std::mt19937_64 rng(seed);
  std::vector<uint32_t> schedule;
  uint64_t steps = 0;
  for (;;) {
    const std::vector<uint64_t> runnable = C().RunnableOps();
    if (runnable.empty()) {
      if (C().WaitAllDone(1)) break;
      throw xfstest::TestFailure(std::string(what) +
                                 ": no runnable operation and not all done "
                                 "(deadlock or lost wakeup)\n" + C().Dump());
    }
    size_t index = 0;
    uint64_t pick = 0;
    if (strict_replay && steps >= replay.size()) {
      throw xfstest::TestFailure(
          std::string(what) +
          ": replay schedule ended before the workload completed\n" +
          C().Dump());
    }
    if (steps < replay.size()) {
      index = replay[steps];
      if (index >= ids.size()) {
        throw xfstest::TestFailure(std::string(what) +
                                   ": replay index out of range");
      }
      pick = ids[index];
      bool found = false;
      for (uint64_t candidate : runnable) {
        if (candidate == pick) found = true;
      }
      if (!found) {
        throw xfstest::TestFailure(
            std::string(what) + ": replay step " + std::to_string(steps) +
            " requested a non-runnable operation\n" + C().Dump());
      }
    } else {
      uint64_t chosen = runnable[rng() % runnable.size()];
      for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == chosen) index = i;
      }
      pick = chosen;
    }
    schedule.push_back(static_cast<uint32_t>(index));
    if (!C().Step(pick)) {
      throw xfstest::TestFailure(
          std::string(what) + ": scheduler step " + std::to_string(steps) +
          " stalled\n" + C().Dump());
    }
    ++steps;
    if ((steps & 0x3ff) == 0) C().TrimEvents(20000);
    if (steps > 4000000) {
      throw xfstest::TestFailure(std::string(what) +
                                 ": step limit exceeded\n" + C().Dump());
    }
  }
  if (!C().WaitAllDone(60000)) {
    throw xfstest::TestFailure(std::string(what) +
                               ": operations did not finish\n" + C().Dump());
  }
  return schedule;
}

std::string ScheduleText(const std::vector<uint32_t>& schedule) {
  std::string out;
  for (size_t i = 0; i < schedule.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += std::to_string(schedule[i]);
  }
  return out;
}

}  // namespace

TEST(conc_stress_mixed_namespace_data_xattr_seeded) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const size_t thread_count =
      static_cast<size_t>(EnvUnsigned("XFSWIN_CONC_STRESS_THREADS", 4));
  const size_t iterations =
      static_cast<size_t>(EnvUnsigned("XFSWIN_CONC_STRESS_ITERS", 5));
  const uint64_t seed = EnvUnsigned("XFSWIN_CONC_STRESS_SEED",
                                    0x0c0ffee5eedULL);
  if (thread_count < 1 || thread_count > 8) {
    throw xfstest::TestFailure("XFSWIN_CONC_STRESS_THREADS must be 1..8");
  }

  std::vector<uint64_t> dirs(thread_count);
  for (size_t i = 0; i < thread_count; ++i) {
    const std::string dir_name = "conc-dir-" + std::to_string(i);
    RequireOk(fs.Mkdir(root, dir_name, 0755, 0, 0), "mkdir stress dir");
    dirs[i] = Take(fs.LookupName(root, dir_name), "lookup stress dir").ino;
  }

  ScenarioScope scenario;
  std::vector<std::function<void()>> bodies;
  std::vector<std::string> labels;
  for (size_t i = 0; i < thread_count; ++i) {
    labels.push_back("stress-" + std::to_string(i));
    bodies.push_back([&fs, dir = dirs[i], i, iterations] {
      for (size_t iter = 0; iter < iterations; ++iter) {
        const std::string name =
            "f" + std::to_string(iter) + "-" + std::to_string(i) + ".bin";
        const auto created = fs.CreateRegularFile(dir, name, 0644, 0, 0);
        if (!created.ok()) {
          throw xfstest::TestFailure("stress create: " +
                                     created.error().Format());
        }
        const uint64_t ino = created.value();
        std::vector<uint8_t> data(8192);
        for (size_t b = 0; b < data.size(); ++b) {
          data[b] = static_cast<uint8_t>((b * 131u + iter * 37u + i * 11u) ^
                                         (b >> 5));
        }
        auto wrote = fs.WriteFileData(ino, 0, data.data(), data.size());
        if (!wrote.ok()) {
          throw xfstest::TestFailure("stress write: " +
                                     wrote.error().Format());
        }
        std::vector<uint8_t> readback(8192);
        auto read = fs.ReadFile(ino, 0, readback.data(), readback.size());
        if (!read.ok() || read.value() != readback.size() ||
            readback != data) {
          throw xfstest::TestFailure("stress readback mismatch");
        }
        std::vector<uint8_t> attr(64, static_cast<uint8_t>(iter + i));
        auto set = fs.SetXattr(ino, "user.stress", attr.data(), attr.size(),
                               xfs::XfsFilesystem::XattrSetMode::kUpsert);
        auto stored = fs.GetXattr(ino, "user.stress");
        if (!set.ok() || !stored.ok() || stored.value().value != attr) {
          throw xfstest::TestFailure("stress xattr mismatch");
        }
        const std::string renamed = name + ".moved";
        auto moved =
            fs.Rename(dir, name, dir, renamed, /*replace_existing=*/false);
        if (!moved.ok()) {
          throw xfstest::TestFailure("stress rename: " +
                                     moved.error().Format());
        }
        auto lookup = fs.LookupName(dir, renamed);
        if (!lookup.ok() || lookup.value().ino != ino) {
          throw xfstest::TestFailure("stress rename lookup mismatch");
        }
        auto removed = fs.Unlink(dir, renamed);
        if (!removed.ok()) {
          throw xfstest::TestFailure("stress unlink: " +
                                     removed.error().Format());
        }
      }
    });
  }

  std::vector<uint32_t> schedule;
  try {
    schedule = RunScheduled(seed, bodies, labels, "mixed stress");
  } catch (const xfstest::TestFailure& failure) {
    throw xfstest::TestFailure(
        std::string(failure.what()) + "\nseed=" + std::to_string(seed) +
        " schedule=" + ScheduleText(schedule));
  }
  scenario.Finish();

  // Every worker removed its private directory's contents; the directories
  // themselves must still be empty and consistent.
  for (size_t i = 0; i < thread_count; ++i) {
    auto entries = fs.ListDirectory(dirs[i]);
    CHECK(entries.ok());
    for (const auto& entry : entries.value()) {
      CHECK(entry.name == "." || entry.name == "..");
    }
  }
  RequireConsistent(fs, "mixed stress");
  MaybeExportImage(fs, "mixed");
}

TEST(conc_stress_shared_names_history_is_legal) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const size_t thread_count =
      static_cast<size_t>(EnvUnsigned("XFSWIN_CONC_STRESS_THREADS", 4));
  const size_t iterations =
      static_cast<size_t>(EnvUnsigned("XFSWIN_CONC_STRESS_ITERS", 10));
  const uint64_t seed = EnvUnsigned("XFSWIN_CONC_STRESS_SEED",
                                    0x5eed1234abcdULL);
  if (thread_count < 1 || thread_count > 8) {
    throw xfstest::TestFailure("XFSWIN_CONC_STRESS_THREADS must be 1..8");
  }

  struct Record {
    std::string name;
    bool create = false;
    bool ok = false;
    xfs::XfsError code = xfs::XfsError::kOk;
    uint64_t ino = 0;
  };
  std::mutex record_mutex;
  std::vector<Record> records;

  ScenarioScope scenario;
  std::vector<std::function<void()>> bodies;
  std::vector<std::string> labels;
  for (size_t i = 0; i < thread_count; ++i) {
    labels.push_back("names-" + std::to_string(i));
    bodies.push_back([&fs, &record_mutex, &records, root, i, iterations] {
      std::mt19937_64 rng(0xabcdef0123456789ULL + i);
      for (size_t iter = 0; iter < iterations; ++iter) {
        Record record;
        const bool do_create = (rng() & 1) != 0;
        record.name = (rng() & 1) != 0 ? "hot-0.bin" : "hot-1.bin";
        record.create = do_create;
        if (do_create) {
          auto created = fs.CreateRegularFile(root, record.name, 0644, 0, 0);
          record.ok = created.ok();
          if (created.ok()) {
            record.ino = created.value();
          } else {
            record.code = created.error().code;
          }
        } else {
          auto removed = fs.Unlink(root, record.name);
          record.ok = removed.ok();
          if (!removed.ok()) record.code = removed.error().code;
        }
        std::lock_guard<std::mutex> lock(record_mutex);
        records.push_back(record);
      }
    });
  }

  std::vector<uint32_t> schedule;
  try {
    schedule = RunScheduled(seed, bodies, labels, "shared-name stress");
  } catch (const xfstest::TestFailure& failure) {
    throw xfstest::TestFailure(
        std::string(failure.what()) + "\nseed=" + std::to_string(seed) +
        " schedule=" + ScheduleText(schedule));
  }
  scenario.Finish();

  // The namespace mutation barrier serializes every operation, and each
  // operation records its result before doing anything else, so `records`
  // is the serial history. Every entry must be explainable by the name's
  // state immediately before it.
  bool present[2] = {false, false};
  uint64_t ino[2] = {0, 0};
  for (const Record& record : records) {
    const int slot = record.name == "hot-0.bin" ? 0 : 1;
    if (record.create) {
      if (record.ok) {
        CHECK(!present[slot]);
        present[slot] = true;
        ino[slot] = record.ino;
      } else {
        CHECK_EQ(record.code, xfs::XfsError::kAlreadyExists);
        CHECK(present[slot]);
      }
    } else {
      if (record.ok) {
        CHECK(present[slot]);
        present[slot] = false;
        ino[slot] = 0;
      } else {
        CHECK_EQ(record.code, xfs::XfsError::kNotFound);
        CHECK(!present[slot]);
      }
    }
  }
  for (int slot = 0; slot < 2; ++slot) {
    const std::string name = slot == 0 ? "hot-0.bin" : "hot-1.bin";
    auto lookup = fs.LookupName(root, name);
    CHECK_EQ(lookup.ok(), present[slot]);
    if (lookup.ok()) CHECK_EQ(lookup.value().ino, ino[slot]);
  }
  RequireConsistent(fs, "shared-name stress");
  MaybeExportImage(fs, "shared-names");
}

TEST(conc_stress_cow_both_sides_seeded) {
  auto image = MakeConcImage(/*reflink=*/true);
  auto& fs = *image.fs;
  const uint64_t root = fs.RootInode();
  const uint64_t src = MakeFile(fs, root, "stress-src.bin");
  const uint64_t dst = MakeFile(fs, root, "stress-dst.bin");
  auto original = Pattern(8192, 0x55);
  CHECK(fs.WriteFileData(src, 0, original.data(), original.size()).ok());
  CHECK(fs.ReflinkRange(src, 0, dst, 0, 8192).ok());
  const size_t iterations =
      static_cast<size_t>(EnvUnsigned("XFSWIN_CONC_STRESS_ITERS", 4));
  const uint64_t seed = EnvUnsigned("XFSWIN_CONC_STRESS_SEED",
                                    0xc0dec0de1234ULL);

  std::vector<std::vector<uint8_t>> last(4);
  for (size_t w = 0; w < 4; ++w) last[w] = Pattern(4096, 0x10 + w);

  ScenarioScope scenario;
  std::vector<std::function<void()>> bodies;
  std::vector<std::string> labels;
  for (size_t w = 0; w < 4; ++w) {
    labels.push_back("cow-" + std::to_string(w));
    bodies.push_back([&fs, &last, w, iterations, src, dst, seed] {
      const uint64_t ino = (w < 2) ? src : dst;
      const uint64_t offset = (w % 2 == 0) ? 0 : 4096;
      for (size_t iter = 0; iter < iterations; ++iter) {
        std::vector<uint8_t> data(4096);
        for (size_t b = 0; b < data.size(); ++b) {
          data[b] = static_cast<uint8_t>((b * 53u + iter * 29u + w * 7u +
                                          seed) & 0xff);
        }
        auto wrote = fs.WriteFileData(ino, offset, data.data(), data.size());
        if (!wrote.ok()) {
          throw xfstest::TestFailure("CoW stress write: " +
                                     wrote.error().Format());
        }
        last[w] = std::move(data);
      }
    });
  }

  std::vector<uint32_t> schedule;
  try {
    schedule = RunScheduled(seed, bodies, labels, "CoW stress");
  } catch (const xfstest::TestFailure& failure) {
    throw xfstest::TestFailure(
        std::string(failure.what()) + "\nseed=" + std::to_string(seed) +
        " schedule=" + ScheduleText(schedule));
  }
  scenario.Finish();

  std::vector<uint8_t> src_data(8192);
  std::vector<uint8_t> dst_data(8192);
  CHECK(fs.ReadFile(src, 0, src_data.data(), src_data.size()).ok());
  CHECK(fs.ReadFile(dst, 0, dst_data.data(), dst_data.size()).ok());
  for (size_t w = 0; w < 4; ++w) {
    const size_t offset = (w % 2 == 0) ? 0 : 4096;
    const std::vector<uint8_t>& actual = (w < 2) ? src_data : dst_data;
    for (size_t b = 0; b < 4096; ++b) {
      CHECK_EQ(actual[offset + b], last[w][b]);
    }
  }
  RequireConsistent(fs, "CoW stress");
  MaybeExportImage(fs, "cow");
}

// ---------------------------------------------------------------------------
// Reproducibility: the same seed produces the same event sequence, and a
// recorded schedule replays the same interleaving on an identical image. A
// failing stress schedule can therefore be minimized and re-run as a list.
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> NormalizedEvents() {
  std::vector<std::string> out;
  for (const auto& event : C().Events()) {
    // Thread startup order before the first scheduled turn is OS-defined;
    // op.enter is bookkeeping, not part of the controlled interleaving.
    if (event.kind == EventKind::kOpEnter) continue;
    out.push_back(std::string(xfs::conc::ToString(event.kind)) + "|" +
                  event.point + "|" + event.op_label + "|" +
                  std::to_string(event.ino));
  }
  return out;
}

// Two operations that race on the same names and files; deterministic under a
// fixed seed, and different operations interleave differently.
void RunReplayWorkload(int seed_tag, std::vector<uint32_t>* schedule,
                       std::vector<std::string>* events,
                       const std::vector<uint32_t>& replay) {
  auto image = MakeConcImage(/*reflink=*/false);
  auto& fs = *image.fs;
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "replay.bin");
  (void)ino;
  std::vector<std::function<void()>> bodies;
  bodies.push_back([&fs, root, seed_tag] {
    for (int i = 0; i < 3; ++i) {
      const std::string name = "r" + std::to_string(i + seed_tag) + ".bin";
      auto created = fs.CreateRegularFile(root, name, 0644, 0, 0);
      if (!created.ok()) throw xfstest::TestFailure("replay create");
      std::vector<uint8_t> data(4096, static_cast<uint8_t>(i + seed_tag));
      auto wrote =
          fs.WriteFileData(created.value(), 0, data.data(), data.size());
      if (!wrote.ok()) throw xfstest::TestFailure("replay write");
      auto moved = fs.Rename(root, name, root, name + ".x", false);
      if (!moved.ok()) throw xfstest::TestFailure("replay rename");
      auto removed = fs.Unlink(root, name + ".x");
      if (!removed.ok()) throw xfstest::TestFailure("replay unlink");
    }
  });
  bodies.push_back([&fs, root, seed_tag] {
    for (int i = 0; i < 3; ++i) {
      const std::string name = "s" + std::to_string(i + seed_tag) + ".bin";
      auto created = fs.CreateRegularFile(root, name, 0644, 0, 0);
      if (!created.ok()) throw xfstest::TestFailure("replay create b");
      std::vector<uint8_t> data(4096, static_cast<uint8_t>(0x80 + i));
      auto wrote =
          fs.WriteFileData(created.value(), 0, data.data(), data.size());
      if (!wrote.ok()) throw xfstest::TestFailure("replay write b");
      auto removed = fs.Unlink(root, name);
      if (!removed.ok()) throw xfstest::TestFailure("replay unlink b");
    }
  });
  ScenarioScope scenario;
  *schedule = RunScheduled(0x1234abcd5678ULL, bodies,
                           {"replay-a", "replay-b"}, "replay workload",
                           replay);
  *events = NormalizedEvents();
  scenario.Finish();
}

}  // namespace

TEST(conc_seeded_schedule_is_reproducible_and_replayable) {
  std::vector<uint32_t> schedule_a;
  std::vector<std::string> events_a;
  RunReplayWorkload(0, &schedule_a, &events_a, {});
  CHECK(!schedule_a.empty());
  CHECK(!events_a.empty());

  std::vector<uint32_t> schedule_b;
  std::vector<std::string> events_b;
  RunReplayWorkload(0, &schedule_b, &events_b, schedule_a);
  CHECK(schedule_b == schedule_a);
  if (events_b != events_a) {
    size_t at = 0;
    while (at < events_a.size() && at < events_b.size() &&
           events_a[at] == events_b[at]) {
      ++at;
    }
    throw xfstest::TestFailure(
        "event sequence differs at " + std::to_string(at) + ": a=" +
        (at < events_a.size() ? events_a[at] : "<end>") + " b=" +
        (at < events_b.size() ? events_b[at] : "<end>") + " sizes " +
        std::to_string(events_a.size()) + "/" +
        std::to_string(events_b.size()));
  }

  // A truncated replay prefix must fail to be a valid schedule for the full
  // workload: the remaining operations can never be reached by replay alone.
  std::vector<uint32_t> truncated(schedule_a.begin(),
                                  schedule_a.begin() +
                                      static_cast<long>(schedule_a.size() / 2));
  bool rejected = false;
  try {
    std::vector<uint32_t> ignored_schedule;
    std::vector<std::string> ignored_events;
    RunReplayWorkload(0, &ignored_schedule, &ignored_events, truncated);
  } catch (const xfstest::TestFailure&) {
    rejected = true;
  }
  CHECK(rejected);
}

// ---------------------------------------------------------------------------
// Cache, ENOSPC and a remaining serialization direction.
// ---------------------------------------------------------------------------

TEST(conc_data_cache_not_stale_after_truncate_and_rewrite) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  const uint64_t ino = MakeFile(fs, root, "data-cache.bin");
  auto old_data = Pattern(8192, 0x61);
  auto new_data = Pattern(4096, 0x62);
  CHECK(fs.WriteFileData(ino, 0, old_data.data(), old_data.size()).ok());
  std::vector<uint8_t> warm(8192);
  CHECK(fs.ReadFile(ino, 0, warm.data(), warm.size()).ok());
  CHECK(warm == old_data);

  CHECK(fs.SetFileSize(ino, 0).ok());
  std::vector<uint8_t> empty(8192);
  auto after_truncate = fs.ReadFile(ino, 0, empty.data(), empty.size());
  CHECK(after_truncate.ok());
  CHECK_EQ(after_truncate.value(), 0u);

  CHECK(fs.WriteFileData(ino, 0, new_data.data(), new_data.size()).ok());
  std::vector<uint8_t> fresh(4096);
  auto after_write = fs.ReadFile(ino, 0, fresh.data(), fresh.size());
  CHECK(after_write.ok());
  CHECK_EQ(after_write.value(), fresh.size());
  CHECK(fresh == new_data);
  RequireConsistent(fs, "data cache truncate/rewrite");
}

TEST(conc_namespace_rmdir_wins_against_child_create) {
  FixtureImage::Instance().Reset();
  auto mounted = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(mounted.ok());
  auto& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  RequireOk(fs.Mkdir(root, "vanish", 0755, 0, 0), "mkdir vanish");
  const uint64_t dir = Take(fs.LookupName(root, "vanish"), "lookup").ino;
  xfs::Status removed;
  xfs::Status created;

  ScenarioScope scenario;
  uint64_t remover = C().Spawn("rmdir", [&] {
    removed = fs.RemoveEmptyDirectory(root, "vanish");
  });
  uint64_t creator = C().Spawn("create-child", [&] {
    auto r = fs.CreateRegularFile(dir, "late.bin", 0644, 0, 0);
    created = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(remover, "txn.begin");
  C().Start(creator);
  WaitSync(creator, "namespace.barrier.before");
  C().Resume(creator);
  ContinueToLock(creator, "write_barrier_");
  RunToEnd(remover);
  FinishOp(creator);
  scenario.Finish();

  CHECK(removed.ok());
  CHECK(!created.ok());
  CHECK(!fs.LookupName(root, "vanish").ok());
  RequireConsistent(fs, "rmdir wins against child create");
}

// Fills the free space with unwritten extents, then lets a writer race the
// truncate that releases them. The exclusive namespace/data barrier orders
// the two, so the writer must succeed after the free and the final allocator
// counters must remain exact (CheckConsistency validates fdblocks, bnobt,
// cntbt, AGFL and rmap).
TEST(conc_enospc_free_and_reallocate) {
  auto image = MakeConcImage(/*reflink=*/false);
  auto& fs = *image.fs;
  const uint64_t root = fs.RootInode();
  const uint64_t hog = MakeFile(fs, root, "hog.bin");
  const uint64_t target = MakeFile(fs, root, "recover.bin");
  std::vector<uint8_t> target_data = Pattern(4096, 0x4d);
  const uint64_t chunk = 4ull << 20;
  uint64_t offset = 0;
  size_t allocations = 0;
  bool saw_no_space = false;
  for (int i = 0; i < 200; ++i) {
    auto alloc = fs.AllocateUnwritten(hog, offset, chunk);
    if (alloc.ok()) {
      ++allocations;
      offset += chunk;
      continue;
    }
    if (alloc.error().code == xfs::XfsError::kNoSpace) {
      saw_no_space = true;
      break;
    }
    throw xfstest::TestFailure("unexpected allocation error: " +
                               alloc.error().Format());
  }
  CHECK(saw_no_space);
  CHECK(allocations > 5);

  xfs::Status truncated;
  xfs::Status wrote;

  ScenarioScope scenario;
  uint64_t truncater = C().Spawn("truncate-hog", [&] {
    auto r = fs.SetFileSize(hog, 0);
    truncated = r.ok() ? xfs::OkStatus() : r.error();
  });
  uint64_t writer = C().Spawn("write-target", [&] {
    auto r = fs.WriteFileData(target, 0, target_data.data(),
                              target_data.size());
    wrote = r.ok() ? xfs::OkStatus() : r.error();
  });

  PauseAt(truncater, "alloc.release");
  C().Start(writer);
  WaitSync(writer, "barrier.before");
  C().Resume(writer);
  ContinueToLock(writer, "write_barrier_");
  RunToEnd(truncater);
  FinishOp(writer);
  scenario.Finish();

  CHECK(truncated.ok());
  CHECK(wrote.ok());
  std::vector<uint8_t> actual(4096);
  auto read = fs.ReadFile(target, 0, actual.data(), actual.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), actual.size());
  CHECK(actual == target_data);
  RequireConsistent(fs, "ENOSPC free+reallocate");
}

namespace {

// The external oracle pipeline needs an image to inspect. When the CI job
// sets XFSWIN_CONC_IMAGE_DIR, copy the (already clean-unmounted) device bytes
// to <dir>/conc-<name>.img; XFSWIN_CONC_IMAGE_OUT still selects one exact
// path for interactive single-test runs. MemoryBlockDevice is in-memory, so
// this is the only way to hand the exact concurrency-built image to
// xfs_repair / the Linux kernel.
void MaybeExportImage(xfs::XfsFilesystem& fs, const char* name) {
  const char* single = std::getenv("XFSWIN_CONC_IMAGE_OUT");
  const char* dir = std::getenv("XFSWIN_CONC_IMAGE_DIR");
  std::string path;
  if (single != nullptr && *single != '\0') {
    path = single;
  } else if (dir != nullptr && *dir != '\0') {
    path = std::string(dir) + "/conc-" + name + ".img";
  } else {
    return;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(fs.device().Size()));
  if (!fs.device().Read(0, bytes.data(), bytes.size())) {
    throw xfstest::TestFailure("cannot read device for image export");
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw xfstest::TestFailure("cannot open image export path");
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  if (!out) throw xfstest::TestFailure("cannot write image export");
}

}  // namespace

#endif  // XFSWIN_CONCURRENCY_HOOKS
