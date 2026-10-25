#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent RW-6C refcountbt / reflink oracle.

This program does not import or invoke any C++ code.  It combines two
independent views of the filesystem:

  * the forward metadata (AG headers, inobt, every allocated inode's data
    fork and bmap btree) gives the number of written file-data mappings for
    every physical block;
  * the refcountbt gives the declared sharing for those same blocks.

A written block mapped by exactly one inode must have no refcount record; a
block mapped by N >= 2 inodes must have a shared record with refcount N.  CoW
staging records must be refcount one, must not be mapped by any inode, and (on
rmapbt filesystems) must have an OWN_COW reverse mapping.  The same forward
metadata is also fed to the independent RW-6B rmap oracle, extended with the
refcountbt block ownership and the legal shared-data overlaps.

usage: rw6c_refcount_probe.py <image> [--json OUT] [--quiet] [--allow-cow]
       [--allow-no-rmapbt]

Exit 0 means every checked invariant held.  This is an acceptance oracle, not
a replacement for xfs_repair or a Linux mount.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rw6b_rmap_probe as r  # noqa: E402

REFC_MAGIC = 0x52334643  # 'R3FC'
COW_FLAG = 1 << 31
AGBLOCK_MASK = COW_FLAG - 1
HEADER = 56


def load_refcount(img, geo, agno, agf):
    """Read and verify one AG's refcountbt; returns (records, blocks)."""
    root = agf["refcount_root"]
    level = agf["refcount_level"]
    ag_blocks = geo.ag_blocks(agno)
    if not geo.reflink:
        r.fail("AG%d: refcountbt requested on a non-reflink filesystem" % agno)
    if root >= ag_blocks or level < 1 or level > 16:
        r.fail("AG%d: invalid refcount root/level %d/%d" %
               (agno, root, level))
    if ag_blocks > COW_FLAG:
        r.fail("AG%d: AG is too large for the refcount CoW flag" % agno)
    max_leaf = (geo.blocksize - HEADER) // 12
    max_node = (geo.blocksize - HEADER) // 8
    if max_leaf == 0 or max_node == 0:
        r.fail("AG%d: no room for refcount records" % agno)

    records = []
    blocks = []
    seen = set()
    last_level = {}
    actual_intervals = []

    def visit(agbno, expected):
        if agbno == 0xFFFFFFFF or agbno >= ag_blocks or agbno in seen:
            r.fail("AG%d: refcount pointer/cycle %d" % (agno, agbno))
        seen.add(agbno)
        off = geo.ag_offset(agno) + agbno * geo.blocksize
        buf = img.read(off, geo.blocksize)
        if r.u32(buf, 0) != REFC_MAGIC:
            r.fail("AG%d: refcount bad magic at %d" % (agno, agbno))
        r.checked_crc(buf, 52, "AG%d refcount block %d" % (agno, agbno))
        if r.u32(buf, 48) != agno or buf[32:48] != geo.meta_uuid:
            r.fail("AG%d: refcount owner/uuid at %d" % (agno, agbno))
        want_blkno = (geo.ag_offset(agno) >> 9) + \
            (agbno << (geo.blocklog - 9))
        if r.u64(buf, 16) != want_blkno:
            r.fail("AG%d: refcount blkno at %d" % (agno, agbno))
        lvl, num = r.u16(buf, 4), r.u16(buf, 6)
        if lvl != expected:
            r.fail("AG%d: refcount level at %d" % (agno, agbno))
        maxr = max_leaf if lvl == 0 else max_node
        empty_root = lvl == 0 and agbno == root and num == 0
        if (num == 0 and not empty_root) or num > maxr:
            r.fail("AG%d: refcount numrecs at %d" % (agno, agbno))
        if not empty_root and agbno != root and num < maxr // 2:
            r.fail("AG%d: underfull refcount block at %d" % (agno, agbno))
        left, right = r.u32(buf, 8), r.u32(buf, 12)
        if lvl in last_level:
            prev, prev_right = last_level[lvl]
            if left != prev or prev_right != agbno:
                r.fail("AG%d: refcount sibling links at %d" %
                       (agno, agbno))
        elif left != 0xFFFFFFFF:
            r.fail("AG%d: first refcount block has left sibling" % agno)
        last_level[lvl] = (agbno, right)
        blocks.append(agbno)

        if lvl == 0:
            low_out = 0
            prev_key = None
            for i in range(num):
                raw = buf[HEADER + i * 12:HEADER + (i + 1) * 12]
                encoded = r.u32(raw, 0)
                count = r.u32(raw, 4)
                refcount = r.u32(raw, 8)
                domain = "cow" if encoded & COW_FLAG else "shared"
                start = encoded & AGBLOCK_MASK
                if prev_key is not None and encoded <= prev_key:
                    r.fail("AG%d: refcount keys not strictly ordered" % agno)
                prev_key = encoded
                if start >= ag_blocks or count == 0 or \
                        count > ag_blocks - start:
                    r.fail("AG%d: refcount extent out of range" % agno)
                if domain == "cow":
                    if refcount != 1:
                        r.fail("AG%d: CoW record refcount != 1" % agno)
                elif refcount < 2:
                    r.fail("AG%d: shared record refcount < 2" % agno)
                records.append({"domain": domain, "start": start,
                                "count": count, "refcount": refcount})
                actual_intervals.append((start, count, domain))
                if i == 0:
                    low_out = encoded
            return low_out

        ptrbase = HEADER + max_node * 4
        low_out = None
        prev_low = None
        for i in range(num):
            low = r.u32(buf, HEADER + i * 4)
            if prev_low is not None and low <= prev_low:
                r.fail("AG%d: refcount node keys out of order" % agno)
            prev_low = low
            child_low = visit(r.u32(buf, ptrbase + i * 4), lvl - 1)
            if child_low != low:
                r.fail("AG%d: refcount parent key != child key at %d" %
                       (agno, agbno))
            if low_out is None:
                low_out = low
        return low_out

    visit(root, level - 1)
    for lvl, (_, right) in last_level.items():
        if right != 0xFFFFFFFF:
            r.fail("AG%d: rightmost refcount block has right sibling" % agno)
    if len(blocks) != agf["refcount_blocks"]:
        r.fail("AG%d: AGF refcount_blocks %d != reachable %d" %
               (agno, agf["refcount_blocks"], len(blocks)))
    if agf["refcount_blocks"] == 0:
        r.fail("AG%d: AGF refcount_blocks is zero" % agno)

    for i in range(1, len(actual_intervals)):
        p = actual_intervals[i - 1]
        c = actual_intervals[i]
        if p[0] + p[1] > c[0] and c[0] + c[1] > p[0]:
            r.fail("AG%d: refcount records overlap physically" % agno)
    return records, blocks


