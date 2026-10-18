#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent structural oracle for XFS images written by RW-3.

This tool re-implements, from the public on-disk format, the checks xfs_repair
performs for the structures RW-3 touches: the inode allocation btree (inobt)
and the AGI counters, and the directory formats (shortform, block, leaf1,
leafn and node, with their data/leaf/free blocks and the da btree). It does not
use the C++ parser, so a shared bug cannot hide: CI runs it on every
Windows-written image before a real Linux kernel mounts the same image.

usage:
  rw3_probe.py <image> [--expect SPEC.json] [--quiet]

  SPEC.json describes one directory (the root or an RW-3-created one):
    {"dir_ino": N,
     "entries": [{"name": "...", "ftype": "file"|"dir", "size": S}, ...],
     "absent":  ["name", ...]}
  Every listed entry must exist with that type and size (files), no other
  entries may exist, and the absent names must not be found. Every listed
  inode is cross-checked against the inobt's allocation bits.
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rw2_probe  # noqa: E402

INOBT_MAGIC = 0x49414233          # 'IAB3'
AGI_MAGIC = 0x58414749
DA3_NODE_MAGIC = 0x3EBE
DIR3_LEAF1_MAGIC = 0x3DF1
DIR3_LEAFN_MAGIC = 0x3DFF
DIR3_BLOCK_MAGIC = 0x58444233     # 'XDB3'
DIR3_DATA_MAGIC = 0x58444433      # 'XDD3'
DIR3_FREE_MAGIC = 0x58444633      # 'XDF3'
DATA_FREE_TAG = 0xFFFF
NULLDATAOFF = 0xFFFF
SPACE_SIZE = 1 << 35
INODES_PER_CHUNK = 64
DATA_HDR = 64                     # sizeof(struct xfs_dir3_data_hdr)
LEAF_HDR = 64                     # sizeof(struct xfs_dir3_leaf_hdr)
NODE_HDR = 64                     # sizeof(struct xfs_da3_node_hdr)
FREE_HDR = 64                     # sizeof(struct xfs_dir3_free_hdr)
BLOCK_TAIL = 8                    # count + stale of the single-block format
FTYPE_NAME = {
    0: "unknown",
    1: "file",
    2: "dir",
    3: "chrdev",
    4: "blkdev",
    5: "fifo",
    6: "sock",
    7: "symlink",
    8: "wht",
}
FMT_LOCAL = 1
FMT_EXTENTS = 2
FMT_BTREE = 3


def u16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def fail(message):
    raise SystemExit(message)


# ---------------------------------------------------------------------------
# Inode allocation btree and AGI
# ---------------------------------------------------------------------------

