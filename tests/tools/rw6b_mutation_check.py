#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Windows-side state checker for the RW-6B Linux-fixture mutation matrix.

This is deliberately *not* an rmap oracle: ownership is checked by
rw6b_rmap_probe.py, and Linux checks xfs_repair/mount.  This program checks the
user-visible namespace, sizes, link counts, symlink targets and file bytes
through the real xfswin CLI after every mutation stage.  At the end it writes
expected.json, which the Linux verifier consumes without running any Windows
code.

usage:
  rw6b_mutation_check.py --exe xfswin.exe --image rmap.img --stage final \
      [--expected-out expected.json]
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

BLOCK = 4096
UNWRITTEN_SIZE = 1 << 20
REMOTE_TARGET = "../" + ("x/" * 100)
CREATE_EXTEND_SIZE = 200000
CREATE_TRUNCATE_SIZE = 100000
CREATE_REGROW_SIZE = 300000


class CheckError(AssertionError):
    pass


class Cli:
    def __init__(self, exe, image):
        self.exe = exe
        self.image = image

    def run(self, *args, binary=False):
        proc = subprocess.run(
            [self.exe, *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if proc.returncode != 0:
            raise CheckError(
                "xfswin %s failed (%d): %s"
                % (" ".join(args), proc.returncode,
                   proc.stderr.decode("utf-8", "replace").strip()))
        return proc.stdout if binary else proc.stdout.decode("utf-8", "replace")

    def cat(self, path):
        return self.run("cat", self.image, path, binary=True)

    def listing(self, path):
        return json.loads(self.run("ls", self.image, path, "--json"))

    def entry(self, path):
        parent, name = os.path.split(path.rstrip("/"))
        if not parent:
            parent = "/"
        for item in self.listing(parent):
            if item["name"] == name:
                return item
        return None

    def bmap(self, path):
        return json.loads(self.run("bmap", self.image, path, "--json"))


def fail(message):
    raise CheckError(message)


def require_file(cli, path, content, nlink=None):
    item = cli.entry(path)
    if item is None or item.get("dir") or item.get("symlink"):
        fail("%s is not a regular file" % path)
    if item["size"] != len(content):
        fail("%s size %d != %d" % (path, item["size"], len(content)))
    if nlink is not None and item["nlink"] != nlink:
        fail("%s nlink %d != %d" % (path, item["nlink"], nlink))
    actual = cli.cat(path)
    if actual != content:
        fail("%s content mismatch (%d bytes)" % (path, len(actual)))
    return item


def require_absent(cli, path):
    if cli.entry(path) is not None:
        fail("%s still exists" % path)


def require_dir(cli, path):
    item = cli.entry(path)
    if item is None or not item.get("dir"):
        fail("%s is not a directory" % path)
    return item


def require_symlink(cli, path, target):
    item = cli.entry(path)
    if item is None or not item.get("symlink"):
        fail("%s is not a symlink" % path)
    if item.get("target") != target:
        fail("%s target %r != %r" % (path, item.get("target"), target))
    actual = cli.cat(path)
    if actual.rstrip(b"\n") != target.encode():
        fail("%s cat target content mismatch" % path)


def extend_payload(offset, size):
    return bytes((((offset + i) * 131 + 7) & 0xFF) for i in range(size - offset))


def create_file_expectation(stage):
    """Bytes of /qual/create.bin after every stage that has the file."""
    if stage == "write":
        return b"created"
    if stage == "append":
        return b"createdAPPEND"
    if stage == "overwrite":
        return b"OVERWRITEPEND"
    base = b"OVERWRITEPEND"
    extended = base + extend_payload(len(base), CREATE_EXTEND_SIZE)
    if stage == "extend":
        return extended
    if stage == "truncate":
        return extended[:CREATE_TRUNCATE_SIZE]
    # regrow and every later stage
    return extended[:CREATE_TRUNCATE_SIZE] + bytes(
        CREATE_REGROW_SIZE - CREATE_TRUNCATE_SIZE)


def check_original_samples(cli, manifest):
    samples = ["small/s00.bin", "bigdir/n00042.bin", "fragmented.bin",
               "sparse.bin", "unwritten.bin"]
    wanted = {}
    with open(manifest, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ")
            if len(parts) >= 5 and parts[0] == "file" and parts[1] in samples:
                wanted[parts[1]] = (int(parts[2]), parts[4])
            if len(parts) >= 3 and parts[0] == "link" and parts[1] in samples:
                wanted[parts[1]] = ("link", " ".join(parts[2:]))
    for sample in samples:
        if sample not in wanted:
            fail("manifest has no entry for sample /%s" % sample)
        expect = wanted[sample]
        item = cli.entry("/" + sample)
        if item is None:
            fail("original /%s disappeared" % sample)
        if expect[0] == "link":
            if not item.get("symlink") or item.get("target") != expect[1]:
                fail("original symlink /%s changed" % sample)
            continue
        if item["size"] != expect[0]:
            fail("original /%s size changed" % sample)
        digest = hashlib.sha256(cli.cat("/" + sample)).hexdigest()
        if digest != expect[1]:
            fail("original /%s content changed" % sample)


def check_stage(cli, stage, manifest):
    qual = cli.entry("/qual")
    if stage not in ("initial",) and (qual is None or not qual.get("dir")):
        fail("/qual is missing")
    if manifest:
        check_original_samples(cli, manifest)

    create = "/qual/create.bin"
    unwritten = "/qual/unwritten.bin"

    if stage == "mkdir":
        if cli.listing("/qual"):
            fail("/qual is not empty after mkdir")
    elif stage in ("create",):
        item = cli.entry(create)
        if item is None or item["size"] != 0:
            fail("empty create.bin missing")
    elif stage in ("write", "append", "overwrite", "extend", "truncate",
                   "regrow", "unlink", "hardlink", "unlink_one",
                   "last_unlink", "rmdir", "rename_same", "rename_cross",
                   "replace", "symlink", "dir_grow_shrink",
                   "dir_shrink_empty", "bmbt_grow", "bmbt_shrink",
                   "reuse_b", "reuse_c", "inode_churn", "final"):
        if stage not in ("final",):
            require_file(cli, create, create_file_expectation(stage))
        else:
            require_file(cli, create,
                         create_file_expectation("regrow"))
    if stage == "unwritten":
        item = require_file(cli, unwritten, bytes(UNWRITTEN_SIZE))
        bmap = cli.bmap(unwritten)
        if bmap["size"] != UNWRITTEN_SIZE:
            fail("unwritten.bin size mismatch")
        if not bmap["extents"] or any(not e["unwritten"] for e in bmap["extents"]):
            fail("unwritten.bin is not fully unwritten")
    elif stage == "unwritten_write":
        content = b"DATA" + bytes(UNWRITTEN_SIZE - 4)
        require_file(cli, unwritten, content)
        bmap = cli.bmap(unwritten)
        first = [e for e in bmap["extents"] if e["startoff"] == 0]
        if not first or first[0]["unwritten"]:
            fail("first block was not converted by the write")
    elif stage == "convert":
        content = b"DATA" + bytes(UNWRITTEN_SIZE - 4)
        require_file(cli, unwritten, content)
        bmap = cli.bmap(unwritten)
        written = set()
        for e in bmap["extents"]:
            if not e["unwritten"]:
                written.update(range(e["startoff"],
                                     e["startoff"] + e["blockcount"]))
        if not {0, 1, 2}.issubset(written):
            fail("convert-unwritten did not write blocks 0..2")
        remain = [e for e in bmap["extents"] if e["unwritten"]]
        if not remain or remain[-1]["startoff"] + remain[-1]["blockcount"] != 256:
            fail("unwritten tail was not preserved")
    elif stage == "unlink":
        require_absent(cli, "/qual/unlink.bin")
    elif stage == "hardlink":
        a = require_file(cli, "/qual/link.bin", b"hard", nlink=2)
        b = require_file(cli, "/qual/hard.bin", b"hard", nlink=2)
        if a["inode"] != b["inode"]:
            fail("hard links point at different inodes")
    elif stage == "unlink_one":
        require_absent(cli, "/qual/hard.bin")
        require_file(cli, "/qual/link.bin", b"hard", nlink=1)
    elif stage == "last_unlink":
        require_absent(cli, "/qual/link.bin")
    elif stage == "rmdir":
        require_absent(cli, "/qual/victim")
    elif stage == "rename_same":
        require_absent(cli, "/qual/r1.bin")
        require_file(cli, "/qual/r2.bin", b"rename")
    elif stage == "rename_cross":
        require_absent(cli, "/qual/a/x.bin")
        require_file(cli, "/qual/b/x.bin", b"cross")
    elif stage == "replace":
        require_absent(cli, "/qual/repl-src")
        require_file(cli, "/qual/repl-dst", b"SOURCE")
    elif stage == "symlink":
        require_symlink(cli, "/qual/inline.link", "r2.bin")
        require_symlink(cli, "/qual/remote.link", REMOTE_TARGET)
    elif stage in ("dir_grow_shrink", "dir_shrink_empty"):
        grow = cli.entry("/qual/grow")
        if stage == "dir_grow_shrink":
            if grow is None or not grow.get("dir"):
                fail("/qual/grow disappeared")
            names = {e["name"] for e in cli.listing("/qual/grow")}
            expected = {"c%04d" % i for i in range(500, 600)}
            if names != expected:
                fail("directory growth/shrink left %d entries" % len(names))
        else:
            require_absent(cli, "/qual/grow")
    elif stage == "bmbt_grow":
        bmap = cli.bmap("/qual/many.bin")
        if len(bmap["extents"]) != 48:
            fail("many.bin has %d extents, expected 48" % len(bmap["extents"]))
        if bmap["size"] != 47 * 2 * BLOCK + 1:
            fail("many.bin size mismatch")
    elif stage == "bmbt_shrink":
        bmap = cli.bmap("/qual/many.bin")
        if len(bmap["extents"]) != 1 or bmap["size"] != BLOCK:
            fail("many.bin did not collapse to one extent")
        content = cli.cat("/qual/many.bin")
        if content[0:1] != b"x" or content[1:] != bytes(BLOCK - 1):
            fail("many.bin content after truncate mismatch")
    elif stage == "reuse_b":
        require_absent(cli, "/qual/reuse-a.bin")
        bmap = cli.bmap("/qual/reuse-b.bin")
        if len(bmap["extents"]) != 96:
            fail("reuse-b.bin extent count mismatch")
    elif stage == "reuse_c":
        require_absent(cli, "/qual/reuse-b.bin")
        bmap = cli.bmap("/qual/reuse-c.bin")
        if len(bmap["extents"]) != 96:
            fail("reuse-c.bin extent count mismatch")
        expected = bytearray(95 * 2 * BLOCK + 1)
        for i in range(96):
            expected[i * 2 * BLOCK] = ord("x")
        require_file(cli, "/qual/reuse-c.bin", bytes(expected))
    elif stage == "inode_churn":
        names = {e["name"] for e in cli.listing("/qual")}
        leaked = [n for n in names if n.startswith("i") and n.endswith(".bin")]
        if leaked:
            fail("inode churn leaked %d entries" % len(leaked))
    elif stage == "final":
        require_file(cli, "/qual/r2.bin", b"rename")

        require_file(cli, "/qual/repl-dst", b"SOURCE")
        require_file(cli, "/qual/b/x.bin", b"cross")
        require_symlink(cli, "/qual/inline.link", "r2.bin")
        require_symlink(cli, "/qual/remote.link", REMOTE_TARGET)
        require_dir(cli, "/qual/b")
        require_absent(cli, "/qual/a")
        require_absent(cli, "/qual/grow")
        require_absent(cli, "/qual/dir")
        require_absent(cli, "/qual/victim")
        require_absent(cli, "/qual/repl-src")
        require_absent(cli, "/qual/many.bin")
        require_absent(cli, "/qual/reuse-a.bin")
        require_absent(cli, "/qual/reuse-b.bin")
        require_absent(cli, "/qual/link.bin")
        require_absent(cli, "/qual/hard.bin")
        require_absent(cli, "/qual/unlink.bin")
        bmap = cli.bmap("/qual/reuse-c.bin")
        if len(bmap["extents"]) != 96:
            fail("final reuse-c.bin extent count mismatch")
    elif stage in ("mkdir", "create", "write", "append", "overwrite",
                   "extend", "truncate", "regrow", "unlink", "hardlink",
                   "unlink_one", "last_unlink", "rmdir", "rename_same",
                   "rename_cross", "replace", "symlink", "dir_grow_shrink",
                   "dir_shrink_empty", "bmbt_grow", "bmbt_shrink", "reuse_b",
                   "reuse_c", "inode_churn", "final"):
        pass
    elif stage != "initial":
        fail("unknown stage %s" % stage)


def expected_entries(cli):
    """Build the final expected manifest from the state model, not from the
    image: contents are computed here and every file is also compared now."""
    entries = [
        {"path": "/qual", "type": "dir"},
        {"path": "/qual/b", "type": "dir"},
        {"path": "/qual/create.bin", "type": "file",
         "content": create_file_expectation("regrow")},
        {"path": "/qual/unwritten.bin", "type": "file",
         "content": b"DATA" + bytes(UNWRITTEN_SIZE - 4)},
        {"path": "/qual/r2.bin", "type": "file", "content": b"rename"},
        {"path": "/qual/repl-dst", "type": "file", "content": b"SOURCE"},
        {"path": "/qual/b/x.bin", "type": "file", "content": b"cross"},
        {"path": "/qual/inline.link", "type": "symlink", "target": "r2.bin"},
        {"path": "/qual/remote.link", "type": "symlink",
         "target": REMOTE_TARGET},
    ]
    reuse = bytearray(95 * 2 * BLOCK + 1)
    for i in range(96):
        reuse[i * 2 * BLOCK] = ord("x")
    entries.append({"path": "/qual/reuse-c.bin", "type": "file",
                    "content": bytes(reuse)})
    out = []
    for item in entries:
        row = {"path": item["path"], "type": item["type"]}
        if item["type"] == "file":
            data = item["content"]
            row["size"] = len(data)
            row["sha256"] = hashlib.sha256(data).hexdigest()
            row["nlink"] = 1
        elif item["type"] == "symlink":
            row["target"] = item["target"]
        out.append(row)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--manifest")
    parser.add_argument("--expected-out")
    args = parser.parse_args()

    cli = Cli(args.exe, args.image)
    try:
        check_stage(cli, args.stage, args.manifest)
        if args.expected_out:
            entries = expected_entries(cli)
            with open(args.expected_out, "w", encoding="utf-8") as f:
                json.dump({"entries": entries}, f, indent=2, sort_keys=True)
                f.write("\n")
    except CheckError as error:
        print("RW-6B state check failed at %s: %s" % (args.stage, error),
              file=sys.stderr)
        return 1
    print("RW-6B state check OK: %s" % args.stage)
    return 0


if __name__ == "__main__":
    sys.exit(main())
