// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - read path.
//
// Owner: inode parsing/lookup, directory enumeration and data reads. It does
// not own caching policy (cache.cpp), mutation or journal state.
#include "xfs/filesystem.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/extent.h"
#include "xfs/ialloc.h"
#include "xfs/inode.h"
#include "xfs/perf_stats.h"
#include "xfs/symlink.h"
#include "xfs/verifier.h"

namespace xfs {

namespace bt = ondisk::btree;
namespace di = ondisk::inode;
namespace sym = ondisk::symlink;
namespace lf = logfmt;

namespace {

constexpr uint32_t kNullAgBlock = 0xffffffffu;

Result<InobtRecord> LookupInodeRecord(BlockDevice& dev, const XfsGeometry& geo,
                                      uint32_t agno, uint32_t agino) {
  auto agi = ReadAgi(dev, geo, agno);
  if (!agi.ok()) return agi.error();
  BtreeSpec spec;
  spec.name = "inobt";
  spec.format = BtreeFormat::kShort;
  spec.block_size = geo.block_size;
  spec.key_size = bt::kInobtKeySize;
  spec.ptr_size = bt::kInobtPtrSize;
  spec.rec_size = bt::kInobtRecSize;
  spec.magic = ondisk::kIbtCrcMagic;
  spec.null_ptr = kNullAgBlock;
  spec.expected_owner = agno;
  spec.owner_size = 4;
  spec.uuid = geo.meta_uuid;
  spec.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
    if (agbno >= geo.AgBlocksFor(agno)) return false;
    *daddr = geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))) /
             512;
    return true;
  };
  InobtRecord found;
  bool have = false;
  BtreeWalkCallbacks cb;
  cb.read_block = [&](uint64_t agbno, std::vector<uint8_t>* buf) {
    if (agbno >= geo.AgBlocksFor(agno)) return false;
    buf->resize(geo.block_size);
    return dev.Read(geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))),
                    buf->data(), buf->size());
  };
  cb.ptr_in_range = [&](uint64_t agbno) {
    return agbno < geo.AgBlocksFor(agno);
  };
  cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                 const uint8_t* buf) -> Status {
    if (meta.level != 0) return OkStatus();
    for (uint16_t i = 0; i < meta.numrecs; ++i) {
      const uint8_t* rec = BtreeLeafRec(buf, meta, spec, i);
      InobtRecord r;
      r.startino = GetBE32(rec);
      r.free = GetBE64(rec + 8);
      if (geo.features.spinodes) {
        r.holemask = GetBE16(rec + 4);
        r.count = GetU8(rec + 6);
        r.freecount = GetU8(rec + 7);
      } else {
        r.holemask = 0;
        r.count = ondisk::kInodesPerChunk;
        r.freecount = GetBE32(rec + 4);
      }
      if (agino >= r.startino &&
          agino < r.startino + ondisk::kInodesPerChunk) {
        found = r;
        have = true;
      }
    }
    return OkStatus();
  };
  Status s = WalkBtreePath(spec, agi.value().root, agino, cb);
  if (!s.ok()) return s.error();
  if (!have) {
    return XFS_ERR(XfsError::kNotFound,
                   "inode number has no inobt record")
        .Ag(agno)
        .Metadata("inobt")
        .Actual(agino);
  }
  return found;
}

}  // namespace

