// XFS for Windows - file extent mapping (data fork).
//
// Implements the unified logical-offset -> physical-extent mapping used by
// file reads, symlink reads and directory reads:
//   - local (data lives in the inode; handled by callers)
//   - extents format (inline extent records)
//   - btree format (bmbt blocks, long-pointer btrees)
//
// Holes and unwritten extents are reported so callers can return zeros.
#pragma once

#include <functional>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/inode.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

namespace xfs {

struct XfsExtent {
  uint64_t startoff = 0;    // file block offset
  uint64_t startblock = 0;  // fs block
  uint64_t blockcount = 0;
  bool unwritten = false;

  uint64_t EndBlock() const { return startoff + blockcount; }
};

// Result of mapping a file byte offset.
struct XfsFileMapping {
  bool is_hole = false;
  bool is_unwritten = false;
  uint64_t fs_block = 0;        // first mapped fs block
  uint64_t block_offset = 0;    // offset of the mapped range within the file
  uint64_t block_count = 0;     // length in fs blocks
};

// Loads the whole data fork extent list (extents or btree format).
// `cap` bounds the number of extents loaded (default 1<<24); exceeding it
// returns kUnsupportedGeometry.
Status LoadDataForkExtents(BlockDevice& dev, const XfsGeometry& geo,
                           const XfsInode& inode,
                           std::vector<XfsExtent>* out,
                           uint64_t cap = 1ull << 24);

// Maps a byte offset in a regular file/directory to an extent (or hole).
// Never reads outside the filesystem; validates every extent record.
Result<XfsFileMapping> MapFileOffset(BlockDevice& dev, const XfsGeometry& geo,
                                     const XfsInode& inode,
                                     uint64_t file_offset);

// Reads `len` bytes at `file_offset` from an inode's data, handling holes and
// unwritten extents (zero-filled), short reads at EOF, and multi-extent
// span. Returns bytes actually copied (copied may be < len only at EOF).
Result<size_t> ReadFileData(BlockDevice& dev, const XfsGeometry& geo,
                            const XfsInode& inode, uint64_t file_offset,
                            void* buffer, size_t len);

// Reads the full data fork of a local (in-inode) fork.
Status ReadLocalData(const XfsInode& inode, uint64_t offset, void* buffer,
                     size_t len, size_t* bytes_read);

}  // namespace xfs