def read_inobt(img, geo, agno):
    """Walks AG `agno`'s inobt and returns (records, count, freecount).

    Each record is (startino, freecount, free) with the no-sparse layout:
    be32 startino, be32 freecount, be64 free (bit set = free)."""
    agi_off = geo.ag_offset(agno) + 2 * geo.sectsize
    agi = img.read(agi_off, geo.sectsize)
    if u32(agi, 0) != AGI_MAGIC:
        fail(f"AG{agno}: bad AGI magic")
    if rw2_probe.xfs_crc(agi, 312) != rw2_probe.le32(agi, 312):
        fail(f"AG{agno}: bad AGI CRC")
    if u32(agi, 8) != agno:
        fail(f"AG{agno}: AGI seqno mismatch")
    root = u32(agi, 20)
    level = u32(agi, 24)
    count = u32(agi, 16)
    freecount = u32(agi, 28)
    newino = u32(agi, 32)

    blocksize = geo.blocksize
    seen = set()
    records = []
    stack = [(root, None)]
    levels = 0
    while stack:
        agbno, expected_level = stack.pop()
        if not (0 <= agbno < geo.ag_blocks(agno)):
            fail(f"AG{agno}: inobt pointer {agbno} out of range")
        if agbno in seen:
            fail(f"AG{agno}: inobt cycle at {agbno}")
        seen.add(agbno)
        buf = img.read(geo.ag_offset(agno) + (agbno << geo.blocklog),
                       blocksize)
        if u32(buf, 0) != INOBT_MAGIC:
            fail(f"AG{agno}: bad inobt magic at {agbno}")
        if rw2_probe.xfs_crc(buf, 52) != rw2_probe.le32(buf, 52):
            fail(f"AG{agno}: bad inobt CRC at {agbno}")
        if u32(buf, 48) != agno:
            fail(f"AG{agno}: inobt owner mismatch at {agbno}")
        lvl = u16(buf, 4)
        numrecs = u16(buf, 6)
        levels = max(levels, lvl + 1)
        if expected_level is not None and lvl != expected_level:
            fail(f"AG{agno}: inobt level mismatch at {agbno}")
        if lvl == 0:
            maxrecs = (blocksize - 56) // 16
            if numrecs > maxrecs:
                fail(f"AG{agno}: inobt numrecs {numrecs} > {maxrecs}")
            for i in range(numrecs):
                off = 56 + i * 16
                records.append((u32(buf, off), u32(buf, off + 4),
                                u64(buf, off + 8)))
        else:
            if numrecs == 0:
                fail(f"AG{agno}: empty inobt node")
            maxrecs = (blocksize - 56) // 12
            if numrecs > maxrecs:
                fail(f"AG{agno}: inobt node numrecs {numrecs} > {maxrecs}")
            for i in range(numrecs):
                stack.append((u32(buf, 56 + maxrecs * 8 + i * 4), lvl - 1))
    if levels != level:
        fail(f"AG{agno}: AGI level {level} != inobt height {levels}")
    records.sort(key=lambda r: r[0])
    total_count = 0
    total_free = 0
    prev_end = None
    ino_per_block = 1 << geo.inopblog
    for startino, rec_freecount, free in records:
        if startino % ino_per_block != 0:
            fail(f"AG{agno}: chunk startino {startino} is not block-aligned")
        if prev_end is not None and startino < prev_end:
            fail(f"AG{agno}: inobt records overlap at {startino}")
        prev_end = startino + INODES_PER_CHUNK
        if startino + INODES_PER_CHUNK > (geo.ag_blocks(agno) <<
                                          geo.inopblog):
            fail(f"AG{agno}: chunk {startino} outside the AG")
        if bin(free).count("1") != rec_freecount:
            fail(f"AG{agno}: free mask popcount {bin(free).count('1')} != "
                 f"freecount {rec_freecount} for chunk {startino}")
        total_count += INODES_PER_CHUNK
        total_free += rec_freecount
    if total_count != count:
        fail(f"AG{agno}: AGI count {count} != inobt {total_count}")
    if total_free != freecount:
        fail(f"AG{agno}: AGI freecount {freecount} != inobt {total_free}")
    if newino not in (0, 0xFFFFFFFF) and newino % ino_per_block != 0:
        fail(f"AG{agno}: AGI newino {newino} is not block-aligned")

    # Chunk blocks are in use: none may be inside a free-space extent.
    agf = img.read(geo.ag_offset(agno) + geo.sectsize, geo.sectsize)
    bno_recs, _, _ = rw2_probe.read_short_btree(
        img, geo, agno, u32(agf, 16), rw2_probe.BNO_MAGIC, key_is_len=False)
    ialloc_blks = INODES_PER_CHUNK >> geo.inopblog
    for startino, _, _ in records:
        chunk_fsb = (agno << geo.agblklog) | (startino >> geo.inopblog)
        for b in range(ialloc_blks):
            agbno = (chunk_fsb + b) & ((1 << geo.agblklog) - 1)
            for start, length in bno_recs:
                if start <= agbno < start + length:
                    fail(f"AG{agno}: inode chunk block {agbno} is also a "
                         f"free-space extent")
    return records, count, freecount


def inode_free_bit(img, geo, ino):
    """True/False when the inobt knows the inode, None when it does not."""
    agno = ino >> (geo.agblklog + geo.inopblog)
    agino = ino & ((1 << (geo.agblklog + geo.inopblog)) - 1)
    records, _, _ = read_inobt(img, geo, agno)
    for startino, _, free in records:
        if startino <= agino < startino + INODES_PER_CHUNK:
            return bool((free >> (agino - startino)) & 1)
    return None


