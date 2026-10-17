// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - central cache invalidation for mutations.
//
// The read path keeps several caches (parsed inodes, inode chunks, XfsFileInfo,
// extent lists, directory listings, file data chunks). Once the filesystem can
// be modified, every mutation must invalidate exactly the caches that may now
// hold stale information.
//
// The rule for this project: a mutation never touches a cache directly. It
// builds a CacheInvalidation and hands it to
// XfsFilesystem::InvalidateCaches(). That keeps the invalidation policy in one
// place, so a cache added later cannot be forgotten at one of the (many) write
// call sites, and the policy table below is the single source of truth.
//
//   mutation                        invalidation
//   ------------------------------  ----------------------------------------
//   overwrite file data in place    data range (bytes written)
//   file extend / truncate          inode + extents + data (whole inode)
//   timestamps / mode / nlink       inode
//   extent tree changes             extents + inode
//   directory entry add/remove      directory (of that directory) + inode
//   inode allocate / free           inode + inode chunk + data
//   anything unknown                Everything()
#pragma once

#include <cstdint>

namespace xfs {

struct CacheInvalidation {
  // One bit per cached object. Multiple objects can be invalidated at once.
  enum Object : uint32_t {
    kInode = 1u << 0,       // parsed inode (size, timestamps, forks)
    kInodeChunk = 1u << 1,  // 16 KiB inode chunk read-ahead buffer
    kFileInfo = 1u << 2,    // XfsFileInfo snapshot used by WinFsp/CLI
    kExtents = 1u << 3,     // parsed data-fork extent list
    kDirectory = 1u << 4,   // directory listing + name index
    kData = 1u << 5,        // file data chunks overlapping [offset, offset+len)
    kAll = 1u << 6,         // drop everything
  };

  uint32_t objects = 0;
  uint64_t ino = 0;  // subject inode (directory, file, ...)
  // Data chunk range in bytes; only meaningful with kData. A zero length
  // invalidates every chunk of `ino`.
  uint64_t data_offset = 0;
  uint64_t data_length = 0;

  // ---- builders for the common mutation shapes ----

  // Timestamps, size counters, flags: the inode itself (and its chunk buffer).
  static CacheInvalidation Inode(uint64_t ino) {
    CacheInvalidation inv;
    inv.objects = kInode | kInodeChunk | kFileInfo;
    inv.ino = ino;
    return inv;
  }
  // Extent tree changed: inode metadata plus the parsed extent list. Any byte
  // of the file may now map to a different place, so all data chunks go too.
  static CacheInvalidation Extents(uint64_t ino) {
    CacheInvalidation inv = Inode(ino);
    inv.objects |= kExtents | kData;
    return inv;
  }
  // Bytes were written to already allocated, already written blocks: only the
  // affected data chunks change (no metadata is touched).
  static CacheInvalidation DataRange(uint64_t ino, uint64_t offset,
                                     uint64_t length) {
    CacheInvalidation inv;
    inv.objects = kData;
    inv.ino = ino;
    inv.data_offset = offset;
    inv.data_length = length;
    return inv;
  }
  // Directory entries changed.
  static CacheInvalidation Directory(uint64_t ino) {
    CacheInvalidation inv = Inode(ino);
    inv.objects |= kDirectory;
    inv.ino = ino;
    return inv;
  }
  // Unknown scope (or a structural change): drop everything.
  static CacheInvalidation Everything() {
    CacheInvalidation inv;
    inv.objects = kAll;
    return inv;
  }

  bool Has(Object object) const { return (objects & object) != 0; }
};

}  // namespace xfs
