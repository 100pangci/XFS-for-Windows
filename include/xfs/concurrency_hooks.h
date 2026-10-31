// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - deterministic concurrency test hooks (CONC-1).
//
// This header provides drop-in replacements for the mutex types used by the
// filesystem core and a small set of named sync points. In a normal build
// (XFSWIN_CONCURRENCY_HOOKS undefined) the wrappers are direct forwards to
// std::mutex / std::shared_mutex and every sync-point macro compiles away.
//
// In a test build the wrappers consult a cooperative scheduler: an operation
// thread runs only while the test controller has granted it a turn; sync
// points and lock acquisitions are reported as events and block until the
// controller resumes them. Because the scheduler owns lock compatibility,
// waiters never block inside the real mutex, which makes an interleaving
// exactly reproducible. See docs/CONCURRENCY.md.
#pragma once

#include <cstdint>
#include <exception>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace xfs::conc {

#ifdef XFSWIN_CONCURRENCY_HOOKS

// True when a controller is active. The fast path in the wrappers still does
// one relaxed atomic load; only CONC-1 test builds compile this branch in.
bool HooksActive();

// The current operation (0 outside a scheduled operation).
uint64_t CurrentOp();

// Thrown by the mutex wrappers when the controller abandons a session while
// this operation is blocked. The operation unwinds without touching the real
// mutex, so a deliberately induced lock-order cycle cannot hang JoinAll.
struct Cancelled : std::exception {
  const char* what() const noexcept override {
    return "concurrency scheduler cancelled";
  }
};

// Scheduler-side lock protocol. LockBefore may block until the controller
// resumes this operation and returns false when the session was abandoned;
// LockAfter reports that the real mutex was acquired; LockRelease runs before
// the real mutex is released and frees the shadow ownership so a waiter can
// be resumed.
bool LockBefore(const void* lock, const char* name, bool exclusive);
void LockAfter(const void* lock, bool exclusive);
void LockRelease(const void* lock, bool exclusive);
bool TryLockBefore(const void* lock, const char* name, bool exclusive);

// Named synchronization point. Blocks until the controller resumes this
// operation. `detail` is copied into the event log.
void Sync(const char* point, std::string detail = {});

// Thread-local context attached to every event: the inode and transaction the
// operation is working on. Sticky until changed.
void SetIno(uint64_t ino);
void SetTxn(uint64_t txn);
uint64_t CurrentIno();
uint64_t CurrentTxn();

// Operation lifetime, called by the controller-owned thread wrapper.
void EnterOp(uint64_t op_id, const char* label);
void LeaveOp();

#else  // !XFSWIN_CONCURRENCY_HOOKS

inline bool HooksActive() { return false; }
inline uint64_t CurrentOp() { return 0; }
inline bool LockBefore(const void*, const char*, bool) { return true; }
inline void LockAfter(const void*, bool) {}
inline void LockRelease(const void*, bool) {}
inline bool TryLockBefore(const void*, const char*, bool) { return true; }
inline void Sync(const char*, std::string = {}) {}
inline void SetIno(uint64_t) {}
inline void SetTxn(uint64_t) {}
inline uint64_t CurrentIno() { return 0; }
inline uint64_t CurrentTxn() { return 0; }
inline void EnterOp(uint64_t, const char*) {}
inline void LeaveOp() {}

#endif  // XFSWIN_CONCURRENCY_HOOKS

// Drop-in Lockable used instead of std::mutex for the filesystem's own locks.
class Mutex {
 public:
  Mutex() = default;
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

#ifdef XFSWIN_CONCURRENCY_HOOKS
  void lock() {
    if (!HooksActive()) {
      impl_.lock();
      return;
    }
    if (!LockBefore(this, name_, true)) throw Cancelled{};
    impl_.lock();
    LockAfter(this, true);
  }
  bool try_lock() {
    if (!HooksActive()) return impl_.try_lock();
    if (!TryLockBefore(this, name_, true)) return false;
    if (impl_.try_lock()) {
      LockAfter(this, true);
      return true;
    }
    LockRelease(this, true);
    return false;
  }
  void unlock() {
    if (HooksActive()) LockRelease(this, true);
    impl_.unlock();
  }
#else
  void lock() { impl_.lock(); }
  bool try_lock() { return impl_.try_lock(); }
  void unlock() { impl_.unlock(); }
#endif
  void SetName(const char* name) { name_ = name; }

 private:
  std::mutex impl_;
  const char* name_ = "mutex";
};

// Drop-in SharedLockable used instead of std::shared_mutex.
class SharedMutex {
 public:
  SharedMutex() = default;
  SharedMutex(const SharedMutex&) = delete;
  SharedMutex& operator=(const SharedMutex&) = delete;

#ifdef XFSWIN_CONCURRENCY_HOOKS
  void lock() {
    if (!HooksActive()) {
      impl_.lock();
      return;
    }
    if (!LockBefore(this, name_, true)) throw Cancelled{};
    impl_.lock();
    LockAfter(this, true);
  }
  bool try_lock() {
    if (!HooksActive()) return impl_.try_lock();
    if (!TryLockBefore(this, name_, true)) return false;
    if (impl_.try_lock()) {
      LockAfter(this, true);
      return true;
    }
    LockRelease(this, true);
    return false;
  }
  void unlock() {
    if (HooksActive()) LockRelease(this, true);
    impl_.unlock();
  }
  void lock_shared() {
    if (!HooksActive()) {
      impl_.lock_shared();
      return;
    }
    if (!LockBefore(this, name_, false)) throw Cancelled{};
    impl_.lock_shared();
    LockAfter(this, false);
  }
  bool try_lock_shared() {
    if (!HooksActive()) return impl_.try_lock_shared();
    if (!TryLockBefore(this, name_, false)) return false;
    if (impl_.try_lock_shared()) {
      LockAfter(this, false);
      return true;
    }
    LockRelease(this, false);
    return false;
  }
  void unlock_shared() {
    if (HooksActive()) LockRelease(this, false);
    impl_.unlock_shared();
  }
#else
  void lock() { impl_.lock(); }
  bool try_lock() { return impl_.try_lock(); }
  void unlock() { impl_.unlock(); }
  void lock_shared() { impl_.lock_shared(); }
  bool try_lock_shared() { return impl_.try_lock_shared(); }
  void unlock_shared() { impl_.unlock_shared(); }
#endif
  void SetName(const char* name) { name_ = name; }

 private:
  std::shared_mutex impl_;
  const char* name_ = "shared_mutex";
};

}  // namespace xfs::conc

// Sync points compile out completely in production builds.
#ifdef XFSWIN_CONCURRENCY_HOOKS
#define XFS_CONC_SYNC(point, detail) ::xfs::conc::Sync((point), (detail))
#define XFS_CONC_INO(ino) ::xfs::conc::SetIno(ino)
#define XFS_CONC_TXN(tid) ::xfs::conc::SetTxn(tid)
#else
#define XFS_CONC_SYNC(point, detail) \
  do {                               \
  } while (0)
#define XFS_CONC_INO(ino) \
  do {                    \
  } while (0)
#define XFS_CONC_TXN(tid) \
  do {                    \
  } while (0)
#endif
