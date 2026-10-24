#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Negative RW-6B tests on copies of genuine Linux fixtures.

This is fault injection, not an ownership oracle or a synthetic-RUI acceptance
fixture. Every refused operation must preserve the full image SHA256.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

import recovery_probe as log
from rw6b_rmap_probe import xfs_crc


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 << 20), b""):
            result.update(chunk)
    return result.hexdigest()


def invoke(exe, *args):
    return subprocess.run([exe, *map(str, args)], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def metadata_faults(path, exe):
    with path.open("rb") as stream:
        sb = stream.read(512)
        blocksize = log.be32(sb, 4)
        agblocks = log.be32(sb, 84)
        stream.seek(log.be16(sb, 102))
        agf = stream.read(512)
        root = log.be32(agf, 24)
        stream.seek(root * blocksize)
        root_data = stream.read(blocksize)
        assert log.be16(root_data, 4) > 0, "fixture must have an internal rmap root"
        maxnode = (blocksize - 56) // 44
        leaf = log.be32(root_data, 56 + maxnode * 40)
        stream.seek(leaf * blocksize)
        leaf_data = stream.read(blocksize)
        while log.be16(leaf_data, 4):
            leaf = log.be32(leaf_data, 56 + maxnode * 40)
            stream.seek(leaf * blocksize)
            leaf_data = stream.read(blocksize)
    assert log.be16(leaf_data, 6) >= 2
    faults = []

    def add(name, block, original, offset, fmt, value, checksum=True):
        data = bytearray(original)
        struct.pack_into(fmt, data, offset, value)
        if checksum:
            struct.pack_into("<I", data, 52, xfs_crc(data, 52))
        faults.append((name, block * blocksize, data, None))

    add("bad-crc", root, root_data, 52, "<I",
        struct.unpack_from("<I", root_data, 52)[0] ^ 1, False)
    add("zero-length", leaf, leaf_data, 60, ">I", 0)
    add("invalid-owner", leaf, leaf_data, 64, ">Q", 0)
    add("out-of-ag", leaf, leaf_data, 56, ">I", agblocks)
    add("parent-high-key", root, root_data, 76, ">I", agblocks)
    add("sibling-cycle", leaf, leaf_data, 12, ">I", leaf)
    # A valid-but-wrong inode owner need not violate key order or parent keys.
    # Truncating that file must refuse the mismatched FreeOwned before writes.
    mapped = invoke(exe, "bmap", path, "/small/s00.bin", "--json")
    assert mapped.returncode == 0, mapped.stderr
    mapped = json.loads(mapped.stdout)
    assert mapped["size"] > 0 and mapped["extents"]
    fsb = mapped["extents"][0]["startblock"]
    assert fsb >> sb[124] == 0
    stack = [root]
    found = False
    with path.open("rb") as stream:
        while stack and not found:
            block = stack.pop()
            stream.seek(block * blocksize)
            data = stream.read(blocksize)
            count = log.be16(data, 6)
            if log.be16(data, 4):
                stack.extend(log.be32(data, 56 + maxnode * 40 + i * 4)
                             for i in range(count))
                continue
            for i in range(1, count - 1):
                rec = 56 + i * 24
                if (log.be32(data, rec) <= fsb < log.be32(data, rec) + log.be32(data, rec + 4)
                        and log.be64(data, rec + 8) == mapped["inode"]):
                    add("ownership-mismatch", block, data, rec + 8, ">Q", mapped["inode"] + 1)
                    name, offset, changed, _ = faults[-1]
                    faults[-1] = (name, offset, changed, ("truncate", "/small/s00.bin", 0))
                    found = True
                    break
    assert found, "no interior file ownership record found for fault injection"
    return faults


def shared_rui_fault(path, shared_op):
    inventory = log.scan(path)
    committed = {item["id"] for trans in inventory["transactions"]
                 for item in trans["items"] if item["type"] == "XFS_LI_RUI"}
    assert committed, "source must contain a committed genuine Linux RUI"
    with path.open("rb") as stream:
        geo = log.Geometry(stream.read(512))
        stream.seek(geo.log_byte)
        image = log.LogImage(stream.read(geo.log_bb * 512), geo)
    tail = log.find_tail(image, geo)
    for block, header, original_header, original_data in log.iterate_records(
            image, tail["head"], tail["tail"], geo):
        pos = 0
        for _ in range(header["num_ops"]):
            length = log.be32(original_data, pos + 4)
            flags = original_data[pos + 9]
            pos += 12
            region = original_data[pos:pos + length]
            if (not flags & (log.XLOG_WAS_CONT_TRANS | log.XLOG_CONTINUE_TRANS)
                    and len(region) >= 48 and log.le16(region, 0) == 0x1240
                    and struct.unpack_from("<Q", region, 8)[0] in committed
                    and len(region) == 16 + log.le32(region, 4) * 32):
                data = bytearray(original_data)
                struct.pack_into("<I", data, pos + 44, shared_op)
                packed_header = bytearray(original_header)
                # Preserve the kernel record's CRC layout and cycle-stamping.
                _, _, stamped = log.read_record(image, block,
                    len(original_header) // 512, header["hsize"], geo)
                fixed = 328
                if header["crc"] and log.record_crc(original_header, stamped) != header["crc"]:
                    assert log.record_crc(original_header, stamped, fixed=324) == header["crc"]
                    fixed = 324
                for i in range(len(data) // 512):
                    saved_at = 44 + i * 4 if i < 64 else (i // 64) * 512 + 4 + (i % 64) * 4
                    packed_header[saved_at:saved_at + 4] = data[i * 512:i * 512 + 4]
                    struct.pack_into(">I", data, i * 512, header["cycle"])
                struct.pack_into("<I", packed_header, 32,
                                 log.record_crc(packed_header, data, fixed=fixed))
                return geo, block, bytes(packed_header + data)
            pos += length
    raise AssertionError("no complete committed RUI format region available for injection")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--kind", choices=("metadata", "shared-rui"), required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    source = Path(args.source).resolve()
    out = Path(args.work_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    target = out / "negative.img"
    assert target != source
    evidence = []
    source_hash = digest(source)

    if args.kind == "metadata":
        # Positive control: don't accept a policy refusal in place of a
        # malformed-metadata refusal. This must use the ordinary writer gate.
        shutil.copyfile(source, target)
        # An inode-only create need not touch rmapbt at all. Allocate data for
        # an AG0 file so the corrupted AG0 tree is actually loaded/edited.
        allocation = ("fallocate", target, "/small/s00.bin", 1048576, 4096,
                      "--rw-experimental")
        control = invoke(args.exe, *allocation)
        assert control.returncode == 0, control.stdout + control.stderr
        cases = metadata_faults(source, args.exe)
    else:
        cases = [("shared-%d" % op, *shared_rui_fault(source, op))
                 for op in (2, 4, 6)]

    for case in cases:
        name = case[0]
        shutil.copyfile(source, target)
        with target.open("r+b") as stream:
            if args.kind == "metadata":
                _, offset, data, _ = case
                stream.seek(offset)
                stream.write(data)
            else:
                _, geo, block, data = case
                for i in range(0, len(data), 512):
                    stream.seek(geo.log_byte + ((block + i // 512) % geo.log_bb) * 512)
                    stream.write(data[i:i + 512])
        before = digest(target)
        if args.kind == "metadata":
            command = case[3]
            result = (invoke(args.exe, command[0], target, *command[1:], "--rw-experimental")
                      if command else invoke(args.exe, *allocation))
        else:
            # Verify the injected log is still CRC-valid and reconstructable.
            assert log.scan(target)["transactions"]
            result = invoke(args.exe, "recover", target, "--yes-i-really-mean-it")
            assert "unsupported reflink semantics" in result.stdout + result.stderr
        after = digest(target)
        assert result.returncode != 0, "%s unexpectedly succeeded" % name
        assert before == after, "%s wrote to the refused image" % name
        evidence.append({"case": name, "before_sha256": before,
                         "after_sha256": after, "exit": result.returncode,
                         "diagnostic": result.stdout + result.stderr})
        print("refused without writes:", name, flush=True)
    assert digest(source) == source_hash, "source fixture changed"
    (out / "refusals.json").write_text(json.dumps({"kind": args.kind,
        "source_sha256": source_hash, "cases": evidence}, indent=2) + "\n", encoding="utf-8")
    target.unlink()


if __name__ == "__main__":
    main()
