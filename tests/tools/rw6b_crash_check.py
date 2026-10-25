#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Semantic old-or-new check for the RW-6B writer crash matrix.

usage: rw6b_crash_check.py --exe xfswin.exe --image img --mode MODE

The image has already been recovered by xfswin.  Every scenario must expose
either the complete state before the operation or the complete state after it.
A partially applied inode size, extent list, directory entry or unwritten
conversion is a failure.  Rmap ownership itself is checked independently by
rw6b_rmap_probe.py and, on Linux, xfs_repair -n.
"""

import argparse
import json
import subprocess
import sys

BLOCK = 4096
FRAG_SIZE = 128 * 2 * BLOCK - BLOCK
FRAG_OLD_EXTENTS = 128
REMOTE_TARGET = "../" + ("y/" * 120)

class CheckError(AssertionError):
    pass


class Cli:
    def __init__(self, exe, image):
        self.exe = exe
        self.image = image

    def run(self, *args, binary=False):
        proc = subprocess.run([self.exe, *args], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise CheckError("xfswin %s failed (%d): %s" %
                             (" ".join(args), proc.returncode,
                              proc.stderr.decode("utf-8", "replace").strip()))
        if binary:
            return proc.stdout
        return proc.stdout.decode("utf-8", "replace")

    def ls(self, path):
        return json.loads(self.run("ls", self.image, path, "--json"))

    def entry(self, path, name):
        for item in self.ls(path):
            if item.get("name") == name:
                return item
        return None

    def stat(self, path):
        text = self.run("stat", self.image, path)
        out = {}
        for line in text.splitlines():
            if ":" not in line:
                continue
            key, value = line.split(":", 1)
            out[key.strip()] = value.strip()
        result = {}
        for key in ("size", "links"):
            if key in out:
                result[key] = int(out[key].split()[0])
        if "extents" in out:
            result["extents"] = int(out["extents"].split()[0])
        return result

    def bmap(self, path):
        return json.loads(self.run("bmap", self.image, path, "--json"))

    def cat(self, path):
        return self.run("cat", self.image, path, binary=True)


def require(condition, message):
    if not condition:
        raise CheckError(message)


def check_create(cli, base):
    item = cli.entry("/dirB", "new.bin")
    if item is not None:
        require(item.get("dir") is False, "new.bin is not a regular file")
        require(item.get("size") == 0, "partial new.bin size")


def check_write(cli, base):
    st = cli.stat("/rewrite.bin")
    require(st["size"] in (8192, 8197),
            "rewrite.bin has partial size %d" % st["size"])
    data = cli.cat("/rewrite.bin")
    if st["size"] == 8192:
        require(data == b"R" * 8192, "old rewrite.bin content changed")
    else:
        require(data[:8192] == b"R" * 8192,
                "new rewrite.bin lost the old content")
        require(data[8192:] == b"WRITE", "new rewrite.bin tail is wrong")


def check_truncate(cli, base):
    frag_size = FRAG_SIZE
    frag_extents = FRAG_OLD_EXTENTS
    if base is not None:
        frag_size = base.stat("/frag.bin")["size"]
        frag_extents = base.stat("/frag.bin")["extents"]
    st = cli.stat("/frag.bin")
    require(st["size"] in (4096, frag_size),
            "frag.bin has partial size %d" % st["size"])
    if st["size"] == 4096:
        require(st["extents"] == 1,
                "truncated frag.bin has %d extents" % st["extents"])
        require(cli.cat("/frag.bin") == b"F0000000" * (BLOCK // 8),
                "truncated frag.bin content is wrong")
    else:
        require(st["extents"] <= frag_extents,
                "frag.bin grew to %d extents (base %d)" %
                (st["extents"], frag_extents))


def check_unlink(cli, base):
    item = cli.entry("/", "frag.bin")
    if item is None:
        return
    frag_size = FRAG_SIZE
    if base is not None:
        frag_size = base.stat("/frag.bin")["size"]
    require(item.get("dir") is False, "frag.bin is not a regular file")
    require(item.get("size") == frag_size,
            "surviving frag.bin has partial size %d" % item.get("size"))


def check_fallocate(cli, base):
    st = cli.stat("/falloc.bin")
    require(st["size"] in (0, 1048576),
            "falloc.bin has partial size %d" % st["size"])
    extents = cli.bmap("/falloc.bin")["extents"]
    covered = sum(e["blockcount"] for e in extents)
    if st["size"] == 0:
        require(covered == 0, "falloc.bin has extents but size zero")
    else:
        require(covered == 256,
                "falloc.bin covers %d blocks, expected 256" % covered)
        require(all(e["unwritten"] for e in extents),
                "falloc.bin contains written extents")


def check_convert(cli, base):
    st = cli.stat("/unwritten.bin")
    require(st["size"] == 1048576, "unwritten.bin size changed")
    written = set()
    for extent in cli.bmap("/unwritten.bin")["extents"]:
        if not extent["unwritten"]:
            start = extent["startoff"]
            written.update(range(start, start + extent["blockcount"]))
    # Blocks 1 and 2 are the conversion range; block 0 and 3..255 must stay
    # unwritten.  Both outcomes are legal, a partial conversion is not.
    require((1 in written) == (2 in written),
            "convert-unwritten applied only half the range")
    require(0 not in written, "convert-unwritten changed block 0")
    require(not any(block in written for block in range(3, 256)),
            "convert-unwritten changed blocks outside the range")


def check_mkdir(cli, base):
    item = cli.entry("/", "newdir")
    if item is not None:
        require(item.get("dir") is True, "newdir is not a directory")


def check_rmdir(cli, base):
    item = cli.entry("/", "emptydir")
    if item is not None:
        require(item.get("dir") is True, "emptydir is not a directory")
        require(item.get("size") == 0, "emptydir is not empty")


def check_symlink(cli, base):
    item = cli.entry("/dirB", "sl")
    if item is not None:
        require(item.get("symlink") is True, "sl is not a symlink")
        require(item.get("target") == REMOTE_TARGET,
                "sl target is wrong: %r" % item.get("target"))


CHECKS = {
    "create": check_create,
    "write": check_write,
    "truncate": check_truncate,
    "unlink": check_unlink,
    "fallocate": check_fallocate,
    "convert": check_convert,
    "mkdir": check_mkdir,
    "rmdir": check_rmdir,
    "symlink": check_symlink,
}


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--mode", required=True, choices=sorted(CHECKS))
    parser.add_argument("--base", help="pristine crash fixture image")
    args = parser.parse_args(argv[1:])
    cli = Cli(args.exe, args.image)
    base = Cli(args.exe, args.base) if args.base else None
    CHECKS[args.mode](cli, base)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except CheckError as error:
        print("rw6b_crash_check: " + str(error), file=sys.stderr)
        sys.exit(1)
