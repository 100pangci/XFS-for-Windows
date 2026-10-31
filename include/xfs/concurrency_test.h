// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - deterministic concurrency controller (CONC-1 tests).
//
// Only available in builds configured with -DXFSWIN_CONCURRENCY_HOOKS=ON.
// Tests use it to run operation threads one turn at a time, pause them at
// named sync points or lock waits, and record the resulting event trace. The
// same WaitFor/Resume script is the deterministic replay: a different path
// fails the wait (with a full dump) instead of silently passing.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#ifdef XFSWIN_CONCURRENCY_HOOKS

namespace xfs::conc {

enum class EventKind {
  kOpEnter,
  kOpExit,
  kSync,
  kLockAcquire,
  kLockBlock,
  kLockRelease,
  kDeadlock,
};

const char* ToString(EventKind kind);

struct Event {
  uint64_t seq = 0;
  uint64_t op = 0;
  EventKind kind = EventKind::kSync;
  std::string point;   // sync point name or lock name
  std::string detail;
  uint64_t ino = 0;
  uint64_t txn = 0;
  std::string op_label;
};

class Controller {
 public:
  static Controller& Get();

  // Starts a new scheduling session. All previously registered operations and
  // the event log are cleared. Must be called before spawning operations.
  void Enable();
  // Releases every blocked operation and stops intercepting locks. Safe to
  // call while operations are still finishing; JoinAll also disables.
  void Disable();
  bool enabled() const;

  // Spawns a scheduled operation. The thread waits in EnterOp until Start().
  // The returned id is used by Start/Resume/WaitFor.
  uint64_t Spawn(std::string label, std::function<void()> body);
  // Lets a spawned operation begin (or continue after EnterOp was entered
  // before Start). Exactly one operation should be started/resumed at a time.
  void Start(uint64_t op);
  void Resume(uint64_t op);
  // Lets one operation pass every later sync point without stopping. Lock
  // waits are still enforced. Returns true when the operation exited. The
  // controller must have no other runnable operation that the free-running
  // one could contend with (used for "let B finish while A is paused").
  bool RunToCompletion(uint64_t op, uint64_t timeout_ms = 30000);
  // Resumes every operation that is currently blocked. Used by stress tests
  // that only want deterministic interleaving at their own sync points.
  void ResumeAll();

  // Deterministic stress scheduling. RunnableOps returns the operations that
  // can make progress right now: paused at a sync point, or waiting for a
  // lock whose current owners are compatible with the request. The sorted
  // order makes a seeded choice reproducible. Step resumes one operation and
  // waits until it stops again (or exits), so a driver that always picks from
  // RunnableOps advances the whole workload one deterministic turn at a time.
  std::vector<uint64_t> RunnableOps() const;
  bool Step(uint64_t op, uint64_t timeout_ms = 30000);

  // Drops the oldest events once the log exceeds `limit`; the cursor is
  // adjusted so WaitFor/WaitNext keep observing events after the trim.
  void TrimEvents(size_t limit);

  // Waits until `op` reports `kind` with `point`. Empty strings match any.
  // The controller cursor advances past the matched event, so successive
  // waits observe successive events. Returns false on timeout or deadlock.
  bool WaitFor(uint64_t op, EventKind kind, std::string_view point,
               uint64_t timeout_ms = 30000);
  bool WaitFor(uint64_t op, std::string_view point,
               uint64_t timeout_ms = 30000) {
    return WaitFor(op, EventKind::kSync, point, timeout_ms);
  }
  // Waits for any operation to report the given event.
  bool WaitForAny(EventKind kind, std::string_view point,
                  uint64_t timeout_ms = 30000);
  // Returns the next event of `op` after the controller cursor. This lets a
  // test step an operation through an unknown number of intermediate sync
  // points before the one it wants to pause at.
  bool WaitNext(uint64_t op, Event* out, uint64_t timeout_ms = 30000);
  bool WaitDone(uint64_t op, uint64_t timeout_ms = 30000);

  // Snapshot helpers used by invariant checks and on failure.
  std::vector<Event> Events() const;
  std::string Dump() const;
  bool deadlocked() const;
  void ClearCursor();

  // Joins every spawned thread. Blocked operations are abandoned: they
  // unwind with a cancellation (without taking the contended real mutex), so
  // even a deliberately induced deadlock cannot hang test teardown.
  void JoinAll();
  // Disables scheduling and makes every parked operation unwind instead of
  // continuing. Called by JoinAll; exposed for tests that need to inspect the
  // event log after a detected cycle.
  void Abandon();

  // Blocks the calling (controller) thread until all spawned operations have
  // exited. Does not disable scheduling; useful before teardown.
  bool WaitAllDone(uint64_t timeout_ms = 30000);

  // Test-only assertion helper: throws TestFailure (via a callback so the
  // core library does not depend on the test harness).
  using FailureHandler = void (*)(const std::string& message);
  void SetFailureHandler(FailureHandler handler);
  void Fail(const std::string& message);

 private:
  Controller();
  ~Controller();
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;
  struct Impl;
  Impl* impl_;
};

// RAII helper for tests that only need deterministic setup around a scenario.
class Session {
 public:
  Session() { Controller::Get().Enable(); }
  ~Session() { Controller::Get().Disable(); }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
};

}  // namespace xfs::conc

#endif  // XFSWIN_CONCURRENCY_HOOKS