def check_inobt(img, geo):
    total_count = 0
    total_free = 0
    for agno in range(geo.agcount):
        _, count, freecount = read_inobt(img, geo, agno)
        total_count += count
        total_free += freecount
    if total_count != geo.icount:
        fail(f"sb_icount {geo.icount} != AGI total {total_count}")
    if total_free != geo.ifree:
        fail(f"sb_ifree {geo.ifree} != AGI total {total_free}")
    print(f"  inobt: {total_count} inode slots, {total_free} free across "
          f"{geo.agcount} AGs")


# ---------------------------------------------------------------------------
# Directory blocks
# ---------------------------------------------------------------------------

def check_blk_hdr(buf, geo, fsb, owner, what, magic_off):
    """Checks a dir3/da3 block header (magic, CRC, blkno, uuid, owner)."""
    crc_off = 4 if magic_off == 0 else 12
    blkno_off = 8 if magic_off == 0 else 16
    owner_off = 40 if magic_off == 0 else 48
    uuid_off = 24 if magic_off == 0 else 32
    if rw2_probe.xfs_crc(buf, crc_off) != rw2_probe.le32(buf, crc_off):
        fail(f"{what}: bad CRC at fsb {fsb}")
    if u64(buf, blkno_off) != geo.fsb_offset(fsb) // geo.sectsize:
        fail(f"{what}: blkno mismatch at fsb {fsb}")
    if u64(buf, owner_off) != owner:
        fail(f"{what}: owner mismatch at fsb {fsb}")
    if bytes(buf[uuid_off:uuid_off + 16]) != geo.meta_uuid:
        fail(f"{what}: uuid mismatch at fsb {fsb}")


def walk_data_chain(buf, end):
    """Walks a data block's entry chain.

    Returns (entries, bestfree) with entries = [(name, ino, ftype, offset)]
    and bestfree = the free runs [(length, offset)] in chain order."""
    entries = []
    runs = []
    cur = DATA_HDR
    while cur < end:
        if u16(buf, cur) == DATA_FREE_TAG:
            length = u16(buf, cur + 2)
            if length < 8 or length % 8 != 0 or cur + length > end:
                fail(f"data block: bad free run at {cur} length {length}")
            if u16(buf, cur + length - 2) != cur:
                fail(f"data block: free run exit tag at {cur} is wrong")
            runs.append((length, cur))
            cur += length
            continue
        namelen = buf[cur + 8]
        entsize = (8 + 1 + namelen + 1 + 2 + 7) & ~7
        if cur + entsize > end:
            fail(f"data block: entry at {cur} runs past the data")
        tag = u16(buf, cur + entsize - 2)
        if tag != cur:
            fail(f"data block: entry tag {tag} != offset {cur}")
        name = bytes(buf[cur + 9:cur + 9 + namelen])
        ftype = buf[cur + 9 + namelen]
        entries.append((name, u64(buf, cur), ftype, cur))
        cur += entsize
    if cur != end:
        fail(f"data block: entry chain ends at {cur}, expected {end}")
    return entries, runs


def check_bestfree(buf, runs, what):
    wanted = sorted(runs, key=lambda r: (-r[0], r[1]))[:3]
    for i in range(3):
        off, length = struct.unpack_from(">HH", buf, 48 + i * 4)
        want = wanted[i] if i < len(wanted) else (0, 0)
        if (length, off) != want:
            fail(f"{what}: bestfree[{i}] = {off}/{length}, expected "
                 f"{want[1]}/{want[0]}")


def parse_data_block(buf, geo, fsb, owner, what):
    """Checks a dir3 data (or single-block) block; returns its entries."""
    magic = u32(buf, 0)
    if magic == DIR3_BLOCK_MAGIC:
        check_blk_hdr(buf, geo, fsb, owner, what, 0)
        count = u32(buf, geo.blocksize - 8)
        end = geo.blocksize - BLOCK_TAIL - count * 8
    elif magic == DIR3_DATA_MAGIC:
        check_blk_hdr(buf, geo, fsb, owner, what, 0)
        end = geo.blocksize
    else:
        fail(f"{what}: not a data block (magic {magic:#x})")
    entries, runs = walk_data_chain(buf, end)
    check_bestfree(buf, runs, what)
    return entries


