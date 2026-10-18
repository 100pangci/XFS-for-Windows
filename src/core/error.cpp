// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/error.h"

namespace xfs {

const char* ToString(XfsError code) {
  switch (code) {
    case XfsError::kOk: return "OK";
    case XfsError::kIoError: return "I/O error";
    case XfsError::kShortRead: return "short read";
    case XfsError::kPermissionDenied: return "permission denied";
    case XfsError::kNotFound: return "not found";
    case XfsError::kAlreadyExists: return "already exists";
    case XfsError::kInvalidArgument: return "invalid argument";
    case XfsError::kOutOfRange: return "out of range";
    case XfsError::kBadMagic: return "bad magic";
    case XfsError::kBadCrc: return "bad CRC";
    case XfsError::kBadUuid: return "bad UUID";
    case XfsError::kBadOwner: return "bad owner";
    case XfsError::kBadBlockNo: return "bad block number";
    case XfsError::kBadVersion: return "bad version";
    case XfsError::kBadLength: return "bad length";
    case XfsError::kBadValue: return "bad value";
    case XfsError::kCorruptSuperblock: return "corrupt superblock";
    case XfsError::kCorruptAgHeader: return "corrupt AG header";
    case XfsError::kCorruptBtree: return "corrupt btree";
    case XfsError::kCorruptInode: return "corrupt inode";
    case XfsError::kCorruptDirectory: return "corrupt directory";
    case XfsError::kCorruptExtent: return "corrupt extent";
    case XfsError::kUnsupportedFeature: return "unsupported feature";
    case XfsError::kUnsupportedGeometry: return "unsupported geometry";
    case XfsError::kNeedsRepair: return "filesystem needs repair";
    case XfsError::kDirtyLog: return "log needs replay";
    case XfsError::kNotV5: return "not an XFS v5 filesystem";
    case XfsError::kNotDirectory: return "not a directory";
    case XfsError::kNotRegularFile: return "not a regular file";
    case XfsError::kNotSymlink: return "not a symlink";
    case XfsError::kNameTooLong: return "name too long";
    case XfsError::kDirectoryNotEmpty: return "directory not empty";
    case XfsError::kReadOnlyVolume: return "volume is read-only";
    case XfsError::kNotMounted: return "not mounted";
    case XfsError::kWriteExtendsFile:
      return "write would extend the file";
    case XfsError::kWriteNeedsAllocation:
      return "write range covers a hole";
    case XfsError::kWriteNeedsConversion:
      return "write range covers an unwritten extent";
    case XfsError::kNoSpace: return "no space left";
    case XfsError::kInternal: return "internal error";
  }
  return "unknown error";
}

std::string ErrorContext::Format() const {
  std::ostringstream os;
  os << ToString(code);
  if (!metadata.empty()) os << " [" << metadata << "]";
  if (!message.empty()) os << ": " << message;
  if (agno) os << " AG=" << *agno;
  if (agbno) os << " agbno=0x" << std::hex << *agbno << std::dec;
  if (fsb) os << " fsb=0x" << std::hex << *fsb << std::dec;
  if (ino) os << " ino=" << *ino;
  if (dir_block) os << " dir_block=" << *dir_block;
  if (offset) os << " offset=0x" << std::hex << *offset << std::dec;
  if (expected && actual) {
    os << " expected=0x" << std::hex << *expected << " actual=0x" << *actual
       << std::dec;
  } else {
    if (expected) os << " expected=0x" << std::hex << *expected << std::dec;
    if (actual) os << " actual=0x" << std::hex << *actual << std::dec;
  }
  return os.str();
}

ErrorContext MakeError(XfsError code, std::string message) {
  ErrorContext e;
  e.code = code;
  e.message = std::move(message);
  return e;
}

}  // namespace xfs
