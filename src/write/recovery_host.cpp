// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - journal recovery host (RW-5).
//
// This is the filesystem-side orchestration around the recovery engine:
//
//   RecoverJournal()        scan -> validate -> refuse unsupported -> replay
//                           -> re-read superblock -> finish intents
//                           -> process AGI unlinked lists -> flush
//   AnalyzeJournal()        the read-only variant for `xfswin check`
//   CompleteIntent()        xfs_defer_finish_recovery for EFI: free the
//                           extents in a normal transaction and log the EFD
//   ProcessAgiUnlinkedLists()  xlog_recover_process_iunlinks: walk every AGI
//                           bucket, truncate and free zero-link inodes, clear
//                           buckets that cannot be walked
//
// The replay itself (src/recovery/) never uses a transaction: it patches
// metadata directly, exactly like the kernel. Only the completion phases that
// the kernel also performs through normal transactions (intents, unlinked
// inodes, the final clean record) use the writer, and only after the log
// state has been recovered:
//
//   * the next write continues at the recovered head,
//   * h_tail_lsn stays pinned to the replayed window while intents are
//     unresolved, so a crash during completion re-replays the whole window
//     (the intent/done pairs make that idempotent),
//   * the filesystem is only visible to users after this has finished.
#include "xfs/filesystem.h"

#include <cstring>
#include <string>
#include <vector>

#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/ialloc.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"
#include "xfs/refcount.h"
#include "xfs/refcount_intent.h"
#include "xfs/rmap_intent.h"
#include "xfs/transaction.h"

namespace xfs {
namespace {

namespace lf = logfmt;

uint32_t LoadLE32At(const uint8_t* p, size_t off) { return LoadLE32(p + off); }
uint64_t LoadLE64At(const uint8_t* p, size_t off) {
  return static_cast<uint64_t>(LoadLE32(p + off)) |
         (static_cast<uint64_t>(LoadLE32(p + off + 4)) << 32);
}

// One entry of an EFI/EFD extent array (host byte order in the log).
struct LoggedExtent {
  uint64_t start = 0;  // absolute fs block
  uint32_t len = 0;
};

std::string IntentContext(const PendingIntent& intent) {
  return std::string(LogItemTypeName(intent.type) ? LogItemTypeName(intent.type)
                                                  : "unknown") +
         " tid=" + std::to_string(intent.tid) + " lsn=0x" + [&] {
           char buf[32];
           std::snprintf(buf, sizeof(buf), "%llx",
                         static_cast<unsigned long long>(intent.lsn));
           return std::string(buf);
         }();
}

}  // namespace

Result<XfsFilesystem::RecoverySummary> XfsFilesystem::AnalyzeJournal() const {
  RecoverySummary summary;
  auto scan = ScanLog(*device_, superblock_, geometry_);
  if (!scan.ok()) return scan.error();
  const LogScan& log = scan.value();
  summary.state = log.state;
  summary.detail = log.detail;
  summary.needed = log.state == LogState::kDirty;
  summary.recoverable_transactions = log.transactions.size();
  summary.items = log.ItemCount();
  summary.unsupported_items = log.unsupported_items;
  if (summary.needed && summary.recoverable()) {
    // Unresolved intents the completion pass cannot finish are a refusal too:
    // the scan only proves the replay half can be done.
    for (const PendingIntent& intent : CollectPendingIntents(log)) {
      if (intent.type == lf::kItemEfi || intent.type == lf::kItemRui ||
          intent.type == lf::kItemCui) {
        continue;
      }
      summary.unsupported_items.push_back(
          "unresolved intent " + IntentContext(intent) +
          " requires deferred completion this build does not implement");
    }
  }
  return summary;
}

Result<XfsFilesystem::RecoveryReport> XfsFilesystem::RecoverJournal(
    RecoveryFinish finish) {
  if (!device_->Info().writable) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "journal recovery needs a writable device")
        .Metadata("log");
  }
  auto scan = ScanLog(*device_, superblock_, geometry_);
  if (!scan.ok()) return scan.error();
  return RecoverScannedLog(scan.value(), finish);
}