def parse_leaf(buf, geo, fsb, owner, what, magic_expected):
    """Parses a leaf1/leafn block; returns (entries, forw, back, bests)."""
    magic = u16(buf, 8)
    if magic != magic_expected:
        fail(f"{what}: leaf magic {magic:#x} != {magic_expected:#x}")
    check_blk_hdr(buf, geo, fsb, owner, what, 8)
    count = u16(buf, 56)
    stale = u16(buf, 58)
    max_ents = (geo.blocksize - LEAF_HDR) // 8
    if count > max_ents:
        fail(f"{what}: leaf count {count} > {max_ents}")
    entries = []
    for i in range(count):
        hashval = u32(buf, LEAF_HDR + i * 8)
        addr = u32(buf, LEAF_HDR + i * 8 + 4)
        if i and hashval < entries[i - 1][0]:
            fail(f"{what}: leaf hashes out of order at {i}")
        entries.append((hashval, addr))
    if sum(1 for _, addr in entries if addr == 0) != stale:
        fail(f"{what}: stale count {stale} does not match the null addresses")
    bests = None
    if magic_expected == DIR3_LEAF1_MAGIC:
        bestcount = u32(buf, geo.blocksize - 4)
        bests_off = geo.blocksize - 4 - bestcount * 2
        if bests_off < LEAF_HDR + count * 8:
            fail(f"{what}: leaf1 entries and bests overlap")
        bests = [u16(buf, bests_off + i * 2) for i in range(bestcount)]
    return entries, u32(buf, 0), u32(buf, 4), bests


def parse_node(buf, geo, fsb, owner, what):
    """Parses a da btree node; returns (level, [(hash, child_dablk)])."""
    if u16(buf, 8) != DA3_NODE_MAGIC:
        fail(f"{what}: not a da node (magic {u16(buf, 8):#x})")
    check_blk_hdr(buf, geo, fsb, owner, what, 8)
    level = u16(buf, 58)
    count = u16(buf, 56)
    max_ents = (geo.blocksize - NODE_HDR) // 8
    if count == 0 or count > max_ents:
        fail(f"{what}: node count {count} out of range")
    ents = [(u32(buf, NODE_HDR + i * 8), u32(buf, NODE_HDR + i * 8 + 4))
            for i in range(count)]
    for i in range(1, count):
        if ents[i][0] < ents[i - 1][0]:
            fail(f"{what}: node keys out of order at {i}")
    return level, ents


class Directory:
    """A parsed directory inode plus its file-block -> fsb mapping."""

    def __init__(self, img, geo, ino):
        self.img = img
        self.geo = geo
        self.ino = ino
        self.inode = rw2_probe.read_inode(img, geo, ino)
        if self.inode["mode"] & 0xF000 != 0x4000:
            fail(f"inode {ino}: not a directory")
        if self.inode["format"] not in (FMT_LOCAL, FMT_EXTENTS, FMT_BTREE):
            fail(f"inode {ino}: unsupported directory fork format "
                 f"{self.inode['format']}")
        self.extents = self.inode["extents"]
        self.size = self.inode["size"]

    def fsb_for_block(self, file_block):
        for startoff, startblock, blockcount, _ in self.extents:
            if startoff <= file_block < startoff + blockcount:
                return startblock + (file_block - startoff)
        return None

    def block(self, file_block, what):
        fsb = self.fsb_for_block(file_block)
        if fsb is None:
            fail(f"{what}: file block {file_block} is not mapped")
        return self.img.read(self.geo.fsb_offset(fsb), self.geo.blocksize), fsb

    def data_block(self, dablk):
        return self.block(dablk, f"dir {self.ino} data block {dablk}")

    def bestfree_of(self, dablk):
        buf, fsb = self.data_block(dablk)
        magic = u32(buf, 0)
        end = self.geo.blocksize
        if magic == DIR3_BLOCK_MAGIC:
            end = self.geo.blocksize - BLOCK_TAIL - u32(buf, self.geo.blocksize - 8) * 8
        _, runs = walk_data_chain(buf, end)
        return max((r[0] for r in runs), default=0)

    def leaf_block(self, file_block):
        # Da btree pointers (root, forw/back, node `before`) are file block
        # numbers: the first leaf/free block already includes the space offset
        # (xfs_dir2_da_to_db: db = (da - geo.leafblk) >> fsblog).
        return self.block(file_block, f"dir {self.ino} leaf {file_block}")

    def free_block(self, file_block):
        return self.block(file_block, f"dir {self.ino} free {file_block}")