Result<XfsInode> XfsFilesystem::OpenInode(uint64_t ino) const {
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  if (perf_on) perf.inode_opens.fetch_add(1, std::memory_order_relaxed);
  auto overlay_deferred = [&](XfsInode& inode) {
    std::lock_guard<std::mutex> deferred_lock(deferred_mutex_);
    auto it = deferred_times_.find(ino);
    if (it != deferred_times_.end()) {
      inode.mtime = it->second;
      inode.ctime = it->second;
    }
  };
  auto finish = [&](XfsInode inode) -> Result<XfsInode> {
    if (inode.OnUnlinkedList() && !writable_) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "inode is on the unlinked list; reading it read-only is "
                     "not safe without journal replay")
          .Ino(ino)
          .Metadata("inode");
    }
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      constexpr size_t kMaxCachedInodes = 32768;
      if (inode_cache_.size() >= kMaxCachedInodes) inode_cache_.clear();
      inode_cache_.emplace(ino, inode);
    }
    overlay_deferred(inode);
    return inode;
  };

  bool cache_hit = false;
  XfsInode cached;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = inode_cache_.find(ino);
    if (it != inode_cache_.end()) {
      cached = it->second;
      cache_hit = true;
    }
  }
  if (cache_hit) {
    if (perf_on) perf.inode_cache_hits.fetch_add(1, std::memory_order_relaxed);
    // Overlay after dropping cache_mutex_: deferred_mutex_ is a leaf and must
    // not extend the inode-cache critical section.
    overlay_deferred(cached);
    return cached;
  }
  if (perf_on) perf.inode_cache_misses.fetch_add(1, std::memory_order_relaxed);

  if (!geometry_.InoValid(ino)) {
    return XFS_ERR(XfsError::kOutOfRange, "inode number out of range")
        .Ino(ino)
        .Metadata("inode");
  }

  // Sparse-inode filesystems have 64 logical slots per inode record but may
  // back only some of them. A hole slot has no inode cluster at all; report it
  // as not allocated instead of reading unrelated device bytes.
  if (geometry_.features.spinodes) {
    const uint32_t agno = geometry_.InoAgno(ino);
    const uint32_t agino = geometry_.InoAgino(ino);
    auto rec = LookupInodeRecord(*device_, geometry_, agno, agino);
    if (!rec.ok()) return rec.error();
    const uint32_t offset = agino - rec.value().startino;
    if (!((InobtAllocMask(rec.value().holemask) >> offset) & 1ULL)) {
      return XFS_ERR(XfsError::kNotFound,
                     "inode number is a sparse inode chunk hole")
          .Ino(ino)
          .Metadata("inode")
          .Actual(agino);
    }
  }

  // Read a whole 16 KiB inode chunk once; a directory full of files usually
  // has many inodes in the same chunk, so this cuts device reads ~32x.
  const uint64_t inode_off = geometry_.InoToByteOffset(ino);
  const uint64_t chunk_start = inode_off & ~(kInodeChunkBytes - 1);
  std::shared_ptr<const std::vector<uint8_t>> chunk;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = inode_chunk_cache_.find(chunk_start);
    if (it != inode_chunk_cache_.end()) chunk = it->second;
  }
  if (!chunk) {
    const uint64_t dev_size = device_->Size();
    uint64_t len = kInodeChunkBytes;
    if (chunk_start + len > dev_size) {
      len = dev_size > chunk_start ? dev_size - chunk_start : 0;
    }
    if (len >= geometry_.inode_size) {
      auto buf = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(len));
      if (!device_->Read(chunk_start, buf->data(), buf->size())) {
        return XFS_ERR(XfsError::kIoError, "failed to read inode")
            .Ino(ino)
            .Offset(chunk_start)
            .Metadata("inode");
      }
      {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        constexpr size_t kMaxChunks = 4096;  // 64 MiB of inode chunks
        if (inode_chunk_cache_.size() >= kMaxChunks) inode_chunk_cache_.clear();
        inode_chunk_cache_.emplace(chunk_start, buf);
      }
      chunk = std::move(buf);
    }
  }

  const uint64_t local = inode_off - chunk_start;
  if (chunk && local + geometry_.inode_size <= chunk->size()) {
    XfsInode out;
    Status s = ParseInode(chunk->data() + local,
                          static_cast<size_t>(geometry_.inode_size),
                          geometry_, ino, &out);
    if (!s.ok()) return s.error();
    return finish(std::move(out));
  }

  auto inode = ReadInode(*device_, geometry_, ino);
  if (!inode.ok()) return inode.error();
  return finish(std::move(inode.value()));
}


Result<XfsInode> XfsFilesystem::LookupPath(std::string_view path) const {
  uint64_t ino = geometry_.rootino;
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();

  size_t pos = 0;
  while (pos < path.size()) {
    while (pos < path.size() && path[pos] == '/') ++pos;
    if (pos >= path.size()) break;
    size_t end = path.find('/', pos);
    if (end == std::string_view::npos) end = path.size();
    std::string_view component = path.substr(pos, end - pos);
    pos = end;

    if (component == ".") continue;
    if (!inode.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory, "path component is not a "
                                             "directory")
          .Ino(inode.value().ino);
    }
    auto entry = LookupName(inode.value().ino, component);
    if (!entry.ok()) return entry.error();
    auto next = OpenInode(entry.value().ino);
    if (!next.ok()) return next.error();
    inode = std::move(next);
  }
  return inode;
}

