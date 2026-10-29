#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Build a small libFuzzer seed corpus from a real XFS image.

The fuzz harnesses must start from structurally valid metadata, otherwise a
random 512-byte mutation never reaches the fork/btree/attribute parsers. The
seed image is generated in CI (and can be generated locally) with mkfs.xfs:

    truncate -s 512M seed.img
    mkfs.xfs -f -b size=4096 -s size=512 -i size=512 seed.img
    python tests/fuzz/make_corpus.py seed.img build-fuzz/corpus
    ./build-fuzz/tests/fuzz/fuzz_inode build-fuzz/corpus/fuzz_inode

This script intentionally parses the on-disk superblock itself; it does not
depend on the C++ implementation being correct.
"""

import os
import struct
import sys


def be16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def be64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def write_seed(directory, name, payload, minimum):
    os.makedirs(directory, exist_ok=True)
    if len(payload) < minimum:
        payload = payload + bytes(minimum - len(payload))
    with open(os.path.join(directory, name), "wb") as out:
        out.write(payload)


def main():
    if len(sys.argv) != 3:
        print("usage: make_corpus.py <xfs-image> <corpus-dir>", file=sys.stderr)
        return 2
    image_path, corpus = sys.argv[1], sys.argv[2]
    with open(image_path, "rb") as image:
        head = image.read(4096)
        if len(head) < 4096 or head[:4] != b"XFSB":
            print("not an XFS image (bad superblock magic)", file=sys.stderr)
            return 1
        blocksize = be32(head, 4)
        agblocks = be32(head, 84)
        inodesize = be16(head, 104)
        inopblock = be16(head, 106)
        blocklog = head[120]
        inopblog = head[123]
        agblklog = head[124]
        rootino = be64(head, 56)
        if blocksize != (1 << blocklog) or blocksize < 512:
            print("unsupported block size", file=sys.stderr)
            return 1

        def read_at(offset, length):
            image.seek(offset)
            return image.read(length)

        write_seed(os.path.join(corpus, "fuzz_superblock"), "seed",
                   head[:512], 512)
        write_seed(os.path.join(corpus, "fuzz_ag"), "seed",
                   read_at(512, 1536), 1536)

        agino_log = agblklog + inopblog
        agno = rootino >> agino_log
        agino = rootino & ((1 << agino_log) - 1)
        agbno = agino >> inopblog
        index = agino & (inopblock - 1)
        inode_off = (agno * agblocks * blocksize + agbno * blocksize +
                     index * inodesize)
        inode = read_at(inode_off, inodesize)
        write_seed(os.path.join(corpus, "fuzz_inode"), "seed", inode,
                   inodesize)

        agf = read_at(512, 512)
        bno_root = be32(agf, 16)
        btree_off = bno_root * blocksize
        write_seed(os.path.join(corpus, "fuzz_btree"), "seed",
                   read_at(btree_off, blocksize), blocksize)

        core = 176
        forkoff = inode[82] if len(inode) > 82 else 0
        data_end = forkoff * 8 if forkoff else inodesize
        data_fork = inode[core:min(data_end, inodesize)]
        write_seed(os.path.join(corpus, "fuzz_directory"), "seed", data_fork,
                   64)
        attr_fork = inode[forkoff * 8:] if forkoff else b""
        if not attr_fork:
            # Empty shortform attribute fork: totsize=4, count=0.
            attr_fork = struct.pack(">HBB", 4, 0, 0)
        write_seed(os.path.join(corpus, "fuzz_xattr"), "seed", attr_fork, 64)
        write_seed(os.path.join(corpus, "fuzz_log_item"), "seed",
                   b"\x00" * 64, 64)

    print("wrote corpus under", corpus)
    return 0


if __name__ == "__main__":
    sys.exit(main())