def enumerate_directory(img, geo, ino):
    """Enumerates a directory from the raw blocks.

    Returns {name: (ino, ftype)} including "." and "..", validating the data
    blocks, the bestfree tables, the leaf chain, the da btree keys and the
    free-block bests while walking."""
    d = Directory(img, geo, ino)
    found = {}

    def add(name, ino2, ftype, what):
        if name in found:
            fail(f"{what}: duplicate name {name!r}")
        found[name] = (ino2, ftype)

    if d.inode["format"] == FMT_LOCAL:
        buf = d.inode["raw"]
        literal = buf[176:176 + d.inode["size"]]
        count = literal[0]
        i8count = literal[1]
        pos = 10 if i8count else 6
        for i in range(count):
            namelen = literal[pos]
            name = bytes(literal[pos + 3:pos + 3 + namelen])
            ftype = literal[pos + 3 + namelen]
            ino_off = pos + 4 + namelen
            ino2 = u64(literal, ino_off) if i8count else u32(literal, ino_off)
            add(name, ino2, ftype, f"dir {ino} shortform entry {i}")
            pos = ino_off + (8 if i8count else 4)
        if pos != d.inode["size"]:
            fail(f"dir {ino}: shortform entries end at {pos}, di_size is "
                 f"{d.inode['size']}")
        return found

    # A directory that outgrew the inode maps data block 0 first; the
    # single-block format keeps its embedded leaf in that same block, every
    # other format has its root in the leaf space.
    data0, data0_fsb = d.data_block(0)
    if u32(data0, 0) == DIR3_BLOCK_MAGIC:
        entries = parse_data_block(data0, d.geo, data0_fsb, d.ino,
                                   f"dir {d.ino} single-block")
        count = u32(data0, d.geo.blocksize - 8)
        stale = u32(data0, d.geo.blocksize - 4)
        leaf_start = d.geo.blocksize - BLOCK_TAIL - count * 8
        addrs = set()
        for i in range(count):
            addr = u32(data0, leaf_start + i * 8 + 4)
            if addr != 0:
                addrs.add(addr)
        if len(addrs) != len(entries) or stale != count - len(entries):
            fail(f"dir {d.ino}: single-block leaf entries do not match the "
                 f"data entries")
        for name, ino2, ftype, _ in entries:
            add(name, ino2, ftype, f"dir {d.ino} single-block entry")
        return found

    root_buf, root_fsb = d.leaf_block(SPACE_SIZE >> d.geo.blocklog)
    root_magic = u16(root_buf, 8)
    data_dablks = set()
    if root_magic == DIR3_LEAF1_MAGIC:
        entries, _, _, bests = parse_leaf(root_buf, d.geo, root_fsb, d.ino,
                                          f"dir {d.ino} leaf1",
                                          DIR3_LEAF1_MAGIC)
        bestcount = len(bests)
        if (bestcount << d.geo.blocklog) != d.size:
            fail(f"dir {d.ino}: leaf1 bestcount {bestcount} does not cover "
                 f"di_size {d.size}")
        for db in range(bestcount):
            data_dablks.add(db)
            want = d.bestfree_of(db)
            if bests[db] != want:
                fail(f"dir {d.ino}: leaf1 bests[{db}] = {bests[db]}, data "
                     f"block says {want}")
        collect_leaf_entries(d, entries, f"dir {d.ino} leaf1", add)
        return found
    if root_magic not in (DIR3_LEAFN_MAGIC, DA3_NODE_MAGIC):
        fail(f"dir {d.ino}: root block magic {root_magic:#x}/"
             f"{u32(root_buf, 0):#x} is not a directory block")

    # Leafn/node format: walk the leaf chain and then the free blocks.
    leafblk = SPACE_SIZE >> d.geo.blocklog
    first_leaf = leafblk
    if root_magic == DA3_NODE_MAGIC:
        level, ents = parse_node(root_buf, d.geo, root_fsb, d.ino,
                                 f"dir {d.ino} root node")
        child = ents[0][1]
        want_level = level - 1
        seen = set()
        while True:
            if child in seen:
                fail(f"dir {d.ino}: node cycle at {child}")
            seen.add(child)
            buf, fsb = d.leaf_block(child)
            if u16(buf, 8) != DA3_NODE_MAGIC:
                break
            if want_level <= 0:
                fail(f"dir {d.ino}: node {child} deeper than the root")
            lvl, sub = parse_node(buf, d.geo, fsb, d.ino,
                                  f"dir {d.ino} node {child}")
            if lvl != want_level:
                fail(f"dir {d.ino}: node {child} level {lvl} != {want_level}")
            child = sub[0][1]
            want_level -= 1
        first_leaf = child
    # Walk the chain from the leftmost leaf; the root node keys are checked
    # against the last hash of each child.
    leaf_dablk = first_leaf
    while True:
        buf, fsb = (root_buf, root_fsb) if leaf_dablk == leafblk and \
            root_magic == DIR3_LEAFN_MAGIC else d.leaf_block(leaf_dablk)
        entries, forw, back, _ = parse_leaf(
            buf, d.geo, fsb, d.ino, f"dir {d.ino} leaf {leaf_dablk}",
            u16(buf, 8))
        if root_magic == DA3_NODE_MAGIC:
            verify_leaf_key(d, root_buf, root_fsb, leaf_dablk,
                            entries[-1][0] if entries else None)
        collect_leaf_entries(d, entries, f"dir {d.ino} leaf {leaf_dablk}",
                             add, data_dablks)
        if forw == 0:
            break
        nbuf, nfsb = d.leaf_block(forw)
        nentries, _, nback, _ = parse_leaf(
            nbuf, d.geo, nfsb, d.ino, f"dir {d.ino} leaf {forw}",
            u16(nbuf, 8))
        if nback != leaf_dablk:
            fail(f"dir {d.ino}: leaf {forw} back {nback} != {leaf_dablk}")
        if entries and nentries and nentries[0][0] < entries[-1][0]:
            fail(f"dir {d.ino}: leaf chain hashes decrease at {forw}")
        leaf_dablk, buf, fsb = forw, nbuf, nfsb

    # The own size of every data block in the data space must be covered by
    # the free-block bests table.
    check_free_blocks(d, sorted(data_dablks))
    return found


