#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Shared helpers for the RW-6D xattr round-trip tools.

The tools deliberately drive the public CLI (`xfswin xattr ...`) and the
independent Python oracle (`rw6d_xattr_probe.py`).  Nothing here imports or
links the C++ implementation.
"""

import hashlib
import json
import os
import struct
import subprocess
import sys


def pattern(size, seed):
    return bytes(((i * 37 + seed * 17 + 11) & 0xFF) for i in range(size))


# The deep multi-level attribute tree uses 240-byte on-disk names.  Fifteen
# such entries fit one 4 KiB attribute leaf, so 9000 entries need more than
# the 504 children a 4 KiB DA node can hold (root level 2).
DEEP_ATTRS = 9000


def deep_name(index):
    return "user.d%04d%s" % (index, "x" * 235)


def deep_value(index):
    return struct.pack(">Q", index)



def sha256(data):
    return hashlib.sha256(data).hexdigest()


def run(exe, args, env=None, binary=False, check=True):
    proc = subprocess.run([exe] + list(args), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env)
    text = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    if check and proc.returncode != 0:
        raise SystemExit("xfswin %s failed (exit %d):\n%s" %
                         (" ".join(args), proc.returncode, text))
    if binary:
        return proc.returncode, proc.stdout, proc.stderr
    return proc.returncode, text


def must(exe, args, env=None):
    return run(exe, args, env=env, check=True)[1]


class Cli:
    def __init__(self, exe, image, work_dir, options=None):
        self.exe = os.path.abspath(exe)
        self.image = os.path.abspath(image)
        self.work_dir = os.path.abspath(work_dir)
        self.options = list(options or ["--rw-experimental"])
        os.makedirs(self.work_dir, exist_ok=True)

    def check(self):
        return must(self.exe, ["check", self.image])

    def list_raw(self, path):
        return must(self.exe, ["xattr", "list", self.image, path,
                               "--json"])

    def list(self, path):
        return json.loads(self.list_raw(path))

    def get(self, path, name):
        out = os.path.join(self.work_dir, "xattr-out.bin")
        if os.path.exists(out):
            os.unlink(out)
        must(self.exe, ["xattr", "get", self.image, path, name,
                        "--output=" + out])
        with open(out, "rb") as f:
            return f.read()

    def set(self, path, name, value, mode=None):
        source = os.path.join(self.work_dir, "xattr-in.bin")
        with open(source, "wb") as f:
            f.write(value)
        args = ["xattr", "set", self.image, path, name,
                "--value-file=" + source] + self.options
        if mode == "create":
            args.append("--create")
        elif mode == "replace":
            args.append("--replace")
        return must(self.exe, args)

    def remove(self, path, name):
        return must(self.exe, ["xattr", "remove", self.image, path, name] +
                    self.options)

    def cat(self, path):
        rc, out, err = run(self.exe, ["cat", self.image, path], binary=True,
                           check=False)
        if rc != 0:
            raise SystemExit("cat %s failed:\n%s" %
                             (path, err.decode("utf-8", "replace")))
        return out

    def expect_missing(self, path):
        rc, _, _ = run(self.exe, ["cat", self.image, path], binary=True,
                       check=False)
        if rc == 0:
            raise SystemExit("%s is still readable" % path)

    def stat_inode(self, path):
        import re
        text = must(self.exe, ["stat", self.image, path])
        match = re.search(r"inode\s*:\s*(\d+)", text)
        if not match:
            raise SystemExit("cannot parse inode for %s:\n%s" % (path, text))
        return int(match.group(1))

    def create(self, path):
        return must(self.exe, ["create", self.image, path] + self.options)

    def mkdir(self, path):
        return must(self.exe, ["mkdir", self.image, path] + self.options)

    def write(self, path, data, offset=0):
        source = os.path.join(self.work_dir, "data.bin")
        with open(source, "wb") as f:
            f.write(data)
        return must(self.exe, ["pwrite", self.image, path, str(offset),
                               "--from=" + source] + self.options)

    def truncate(self, path, size):
        return must(self.exe, ["truncate", self.image, path, str(size)] +
                    self.options)

    def fallocate(self, path, offset, length):
        return must(self.exe, ["fallocate", self.image, path, str(offset),
                               str(length)] + self.options)

    def rename(self, src, dst, replace=False):
        args = ["rename", self.image, src, dst] + self.options
        if replace:
            args.append("--replace")
        return must(self.exe, args)

    def link(self, existing, new):
        return must(self.exe, ["link", self.image, existing, new] +
                    self.options)

    def unlink(self, path):
        return must(self.exe, ["unlink", self.image, path] + self.options)

    def reflink(self, src, dst):
        return must(self.exe, ["reflink", self.image, src, dst, "0", "0",
                               "0"] + self.options)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def probe_path():
    return os.path.join(repo_root(), "tools", "rw6d_xattr_probe.py")


def run_probe(image, inodes, manifest=None, json_out=None, python=None,
              quiet=True):
    python = python or sys.executable
    out = json_out or (os.path.abspath(image) + ".oracle.json")
    cmd = [python, probe_path(), os.path.abspath(image),
           "--json", out]
    if quiet:
        cmd.append("--quiet")
    if manifest:
        cmd.extend(["--manifest", os.path.abspath(manifest)])
    for ino in inodes:
        cmd.extend(["--inode", str(ino)])
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise SystemExit((proc.stdout + proc.stderr).decode(
            "utf-8", "replace"))
    with open(out, "r", encoding="utf-8") as f:
        return json.load(f)


def manifest_inodes(manifest):
    return [int(item["ino"]) for item in manifest.get("inodes", [])]


def attribute_map(item):
    return {attr["name"]: attr for attr in item.get("attributes", [])}


def set_manifest_attribute(manifest, path, name, value, remote=None):
    item = next((i for i in manifest.get("inodes", [])
                 if i["path"] == path), None)
    if item is None:
        raise SystemExit("manifest has no path %s" % path)
    attrs = attribute_map(item)
    attrs[name] = {
        "name": name,
        "length": len(value),
        "sha256": sha256(value),
    }
    if remote is not None:
        attrs[name]["remote"] = bool(remote)
    item["attributes"] = sorted(attrs.values(), key=lambda a: a["name"])


def remove_manifest_attribute(manifest, path, name):
    item = next((i for i in manifest.get("inodes", [])
                 if i["path"] == path), None)
    if item is None:
        raise SystemExit("manifest has no path %s" % path)
    item["attributes"] = [a for a in item.get("attributes", [])
                          if a["name"] != name]


def set_manifest_file(manifest, path, data):
    manifest.setdefault("files", {})[path.lstrip("/")] = {
        "size": len(data),
        "sha256": sha256(data),
    }


def build_manifest_from_probe(result, paths, files=None, kind="rw6d"):
    """Builds a comparison manifest from the independent oracle's output.

    `paths` maps inode number -> path (or None to keep the inode without a
    path).  The oracle has already verified every value against the previous
    manifest, so this is the new state after a mutation.
    """
    inodes = []
    for item in result["inodes"]:
        ino = int(item["inode"])
        entry = {
            "ino": ino,
            "format": item["format"],
            "root_level": item["root_level"],
            "aformat": item["aformat"],
            "attributes": [
                {
                    "name": attr["name"],
                    "length": attr["length"],
                    "sha256": attr["sha256"],
                    "remote": attr.get("remote", False),
                }
                for attr in item["attributes"]
            ],
        }
        if paths.get(ino):
            entry["path"] = paths[ino]
        inodes.append(entry)
    manifest = {"kind": kind, "inodes": inodes}
    if files:
        manifest["files"] = files
    return manifest


def write_manifest(path, manifest):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def require(actual, expected, what):
    if actual == expected:
        return
    if len(actual) != len(expected):
        raise SystemExit("%s: size %d != %d" %
                         (what, len(actual), len(expected)))
    for i, (a, b) in enumerate(zip(actual, expected)):
        if a != b:
            raise SystemExit("%s: byte %d is 0x%02x, expected 0x%02x" %
                             (what, i, a, b))


def require_attrs(cli, path, expected):
    """Checks every {name: value} entry through the public get path."""
    for name, value in expected.items():
        actual = cli.get(path, name)
        require(actual, value, "%s %s" % (path, name))
