#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent XFS in-log buffer-cancellation probe (QUAL-2).

This is a small, read-only checker for `XFS_BLF_CANCEL` ("stale") buffer log
items.  It reuses the independent raw-log scanner in recovery_probe.py (which
never calls the project's C++ parser) and adds only the cancellation
analysis:

  * emits the complete stale item geometry: blf_type, blf_size, blf_flags,
    blf_len, blf_blkno, blf_map_size, the data-map words, the number and
    sizes of the following log regions, and the raw format bytes;
  * checks the encoding against the Linux kernel source rules
    (xfs_trans_binval / xfs_buf_item_format in fs/xfs/xfs_buf_item.c):
      - exactly one format region and no payload regions (blf_size == 1);
      - blf_size == 1 with regions == 1;
      - format region length == 20 + 4 * blf_map_size;
      - blf_map_size == ceil(ceil(blf_len * 512 / 128) / 32);
      - the data map is all zero;
      - the stale item carries XFS_BLF_CANCEL and neither
        XFS_BLF_INODE_BUF nor an XFS_BLFT_* buffer type.
  * simulates the kernel's two recovery passes over the whole log:
      - pass 1 (xlog_recover_buf_commit_pass1) adds a reference for every
        stale item keyed by the exact (blf_blkno, blf_len);
      - pass 2 replays transactions in log order with the kernel's
        xlog_recover_reorder_trans ordering (buffers, other items, inode
        buffers, then cancellations in reverse log order), releases one
        reference at each stale item, skips buffer items and inode items
        whose exact geometry is in the table, and skips an ICREATE that
        covers a cancelled cluster;
  * reports, per cancellation, which older items it suppresses and which
    later items replay after the last reference is consumed, so the
    "cancel -> reuse -> cancel -> reuse" contract is visible.

The probe is diagnostic.  It never writes to the image.

Usage:
  cancel_probe.py <image> [--json] [--require-cancel] [--min N]
  cancel_probe.py <image> --summary

Exit status: 0 when the log was parsed and (with --require-cancel) at least
--min cancellations were found with no encoding anomaly; 1 on a parse error or
anomaly; 2 when --require-cancel was given and no cancellation was found.
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recovery_probe as rp  # noqa: E402  (independent raw-log scanner)

XFS_BLF_INODE_BUF = 1 << 0
XFS_BLF_CANCEL = 1 << 1
XFS_BLF_QUOTA_MASK = (1 << 2) | (1 << 3) | (1 << 4)
XFS_BLFT_SHIFT = 11
XFS_BLFT_MASK = 0x1F
XFS_BLF_CHUNK = 128
NBWORD = 32
BUF_FORMAT_BASE = 20  # offsetof(struct xfs_buf_log_format, blf_data_map)

BLFT_NAMES = {
    0: "unknown",
    4: "btree",
    5: "agf",
    6: "agfl",
    7: "agi",
    8: "inode cluster (dino)",
    9: "symlink",
    10: "dir block",
    11: "dir data",
    12: "dir free",
    13: "dir leaf1",
    14: "dir leafn",
    15: "da node (dir/attr)",
    16: "attr leaf",
    17: "attr remote",
    18: "superblock",
}

# On-disk metadata magics used only to name a cancel target when the older
# buffer item is outside the recovery window.  v5 (CRC) forms first.
BYTE_MAGICS = [
    (b"XFSB", "superblock"),
    (b"XAGF", "agf"),
    (b"XAGI", "agi"),
    (b"XAFL", "agfl"),
    (b"AB3B", "bnobt"),
    (b"AB3C", "cntbt"),
    (b"IAB3", "inobt"),
    (b"FIB3", "finobt"),
    (b"BMA3", "bmbt"),
    (b"RMB3", "rmapbt"),
    (b"R3FC", "refcountbt"),
    (b"XDB3", "dir block"),
    (b"XDD3", "dir data"),
    (b"XDF3", "dir free"),
    (b"XARM", "remote attr value"),
]
SHORT_MAGICS = [
    (b"IN", "inode cluster"),
    (b"\x3d\xf1", "dir leaf1"),
    (b"\x3d\xff", "dir leafn"),
    (b"\x3b\xee", "attr leaf"),
    (b"\x3e\xbe", "da node (dir/attr)"),
]


class ProbeError(Exception):
    pass


def parse_buf(item):
    """Parses the buffer log format region of one XFS_LI_BUF item."""
    if not item.regions:
        raise ProbeError("buffer item without a format region")
    first = item.regions[0]
    if len(first) < BUF_FORMAT_BASE:
        raise ProbeError("buffer format region is %d bytes (need 20)" %
                         len(first))
    flags = rp.le16(first, 4)
    map_size = rp.le32(first, 16)
    words = []
    for i in range(map_size):
        off = BUF_FORMAT_BASE + i * 4
        if off + 4 <= len(first):
            words.append(rp.le32(first, off))
    return {
        "type": rp.le16(first, 0),
        "size": rp.le16(first, 2),
        "flags": flags,
        "len": rp.le16(first, 6),
        "blkno": struct.unpack_from("<q", first, 8)[0],
        "map_size": map_size,
        "map_words": words,
        "regions": len(item.regions),
        "region_sizes": [len(r) for r in item.regions],
        "format_bytes": len(first),
        "format_hex": first.hex(),
        "cancel": bool(flags & XFS_BLF_CANCEL),
        "inode_buf": bool(flags & XFS_BLF_INODE_BUF),
        "blft": (flags >> XFS_BLFT_SHIFT) & XFS_BLFT_MASK,
    }


def parse_inode_geometry(item):
    first = item.regions[0] if item.regions else b""
    if len(first) < 48:
        return None
    blkno = struct.unpack_from("<q", first, 40)[0]
    length = rp.le32(first, 48) if len(first) >= 52 else 0
    return {"blkno": blkno, "len": length,
            "ino": struct.unpack_from("<Q", first, 16)[0]}


class RawReconstruction(rp.Reconstruction):
    """Reuses the probe's op state machine but keeps raw item regions."""

    def __init__(self):
        super().__init__()
        self.raw_transactions = []

    def commit(self, trans):
        self.close_item(trans)
        if not trans.items:
            raise ProbeError("committed transaction with no items")
        buffers, items, inode_buffers, cancelled = [], [], [], []
        for item in trans.items:
            tid = rp.item_type(item)
            if tid == 0x123C:
                try:
                    fmt = parse_buf(item)
                except ProbeError:
                    items.append(item)
                    continue
                if fmt["cancel"]:
                    cancelled.append(item)
                elif fmt["inode_buf"] or fmt["blft"] == 8:
                    inode_buffers.append(item)
                else:
                    buffers.append(item)
                continue
            if tid == 0x123F:  # xlog_recover_icreate_reorder()
                buffers.append(item)
                continue
            items.append(item)
        # The kernel's list_move() puts every later cancellation at the head of
        # cancel_list, so stale items within one transaction are processed in
        # reverse log order.
        cancelled.reverse()
        self.raw_transactions.append({
            "tid": trans.tid,
            "lsn": trans.lsn,
            "items": buffers + items + inode_buffers + cancelled,
        })


def classify_on_disk(image, blkno, length):
    """Names the metadata still stored at the cancelled address, if any."""
    if blkno is None or blkno < 0:
        return None
    offset = blkno * 512
    size = length * 512
    try:
        with open(image, "rb") as handle:
            handle.seek(offset)
            data = handle.read(min(size, 64))
    except OSError:
        return None
    if len(data) < 4:
        return None
    for magic, name in BYTE_MAGICS:
        if data.startswith(magic):
            return {"kind": name, "source": "on-disk magic",
                    "magic": magic.hex()}
    for magic, name in SHORT_MAGICS:
        if data.startswith(magic):
            return {"kind": name, "source": "on-disk magic",
                    "magic": magic.hex()}
    return None


def expected_map_size(length_bb):
    chunks = (length_bb * 512 + XFS_BLF_CHUNK - 1) // XFS_BLF_CHUNK
    return (chunks + NBWORD - 1) // NBWORD


def geometry_checks(fmt, image_size):
    problems = []
    if not fmt["cancel"]:
        problems.append("stale item does not carry XFS_BLF_CANCEL")
    if fmt["size"] != 1:
        problems.append("blf_size is %d, Linux formats a stale item with 1" %
                        fmt["size"])
    if fmt["regions"] != 1:
        problems.append("stale item has %d regions, Linux writes one format "
                        "region and no payload" % fmt["regions"])
    if fmt["format_bytes"] != BUF_FORMAT_BASE + 4 * fmt["map_size"]:
        problems.append("format region is %d bytes, expected %d for "
                        "blf_map_size=%d" %
                        (fmt["format_bytes"],
                         BUF_FORMAT_BASE + 4 * fmt["map_size"],
                         fmt["map_size"]))
    want = expected_map_size(fmt["len"])
    if fmt["map_size"] != want:
        problems.append("blf_map_size is %d, geometry implies %d" %
                        (fmt["map_size"], want))
    if any(fmt["map_words"]):
        problems.append("stale item data map is not zero")
    if fmt["inode_buf"]:
        problems.append("stale item keeps XFS_BLF_INODE_BUF (Linux clears it)")
    if fmt["blft"] != 0:
        problems.append("stale item keeps XFS_BLFT_* type %d (Linux clears it)"
                        % fmt["blft"])
    extra = fmt["flags"] & ~(XFS_BLF_CANCEL | XFS_BLF_QUOTA_MASK)
    if extra:
        problems.append("unexpected flag bits 0x%x" % extra)
    if fmt["len"] == 0:
        problems.append("zero-length buffer geometry")
    if fmt["blkno"] < 0 or fmt["blkno"] * 512 + fmt["len"] * 512 > image_size:
        problems.append("cancelled range is outside the image")
    return problems


def analyse(image):
    with open(image, "rb") as stream:
        sb = stream.read(512)
        if len(sb) < 512:
            raise ProbeError("short superblock read")
        geo = rp.Geometry(sb)
        stream.seek(geo.log_byte)
        log_data = stream.read(geo.log_bb * 512)
    if len(log_data) != geo.log_bb * 512:
        raise ProbeError("short log read")
    img = rp.LogImage(log_data, geo)
    tail = rp.find_tail(img, geo)
    # The capture script probes a device-mapper snapshot node, where stat()
    # reports size 0 for the block device.  Fall back to sb_dblocks (the data
    # device size the superblock describes) so the geometry bound still means
    # the same thing on files and block devices.
    image_size = os.path.getsize(image)
    if image_size <= 0:
        image_size = rp.be64(sb, 8) * geo.block_size
    result = {
        "image": os.path.abspath(image),
        "image_size": image_size,
        "state": tail["state"],
        "head": {"block": tail["head"], "cycle": tail["cycle"]},
        "tail": {"block": tail["tail"],
                 "cycle": (tail["tail_lsn"] >> 32)
                 if tail["state"] != "clean" else tail["cycle"]},
        "newest_lsn": "0x%x" % tail["newest_lsn"],
        "records": 0,
        "transactions": 0,
        "cancels": [],
        "buffer_timeline": [],
        "icreate_skips": [],
        "anomalies": [],
    }
    if tail["state"] != "dirty" or tail["head"] == tail["tail"]:
        result["anomalies"].append("log is %s; no recovery window" %
                                   tail["state"])
        return result

    reconstruction = RawReconstruction()
    for rblk, header, _header_bytes, data in rp.iterate_records(
            img, tail["head"], tail["tail"], geo):
        result["records"] += 1
        reconstruction.process_record(header, data)
    transactions = reconstruction.raw_transactions
    result["transactions"] = len(transactions)

    # ---- pass 1: count every stale item over the whole log -----------------
    refs = {}
    pass1_events = {}
    for tindex, trans in enumerate(transactions):
        for pos, item in enumerate(trans["items"]):
            if rp.item_type(item) != 0x123C:
                continue
            try:
                fmt = parse_buf(item)
            except ProbeError as exc:
                result["anomalies"].append(
                    "tid=%d lsn=0x%x: %s" % (trans["tid"], trans["lsn"], exc))
                continue
            if not fmt["cancel"]:
                continue
            key = (fmt["blkno"], fmt["len"])
            refs[key] = refs.get(key, 0) + 1
            pass1_events.setdefault(key, []).append(
                {"phase": "pass1_add", "tid": trans["tid"],
                 "lsn": "0x%x" % trans["lsn"], "transaction": tindex,
                 "position": pos, "refcount": refs[key]})

    # ---- pass 2: replay order, cancellation consumption and suppression ---
    timeline = {}
    events = {}
    cancels = []
    for tindex, trans in enumerate(transactions):
        for pos, item in enumerate(trans["items"]):
            tid = rp.item_type(item)
            if tid == 0x123C:
                try:
                    fmt = parse_buf(item)
                except ProbeError:
                    continue
                key = (fmt["blkno"], fmt["len"])
                if fmt["cancel"]:
                    left = max(0, refs.get(key, 0) - 1)
                    if key in refs:
                        if left:
                            refs[key] = left
                        else:
                            del refs[key]
                    target = None
                    problems = geometry_checks(fmt, result["image_size"])
                    older = []
                    for event in events.get(key, []):
                        if event["phase"] == "suppressed":
                            older.append(event)
                    on_disk = classify_on_disk(result["image"],
                                               fmt["blkno"], fmt["len"])
                    older_blft = None
                    for event in events.get(key, []):
                        if event.get("blft"):
                            older_blft = event["blft"]
                    if on_disk:
                        target = on_disk
                    elif older_blft:
                        target = {"kind": BLFT_NAMES.get(older_blft,
                                                         "blft=%d" % older_blft),
                                  "source": "older buffer item blf_flags"}
                    entry = {
                        "transaction": tindex,
                        "position": pos,
                        "tid": trans["tid"],
                        "lsn": "0x%x" % trans["lsn"],
                        "blkno_bb": fmt["blkno"],
                        "blkno_byte": fmt["blkno"] * 512,
                        "len_bb": fmt["len"],
                        "len_byte": fmt["len"] * 512,
                        "blf_type": fmt["type"],
                        "blf_size": fmt["size"],
                        "blf_flags": fmt["flags"],
                        "blf_flags_hex": "0x%x" % fmt["flags"],
                        "blf_map_size": fmt["map_size"],
                        "map_words": fmt["map_words"],
                        "regions": fmt["regions"],
                        "region_sizes": fmt["region_sizes"],
                        "format_bytes": fmt["format_bytes"],
                        "format_hex": fmt["format_hex"],
                        "refcount_before": refs.get(key, 0) + 1,
                        "refcount_after": refs.get(key, 0),
                        "target": target,
                        "suppressed_before": older,
                        "checks": problems,
                    }
                    cancels.append(entry)
                    if problems:
                        result["anomalies"].extend(
                            "tid=%d lsn=0x%x blkno=0x%x len=%d: %s" %
                            (trans["tid"], trans["lsn"], fmt["blkno"],
                             fmt["len"], p) for p in problems)
                    event = {"phase": "cancel", "tid": trans["tid"],
                             "lsn": "0x%x" % trans["lsn"],
                             "transaction": tindex, "position": pos,
                             "refcount_after": refs.get(key, 0)}
                    events.setdefault(key, []).append(event)
                    timeline.setdefault(key, []).append(event)
                    continue
                suppressed = key in refs
                event = {"phase": "suppressed" if suppressed else "replayed",
                         "tid": trans["tid"], "lsn": "0x%x" % trans["lsn"],
                         "transaction": tindex, "position": pos,
                         "type": "XFS_LI_BUF", "blft": fmt["blft"],
                         "regions": fmt["regions"]}
                events.setdefault(key, []).append(event)
                timeline.setdefault(key, []).append(event)
                continue
            if tid == 0x123B:
                geom = parse_inode_geometry(item)
                if geom is None:
                    continue
                key = (geom["blkno"], geom["len"])
                if key in refs:
                    event = {"phase": "suppressed", "tid": trans["tid"],
                             "lsn": "0x%x" % trans["lsn"],
                             "transaction": tindex, "position": pos,
                             "type": "XFS_LI_INODE", "ino": geom["ino"]}
                    events.setdefault(key, []).append(event)
                    timeline.setdefault(key, []).append(event)
                continue
            if tid == 0x123F and item.regions and len(item.regions[0]) >= 28:
                first = item.regions[0]
                agno = rp.be32(first, 4)
                agbno = rp.be32(first, 8)
                length = rp.be32(first, 20)
                # xfs_ialloc_setup_geometry(): inode_cluster_size is
                # max(8192, blocksize) and XFS_FSB_TO_BB scales by the block
                # size.  The ICREATE length is in filesystem blocks.
                cluster_bytes = max(8192, geo.block_size)
                blocks_per_cluster = cluster_bytes // geo.block_size
                bb_per_cluster = cluster_bytes // 512
                fsb_per_bb = geo.block_size // 512
                cancels_found = 0
                total = max(1, length // blocks_per_cluster)
                for i in range(total):
                    fsb = agno * geo.ag_blocks + agbno + i * blocks_per_cluster
                    daddr = fsb * fsb_per_bb
                    if (daddr, bb_per_cluster) in refs:
                        cancels_found += 1
                if cancels_found:
                    entry = {"tid": trans["tid"], "lsn": "0x%x" % trans["lsn"],
                             "transaction": tindex,
                             "clusters": total, "cancelled": cancels_found,
                             "skipped": cancels_found == total}
                    result["icreate_skips"].append(entry)
                    if cancels_found != total:
                        result["anomalies"].append(
                            "tid=%d: partial ICREATE cancellation (%d/%d); "
                            "Linux skips the whole ICREATE" %
                            (trans["tid"], cancels_found, total))
                continue

    result["cancels"] = cancels
    for key in sorted(timeline, key=lambda k: (k[0], k[1])):
        result["buffer_timeline"].append({
            "blkno_bb": key[0], "len_bb": key[1],
            "events": timeline[key]})
    for key, count in refs.items():
        result["anomalies"].append(
            "cancel reference not consumed for blkno=0x%x len=%d (left %d)" %
            (key[0], key[1], count))
    return result


def print_summary(result):
    print("state: %s" % result["state"])
    print("records: %d  transactions: %d  cancels: %d  anomalies: %d" %
          (result["records"], result["transactions"],
           len(result["cancels"]), len(result["anomalies"])))
    for cancel in result["cancels"]:
        target = cancel["target"]["kind"] if cancel["target"] else "unknown"
        print("  tid=%d lsn=%s blkno=0x%x (%d) len=%d BB (%d bytes) "
              "map_size=%d flags=%s target=%s refs=%d->%d" %
              (cancel["tid"], cancel["lsn"], cancel["blkno_bb"],
               cancel["blkno_byte"], cancel["len_bb"], cancel["len_byte"],
               cancel["blf_map_size"], cancel["blf_flags_hex"], target,
               cancel["refcount_before"], cancel["refcount_after"]))
        for problem in cancel["checks"]:
            print("    ENCODING: %s" % problem)
    for anomaly in result["anomalies"]:
        print("  ANOMALY: %s" % anomaly)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--require-cancel", action="store_true",
                        help="fail with status 2 when the window has none")
    parser.add_argument("--min", type=int, default=1,
                        help="minimum cancellations (with --require-cancel)")
    args = parser.parse_args(argv[1:])

    result = analyse(args.image)
    if args.json:
        json.dump(result, sys.stdout, indent=2)
        print()
    else:
        print_summary(result)
    if result["anomalies"]:
        return 1
    if args.require_cancel and len(result["cancels"]) < args.min:
        print("cancel_probe: found %d cancellation(s), need %d" %
              (len(result["cancels"]), args.min), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except (ProbeError, rp.LogError) as exc:
        print("cancel_probe: %s" % exc, file=sys.stderr)
        sys.exit(1)
