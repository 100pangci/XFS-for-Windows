#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""State model for the second Windows pass after Linux mutating the
Windows-mutated rmapbt image.

usage:
  rw6b_round2_check.py --exe xfswin.exe --image img --stage initial|final \
      [--expected-out expected.json]
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

BLOCK = 4096
REMOTE_TARGET = "../" + ("y/" * 120)
LINUX_OLD_CREATE = None  # computed from the Windows expectation


class CheckError(AssertionError):
    pass


class Cli:
    def __init__(self, exe, image):
        self.exe, self.image = exe, image

    def run(self, *args, binary=False):
        proc = subprocess.run([self.exe, *args], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise CheckError("xfswin %s failed (%d): %s" %
                             (" ".join(args), proc.returncode,
                              proc.stderr.decode("utf-8", "replace").strip()))
        return proc.stdout if binary else proc.stdout.decode("utf-8", "replace")

    def cat(self, path):
        return self.run("cat", self.image, path, binary=True)

    def listing(self, path):
        return json.loads(self.run("ls", self.image, path, "--json"))

    def entry(self, path):
        parent, name = os.path.split(path.rstrip("/"))
        parent = parent or "/"
        for item in self.listing(parent):
            if item["name"] == name:
                return item
        return None

    def bmap(self, path):
        return json.loads(self.run("bmap", self.image, path, "--json"))


def fail(message):
    raise CheckError(message)


def expect_file(cli, path, content, nlink=1):
    item = cli.entry(path)
    if item is None or item.get("dir") or item.get("symlink"):
        fail("%s is not a regular file" % path)
    if item["size"] != len(content):
        fail("%s size %d != %d" % (path, item["size"], len(content)))
    if item["nlink"] != nlink:
        fail("%s nlink %d != %d" % (path, item["nlink"], nlink))
    if cli.cat(path) != content:
        fail("%s content mismatch" % path)


def expect_absent(cli, path):
    if cli.entry(path) is not None:
        fail("%s still exists" % path)


def expect_symlink(cli, path, target):
    item = cli.entry(path)
    if item is None or not item.get("symlink"):
        fail("%s is not a symlink" % path)
    if item.get("target") != target or cli.cat(path).rstrip(b"\n") != target.encode():
        fail("%s target mismatch" % path)


def extend_payload(offset, size):
    return bytes((((offset + i) * 131 + 7) & 0xFF) for i in range(size - offset))


def expected_after_linux():
    create = b"OVERWRITEPEND" + extend_payload(13, 200000)
    create = create[:100000] + bytes(300000 - 100000)
    create = b"LINUX-OVERWRITE" + create[15:]
    unwritten = b"DATA" + bytes((1 << 20) - 4) + bytes(1 << 20)
    reuse = bytearray(95 * 2 * BLOCK + 1)
    for i in range(96):
        reuse[i * 2 * BLOCK] = ord("x")
    return create, unwritten, bytes(reuse)


def check_initial(cli):
    create, unwritten, reuse = expected_after_linux()
    expect_file(cli, "/qual/create.bin", create)
    expect_file(cli, "/qual/unwritten.bin", unwritten)
    expect_file(cli, "/qual/repl-linux", b"SOURCE")
    expect_absent(cli, "/qual/repl-dst")
    expect_absent(cli, "/qual/b/x.bin")
    expect_file(cli, "/qual/r2.bin", b"rename")
    expect_file(cli, "/qual/reuse-c.bin", reuse)
    expect_symlink(cli, "/qual/inline.link", "r2.bin")
    expect_symlink(cli, "/qual/remote.link", "../" + ("x/" * 100))
    expect_file(cli, "/linux-after/source.bin", b"LINUX-SOURCE")
    expect_symlink(cli, "/linux-after/inline.link", "source.bin")
    expect_symlink(cli, "/linux-after/remote.link", REMOTE_TARGET)
    grow = cli.listing("/linux-after/grow")
    names = {e["name"] for e in grow}
    if names != {"n%04d.bin" % i for i in range(400, 500)}:
        fail("Linux grow directory has %d entries" % len(names))
    listing = cli.listing("/linux-after")
    odds = [e["name"] for e in listing
            if e["name"].startswith("i") and e["name"].endswith(".bin")]
    if len(odds) != 1000 or "i0001.bin" not in odds or "i0000.bin" in odds:
        fail("Linux inode churn state mismatch (%d entries)" % len(odds))
    if cli.entry("/linux-after/frag.bin") is not None:
        fail("Linux fragmented file was not unlinked")


def check_final(cli):
    create, unwritten, reuse = expected_after_linux()
    expect_file(cli, "/qual/create.bin", create)
    expect_file(cli, "/qual/unwritten.bin", unwritten)
    expect_file(cli, "/qual/repl-linux", b"SOURCE")
    expect_file(cli, "/qual/reuse-c.bin", reuse)
    expect_symlink(cli, "/qual/inline.link", "r2.bin")
    expect_symlink(cli, "/qual/remote.link", "../" + ("x/" * 100))
    expect_file(cli, "/linux-after/source.bin", b"LINUX")
    expect_absent(cli, "/linux-after/hard.bin")
    expect_absent(cli, "/linux-after/grow")
    expect_absent(cli, "/linux-after/remote.link")
    expect_file(cli, "/win2/round2.bin", b"round2")
    expect_file(cli, "/win2/falloc.bin", b"W2" + bytes((1 << 20) - 2))
    bmap = cli.bmap("/win2/falloc.bin")
    written = set()
    for e in bmap["extents"]:
        if not e["unwritten"]:
            written.update(range(e["startoff"], e["startoff"] + e["blockcount"]))
    if not {0, 1, 2}.issubset(written):
        fail("round2 falloc conversion state mismatch")
    expect_file(cli, "/win2/frag.bin", b"R" + bytes(2 * BLOCK - 1))
    bmap = cli.bmap("/win2/frag.bin")
    if len(bmap["extents"]) != 1 or bmap["size"] != 2 * BLOCK:
        fail("round2 fragmented file extent state mismatch")
    expect_absent(cli, "/win2/frag-deleted.bin")
    expect_symlink(cli, "/win2/link", "round2.bin")
    expect_file(cli, "/win2/movedir-renamed/entry.bin", b"moved")
    expect_absent(cli, "/win2/movedir")
    listing = cli.listing("/win2/grow")
    if {e["name"] for e in listing} != {"n%04d.bin" % i
                                        for i in range(400, 500)}:
        fail("moved grow directory contents changed")
    if cli.entry("/win2/hard.bin") is not None:
        fail("round2 hardlink was not removed")


def expected_entries():
    create, unwritten, reuse = expected_after_linux()
    entries = [
        {"path": "/qual", "type": "dir"},
        {"path": "/qual/b", "type": "dir"},
        {"path": "/win2", "type": "dir"},
        {"path": "/win2/grow", "type": "dir"},
        {"path": "/win2/movedir-renamed", "type": "dir"},
        {"path": "/linux-after", "type": "dir"},
        {"path": "/qual/create.bin", "type": "file", "content": create},
        {"path": "/qual/unwritten.bin", "type": "file", "content": unwritten},
        {"path": "/qual/reuse-c.bin", "type": "file", "content": reuse},
        {"path": "/qual/r2.bin", "type": "file", "content": b"rename"},
        {"path": "/qual/repl-linux", "type": "file", "content": b"SOURCE"},
        {"path": "/linux-after/source.bin", "type": "file", "content": b"LINUX"},
        {"path": "/win2/round2.bin", "type": "file", "content": b"round2"},
        {"path": "/win2/falloc.bin", "type": "file",
         "content": b"W2" + bytes((1 << 20) - 2)},
        {"path": "/win2/frag.bin", "type": "file",
         "content": b"R" + bytes(2 * BLOCK - 1)},
        {"path": "/win2/movedir-renamed/entry.bin", "type": "file",
         "content": b"moved"},
        {"path": "/qual/inline.link", "type": "symlink", "target": "r2.bin"},
        {"path": "/qual/remote.link", "type": "symlink",
         "target": "../" + ("x/" * 100)},
        {"path": "/win2/link", "type": "symlink", "target": "round2.bin"},
    ]
    for i in range(400, 500):
        entries.append({"path": "/win2/grow/%04d.bin" % i, "type": "file",
                        "content": b"n%04d" % i})
    for i in range(1, 2000, 2):
        entries.append({"path": "/linux-after/i%04d.bin" % i,
                        "type": "file", "content": b"i"})
    out = []
    for item in entries:
        row = {"path": item["path"], "type": item["type"]}
        if item["type"] == "file":
            data = item["content"]
            row.update({"size": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "nlink": 1})
        elif item["type"] == "symlink":
            row["target"] = item["target"]
        out.append(row)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--stage", choices=("initial", "final"), required=True)
    parser.add_argument("--expected-out")
    args = parser.parse_args()
    cli = Cli(args.exe, args.image)
    try:
        if args.stage == "initial":
            check_initial(cli)
        else:
            check_final(cli)
            if args.expected_out:
                with open(args.expected_out, "w", encoding="utf-8") as f:
                    json.dump({"entries": expected_entries()}, f, indent=2,
                              sort_keys=True)
                    f.write("\n")
    except CheckError as error:
        print("RW-6B round2 state check failed at %s: %s" %
              (args.stage, error), file=sys.stderr)
        return 1
    print("RW-6B round2 state check OK: %s" % args.stage)
    return 0


if __name__ == "__main__":
    sys.exit(main())