Result<std::vector<XfsDirEntry>> XfsFilesystem::ListDirectory(
    uint64_t ino) const {
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = dir_cache_.find(ino);
    if (it != dir_cache_.end()) {
      it->second.last_used = ++dir_cache_clock_;
      if (perf_on) {
        perf.directory_cache_hits.fetch_add(1, std::memory_order_relaxed);
      }
      return it->second.entries;
    }
  }
  if (perf_on) {
    perf.directory_cache_misses.fetch_add(1, std::memory_order_relaxed);
  }

  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (!inode.value().IsDir()) {
    return XFS_ERR(XfsError::kNotDirectory, "inode is not a directory")
        .Ino(ino);
  }
  std::vector<XfsDirEntry> entries;
  Status s = xfs::ListDirectory(
      *device_, geometry_, inode.value(),
      [&](const XfsDirEntry& e) -> Status {
        entries.push_back(e);
        return OkStatus();
      });
  if (!s.ok()) return s.error();
  CacheDirectory(ino, entries);
  return entries;
}

Result<XfsDirEntry> XfsFilesystem::LookupName(uint64_t dir_ino,
                                              std::string_view name) const {
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = dir_cache_.find(dir_ino);
    if (it != dir_cache_.end()) {
      it->second.last_used = ++dir_cache_clock_;
      if (perf_on) {
        perf.directory_cache_hits.fetch_add(1, std::memory_order_relaxed);
      }
      auto indexed = it->second.index.find(std::string(name));
      if (indexed == it->second.index.end()) {
        return XFS_ERR(XfsError::kNotFound, "name not found in directory")
            .Ino(dir_ino);
      }
      return it->second.entries[indexed->second];
    }
  }
  if (perf_on) {
    perf.directory_cache_misses.fetch_add(1, std::memory_order_relaxed);
  }

  // Populate the full directory cache (bounded by CacheDirectory) instead of
  // doing a one-off targeted lookup. The common caller pattern is a namespace
  // loop -- create/unlink/stat repeatedly in the same directory -- and a
  // targeted lookup leaves the cache cold, making every iteration re-read the
  // directory and turning a large-directory create loop quadratic. A single
  // random lookup on a huge directory pays one bounded parse here and all
  // following lookups are hash hits.
  auto listed = ListDirectory(dir_ino);
  if (!listed.ok()) return listed.error();
  bool cache_complete = false;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = dir_cache_.find(dir_ino);
    if (it != dir_cache_.end()) {
      cache_complete = true;
      auto indexed = it->second.index.find(std::string(name));
      if (indexed != it->second.index.end()) {
        if (perf_on) {
          perf.directory_cache_hits.fetch_add(1, std::memory_order_relaxed);
        }
        return it->second.entries[indexed->second];
      }
    }
  }
  if (cache_complete) {
    return XFS_ERR(XfsError::kNotFound, "name not found in directory")
        .Ino(dir_ino);
  }
  // The directory is larger than the bounded cache budget. Fall back to the
  // targeted on-disk lookup instead of reporting a false negative.
  auto inode = OpenInode(dir_ino);
  if (!inode.ok()) return inode.error();
  return xfs::LookupName(*device_, geometry_, inode.value(), name);
}

Result<size_t> XfsFilesystem::ReadFile(uint64_t ino, uint64_t offset,
                                       void* buffer, size_t length) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  return ReadFile(inode.value(), nullptr, offset, buffer, length);
}

