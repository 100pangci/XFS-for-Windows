#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent RW-6D extended-attribute oracle.

This is a second implementation (Python only; it never calls the C++ parser)
of the XFS attribute fork:

  * inode forkoff / aformat / anextents (including NREXT64);
  * attr-fork extent maps and BMBT trees;
  * shortform headers/entries;
  * v5 attribute leaf blocks (header, CRC, UUID, owner, blkno, entry table,
    free map, hash order);
  * DA node trees (levels, keys, child pointers, sibling chains);
  * remote value blocks (XARM header, CRC, owner, offset, bytes, NULLCOMMITLSN);
  * namespace flags and the canonical display name;
  * value length + SHA-256.

usage:
  rw6d_xattr_probe.py <image> --inode N [--inode N ...] [--json OUT]
                              [--manifest FILE] [--quiet]

The optional manifest is a JSON object:
  {"inodes": [{"ino": 123,
               "attributes": [{"name": "user.foo",
                               "length": 5,
                               "sha256": "..."}]}]}
Exit code 0 means the structural walk and every manifest comparison passed.
"""

import argparse
import hashlib
import json
import struct
import sys

SB_MAGIC = 0x58465342
DINODE_MAGIC = 0x494E
BMBT_MAGIC = 0x424D4133
ATTR_LEAF_MAGIC = 0x3BEE
DA_NODE_MAGIC = 0x3EBE
ATTR_RMT_MAGIC = 0x5841524D  # XARM

IN_BIGTIME = 1 << 3
IN_NREXT64 = 1 << 5
DIFLAG2_NREXT64 = 1 << 4
NULLCOMMITLSN = (1 << 64) - 1

ATTR_LOCAL = 1 << 0
ATTR_ROOT = 1 << 1
ATTR_SECURE = 1 << 2
ATTR_PARENT = 1 << 3
ATTR_INCOMPLETE = 1 << 7
ATTR_NSP_MASK = ATTR_ROOT | ATTR_SECURE | ATTR_PARENT
ATTR_ON_DISK_MASK = ATTR_NSP_MASK | ATTR_LOCAL | ATTR_INCOMPLETE
ATTR_LEAF_MAPSIZE = 3
ATTR_LEAF_HDR_SIZE = 80
ATTR_LEAF_ENTRY_SIZE = 8
ATTR_NODE_HDR_SIZE = 64
ATTR_NODE_ENTRY_SIZE = 8
ATTR_RMT_HDR_SIZE = 56
ATTR_MAX_VALUE = 1 << 16


def fail(message):
    raise AssertionError(message)


def make_crc_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


CRC_TABLE = make_crc_table()


def crc32c(data):
    c = 0xFFFFFFFF
    for byte in data:
        c = CRC_TABLE[(c ^ byte) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def xfs_crc(buf, offset):
    copy = bytearray(buf)
    copy[offset:offset + 4] = b"\x00\x00\x00\x00"
    return crc32c(bytes(copy))


def u16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def rol32(value, shift):
    shift &= 31
    if not shift:
        return value
    return ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF


def attr_hash(name):
    data = name.encode("utf-8") if isinstance(name, str) else name
    h = 0
    i = 0
    n = len(data)
    while n - i >= 4:
        h = ((data[i] << 21) ^ (data[i + 1] << 14) ^
             (data[i + 2] << 7) ^ data[i + 3] ^ rol32(h, 28)) & 0xFFFFFFFF
        i += 4
    rest = n - i
    if rest == 3:
        return ((data[i] << 14) ^ (data[i + 1] << 7) ^ data[i + 2] ^
                rol32(h, 21)) & 0xFFFFFFFF
    if rest == 2:
        return ((data[i] << 7) ^ data[i + 1] ^ rol32(h, 14)) & 0xFFFFFFFF
    if rest == 1:
        return (data[i] ^ rol32(h, 7)) & 0xFFFFFFFF
    return h


def display_name(flags, name):
    nsp = flags & ATTR_NSP_MASK
    if nsp == ATTR_ROOT:
        if name == "SGI_ACL_FILE":
            return "system.posix_acl_access"
        if name == "SGI_ACL_DEFAULT":
            return "system.posix_acl_default"
        return "trusted." + name
    if nsp == ATTR_SECURE:
        return "security." + name
    if nsp == ATTR_PARENT:
        return "parent." + name
    return "user." + name


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()

    def read(self, offset, size):
        if offset < 0 or offset + size > len(self.data):
            fail("read out of bounds: %d+%d" % (offset, size))
        return self.data[offset:offset + size]


class Geometry:
    def __init__(self, img):
        sb = img.read(0, 512)
        if u32(sb, 0) != SB_MAGIC:
            fail("bad superblock magic")
        self.blocksize = u32(sb, 4)
        self.dblocks = u64(sb, 8)
        self.uuid = sb[32:48]
        self.agblocks = u32(sb, 84)
        self.agcount = u32(sb, 88)
        self.sectsize = u16(sb, 102)
        self.inodesize = u16(sb, 104)
        self.inopblock = u16(sb, 106)
        self.blocklog = sb[120]
        self.inodelog = sb[122]
        self.inopblog = sb[123]
        self.agblklog = sb[124]
        self.features_incompat = u32(sb, 216)

    def ag_blocks(self, agno):
        if agno + 1 < self.agcount:
            return self.agblocks
        return self.dblocks - self.agblocks * (self.agcount - 1)

    def fsb_offset(self, fsb):
        return fsb << self.blocklog

    def inode_offset(self, ino):
        agno = ino >> (self.agblklog + self.inopblog)
        if agno >= self.agcount:
            fail("inode %d: AG out of range" % ino)
        agbno = (ino >> self.inopblog) & ((1 << self.agblklog) - 1)
        index = ino & (self.inopblock - 1)
        return (agno * self.agblocks * self.blocksize +
                agbno * self.blocksize + index * self.inodesize)


def checked_crc(buf, off, what):
    if le32(buf, off) != xfs_crc(buf, off):
        fail("%s: bad CRC" % what)


def checked_identity(buf, what, fsb, geo, owner=None):
    if buf[32:48] != geo.uuid:
        fail("%s: bad uuid" % what)
    if u64(buf, 16) != (geo.fsb_offset(fsb) >> 9):
        fail("%s: bad blkno" % what)
    if owner is not None and u64(buf, 48) != owner:
        fail("%s: bad owner" % what)


def decode_bmap(rec):
    l0, l1 = struct.unpack(">QQ", rec)
    return (((l0 >> 9) & ((1 << 54) - 1)),
            (((l0 & 0x1FF) << 43) | (l1 >> 21)),
            (l1 & 0x1FFFFF), bool(l0 >> 63))


def read_bmbt_extents(img, geo, ino, fmt, root, fork_bytes, nextents):
    """Returns [(startoff, startblock, blockcount, unwritten)] for a fork."""
    recs = []
    if fmt == 2:
        if nextents * 16 > len(fork_bytes):
            fail("inode %d: %s extent list overflows fork" % (ino, root))
        for i in range(nextents):
            recs.append(decode_bmap(fork_bytes[i * 16:(i + 1) * 16]))
        return recs
    if fmt != 3:
        fail("inode %d: %s fork format %d is not an extent map" % (ino, root, fmt))
    raw = fork_bytes
    if len(raw) < 4:
        fail("inode %d: %s bmdr too small" % (ino, root))
    level = u16(raw, 0)
    num = u16(raw, 2)
    maxr = (len(raw) - 4) // 16
    if level < 1 or num == 0 or num > maxr:
        fail("inode %d: %s bmdr level/count" % (ino, root))
    ptrbase = 4 + maxr * 8
    children = [(u64(raw, ptrbase + i * 8), level - 1) for i in range(num)]

    def visit(fsb, expected):
        if fsb == 0 or fsb >= geo.dblocks:
            fail("inode %d: %s bmbt pointer out of range" % (ino, root))
        block = img.read(geo.fsb_offset(fsb), geo.blocksize)
        if u32(block, 0) != BMBT_MAGIC or u64(block, 56) != ino:
            fail("inode %d: %s bmbt identity" % (ino, root))
        checked_crc(block, 64, "inode %d %s bmbt" % (ino, root))
        if block[40:56] != geo.uuid or \
                u64(block, 24) != (geo.fsb_offset(fsb) >> 9):
            fail("inode %d: %s bmbt header" % (ino, root))
        lvl = u16(block, 4)
        n = u16(block, 6)
        maxrec = (geo.blocksize - 72) // 16
        if lvl != expected or n == 0 or n > maxrec:
            fail("inode %d: %s bmbt level/count" % (ino, root))
        if lvl == 0:
            for i in range(n):
                recs.append(decode_bmap(block[72 + i * 16:88 + i * 16]))
        else:
            pbase = 72 + maxrec * 8
            for i in range(n):
                visit(u64(block, pbase + i * 8), lvl - 1)

    for child, lvl in children:
        visit(child, lvl)
    return recs


class AttrFork:
    def __init__(self, img, geo, ino, raw):
        self.img = img
        self.geo = geo
        self.ino = ino
        self.raw = raw
        self.forkoff = raw[82]
        self.aformat = raw[83]
        flags2 = u64(raw, 120)
        self.nrext64 = bool(flags2 & DIFLAG2_NREXT64)
        self.anextents = u32(raw, 76) if self.nrext64 else u16(raw, 80)
        self.extents = []
        self.entries = []
        self.format = "none"
        self.root_level = 0
        self.leaf_count = 0
        self.node_count = 0
        self.remote_blocks = 0
        self.remote_logical = set()
        if self.forkoff == 0:
            if self.anextents != 0:
                fail("inode %d: attr extents without forkoff" % ino)
            return
        litino = geo.inodesize - 176
        attr_off = 176 + self.forkoff * 8
        if attr_off >= geo.inodesize:
            fail("inode %d: forkoff out of range" % ino)
        fork_size = geo.inodesize - attr_off
        fork_bytes = raw[attr_off:geo.inodesize]
        if self.aformat == 1:
            self.format = "shortform"
            self.parse_shortform(fork_bytes)
            return
        if self.aformat not in (2, 3):
            fail("inode %d: bad aformat %d" % (ino, self.aformat))
        if self.anextents == 0:
            self.format = "extents-empty"
            return
        self.extents = read_bmbt_extents(
            img, geo, ino, self.aformat, "attr", fork_bytes, self.anextents)
        if len(self.extents) != self.anextents:
            fail("inode %d: attr extent count differs" % ino)
        self.check_extents()
        if not any(e[0] == 0 for e in self.extents):
            fail("inode %d: attr root logical block 0 is not mapped" % ino)
        self.parse_root()
        self.check_remote_overlaps()

    def check_remote_overlaps(self):
        tree_logical = set(leaf[0] for leaf in getattr(self, "leaves", []))
        tree_logical.update(node[0] for node in getattr(self, "nodes", []))
        for entry in self.entries:
            if not entry["remote"] or entry["incomplete"]:
                continue
            start = entry["remote_lbno"]
            for i in range(entry["remote_count"]):
                if start + i in tree_logical:
                    fail("inode %d: remote attr overlaps a DA block" % self.ino)
                if (start + i) in self.remote_logical:
                    fail("inode %d: remote attrs overlap" % self.ino)
                self.remote_logical.add(start + i)

    def check_extents(self):
        last_end = 0
        for startoff, startblock, count, unwritten in self.extents:
            if count == 0 or unwritten:
                fail("inode %d: invalid attr extent" % self.ino)
            if startoff < last_end:
                fail("inode %d: overlapping attr extents" % self.ino)
            agno = startblock >> self.geo.agblklog
            agbno = startblock & ((1 << self.geo.agblklog) - 1)
            if agno >= self.geo.agcount or \
                    agbno >= self.geo.ag_blocks(agno) or \
                    count > self.geo.ag_blocks(agno) - agbno:
                fail("inode %d: attr extent out of range" % self.ino)
            last_end = startoff + count

    def map_logical(self, logical):
        for startoff, startblock, count, _ in self.extents:
            if startoff <= logical < startoff + count:
                return startblock + (logical - startoff)
        fail("inode %d: attr logical block %d is not mapped" %
             (self.ino, logical))

    def read_block(self, logical):
        fsb = self.map_logical(logical)
        return fsb, self.img.read(self.geo.fsb_offset(fsb), self.geo.blocksize)

    def parse_shortform(self, fork):
        if len(fork) < 4:
            fail("inode %d: shortform too small" % self.ino)
        total = u16(fork, 0)
        count = fork[2]
        if total < 4 or total > len(fork):
            fail("inode %d: shortform totsize" % self.ino)
        pos = 4
        for _ in range(count):
            if pos + 3 > total:
                fail("inode %d: shortform entry overrun" % self.ino)
            nlen, vlen, flags = fork[pos], fork[pos + 1], fork[pos + 2]
            if nlen == 0 or (flags & ~ATTR_NSP_MASK):
                fail("inode %d: shortform flags/name" % self.ino)
            if flags & ATTR_NSP_MASK and \
                    bin(flags & ATTR_NSP_MASK).count("1") != 1:
                fail("inode %d: shortform namespace" % self.ino)
            size = 3 + nlen + vlen
            if pos + size > total:
                fail("inode %d: shortform entry size" % self.ino)
            name = fork[pos + 3:pos + 3 + nlen].decode("utf-8", "surrogateescape")
            value = fork[pos + 3 + nlen:pos + size]
            self.entries.append({
                "namespace": display_name(flags, name).split(".", 1)[0],
                "name": display_name(flags, name),
                "value": value,
                "hash": attr_hash(name),
                "remote": False,
                "incomplete": False,
            })
            pos += size
        if pos != total:
            fail("inode %d: shortform does not fill totsize" % self.ino)

    def parse_root(self):
        fsb, block = self.read_block(0)
        magic = u16(block, 8)
        if magic == ATTR_LEAF_MAGIC:
            self.format = "leaf"
            self.leaf_count = 1
            self.parse_leaf(0, None)
            return
        if magic != DA_NODE_MAGIC:
            fail("inode %d: attr root magic 0x%x" % (self.ino, magic))
        self.format = "node"
        self.leaves = []
        self.nodes = []
        self.root_level = 0

        def walk(logical, expected, is_root=False):
            fsb, block = self.read_block(logical)
            magic = u16(block, 8)
            if magic == ATTR_LEAF_MAGIC:
                if expected != 0:
                    fail("inode %d: leaf at node level" % self.ino)
                self.parse_leaf(logical, block)
                return
            if magic != DA_NODE_MAGIC:
                fail("inode %d: DA node magic 0x%x" % (self.ino, magic))
            checked_crc(block, 12, "inode %d attr node" % self.ino)
            checked_identity(block, "inode %d attr node" % self.ino, fsb,
                             self.geo, self.ino)
            level = u16(block, 58)
            count = u16(block, 56)
            if level == 0 or level > 5 or count == 0:
                fail("inode %d: DA node level/count" % self.ino)
            if expected is not None and level != expected:
                fail("inode %d: DA node level mismatch" % self.ino)
            if is_root:
                self.root_level = level
            maxrec = (self.geo.blocksize - ATTR_NODE_HDR_SIZE) // ATTR_NODE_ENTRY_SIZE
            if count > maxrec:
                fail("inode %d: DA node count too large" % self.ino)
            entries = []
            last = 0
            for i in range(count):
                p = ATTR_NODE_HDR_SIZE + i * ATTR_NODE_ENTRY_SIZE
                h, child = u32(block, p), u32(block, p + 4)
                if h < last or child == 0:
                    fail("inode %d: DA node entries" % self.ino)
                last = h
                entries.append((h, child))
            for _, child in entries:
                walk(child, level - 1)
            last_hash = 0
            for h, child in entries:
                # The key is the last hash in the child subtree.
                leaf = next((x for x in self.leaves if x[0] == child), None)
                if leaf is not None:
                    child_last = leaf[1]
                else:
                    node = next((x for x in self.nodes if x[0] == child), None)
                    if node is None:
                        fail("inode %d: DA node child missing" % self.ino)
                    child_last = node[1]
                if child_last != h:
                    fail("inode %d: DA node key mismatch" % self.ino)
                last_hash = h
            self.nodes.append((logical, last_hash, level,
                               u32(block, 4), u32(block, 0)))

        walk(0, None, True)
        self.node_count = len(self.nodes)
        # Leaf and node sibling chains must follow key order.
        for i in range(len(self.leaves)):
            logical, _, back, forw, _ = self.leaves[i]
            want_back = self.leaves[i - 1][0] if i else 0
            want_forw = self.leaves[i + 1][0] if i + 1 < len(self.leaves) else 0
            if back != want_back or forw != want_forw:
                fail("inode %d: attr leaf sibling chain" % self.ino)
        for level in range(1, 6):
            nodes = [n for n in self.nodes if n[2] == level]
            for i, node in enumerate(nodes):
                logical, _, _, back, forw = node
                want_back = nodes[i - 1][0] if i else 0
                want_forw = nodes[i + 1][0] if i + 1 < len(nodes) else 0
                if back != want_back or forw != want_forw:
                    fail("inode %d: attr node sibling chain" % self.ino)

    def parse_leaf(self, logical, block):
        if block is None:
            fsb, block = self.read_block(logical)
        else:
            fsb = self.map_logical(logical)
        checked_crc(block, 12, "inode %d attr leaf" % self.ino)
        checked_identity(block, "inode %d attr leaf" % self.ino, fsb,
                         self.geo, self.ino)
        count = u16(block, 56)
        used = u16(block, 58)
        firstused = u16(block, 60)
        if firstused == 0 and self.geo.blocksize == 65536:
            firstused = self.geo.blocksize
        if firstused < ATTR_LEAF_HDR_SIZE or firstused > self.geo.blocksize:
            fail("inode %d: attr leaf firstused" % self.ino)
        entries_end = ATTR_LEAF_HDR_SIZE + count * ATTR_LEAF_ENTRY_SIZE
        if entries_end > firstused:
            fail("inode %d: attr leaf entry table" % self.ino)
        seen = 0
        last_hash = 0
        leaf_entries = []
        for i in range(count):
            p = ATTR_LEAF_HDR_SIZE + i * ATTR_LEAF_ENTRY_SIZE
            h = u32(block, p)
            nameidx = u16(block, p + 4)
            flags = block[p + 6]
            if h < last_hash or flags & ~ATTR_ON_DISK_MASK:
                fail("inode %d: attr leaf entry" % self.ino)
            last_hash = h
            nsp = flags & ATTR_NSP_MASK
            if nsp and bin(nsp).count("1") != 1:
                fail("inode %d: attr leaf namespace" % self.ino)
            if nameidx < firstused or nameidx >= self.geo.blocksize:
                fail("inode %d: attr leaf nameidx" % self.ino)
            if flags & ATTR_LOCAL:
                if nameidx + 3 > self.geo.blocksize:
                    fail("inode %d: attr local header" % self.ino)
                vlen, nlen = u16(block, nameidx), block[nameidx + 2]
                size = (3 + nlen + vlen + 3) & ~3
                if nlen == 0 or nameidx + size > self.geo.blocksize:
                    fail("inode %d: attr local size" % self.ino)
                name = block[nameidx + 3:nameidx + 3 + nlen].decode("utf-8", "surrogateescape")
                value = block[nameidx + 3 + nlen:nameidx + 3 + nlen + vlen]
                remote = False
            else:
                if nameidx + 9 > self.geo.blocksize:
                    fail("inode %d: attr remote header" % self.ino)
                valueblk = u32(block, nameidx)
                vlen = u32(block, nameidx + 4)
                nlen = block[nameidx + 8]
                size = (11 + nlen + 3) & ~3
                if nlen == 0 or nameidx + size > self.geo.blocksize:
                    fail("inode %d: attr remote size" % self.ino)
                if not (flags & ATTR_INCOMPLETE) and valueblk == 0:
                    fail("inode %d: attr remote has no value block" % self.ino)
                name = block[nameidx + 9:nameidx + 9 + nlen].decode(
                    "utf-8", "surrogateescape")
                if flags & ATTR_INCOMPLETE and valueblk == 0:
                    value = b""
                else:
                    value = self.read_remote(valueblk, vlen)
                    self.remote_blocks += (vlen + (self.geo.blocksize -
                                                   ATTR_RMT_HDR_SIZE) - 1) // \
                                          (self.geo.blocksize -
                                           ATTR_RMT_HDR_SIZE)
                remote = True
            seen += size
            leaf_entries.append({
                "namespace": display_name(flags, name).split(".", 1)[0],
                "name": display_name(flags, name),
                "value": value,
                "hash": h,
                "remote": remote,
                "incomplete": bool(flags & ATTR_INCOMPLETE),
                "logical": logical,
                "nameidx": nameidx,
                "remote_lbno": valueblk if not (flags & ATTR_LOCAL) else 0,
                "remote_count": ((vlen + (self.geo.blocksize -
                                          ATTR_RMT_HDR_SIZE) - 1) //
                                 (self.geo.blocksize - ATTR_RMT_HDR_SIZE)
                                 if not (flags & ATTR_LOCAL) else 0),
            })
        if seen != used:
            fail("inode %d: attr leaf usedbytes %d != %d" %
                 (self.ino, used, seen))
        if not hasattr(self, "leaves"):
            self.leaves = []
        self.leaves.append((logical, leaf_entries[-1]["hash"] if leaf_entries
                            else 0, u32(block, 4), u32(block, 0), block))
        for entry in leaf_entries:
            if entry["incomplete"]:
                continue
            self.entries.append(entry)

    def read_remote(self, logical, length):
        if length == 0:
            return b""
        space = self.geo.blocksize - ATTR_RMT_HDR_SIZE
        out = bytearray()
        offset = 0
        for i in range((length + space - 1) // space):
            fsb = self.map_logical(logical + i)
            block = self.img.read(self.geo.fsb_offset(fsb), self.geo.blocksize)
            if u32(block, 0) != ATTR_RMT_MAGIC:
                fail("inode %d: remote attr magic" % self.ino)
            checked_crc(block, 12, "inode %d remote attr" % self.ino)
            if block[16:32] != self.geo.uuid or u64(block, 32) != self.ino:
                fail("inode %d: remote attr identity" % self.ino)
            if u64(block, 40) != (self.geo.fsb_offset(fsb) >> 9):
                fail("inode %d: remote attr blkno" % self.ino)
            if u64(block, 48) != NULLCOMMITLSN:
                fail("inode %d: remote attr LSN" % self.ino)
            if u32(block, 4) != offset:
                fail("inode %d: remote attr offset" % self.ino)
            n = u32(block, 8)
            if n > space or offset + n > ATTR_MAX_VALUE:
                fail("inode %d: remote attr bytes" % self.ino)
            out.extend(block[ATTR_RMT_HDR_SIZE:ATTR_RMT_HDR_SIZE + n])
            offset += n
        if offset != length:
            fail("inode %d: remote attr length" % self.ino)
        return bytes(out)

    def canonical(self):
        attrs = []
        for entry in sorted(self.entries, key=lambda e: (e["hash"], e["name"])):
            value = entry["value"]
            attrs.append({
                "namespace": entry["namespace"],
                "name": entry["name"],
                "length": len(value),
                "sha256": hashlib.sha256(value).hexdigest(),
                "remote": entry["remote"],
                "hash": entry["hash"],
            })
        return {
            "inode": self.ino,
            "format": self.format,
            "forkoff": self.forkoff,
            "aformat": self.aformat,
            "anextents": self.anextents,
            "root_level": self.root_level,
            "leaf_count": self.leaf_count if self.format == "leaf"
            else len(getattr(self, "leaves", [])),
            "node_count": self.node_count,
            "remote_blocks": self.remote_blocks,
            "extents": [{"startoff": a, "startblock": b, "blockcount": c}
                        for a, b, c, _ in self.extents],
            "attributes": attrs,
        }


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("--inode", type=int, action="append", default=[])
    parser.add_argument("--json")
    parser.add_argument("--manifest")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    img = Image(args.image)
    geo = Geometry(img)
    if not args.inode:
        fail("at least one --inode is required")
    results = []
    for ino in args.inode:
        raw = img.read(geo.inode_offset(ino), geo.inodesize)
        if u16(raw, 0) != DINODE_MAGIC:
            fail("inode %d: magic" % ino)
        if u64(raw, 152) != ino:
            fail("inode %d: di_ino" % ino)
        checked_crc(raw, 100, "inode %d" % ino)
        fork = AttrFork(img, geo, ino, raw)
        results.append(fork.canonical())
    if args.manifest:
        with open(args.manifest, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        expected = {int(item["ino"]): item for item in manifest.get("inodes", [])}
        for result in results:
            item = expected.get(result["inode"])
            if item is None:
                fail("manifest has no inode %d" % result["inode"])
            want = {a["name"]: a for a in item.get("attributes", [])}
            got = {a["name"]: a for a in result["attributes"]}
            if set(want) != set(got):
                fail("inode %d attribute names differ: %s != %s" %
                     (result["inode"], sorted(want), sorted(got)))
            if "format" in item and item["format"] != result["format"]:
                fail("inode %d format %s != %s" %
                     (result["inode"], item["format"], result["format"]))
            if "root_level" in item and \
                    int(item["root_level"]) != result["root_level"]:
                fail("inode %d root_level %d != %d" %
                     (result["inode"], int(item["root_level"]),
                      result["root_level"]))
            for name, a in want.items():
                b = got[name]
                if "value_hex" in a:
                    raw = bytes.fromhex(a["value_hex"])
                    if len(raw) != b["length"] or \
                            hashlib.sha256(raw).hexdigest() != b["sha256"]:
                        fail("inode %d attribute %s value differs" %
                             (result["inode"], name))
                    continue
                if a.get("length") != b["length"] or \
                        a.get("sha256") != b["sha256"]:
                    fail("inode %d attribute %s value differs" %
                         (result["inode"], name))
    data = {"inodes": results}
    text = json.dumps(data, indent=2, sort_keys=True)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    if not args.quiet:
        print(text)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except AssertionError as error:
        print("rw6d probe: FAIL: %s" % error, file=sys.stderr)
        sys.exit(1)