def collect_leaf_entries(d, entries, what, add, data_dablks=None):
    """Resolves a leaf's (hash, address) entries to names."""
    for i, (_, addr) in enumerate(entries):
        if addr == 0:
            continue
        db = addr >> (d.geo.blocklog - 3)
        off = (addr & ((1 << (d.geo.blocklog - 3)) - 1)) << 3
        buf, fsb = d.data_block(db)
        block_entries = parse_data_block(buf, d.geo, fsb, d.ino,
                                         f"{what} data block {db}")
        if data_dablks is not None:
            data_dablks.add(db)
        for name, ino2, ftype, eoff in block_entries:
            if eoff == off:
                add(name, ino2, ftype, f"{what} addr {addr}")
                break
        else:
            fail(f"{what}: no data entry at offset {off}")


def verify_leaf_key(d, root_buf, root_fsb, file_block, last_hash):
    """The root node's entry for the leaf at `file_block` carries `last_hash`."""
    level, ents = parse_node(root_buf, d.geo, root_fsb, d.ino,
                             f"dir {d.ino} root node")
    for hashval, before in ents:
        if before == file_block:
            if last_hash is not None and hashval != last_hash:
                fail(f"dir {d.ino}: node key {hashval} for leaf {file_block} "
                     f"!= last hash {last_hash}")
            return
    # A leaf under an intermediate node is not a direct child of the root; the
    # chain ordering check in the caller covers it.
    return


