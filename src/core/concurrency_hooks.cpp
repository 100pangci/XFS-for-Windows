// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - deterministic concurrency scheduler (CONC-1).
//
// The scheduler is a cooperative token ring: exactly one operation thread is
// runnable at a time. It stops at named sync points and at lock acquisitions
// that are not compatible with the shadow lock state, and waits for the test
// controller to resume it. Real mutexes are still used, but a scheduled
// operation never calls into a contended real mutex: the shadow model makes
// the controller decide the order, so the same WaitFor/Resume script always
// produces the same event trace.
#include "xfs/concurrency_hooks.h"

#ifdef XFSWIN_CONCURRENCY_HOOKS

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xfs/concurrency_test.h"

namespace xfs::conc {
namespace {

struct HeldLock {
  const void* lock = nullptr;
  std::string name;
  bool exclusive = false;
};

struct OpState {
  uint64_t id = 0;
  std::string label;
  bool started = false;
  bool blocked = false;
  bool done = false;
  bool free_run = false;
  bool cancelled = false;
  uint64_t epoch = 0;
  uint64_t parks = 0;  // number of times this operation parked
  std::string wait_kind;
  std::string wait_point;
  const void* wait_lock = nullptr;
  bool wait_exclusive = false;
  std::string error;
  std::vector<HeldLock> held;
};

struct LockState {
  std::string name;
  uint64_t exclusive_owner = 0;
  std::vector<uint64_t> shared_owners;
};

struct Scheduler {
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> active{false};
  bool abort_waiters = false;  // guarded by mutex
  uint64_t next_op = 1;
  uint64_t next_event = 1;
  uint64_t event_epoch = 0;
  std::unordered_map<uint64_t, OpState> ops;
  std::unordered_map<const void*, LockState> locks;
  std::vector<Event> events;
  std::string deadlock;
};

Scheduler& S() {
  static Scheduler instance;
  return instance;
}

thread_local uint64_t t_op = 0;
thread_local uint64_t t_ino = 0;
thread_local uint64_t t_txn = 0;

void AppendLocked(uint64_t op, EventKind kind, std::string point,
                  std::string detail) {
  Scheduler& s = S();
  Event event;
  event.seq = s.next_event++;
  event.op = op;
  event.kind = kind;
  event.point = std::move(point);
  event.detail = std::move(detail);
  event.ino = t_ino;
  event.txn = t_txn;
  auto it = s.ops.find(op);
  if (it != s.ops.end()) event.op_label = it->second.label;
  s.events.push_back(std::move(event));
  ++s.event_epoch;
  s.cv.notify_all();
}

std::string HeldText(const OpState& state) {
  std::ostringstream out;
  bool first = true;
  for (const HeldLock& held : state.held) {
    if (!first) out << ",";
    first = false;
    out << held.name << (held.exclusive ? "[x]" : "[s]");
  }
  return first ? "-" : out.str();
}

// The operation that currently blocks `op` from taking `lock`.
uint64_t BlockerLocked(const LockState& lock, bool exclusive, uint64_t op) {
  if (lock.exclusive_owner != 0 && lock.exclusive_owner != op) {
    return lock.exclusive_owner;
  }
  if (exclusive) {
    // Exclusive conflicts with every shared owner. If this operation already
    // holds the lock in either mode, the request is re-entrant; report it as
    // a self-cycle rather than corrupting the shadow state.
    for (uint64_t owner : lock.shared_owners) {
      if (owner != op) return owner;
    }
    if (lock.exclusive_owner == op) return op;
    for (uint64_t owner : lock.shared_owners) {
      if (owner == op) return op;
    }
    return 0;
  }
  // Shared requests only conflict with an exclusive owner; multiple shared
  // holders are exactly the concurrency the barrier is designed to allow.
  if (lock.exclusive_owner == op) return op;
  return 0;
}

bool WaitForPathLocked(uint64_t from, uint64_t target,
                       std::vector<uint64_t>* seen) {
  if (from == target) return true;
  if (std::find(seen->begin(), seen->end(), from) != seen->end()) return false;
  seen->push_back(from);
  auto it = S().ops.find(from);
  if (it == S().ops.end() || !it->second.blocked || !it->second.wait_lock) {
    return false;
  }
  auto lock_it = S().locks.find(it->second.wait_lock);
  if (lock_it == S().locks.end()) return false;
  const LockState& lock = lock_it->second;
  if (lock.exclusive_owner != 0 &&
      WaitForPathLocked(lock.exclusive_owner, target, seen)) {
    return true;
  }
  for (uint64_t owner : lock.shared_owners) {
    if (WaitForPathLocked(owner, target, seen)) return true;
  }
  return false;
}

void DetectCycleLocked(uint64_t op, uint64_t blocker) {
  Scheduler& s = S();
  if (s.deadlock.empty() && blocker != 0) {
    std::vector<uint64_t> seen;
    if (WaitForPathLocked(blocker, op, &seen)) {
      std::ostringstream out;
      out << "lock-order deadlock: op " << op;
      auto op_it = s.ops.find(op);
      if (op_it != s.ops.end()) out << " (" << op_it->second.label << ")";
      out << " waits for op " << blocker;
      auto blocker_it = s.ops.find(blocker);
      if (blocker_it != s.ops.end()) {
        out << " (" << blocker_it->second.label << ")";
      }
      out << " through held locks";
      s.deadlock = out.str();
      AppendLocked(op, EventKind::kDeadlock, "deadlock", s.deadlock);
    }
  }
}

void TakeShadowLocked(const void* lock_ptr, LockState* lock, uint64_t op,
                      bool exclusive) {
  if (exclusive) {
    lock->exclusive_owner = op;
  } else {
    lock->shared_owners.push_back(op);
  }
  auto it = S().ops.find(op);
  if (it != S().ops.end()) {
    it->second.held.push_back(HeldLock{lock_ptr, lock->name, exclusive});
  }
}

void ReleaseShadowLocked(const void* lock_ptr, uint64_t op, bool exclusive) {
  Scheduler& s = S();
  auto lock_it = s.locks.find(lock_ptr);
  if (lock_it == s.locks.end()) return;
  LockState& lock = lock_it->second;
  if (exclusive) {
    if (lock.exclusive_owner == op) lock.exclusive_owner = 0;
  } else {
    auto it = std::find(lock.shared_owners.begin(), lock.shared_owners.end(),
                        op);
    if (it != lock.shared_owners.end()) lock.shared_owners.erase(it);
  }
  auto op_it = s.ops.find(op);
  if (op_it != s.ops.end()) {
    auto& held = op_it->second.held;
    auto it = std::find_if(held.begin(), held.end(), [&](const HeldLock& h) {
      return h.lock == lock_ptr && h.exclusive == exclusive;
    });
    if (it != held.end()) held.erase(it);
  }
}

void WaitForResumeLocked(std::unique_lock<std::mutex>& lk, OpState* state,
                         const std::string& kind, const std::string& point) {
  Scheduler& s = S();
  state->blocked = true;
  state->wait_kind = kind;
  state->wait_point = point;
  ++state->parks;
  s.cv.notify_all();
  const uint64_t epoch = state->epoch;
  s.cv.wait(lk, [&] {
    return state->epoch != epoch || !s.active.load() || s.abort_waiters;
  });
  state->blocked = false;
  state->wait_kind.clear();
  state->wait_point.clear();
  state->wait_lock = nullptr;
  state->wait_exclusive = false;
}

const char* SafeLockName(const char* name) {
  return name != nullptr ? name : "lock";
}

}  // namespace

bool HooksActive() { return S().active.load(std::memory_order_relaxed); }
uint64_t CurrentOp() { return t_op; }
uint64_t CurrentIno() { return t_ino; }
uint64_t CurrentTxn() { return t_txn; }

void SetIno(uint64_t ino) { t_ino = ino; }
void SetTxn(uint64_t txn) { t_txn = txn; }

void EnterOp(uint64_t op_id, const char* label) {
  Scheduler& s = S();
  t_op = op_id;
  std::unique_lock<std::mutex> lk(s.mutex);
  OpState& state = s.ops[op_id];
  state.id = op_id;
  if (label != nullptr) state.label = label;
  if (state.label.empty()) state.label = "op";
  AppendLocked(op_id, EventKind::kOpEnter, "op.enter", state.label);
  if (!state.started) {
    const uint64_t epoch = state.epoch;
    s.cv.wait(lk, [&] {
      return state.started || state.epoch != epoch || !s.active.load() ||
             s.abort_waiters;
    });
  }
}

void LeaveOp() {
  Scheduler& s = S();
  std::unique_lock<std::mutex> lk(s.mutex);
  auto it = s.ops.find(t_op);
  if (it != s.ops.end()) {
    it->second.done = true;
    if (!it->second.held.empty()) {
      it->second.error = "operation exited holding locks: " +
                         HeldText(it->second);
    }
    AppendLocked(t_op, EventKind::kOpExit, "op.exit", it->second.label);
  }
  t_op = 0;
}

void Sync(const char* point, std::string detail) {
  Scheduler& s = S();
  if (!s.active.load(std::memory_order_relaxed) || t_op == 0) return;
  std::unique_lock<std::mutex> lk(s.mutex);
  if (!s.active.load()) return;
  auto it = s.ops.find(t_op);
  if (it == s.ops.end()) return;
  if (it->second.free_run) {
    AppendLocked(t_op, EventKind::kSync, SafeLockName(point),
                 std::move(detail));
    return;
  }
  it->second.wait_point = SafeLockName(point);
  AppendLocked(t_op, EventKind::kSync, SafeLockName(point), std::move(detail));
  WaitForResumeLocked(lk, &it->second, "sync", SafeLockName(point));
}

bool LockBefore(const void* lock_ptr, const char* name, bool exclusive) {
  Scheduler& s = S();
  if (!s.active.load(std::memory_order_relaxed) || t_op == 0) return true;
  std::unique_lock<std::mutex> lk(s.mutex);
  if (!s.active.load()) return !s.abort_waiters;
  auto op_it = s.ops.find(t_op);
  if (op_it == s.ops.end()) return true;
  OpState* state = &op_it->second;
  for (;;) {
    if (s.abort_waiters) return false;
    if (!s.active.load()) return true;
    LockState& lock = s.locks[lock_ptr];
    lock.name = SafeLockName(name);
    const uint64_t blocker = BlockerLocked(lock, exclusive, t_op);
    if (blocker == 0) {
      TakeShadowLocked(lock_ptr, &lock, t_op, exclusive);
      // The real mutex is free from the shadow model's point of view, so the
      // caller's lock() returns immediately. Ownership is reported after the
      // real acquisition in LockAfter.
      return true;
    }
    state->blocked = true;
    state->wait_kind = "lock";
    state->wait_point = lock.name;
    state->wait_lock = lock_ptr;
    state->wait_exclusive = exclusive;
    std::ostringstream detail;
    detail << "holder_op=" << blocker << " mode="
           << (exclusive ? "exclusive" : "shared");
    auto holder_it = s.ops.find(blocker);
    if (holder_it != s.ops.end()) detail << " holder=" << holder_it->second.label;
    AppendLocked(t_op, EventKind::kLockBlock, lock.name, detail.str());
    DetectCycleLocked(t_op, blocker);
    WaitForResumeLocked(lk, state, "lock", lock.name);
  }
}

bool TryLockBefore(const void* lock_ptr, const char* name, bool exclusive) {
  Scheduler& s = S();
  if (!s.active.load(std::memory_order_relaxed) || t_op == 0) return true;
  std::unique_lock<std::mutex> lk(s.mutex);
  if (s.abort_waiters) throw Cancelled{};
  if (!s.active.load()) return true;
  LockState& lock = s.locks[lock_ptr];
  lock.name = SafeLockName(name);
  if (BlockerLocked(lock, exclusive, t_op) != 0) return false;
  TakeShadowLocked(lock_ptr, &lock, t_op, exclusive);
  return true;
}

void LockAfter(const void* lock_ptr, bool exclusive) {
  Scheduler& s = S();
  if (!s.active.load(std::memory_order_relaxed) || t_op == 0) return;
  std::unique_lock<std::mutex> lk(s.mutex);
  if (!s.active.load()) return;
  auto lock_it = s.locks.find(lock_ptr);
  if (lock_it == s.locks.end()) return;
  AppendLocked(t_op, EventKind::kLockAcquire, lock_it->second.name,
               exclusive ? "exclusive" : "shared");
}

void LockRelease(const void* lock_ptr, bool exclusive) {
  Scheduler& s = S();
  if (!s.active.load(std::memory_order_relaxed) || t_op == 0) return;
  std::unique_lock<std::mutex> lk(s.mutex);
  if (!s.active.load()) return;
  auto lock_it = s.locks.find(lock_ptr);
  if (lock_it == s.locks.end()) return;
  const std::string name = lock_it->second.name;
  ReleaseShadowLocked(lock_ptr, t_op, exclusive);
  AppendLocked(t_op, EventKind::kLockRelease, name,
               exclusive ? "exclusive" : "shared");
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

const char* ToString(EventKind kind) {
  switch (kind) {
    case EventKind::kOpEnter:
      return "op.enter";
    case EventKind::kOpExit:
      return "op.exit";
    case EventKind::kSync:
      return "sync";
    case EventKind::kLockAcquire:
      return "lock.acquire";
    case EventKind::kLockBlock:
      return "lock.block";
    case EventKind::kLockRelease:
      return "lock.release";
    case EventKind::kDeadlock:
      return "deadlock";
  }
  return "?";
}

struct Controller::Impl {
  std::unordered_map<uint64_t, std::thread> threads;
  std::vector<std::pair<uint64_t, std::thread>> joinable;
  size_t cursor = 0;
  FailureHandler failure_handler = nullptr;
};

Controller& Controller::Get() {
  static Controller controller;
  return controller;
}

Controller::Controller() : impl_(new Impl) {}
Controller::~Controller() {
  JoinAll();
  delete impl_;
}

void Controller::Enable() {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  // A previous session must have joined its operations; dropping the state
  // here is intentional and catches tests that forget to JoinAll by making
  // their event waits time out.
  s.ops.clear();
  s.locks.clear();
  s.events.clear();
  s.deadlock.clear();
  s.abort_waiters = false;
  s.next_event = 1;
  s.event_epoch = 0;
  impl_->cursor = 0;
  s.active.store(true, std::memory_order_relaxed);
}

void Controller::Disable() {
  Scheduler& s = S();
  {
    std::lock_guard<std::mutex> lk(s.mutex);
    s.active.store(false, std::memory_order_relaxed);
  }
  s.cv.notify_all();
}

bool Controller::enabled() const {
  return S().active.load(std::memory_order_relaxed);
}

uint64_t Controller::Spawn(std::string label, std::function<void()> body) {
  Scheduler& s = S();
  uint64_t id;
  {
    std::lock_guard<std::mutex> lk(s.mutex);
    id = s.next_op++;
    OpState& state = s.ops[id];
    state.id = id;
    state.label = std::move(label);
  }
  std::thread thread([id, body = std::move(body)]() mutable {
    EnterOp(id, nullptr);
    try {
      body();
    } catch (const Cancelled&) {
      Scheduler& scheduler = S();
      std::lock_guard<std::mutex> lk(scheduler.mutex);
      auto it = scheduler.ops.find(id);
      if (it != scheduler.ops.end()) it->second.cancelled = true;
    } catch (const std::exception& e) {
      Scheduler& scheduler = S();
      std::lock_guard<std::mutex> lk(scheduler.mutex);
      auto it = scheduler.ops.find(id);
      if (it != scheduler.ops.end()) it->second.error = e.what();
    } catch (...) {
      Scheduler& scheduler = S();
      std::lock_guard<std::mutex> lk(scheduler.mutex);
      auto it = scheduler.ops.find(id);
      if (it != scheduler.ops.end()) it->second.error = "unknown exception";
    }
    LeaveOp();
  });
  std::lock_guard<std::mutex> lk(s.mutex);
  impl_->joinable.emplace_back(id, std::move(thread));
  return id;
}

void Controller::Start(uint64_t op) {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  auto it = s.ops.find(op);
  if (it == s.ops.end()) return;
  it->second.started = true;
  ++it->second.epoch;
  s.cv.notify_all();
}

void Controller::Resume(uint64_t op) {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  auto it = s.ops.find(op);
  if (it == s.ops.end()) return;
  ++it->second.epoch;
  s.cv.notify_all();
}

bool Controller::RunToCompletion(uint64_t op, uint64_t timeout_ms) {
  Scheduler& s = S();
  std::unique_lock<std::mutex> lk(s.mutex);
  auto it = s.ops.find(op);
  if (it == s.ops.end()) return false;
  if (it->second.done) return true;
  it->second.free_run = true;
  ++it->second.epoch;
  s.cv.notify_all();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  uint64_t seen_epoch = s.event_epoch;
  for (;;) {
    auto current = s.ops.find(op);
    if (current == s.ops.end() || current->second.done) return true;
    if (!s.deadlock.empty()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    s.cv.wait_until(lk, deadline, [&] {
      return s.event_epoch != seen_epoch || !s.deadlock.empty();
    });
    seen_epoch = s.event_epoch;
  }
}

void Controller::ResumeAll() {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  for (auto& entry : s.ops) ++entry.second.epoch;
  s.cv.notify_all();
}

std::vector<uint64_t> Controller::RunnableOps() const {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  std::vector<uint64_t> out;
  for (const auto& entry : s.ops) {
    const OpState& state = entry.second;
    if (!state.started || state.done || state.free_run) continue;
    if (!state.blocked) continue;  // currently running on another thread
    if (state.wait_kind != "lock") {
      out.push_back(entry.first);
      continue;
    }
    auto lock_it = s.locks.find(state.wait_lock);
    if (lock_it != s.locks.end() &&
        BlockerLocked(lock_it->second, state.wait_exclusive,
                      entry.first) == 0) {
      out.push_back(entry.first);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool Controller::Step(uint64_t op, uint64_t timeout_ms) {
  Scheduler& s = S();
  std::unique_lock<std::mutex> lk(s.mutex);
  auto it = s.ops.find(op);
  if (it == s.ops.end()) return false;
  if (it->second.done) return true;
  // Wait for the operation to park on a *new* stop. `blocked` alone is stale
  // while the operation is still parked from the previous turn: resuming it
  // clears the flag only when its thread actually wakes up.
  const uint64_t parks_before = it->second.parks;
  it->second.started = true;
  ++it->second.epoch;
  s.cv.notify_all();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  s.cv.wait_until(lk, deadline, [&] {
    auto current = s.ops.find(op);
    return current == s.ops.end() || current->second.done ||
           current->second.parks != parks_before || !s.active.load();
  });
  auto current = s.ops.find(op);
  return current == s.ops.end() || current->second.done ||
         current->second.parks != parks_before;
}

void Controller::TrimEvents(size_t limit) {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  if (s.events.size() <= limit) return;
  const size_t drop = s.events.size() - limit;
  s.events.erase(s.events.begin(), s.events.begin() + drop);
  impl_->cursor = impl_->cursor > drop ? impl_->cursor - drop : 0;
}

bool Controller::WaitFor(uint64_t op, EventKind kind, std::string_view point,
                         uint64_t timeout_ms) {
  Scheduler& s = S();
  std::unique_lock<std::mutex> lk(s.mutex);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  uint64_t seen_epoch = s.event_epoch;
  for (;;) {
    for (size_t i = impl_->cursor; i < s.events.size(); ++i) {
      const Event& event = s.events[i];
      if (event.op != op) continue;
      if (event.kind != kind) continue;
      if (!point.empty() && event.point != point) continue;
      impl_->cursor = i + 1;
      return true;
    }
    if (!s.deadlock.empty()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    s.cv.wait_until(lk, deadline, [&] {
      return s.event_epoch != seen_epoch || !s.deadlock.empty() ||
             !s.active.load();
    });
    seen_epoch = s.event_epoch;
    if (!s.active.load() && impl_->cursor >= s.events.size()) {
      // The session was disabled while waiting; let the caller observe the
      // timeout/dump rather than spinning.
    }
  }
}

bool Controller::WaitForAny(EventKind kind, std::string_view point,
                            uint64_t timeout_ms) {
  Scheduler& s = S();
  std::unique_lock<std::mutex> lk(s.mutex);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  uint64_t seen_epoch = s.event_epoch;
  for (;;) {
    for (size_t i = impl_->cursor; i < s.events.size(); ++i) {
      const Event& event = s.events[i];
      if (event.kind != kind) continue;
      if (!point.empty() && event.point != point) continue;
      impl_->cursor = i + 1;
      return true;
    }
    if (!s.deadlock.empty()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    s.cv.wait_until(lk, deadline, [&] {
      return s.event_epoch != seen_epoch || !s.deadlock.empty() ||
             !s.active.load();
    });
    seen_epoch = s.event_epoch;
  }
}

bool Controller::WaitNext(uint64_t op, Event* out, uint64_t timeout_ms) {
  Scheduler& s = S();
  std::unique_lock<std::mutex> lk(s.mutex);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  uint64_t seen_epoch = s.event_epoch;
  for (;;) {
    for (size_t i = impl_->cursor; i < s.events.size(); ++i) {
      if (s.events[i].op != op) continue;
      if (out != nullptr) *out = s.events[i];
      impl_->cursor = i + 1;
      return true;
    }
    if (!s.deadlock.empty()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    s.cv.wait_until(lk, deadline, [&] {
      return s.event_epoch != seen_epoch || !s.deadlock.empty();
    });
    seen_epoch = s.event_epoch;
  }
}

bool Controller::WaitDone(uint64_t op, uint64_t timeout_ms) {
  return WaitFor(op, EventKind::kOpExit, "", timeout_ms);
}

bool Controller::WaitAllDone(uint64_t timeout_ms) {
  Scheduler& s = S();
  std::unique_lock<std::mutex> lk(s.mutex);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  uint64_t seen_epoch = s.event_epoch;
  for (;;) {
    bool all_done = true;
    for (const auto& entry : s.ops) {
      if (!entry.second.done) {
        all_done = false;
        break;
      }
    }
    if (all_done) return true;
    if (!s.deadlock.empty()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    s.cv.wait_until(lk, deadline, [&] {
      return s.event_epoch != seen_epoch || !s.deadlock.empty();
    });
    seen_epoch = s.event_epoch;
  }
}

void Controller::ClearCursor() {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  impl_->cursor = 0;
}

std::vector<Event> Controller::Events() const {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  return s.events;
}

bool Controller::deadlocked() const {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  return !s.deadlock.empty();
}

std::string Controller::Dump() const {
  Scheduler& s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  std::ostringstream out;
  out << "concurrency event log (" << s.events.size() << " events):\n";
  for (const Event& event : s.events) {
    out << "  #" << event.seq << " op=" << event.op;
    if (!event.op_label.empty()) out << "(" << event.op_label << ")";
    out << " " << ToString(event.kind) << " " << event.point;
    if (!event.detail.empty()) out << " [" << event.detail << "]";
    if (event.ino != 0) out << " ino=" << event.ino;
    if (event.txn != 0) out << " txn=" << event.txn;
    out << "\n";
  }
  out << "operation states:\n";
  for (const auto& entry : s.ops) {
    const OpState& state = entry.second;
    out << "  op=" << state.id << " " << state.label
        << (state.done ? " done" : (state.blocked ? " blocked" : " runnable"))
        << " wait=" << state.wait_kind << ":" << state.wait_point
        << " held=" << HeldText(state);
    if (!state.error.empty()) out << " error=" << state.error;
    out << "\n";
  }
  if (!s.deadlock.empty()) out << "DEADLOCK: " << s.deadlock << "\n";
  return out.str();
}

void Controller::Abandon() {
  Scheduler& s = S();
  {
    std::lock_guard<std::mutex> lk(s.mutex);
    s.abort_waiters = true;
    s.active.store(false, std::memory_order_relaxed);
  }
  s.cv.notify_all();
}

void Controller::JoinAll() {
  Abandon();
  std::vector<std::pair<uint64_t, std::thread>> threads;
  {
    std::lock_guard<std::mutex> lk(S().mutex);
    threads.swap(impl_->joinable);
  }
  std::string first_error;
  for (auto& entry : threads) {
    if (entry.second.joinable()) entry.second.join();
    std::lock_guard<std::mutex> lk(S().mutex);
    auto it = S().ops.find(entry.first);
    if (it != S().ops.end() && !it->second.cancelled &&
        !it->second.error.empty() && first_error.empty()) {
      first_error = it->second.error;
    }
  }
  if (!first_error.empty()) Fail(first_error);
}

void Controller::SetFailureHandler(FailureHandler handler) {
  std::lock_guard<std::mutex> lk(S().mutex);
  impl_->failure_handler = handler;
}

void Controller::Fail(const std::string& message) {
  FailureHandler handler;
  {
    std::lock_guard<std::mutex> lk(S().mutex);
    handler = impl_->failure_handler;
  }
  if (handler != nullptr) handler(message);
}

}  // namespace xfs::conc

#endif  // XFSWIN_CONCURRENCY_HOOKS
