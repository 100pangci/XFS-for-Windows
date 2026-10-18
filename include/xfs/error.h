// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - error reporting.
//
// The XFS core never signals failure with a bare bool: every failure carries
// an XfsError code plus optional structured context (offset, AG, inode,
// block number, expected/actual values, metadata type). This matters when
// debugging a real disk: "Directory block CRC mismatch, AG=2 FSB=0x18a234
// expected=0xdeadbeef actual=0x12345678" is actionable, "false" is not.
#pragma once

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace xfs {

enum class XfsError : uint32_t {
  kOk = 0,

  // I/O and environment
  kIoError,
  kShortRead,
  kPermissionDenied,
  kNotFound,
  kAlreadyExists,
  kInvalidArgument,

  // Bounds / integrity of on-disk data
  kOutOfRange,
  kBadMagic,
  kBadCrc,
  kBadUuid,
  kBadOwner,
  kBadBlockNo,
  kBadVersion,
  kBadLength,
  kBadValue,

  // Corrupt or unsupported metadata
  kCorruptSuperblock,
  kCorruptAgHeader,
  kCorruptBtree,
  kCorruptInode,
  kCorruptDirectory,
  kCorruptExtent,

  // Explicitly unsupported / unsafe conditions
  kUnsupportedFeature,   // feature bit this build does not implement
  kUnsupportedGeometry,  // geometry outside of what we support
  kNeedsRepair,          // sb_inprogress / NEEDSREPAIR set
  kDirtyLog,             // log needs replay; read-only mounts are unsafe
  kNotV5,                // XFS v4 or older

  // Filesystem semantics
  kNotDirectory,
  kNotRegularFile,
  kNotSymlink,
  kNameTooLong,
  kDirectoryNotEmpty,
  kReadOnlyVolume,
  kNotMounted,

  // Read-write mutation limitations (this build has no allocator yet, so a
  // write can only land in blocks that are already allocated and written).
  kWriteExtendsFile,      // the range would move EOF
  kWriteNeedsAllocation,  // the range covers a hole
  kWriteNeedsConversion,  // the range covers an unwritten (preallocated) extent
  kNoSpace,               // an allocation could not be satisfied

  kInternal,
};

const char* ToString(XfsError code);

// Structured context attached to a failure.
struct ErrorContext {
  XfsError code = XfsError::kOk;
  std::string message;  // human readable detail

  // Locators ("what were we looking at").
  std::optional<uint64_t> offset;   // byte offset in the filesystem
  std::optional<uint64_t> fsb;      // filesystem block (absolute)
  std::optional<uint32_t> agno;     // allocation group
  std::optional<uint32_t> agbno;    // block within AG
  std::optional<uint64_t> ino;      // inode number
  std::optional<uint64_t> dir_block; // directory offset in blocks

  // Discrepancy detail.
  std::optional<uint64_t> expected;
  std::optional<uint64_t> actual;

  std::string metadata;  // "superblock", "AGF", "inode", "dir leaf", ...

  // Single line rendering including all available context.
  std::string Format() const;
};

// Convenience builders.
ErrorContext MakeError(XfsError code, std::string message);
inline ErrorContext MakeError(XfsError code, std::string message,
                              std::string metadata) {
  ErrorContext e = MakeError(code, std::move(message));
  e.metadata = std::move(metadata);
  return e;
}

inline ErrorContext MakeError(XfsError code, std::string message, uint64_t off,
                              std::string metadata) {
  ErrorContext e = MakeError(code, std::move(message), std::move(metadata));
  e.offset = off;
  return e;
}

// ---------------------------------------------------------------------------
// Result<T> - value or ErrorContext.
// ---------------------------------------------------------------------------
template <typename T>
class Result {
 public:
  Result(T value) : data_(std::move(value)) {}                  // NOLINT
  Result(ErrorContext error) : data_(std::move(error)) {}       // NOLINT

  bool ok() const { return std::holds_alternative<T>(data_); }
  explicit operator bool() const { return ok(); }

  // Callers must check ok() first.
  T& value() { return std::get<T>(data_); }
  const T& value() const { return std::get<T>(data_); }

  const ErrorContext& error() const { return std::get<ErrorContext>(data_); }

  T ValueOr(T fallback) const { return ok() ? value() : std::move(fallback); }

 private:
  std::variant<T, ErrorContext> data_;
};

// Result<void> equivalent.
struct Unit {};
class Status {
 public:
  Status() = default;  // NOLINT - "ok" by default
  Status(ErrorContext error) : error_(std::move(error)) {}  // NOLINT

  bool ok() const { return error_.code == XfsError::kOk; }
  explicit operator bool() const { return ok(); }
  const ErrorContext& error() const { return error_; }

 private:
  ErrorContext error_;
};

inline Status OkStatus() { return Status(); }

// ---------------------------------------------------------------------------
// Helpers to build errors with locators in a fluent style. ErrorBuilder
// derives from ErrorContext so that a single standard derived-to-base
// conversion lets it be returned as Status or Result<T>.
// ---------------------------------------------------------------------------
class ErrorBuilder : public ErrorContext {
 public:
  explicit ErrorBuilder(XfsError code, std::string message) {
    this->code = code;
    this->message = std::move(message);
  }

  ErrorBuilder& Metadata(std::string v) {
    metadata = std::move(v);
    return *this;
  }
  ErrorBuilder& Offset(uint64_t v) {
    offset = v;
    return *this;
  }
  ErrorBuilder& Fsb(uint64_t v) {
    fsb = v;
    return *this;
  }
  ErrorBuilder& Ag(uint32_t v) {
    agno = v;
    return *this;
  }
  ErrorBuilder& Agbno(uint32_t v) {
    agbno = v;
    return *this;
  }
  ErrorBuilder& Ino(uint64_t v) {
    ino = v;
    return *this;
  }
  ErrorBuilder& DirBlock(uint64_t v) {
    dir_block = v;
    return *this;
  }
  ErrorBuilder& Expected(uint64_t v) {
    expected = v;
    return *this;
  }
  ErrorBuilder& Actual(uint64_t v) {
    actual = v;
    return *this;
  }
};

#define XFS_ERR(code, msg) ErrorBuilder(code, msg)

}  // namespace xfs