def check_free_blocks(d, data_dablks):
    """Checks the free-space blocks of the node format against the data."""
    bestcount = d.size >> d.geo.blocklog
    max_bests = (d.geo.blocksize - FREE_HDR) // 2
    free_dablk = (2 * SPACE_SIZE) >> d.geo.blocklog
    covered = set()
    while True:
        fsb = d.fsb_for_block(free_dablk)
        if fsb is None:
            break
        buf = d.img.read(d.geo.fsb_offset(fsb), d.geo.blocksize)
        if u32(buf, 0) != DIR3_FREE_MAGIC:
            break
        check_blk_hdr(buf, d.geo, fsb, d.ino, DIR3_FREE_MAGIC, 0)
        firstdb = u32(buf, 48)
        nvalid = u32(buf, 52)
        nused = u32(buf, 56)
        if nvalid > max_bests or nused > nvalid:
            fail(f"dir {d.ino}: free block nvalid {nvalid}/nused {nused} "
                 f"out of range")
        if firstdb + nvalid > bestcount:
            fail(f"dir {d.ino}: free block covers beyond the data space")
        nulls = 0
        last_nonnull = -1
        for i in range(nvalid):
            best = u16(buf, FREE_HDR + i * 2)
            if best == NULLDATAOFF:
                nulls += 1
                continue
            last_nonnull = i
            want = d.bestfree_of(firstdb + i)
            if best != want:
                fail(f"dir {d.ino}: free bests[db {firstdb + i}] = {best}, "
                     f"data block says {want}")
            covered.add(firstdb + i)
        if nvalid != last_nonnull + 1:
            fail(f"dir {d.ino}: free block nvalid {nvalid} != last entry + 1 "
                 f"({last_nonnull + 1})")
        if nused != nvalid - nulls:
            fail(f"dir {d.ino}: free block nused {nused} != non-null entries "
                 f"{nvalid - nulls}")
        if bestcount == 0:
            break
        free_dablk += 1
        if free_dablk > (3 * SPACE_SIZE) >> d.geo.blocklog:
            fail(f"dir {d.ino}: free block chain does not terminate")
    if covered and covered != set(data_dablks):
        missing = sorted(set(data_dablks) - covered)
        extra = sorted(covered - set(data_dablks))
        fail(f"dir {d.ino}: free blocks cover {len(covered)} data blocks, "
             f"found {len(data_dablks)} (missing {missing[:5]}, "
             f"extra {extra[:5]})")


# ---------------------------------------------------------------------------
# Expected-directory checking
# ---------------------------------------------------------------------------

def check_expected(img, geo, spec):
    dir_ino = spec["dir_ino"]
    listing = enumerate_directory(img, geo, dir_ino)
    real = {name for name in listing if name not in (b".", b"..")}
    expected = {e["name"].encode() for e in spec.get("entries", [])}
    if real != expected:
        missing = sorted(expected - real)
        extra = sorted(real - expected)
        fail(f"dir {dir_ino}: listing mismatch; missing "
             f"{[m.decode('utf-8', 'replace') for m in missing]}, unexpected "
             f"{[e.decode('utf-8', 'replace') for e in extra]}")
    for e in spec.get("entries", []):
        name = e["name"].encode()
        ino2, ftype = listing[name]
        want_type = 2 if e["ftype"] == "dir" else 1
        if ftype != want_type:
            fail(f"dir {dir_ino}: {e['name']} has ftype {FTYPE_NAME.get(ftype)}"
                 f", expected {e['ftype']}")
        inode = rw2_probe.read_inode(img, geo, ino2)
        want_mode = 0x4000 if want_type == 2 else 0x8000
        if inode["mode"] & 0xF000 != want_mode:
            fail(f"dir {dir_ino}: {e['name']} inode {ino2} mode "
                 f"{inode['mode']:o} does not match ftype")
        if want_type == 1 and inode["size"] != e["size"]:
            fail(f"dir {dir_ino}: {e['name']} size {inode['size']} != "
                 f"{e['size']}")
        if inode["nlink"] == 0:
            fail(f"dir {dir_ino}: {e['name']} inode {ino2} has nlink 0")
        free = inode_free_bit(img, geo, ino2)
        if free is not False:
            fail(f"dir {dir_ino}: {e['name']} inode {ino2} is not marked "
                 f"allocated in the inobt")
    for name in spec.get("absent", []):
        if name.encode() in listing:
            fail(f"dir {dir_ino}: {name} must not exist")
    print(f"  dir {dir_ino}: {len(real)} entries match the expectation")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--expect", help="JSON directory expectation")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv[1:])

    img = rw2_probe.Image(args.image)
    geo = rw2_probe.check_image(img, verbose=not args.quiet)
    check_inobt(img, geo)
    if args.expect:
        with open(args.expect, "r", encoding="utf-8-sig") as f:
            spec = json.load(f)
        check_expected(img, geo, spec)
    print("OK: RW-3 structures (inobt/AGI, directory formats) are consistent")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except SystemExit as error:
        if isinstance(error.code, str):
            print(f"FAILED: {error.code}", file=sys.stderr)
            sys.exit(1)
        raise