Result<XfsFilesystem::RecoveryReport> XfsFilesystem::RecoverScannedLog(
    const LogScan& scan, RecoveryFinish finish) {
  RecoveryReport report;
  MaybeCrash(CrashPoint::kRecoveryScan,
             "records=" + std::to_string(scan.records.size()) + " tail=" +
                 std::to_string(scan.tail_block) + " head=" +
                 std::to_string(scan.head_block));
  if (scan.state != LogState::kDirty) {
    if (scan.state == LogState::kUnknown) {
      return XFS_ERR(XfsError::kDirtyLog,
                     "log contains data but no valid record headers; it "
                     "cannot be replayed")
          .Metadata("log");
    }
    // Clean/empty: only the unlinked lists can still need attention (a crash
    // after the unlink transaction was flushed leaves a legal, clean log with
    // zero-link inodes on the AGI lists). The completion uses transactions, so
    // the filesystem must act writable for the duration.
    const bool was_writable = writable_;
    writable_ = true;
    // Adopt the scanned log position even though nothing is replayed:
    //   * the close path must not classify the log a second time - a torn
    //     record behind the newest good one is trimmed here, but the
    //     record-latest classifier would call the log dirty and refuse to
    //     write,
    //   * a log whose tail already reached the head has no unmount record, so
    //     an explicit recovery still closes it at the scanned head.
    // A zeroed log keeps going through the classifier, which knows the fresh
    // "cycle 1, block 0, no previous record" layout.
    if (scan.state == LogState::kClean) {
      log_position_ = LogPosition{};
      log_position_.cycle = scan.head_cycle;
      log_position_.next_block = scan.head_block;
      log_position_.prev_block = scan.prev_block;
      log_position_.tail_lsn = MakeLsn(scan.head_cycle, scan.head_block);
      log_position_.log_blocks = scan.geometry.log_blocks;
      log_position_.record_size = 32 * 1024;  // XLOG_BIG_RECORD_BSIZE
      log_position_.clean = scan.ends_with_unmount_record;
      log_position_valid_ = true;
      log_dirty_ = !scan.ends_with_unmount_record;
    }
    uint64_t cleared = 0;
    auto processed = ProcessAgiUnlinkedLists(&cleared);
    writable_ = was_writable;
    if (!processed.ok()) return processed.error();
    report.unlinked_inodes_processed = processed.value();
    report.agi_buckets_cleared = cleared;
    return report;
  }
  if (!scan.Recoverable()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "cannot recover dirty log: " +
                       (scan.unsupported_items.empty()
                            ? std::string("unsupported content")
                            : scan.unsupported_items[0]))
        .Metadata("log");
  }
  const std::vector<PendingIntent> intents = CollectPendingIntents(scan);
  for (const PendingIntent& intent : intents) {
    if (intent.type != lf::kItemEfi && intent.type != lf::kItemRui &&
        intent.type != lf::kItemCui) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "cannot recover dirty log: unresolved intent " +
                         IntentContext(intent) +
                         " requires deferred completion this build does not "
                         "implement")
          .Metadata("log");
    }
  }

  MaybeCrash(CrashPoint::kRecoveryBeforePass2,
             "transactions=" + std::to_string(scan.transactions.size()));
  RecoveredLogState log_state;
  auto replay = ReplayLog(*device_, superblock_, geometry_, scan, &log_state);
  if (!replay.ok()) return replay.error();
  report.replay = replay.value();
  report.recovered = true;
  MaybeCrash(CrashPoint::kRecoveryAfterMetadataFlush,
             "buffers=" + std::to_string(report.replay.buffers_replayed));

  // Spec: never keep using metadata cached before the replay, and never keep
  // the geometry derived from a superblock the log may have rewritten.
  InvalidateCaches(CacheInvalidation::Everything());
  auto fresh = ReadSuperblock(*device_, 0, superblock_.crc_verified);
  if (!fresh.ok()) return fresh.error();
  superblock_ = fresh.value();
  geometry_ = MakeGeometry(superblock_);
  XFS_SB_INFO("recovered superblock: icount=" << superblock_.icount
                                              << " ifree=" << superblock_.ifree
                                              << " fdblocks="
                                              << superblock_.fdblocks);

  // Lazy superblock counters are not authoritative after a crash: the kernel
  // waits for recovery and then rebuilds them from the AGI/AGF headers
  // (xfs_initialize_perag_data), and the completion transactions below update
  // them, so do it before they run.
  Status refreshed = RefreshSuperblockCounters();
  if (!refreshed.ok()) return refreshed.error();

  // Adopt the recovered log position: the next record continues at the head
  // and the tail stays pinned until every intent is complete.
  log_position_ = LogPosition{};
  log_position_.cycle = log_state.head_cycle;
  log_position_.next_block = log_state.head_block;
  log_position_.prev_block = log_state.prev_block;
  log_position_.tail_lsn = log_state.tail_lsn;
  log_position_.log_blocks = log_state.log_blocks;
  log_position_.record_size = log_state.record_size;
  log_position_.clean = false;
  log_position_valid_ = true;
  log_dirty_ = true;
  recovery_tail_pin_ = log_state.tail_lsn;
  const bool was_writable = writable_;
  writable_ = true;

  // Deferred intent completion (xlog_recover_finish_intents).
  Status status = OkStatus();
  MaybeCrash(CrashPoint::kRecoveryBeforeIntents,
             "intents=" + std::to_string(intents.size()));
  for (size_t i = 0; i < intents.size(); ++i) {
    status = CompleteIntent(intents[i]);
    if (!status.ok()) break;
    report.intents_completed++;
    MaybeCrash(CrashPoint::kRecoveryMidIntents, IntentContext(intents[i]));
  }
  // AGI unlinked lists (xlog_recover_process_iunlinks), run after intents.
  if (status.ok()) {
    MaybeCrash(CrashPoint::kRecoveryBeforeIunlinks, "");
    uint64_t cleared = 0;
    auto processed = ProcessAgiUnlinkedLists(&cleared);
    if (!processed.ok()) {
      status = processed.error();
    } else {
      report.unlinked_inodes_processed = processed.value();
      report.agi_buckets_cleared = cleared;
      MaybeCrash(CrashPoint::kRecoveryMidIunlinks,
                 "freed=" + std::to_string(processed.value()));
    }
  }
  if (!status.ok()) {
    // The log stays authoritative: the metadata replay is idempotent and the
    // tail is still pinned, so the next recovery redoes the whole window.
    writable_ = was_writable;
    return status.error();
  }

  // Everything is completed: the next record may advance the tail past the
  // replayed window.
  recovery_tail_pin_.reset();
  log_position_.tail_lsn =
      MakeLsn(log_position_.cycle, log_position_.next_block);

  status = Flush();
  if (!status.ok()) {
    writable_ = was_writable;
    return status.error();
  }
  if (finish == RecoveryFinish::kCleanLog) {
    MaybeCrash(CrashPoint::kRecoveryBeforeLogClean, "recovery complete");
    status = WriteCleanUnmountRecord();
    if (!status.ok()) {
      writable_ = was_writable;
      return status.error();
    }
    MaybeCrash(CrashPoint::kRecoveryAfterLogClean, "log is clean");
  }
  if (!was_writable) writable_ = false;
  XFS_INFO("recovery finished: transactions=" << report.replay.transactions_replayed
                                              << " items=" << report.replay.items_replayed
                                              << " buffers=" << report.replay.buffers_replayed
                                              << " inodes=" << report.replay.inodes_replayed
                                              << " skipped_newer=" << report.replay.buffers_skipped_newer
                                              << " intents=" << report.intents_completed
                                              << " unlinked=" << report.unlinked_inodes_processed
                                              << " cleared_buckets=" << report.agi_buckets_cleared);
  return report;
}

