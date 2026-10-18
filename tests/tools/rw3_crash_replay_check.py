#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Verifies a replayed RW-3 crash image through the mounted filesystem.

After the Linux kernel replayed the log we left behind, the visible state must
be exactly one of the two allowed ones:

  create  the name is absent (old) or a regular file of size 0 (new)
  mkdir   the name is absent (old) or a directory (new)
  unlink  the name is absent (new) or the original regular file with its
          original size and content (old)
  rmdir   the name is absent (new) or an empty directory (old)

An unlinked inode must never be reachable and a created name must never point
at a half-initialised inode; xfs_repair -n and the structural probe cover the
rest of the filesystem.

usage:
  rw3_crash_replay_check.py <mount-point> <manifest.json> <case-file>
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rw3_content  # noqa: E402


def main(argv):
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 1
    mount, manifest_path, case_file = argv[1:]
    with open(manifest_path, "r", encoding="utf-8-sig") as f:
        manifest = json.load(f)
    cases = {case["file"]: case for case in manifest.get("cases", [])}
    if case_file not in cases:
        print(f"FAILED: no manifest entry for {case_file}", file=sys.stderr)
        return 1
    case = cases[case_file]
    mode = case["mode"]
    rel = case["name"].lstrip("/")
    path = os.path.join(mount, rel)
    old_size = case.get("old_size")
    seed = case.get("seed")

    if not os.path.lexists(path):
        print(f"{case_file}: {mode} -> absent (allowed)")
        return 0
    st = os.lstat(path)
    import stat as statmod

    if mode in ("create", "unlink"):
        if not statmod.S_ISREG(st.st_mode):
            print(f"FAILED: {case_file}: {rel} exists but is not a regular "
                  f"file", file=sys.stderr)
            return 1
        if mode == "create":
            if st.st_size != 0:
                print(f"FAILED: {case_file}: created file has size "
                      f"{st.st_size}", file=sys.stderr)
                return 1
        else:
            if old_size is not None and st.st_size != old_size:
                print(f"FAILED: {case_file}: {rel} has size {st.st_size}, "
                      f"neither removed nor the old {old_size}", file=sys.stderr)
                return 1
            if seed is not None:
                with open(path, "rb") as f:
                    data = f.read()
                if data != rw3_content.payload(st.st_size, seed):
                    print(f"FAILED: {case_file}: {rel} content changed",
                          file=sys.stderr)
                    return 1
    else:
        if not statmod.S_ISDIR(st.st_mode):
            print(f"FAILED: {case_file}: {rel} exists but is not a directory",
                  file=sys.stderr)
            return 1
        if mode == "rmdir":
            if os.listdir(path):
                print(f"FAILED: {case_file}: {rel} is not empty",
                      file=sys.stderr)
                return 1
    print(f"{case_file}: {mode} -> present ({'dir' if os.path.isdir(path) else st.st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