def expected_data_counts(img, geo, agno):
    """Count written data-fork mappings per AG block from forward metadata."""
    owners = []
    data_blocks = []
    inode_count = r.scan_inode_owners(img, geo, agno, owners, data_blocks)
    counts = [0] * geo.ag_blocks(agno)
    for start, length, owner, offset, flags in owners:
        if flags != 0 or owner & (1 << 63):
            continue
        for block in range(start, start + length):
            if block >= len(counts):
                r.fail("AG%d: data extent outside AG" % agno)
            counts[block] += 1
    return counts, inode_count, data_blocks


def verify_ag(img, geo, agno, allow_cow):
    agf = r.read_agf(img, geo, agno)
    records, refcount_blocks = load_refcount(img, geo, agno, agf)
    counts, inode_count, bmbt_blocks = expected_data_counts(img, geo, agno)

    shared = [rec for rec in records if rec["domain"] == "shared"]
    cows = [rec for rec in records if rec["domain"] == "cow"]
    if cows and not allow_cow:
        r.fail("AG%d: %d active CoW staging records (pass --allow-cow)" %
               (agno, len(cows)))

    covered = set()
    for rec in shared:
        for block in range(rec["start"], rec["start"] + rec["count"]):
            if block >= len(counts) or counts[block] != rec["refcount"]:
                r.fail(
                    "AG%d: shared record [%d,+%d) refcount %d, forward "
                    "mapping count %s" %
                    (agno, rec["start"], rec["count"], rec["refcount"],
                     counts[block] if block < len(counts) else "outside"))
            covered.add(block)
    expected = {i for i, count in enumerate(counts) if count >= 2}
    if covered != expected:
        missing = sorted(expected - covered)[:16]
        extra = sorted(covered - expected)[:16]
        r.fail("AG%d: shared coverage mismatch missing=%s extra=%s" %
               (agno, missing, extra))

    cow_blocks = []
    for rec in cows:
        for block in range(rec["start"], rec["start"] + rec["count"]):
            if counts[block] != 0:
                r.fail("AG%d: CoW staging block %d is mapped by an inode" %
                       (agno, block))
            cow_blocks.append(block)

    rmap_errors = {}
    if geo.rmapbt:
        result, rmap_errors = r.check_ag(
            img, geo, agno, refcount_blocks=refcount_blocks,
            cow_blocks=cow_blocks)
        if any(rmap_errors.values()):
            r.fail("AG%d: rmap oracle rejected the image: %s" %
                   (agno, json.dumps(rmap_errors))[:4096])
    elif not allow_cow and cows:
        r.fail("AG%d: CoW records on a filesystem without rmapbt" % agno)

    return {
        "ag": agno,
        "root": agf["refcount_root"],
        "level": agf["refcount_level"],
        "blocks": len(refcount_blocks),
        "records": len(records),
        "shared_records": len(shared),
        "cow_records": len(cows),
        "shared_blocks": len(expected),
        "allocated_inodes": inode_count,
        "bmbt_blocks": len(bmbt_blocks),
        "rmap_checked": geo.rmapbt,
        "rmap_errors": rmap_errors,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--json")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--allow-cow", action="store_true")
    ap.add_argument("--allow-no-rmapbt", action="store_true")
    args = ap.parse_args()

    img = r.Image(args.image)
    geo = r.Geometry(img)
    if not geo.reflink:
        r.fail("filesystem does not have reflink/refcountbt")
    if not geo.rmapbt and not args.allow_no_rmapbt:
        r.fail("filesystem does not have rmapbt (pass --allow-no-rmapbt)")

    results = []
    try:
        for agno in range(geo.agcount):
            result = verify_ag(img, geo, agno, args.allow_cow)
            results.append(result)
            if not args.quiet:
                print(
                    "AG %d: refcount level %d, blocks %d, shared %d "
                    "(%d blocks), CoW %d, rmap %s" %
                    (agno, result["level"], result["blocks"],
                     result["shared_records"], result["shared_blocks"],
                     result["cow_records"],
                     "checked" if result["rmap_checked"] else "off"))
        total_fdblocks = sum(
            r.read_agf(img, geo, agno)["freeblks"] +
            r.read_agf(img, geo, agno)["flcount"] +
            r.read_agf(img, geo, agno)["btreeblks"]
            for agno in range(geo.agcount))
        sb_fdblocks = r.u64(geo.sb, 144)
        if total_fdblocks != sb_fdblocks:
            r.fail("superblock fdblocks %d != AGF contribution %d" %
                   (sb_fdblocks, total_fdblocks))
        summary = {
            "image": args.image,
            "reflink": True,
            "rmapbt": geo.rmapbt,
            "agcount": geo.agcount,
            "shared_records": sum(x["shared_records"] for x in results),
            "shared_blocks": sum(x["shared_blocks"] for x in results),
            "cow_records": sum(x["cow_records"] for x in results),
            "fdblocks": sb_fdblocks,
            "ags": results,
            "result": "ok",
        }
    except AssertionError as exc:
        summary = {"image": args.image, "result": "failed",
                   "error": str(exc), "ags": results}
        if args.json:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(summary, f, indent=2)
        print("rw6c oracle: FAILED: %s" % exc, file=sys.stderr)
        return 1

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
    if not args.quiet:
        print("rw6c oracle: ok (%d shared records, %d shared blocks, "
              "%d CoW)" % (summary["shared_records"],
                           summary["shared_blocks"],
                           summary["cow_records"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