// xfs_extent_free_recover_work: validate the logged extents, free them in one
// transaction and log the matching EFD so a later crash does not redo the
// work (the done item releases the intent during recovery).
Status XfsFilesystem::CompleteIntent(const PendingIntent& intent) {
  if (intent.type == lf::kItemCui) {
    if (!geometry_.features.reflink) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "CUI item on a filesystem without reflink")
          .Metadata("log");
    }
    if (intent.regions.empty()) {
      return XFS_ERR(XfsError::kCorruptBtree, "CUI item has no format region")
          .Metadata("log");
    }
    auto parsed = ParseRefcountIntent(intent.regions[0], geometry_);
    if (!parsed.ok()) return parsed.error();
    const RefcountIntent cui = std::move(parsed.value());
    return RunTransaction([&](XfsTransaction& txn) -> Status {
      BlockAllocator blocks(&txn, device_.get(), &geometry_);
      for (const RefcountIntentExtent& e : cui.extents) {
        Status s = OkStatus();
        switch (e.type()) {
          case RefcountIntentType::kIncrease:
            s = blocks.RefcountIncrease(e.startblock, e.len);
            MaybeCrash(CrashPoint::kAfterRefcountUpdate,
                       "CUI increase id=" + std::to_string(cui.id));
            break;
          case RefcountIntentType::kDecrease:
            // A surviving decrease may free a range whose last reference is
            // gone.  The owning rmap was removed by the original deferred
            // bmap work, so use the EFI-style OWN_UNKNOWN wildcard.
            s = blocks.RefcountDecrease(e.startblock, e.len,
                                        /*unknown_owner=*/true);
            MaybeCrash(CrashPoint::kAfterRefcountUpdate,
                       "CUI decrease id=" + std::to_string(cui.id));
            break;
          case RefcountIntentType::kAllocCow:
            // The original transaction also logged the OWN_COW rmap update;
            // completion owns only the refcount half.
            s = blocks.RefcountCowAlloc(e.startblock, e.len);
            break;
          case RefcountIntentType::kFreeCow:
            s = blocks.RefcountCowFree(e.startblock, e.len);
            break;
          default:
            return XFS_ERR(XfsError::kUnsupportedFeature,
                           "unsupported CUI operation")
                .Actual(e.flags)
                .Metadata("CUI");
        }
        if (!s.ok()) return s;
      }
      // CUD must be in the same committed transaction as the refcountbt
      // buffers. Add it before the allocator budget check.
      MaybeCrash(CrashPoint::kBeforeIntentDone,
                 "CUI id=" + std::to_string(cui.id));
      std::vector<uint8_t> cud(16, 0);
      StoreLE16(cud.data(), lf::kItemCud);
      StoreLE16(cud.data() + 2, 1);
      StoreLE64(cud.data() + lf::kIntentIdOffset, cui.id);
      txn.AddLogOp(std::move(cud));
      Status committed = blocks.Commit();
      if (committed.ok()) {
        MaybeCrash(CrashPoint::kAfterIntentDone,
                   "CUI id=" + std::to_string(cui.id));
        XFS_INFO("CUI completion: id="
                 << cui.id << " extents=" << cui.extents.size()
                 << " log_bytes=" << txn.log_bytes()
                 << " serialized_log_bytes=" << txn.serialized_log_bytes()
                 << " fits_256k="
                 << (txn.serialized_log_bytes() <= lf::kMaxRecordBytes
                         ? "yes"
                         : "no"));
      }
      return committed;
    });
  }
  if (intent.type == lf::kItemRui) {
    if (intent.regions.empty()) {
      return XFS_ERR(XfsError::kCorruptBtree, "RUI item has no format region")
          .Metadata("log");
    }
    auto parsed = ParseRui(intent.regions[0], geometry_);
    if (!parsed.ok()) return parsed.error();
    const RmapIntent rui = std::move(parsed.value());
    return RunTransaction([&](XfsTransaction& txn) -> Status {
      BlockAllocator blocks(&txn, device_.get(), &geometry_);
      for (const RmapIntentExtent& e : rui.extents) {
        RmapOwner owner;
        owner.owner = e.owner;
        owner.offset = e.startoff;
        owner.flags = 0;
        if (e.flags & lf::kRmapExtentAttrFork) owner.flags |= kRmapAttrFork;
        if (e.flags & lf::kRmapExtentBmbtBlock) owner.flags |= kRmapBmbtBlock;
        if (e.flags & lf::kRmapExtentUnwritten) owner.flags |= kRmapUnwritten;
        const uint32_t op = e.flags & lf::kRmapExtentTypeMask;
        Status s = OkStatus();
        switch (op) {
          case lf::kRmapExtentMap:
          case lf::kRmapExtentAlloc:
            s = blocks.RmapMap(e.startblock, e.len, owner);
            break;
          case lf::kRmapExtentUnmap:
          case lf::kRmapExtentFree:
            s = blocks.RmapUnmap(e.startblock, e.len, owner);
            break;
          case lf::kRmapExtentConvert:
            // The intent records the new state; xfs_rmap_finish_one passes
            // !unwritten as the *old* state to xfs_rmap_convert.
            s = blocks.RmapConvert(e.startblock, e.len, owner,
                                   (e.flags & lf::kRmapExtentUnwritten) == 0);
            break;
          default:
            return XFS_ERR(XfsError::kUnsupportedFeature,
                           "unsupported RUI operation")
                .Actual(op)
                .Metadata("RUI");
        }
        if (!s.ok()) return s;
      }
      // RUD must be in the same committed transaction as the rmap buffers;
      // add it before the allocator's budget check so an oversized completion
      // is refused before the first metadata write.
      std::vector<uint8_t> rud(lf::kRudFormatSize, 0);
      StoreLE16(rud.data(), lf::kItemRud);
      StoreLE16(rud.data() + 2, 1);
      StoreLE64(rud.data() + lf::kIntentIdOffset, rui.id);
      txn.AddLogOp(std::move(rud));
      Status committed = blocks.Commit();
      if (committed.ok()) {
        XFS_INFO("RUI completion: id="
                 << rui.id << " extents=" << rui.extents.size()
                 << " log_bytes=" << txn.log_bytes()
                 << " serialized_log_bytes=" << txn.serialized_log_bytes()
                 << " fits_256k="
                 << (txn.serialized_log_bytes() <= lf::kMaxRecordBytes
                         ? "yes"
                         : "no"));
      }
      return committed;
    });
  }
  if (intent.type != lf::kItemEfi) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "cannot complete intent " + IntentContext(intent))
        .Metadata("log");
  }
  const std::vector<uint8_t>& format = intent.regions[0];
  if (format.size() < lf::kIntentLogFormatSize) {
    return XFS_ERR(XfsError::kCorruptBtree, "bad EFI log item")
        .Metadata("log");
  }
  const uint32_t nextents =
      LoadLE32At(format.data(), lf::kIntentNextentsOffset);
  if (nextents == 0 ||
      format.size() < lf::kIntentLogFormatSize +
                          static_cast<size_t>(nextents) * lf::kExtentSize) {
    return XFS_ERR(XfsError::kCorruptBtree, "bad EFI extent count")
        .Actual(nextents)
        .Metadata("log");
  }
  std::vector<LoggedExtent> extents;
  extents.reserve(nextents);
  for (uint32_t i = 0; i < nextents; ++i) {
    const uint8_t* entry =
        format.data() + lf::kIntentLogFormatSize + i * lf::kExtentSize;
    LoggedExtent extent;
    extent.start = LoadLE64At(entry, lf::kExtentStartOffset);
    extent.len = LoadLE32At(entry, lf::kExtentLenOffset);
    if (extent.len == 0 || !geometry_.FsbRangeValid(extent.start, extent.len)) {
      return XFS_ERR(XfsError::kCorruptExtent,
                     "EFI describes an invalid extent")
          .Fsb(extent.start)
          .Expected(extent.len)
          .Metadata("log");
    }
    extents.push_back(extent);
  }

  return RunTransaction([&](XfsTransaction& txn) -> Status {
    BlockAllocator blocks(&txn, device_.get(), &geometry_);
    for (const LoggedExtent& extent : extents) {
      // EFI has no owner: with rmapbt this is the OWN_UNKNOWN wildcard path
      // (fully covering record or no overlap; partial coverage is corruption).
      Status freed = geometry_.features.rmapbt
                         ? blocks.FreeUnknown(extent.start, extent.len)
                         : blocks.Free(extent.start, extent.len);
      if (!freed.ok()) return freed;
    }
    // The done item: the same extent array with the EFD type. Replaying the
    // log again finds the EFD and drops the intent instead of redoing it.  Add
    // it before the allocator's final log-budget check.
    std::vector<uint8_t> efd = format;
    StoreLE16(efd.data(), lf::kItemEfd);
    txn.AddLogOp(std::move(efd));
    return blocks.Commit();
  });
}