Result<size_t> XfsFilesystem::ReadFile(
    const XfsInode& inode, const std::vector<XfsExtent>* cached_extents,
    uint64_t offset, void* buffer, size_t length) const {
  PerfStats::ReadScope read_scope;
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  if (perf_on) perf.read_calls.fetch_add(1, std::memory_order_relaxed);
  if (inode.IsDir()) {
    return XFS_ERR(XfsError::kNotRegularFile, "cannot ReadFile a directory")
        .Ino(inode.ino);
  }

  if (inode.format == di::kFormatLocal) {
    return ReadFileData(*device_, geometry_, inode, offset, buffer, length);
  }

  std::shared_ptr<const std::vector<XfsExtent>> owned_extents;
  const std::vector<XfsExtent>* extents = cached_extents;
  if (!extents) {
    auto loaded = LoadExtentsCached(inode);
    if (!loaded.ok()) return loaded.error();
    owned_extents = std::move(loaded.value());
    extents = owned_extents.get();
  }
  // Large requests read straight into the caller's buffer: routing them
  // through the chunk cache would add a full memory copy per chunk and evict
  // cached thumbnail data.
  if (length >= kDataChunkBytes) {
    auto direct = ReadFileData(*device_, geometry_, inode, offset, buffer,
                               length, extents);
    if (direct.ok() && perf_on) {
      perf.read_bytes.fetch_add(direct.value(), std::memory_order_relaxed);
    }
    return direct;
  }

  // Serve from the 256 KiB chunk cache: a thumbnail decoder issues many small
  // reads against the same few files; the first read fetches a whole chunk
  // (read-ahead) and repeat reads are pure memory copies.
  uint8_t* out = static_cast<uint8_t*>(buffer);
  size_t total = 0;
  const uint64_t size = inode.size;
  while (total < length && offset < size) {
    const size_t want = static_cast<size_t>(
        std::min<uint64_t>(length - total, size - offset));
    const uint64_t chunk_index = offset / kDataChunkBytes;
    const uint64_t chunk_start = chunk_index * kDataChunkBytes;
    const uint64_t chunk_len =
        std::min<uint64_t>(kDataChunkBytes, size - chunk_start);

    // Snapshot the data generation before touching the device: a concurrent
    // write bumps it, which makes both this lookup and the later insert
    // refuse the chunk (see InsertDataChunk).
    const uint64_t generation = DataGeneration(inode.ino);
    auto data = LookupDataChunk(inode.ino, chunk_index);
    if (!data) {
      auto loaded =
          std::make_shared<AlignedBuffer>(static_cast<size_t>(chunk_len));
      auto read = ReadFileData(*device_, geometry_, inode, chunk_start,
                               loaded->data(), loaded->size(), extents);
      if (!read.ok()) return read.error();
      loaded->resize(read.value());
      data = InsertDataChunk(inode.ino, chunk_index, std::move(loaded),
                             generation);
    }

    const size_t in_chunk = static_cast<size_t>(offset - chunk_start);
    if (in_chunk >= data->size()) break;  // safety: never spin
    const size_t n = std::min<size_t>(want, data->size() - in_chunk);
    std::memcpy(out + total, data->data() + in_chunk, n);
    total += n;
    offset += n;
  }
  if (perf_on) {
    perf.read_bytes.fetch_add(total, std::memory_order_relaxed);
  }
  return total;
}

Result<std::shared_ptr<const std::vector<XfsExtent>>>
XfsFilesystem::LoadExtentsCached(const XfsInode& inode) const {
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = extent_cache_.find(inode.ino);
    if (it != extent_cache_.end()) {
      if (perf_on) {
        perf.extent_cache_hits.fetch_add(1, std::memory_order_relaxed);
      }
      return it->second;
    }
  }
  if (perf_on) {
    perf.extent_cache_misses.fetch_add(1, std::memory_order_relaxed);
  }
  auto loaded = std::make_shared<std::vector<XfsExtent>>();
  Status s = LoadDataForkExtents(*device_, geometry_, inode, loaded.get());
  if (!s.ok()) return s.error();
  std::shared_ptr<const std::vector<XfsExtent>> shared(loaded);
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    constexpr size_t kMaxCachedExtentLists = 8192;
    if (extent_cache_.size() >= kMaxCachedExtentLists) extent_cache_.clear();
    // A concurrent mutation may have replaced the extent list between the
    // lookup above and the insert here; keep whichever entry is already there
    // (the invalidation that replaced it knows about the newer state).
    auto existing = extent_cache_.find(inode.ino);
    if (existing != extent_cache_.end()) return existing->second;
    extent_cache_.emplace(inode.ino, shared);
  }
  return shared;
}


Result<std::string> XfsFilesystem::ReadSymlink(uint64_t ino) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (!inode.value().IsSymlink()) {
    return XFS_ERR(XfsError::kNotSymlink, "inode is not a symlink").Ino(ino);
  }
  return ReadSymlinkTarget(*device_, geometry_, inode.value());
}


Result<XfsFileInfo> XfsFilesystem::GetFileInfo(uint64_t ino) const {
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = info_cache_.find(ino);
    if (it != info_cache_.end()) return it->second;
  }

  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  const XfsInode& i = inode.value();
  XfsFileInfo info;
  info.ino = i.ino;
  info.mode = i.mode;
  info.uid = i.uid;
  info.gid = i.gid;
  info.projid = i.projid;
  info.nlink = i.nlink;
  info.size = i.size;
  info.nblocks = i.nblocks;
  info.nextents = i.nextents;
  info.anextents = i.anextents;
  info.atime = i.atime;
  info.mtime = i.mtime;
  info.ctime = i.ctime;
  info.crtime = i.crtime;
  info.di_flags = i.flags;
  info.immutable = (i.flags & di::kDiFlagImmutable) != 0;
  info.append_only = (i.flags & di::kDiFlagAppend) != 0;

  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    constexpr size_t kMaxCachedInfos = 32768;
    if (info_cache_.size() >= kMaxCachedInfos) info_cache_.clear();
    info_cache_.emplace(ino, info);
  }
  return info;
}

}  // namespace xfs