Result<uint64_t> XfsFilesystem::RecoverCowStaging() {
  if (!geometry_.features.reflink) return uint64_t{0};
  uint64_t freed = 0;
  for (uint32_t agno = 0; agno < geometry_.agcount; ++agno) {
    // Read-only scan first; an unreadable tree refuses cleanup instead of
    // leaving ambiguous staging ownership behind.
    auto tree = LoadRefcountTree(*device_, geometry_, agno, 0);
    if (!tree.ok()) return tree.error();
    for (const RefcountRecord& rec : tree.value().records) {
      if (!rec.cow()) continue;
      const uint64_t fsb = geometry_.AgFsb(agno, rec.key.startblock);
      Status s = RunTransaction([&](XfsTransaction& txn) -> Status {
        BlockAllocator blocks(&txn, device_.get(), &geometry_);
        Status released = blocks.ReleaseCowStaging(fsb, rec.blockcount);
        if (!released.ok()) return released;
        return blocks.Commit();
      });
      if (!s.ok()) return s.error();
      ++freed;
      XFS_INFO("recovered orphan CoW staging extent AG" << agno << " block "
               << rec.key.startblock << " len " << rec.blockcount);
    }
  }
  return freed;
}

Result<uint64_t> XfsFilesystem::ProcessAgiUnlinkedLists(
    uint64_t* buckets_cleared) {
  uint64_t processed = 0;
  uint64_t cleared = 0;
  constexpr uint32_t kMaxWalk = 4096;  // guard against corrupt cycles
  for (uint32_t agno = 0; agno < geometry_.agcount; ++agno) {
    auto agi = ReadAgi(*device_, geometry_, agno);
    if (!agi.ok()) {
      XFS_WARN("recovery: cannot read AGI " << agno << ": "
                                            << agi.error().message
                                            << "; skipping its unlinked lists");
      continue;
    }
    for (uint32_t bucket = 0; bucket < ondisk::kAgiUnlinkedBuckets; ++bucket) {
      for (uint32_t step = 0; step < kMaxWalk; ++step) {
        auto fresh = ReadAgi(*device_, geometry_, agno);
        if (!fresh.ok()) return fresh.error();
        const uint32_t head = fresh.value().unlinked[bucket];
        if (head == 0xffffffffu) break;  // NULLAGINO
        const uint64_t ino = geometry_.AginoToIno(agno, head);
        auto inode = OpenInode(ino);
        // Only an allocated, zero-link inode may be freed (the kernel asserts
        // i_nlink == 0 && i_mode != 0); anything else means the bucket is
        // corrupt and is cleared instead of walked.
        const bool unlinked_allocated =
            inode.ok() && inode.value().nlink == 0 && inode.value().mode != 0;
        if (!unlinked_allocated) {
          // xlog_recover_clear_agi_bucket: an unwalkable bucket is emptied so
          // recovery can finish; only a repair scan can free what remains.
          Status s = ClearAgiBucket(
              agno, bucket,
              inode.ok() ? (inode.value().nlink != 0 ? "inode still has links"
                                                     : "inode is not allocated")
                         : "inode cannot be read");
          if (!s.ok()) return s.error();
          ++cleared;
          break;
        }
        Status freed = FreeUnlinkedInode(ino);
        if (!freed.ok()) {
          XFS_WARN("recovery: freeing unlinked inode " << ino << " failed: "
                                                       << freed.error().Format());
          Status s = ClearAgiBucket(agno, bucket, "free failed");
          if (!s.ok()) return s.error();
          ++cleared;
          break;
        }
        ++processed;
        MaybeCrash(CrashPoint::kRecoveryMidIunlinks,
                   "ino=" + std::to_string(ino));
      }
    }
  }
  if (buckets_cleared != nullptr) *buckets_cleared = cleared;
  return processed;
}

Status XfsFilesystem::ClearAgiBucket(uint32_t agno, uint32_t bucket,
                                     const char* reason) {
  XFS_WARN("recovery: clearing AGI " << agno << " unlinked bucket " << bucket
                                     << " (" << (reason ? reason : "") << ")");
  return RunTransaction([&](XfsTransaction& txn) -> Status {
    BlockAllocator blocks(&txn, device_.get(), &geometry_);
    InodeAllocator inodes(&txn, &blocks, device_.get(), &superblock_,
                          &geometry_);
    Status s = inodes.SetUnlinkedBucketHead(agno, bucket, 0xffffffffu);
    if (!s.ok()) return s;
    return inodes.Commit();
  });
}

}  // namespace xfs
