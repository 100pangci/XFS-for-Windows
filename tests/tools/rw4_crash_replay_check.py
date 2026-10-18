#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Verifies a replayed RW-4 crash image through the mounted filesystem.

Every crash scenario has exactly two legal visible states: the old namespace
and the new one. After the Linux kernel replayed the Windows journal, the
mounted state must match one of them - never a mix (one name without the
other, a dangling dirent, a wrong link count, a missing ".." update).

usage:
  rw4_crash_replay_check.py <mount-point> <manifest.json> <case-file>
"""

import json
import os
import stat
import sys


def lstat_ok(path):
    try:
        return os.lstat(path)
    except OSError:
        return None


def check(scenario, params, mount, failures):
    def path(rel):
        return os.path.join(mount, rel.lstrip("/"))

    def error(message):
        failures.append(f"{scenario}: {message}")

    if scenario == "renamesame":
        old = lstat_ok(path("/a.bin"))
        new = lstat_ok(path("/b.bin"))
        if old is not None and new is None:
            if old.st_size != params["size"] or not stat.S_ISREG(old.st_mode):
                error("/a.bin is not the original regular file")
        elif old is None and new is not None:
            if new.st_size != params["size"] or not stat.S_ISREG(new.st_mode):
                error("/b.bin is not the complete new file")
        else:
            error(f"neither the old state (a only: {old is not None}) nor the "
                  f"new state (b only: {new is not None})")
    elif scenario == "renamecross":
        old = lstat_ok(path("/A/f"))
        new = lstat_ok(path("/B/f"))
        if old is not None and new is None:
            pass
        elif old is None and new is not None:
            if new.st_size != params["size"]:
                error("/B/f has the wrong size")
        else:
            error("neither A/f nor B/f is the single legal owner")
    elif scenario == "renamereplace":
        src = lstat_ok(path("/src"))
        dst = lstat_ok(path("/dst"))
        if src is not None and dst is not None:
            if src.st_size != params["src_size"] or \
                    dst.st_size != params["dst_size"]:
                error("old state but the sizes differ")
        elif src is None and dst is not None:
            if dst.st_size != params["src_size"]:
                error("/dst does not serve the moved inode's size")
        else:
            error(f"neither the old (src+dst) nor the new (dst only) state "
                  f"(src={src is not None}, dst={dst is not None})")
    elif scenario == "dirmove":
        old = lstat_ok(path("/A/child"))
        new = lstat_ok(path("/B/child"))
        if old is not None and new is None:
            a = lstat_ok(path("/A"))
            if a is None or a.st_nlink != 3:
                error(f"A nlink {a.st_nlink if a else '?'} != 3 in the old "
                      f"state")
        elif old is None and new is not None:
            if not stat.S_ISDIR(new.st_mode):
                error("/B/child is not a directory")
            b = lstat_ok(path("/B"))
            a = lstat_ok(path("/A"))
            if b is None or b.st_nlink != 3:
                error(f"B nlink {b.st_nlink if b else '?'} != 3 in the new "
                      f"state")
            if a is None or a.st_nlink != 2:
                error(f"A nlink {a.st_nlink if a else '?'} != 2 in the new "
                      f"state")
        else:
            error("neither A/child nor B/child is the single legal owner")
    elif scenario == "dirreplace":
        s = lstat_ok(path("/s"))
        t = lstat_ok(path("/t"))
        root = lstat_ok(mount)
        if s is not None and t is not None:
            if root.st_nlink != 4:
                error(f"root nlink {root.st_nlink} != 4 in the old state")
        elif s is None and t is not None:
            if not stat.S_ISDIR(t.st_mode):
                error("/t is not a directory")
            if root.st_nlink != 3:
                error(f"root nlink {root.st_nlink} != 3 in the new state")
        else:
            error(f"neither the old (s+t) nor the new (t only) state "
                  f"(s={s is not None}, t={t is not None})")
    elif scenario == "link":
        src = lstat_ok(path("/file"))
        dst = lstat_ok(path("/linked"))
        if src is not None and dst is None:
            if src.st_nlink != 1:
                error(f"old state but nlink {src.st_nlink} != 1")
        elif src is not None and dst is not None:
            if src.st_ino != dst.st_ino:
                error("file and linked do not share an inode")
            if src.st_nlink != 2:
                error(f"new state but nlink {src.st_nlink} != 2")
        else:
            error(f"neither the old (file only) nor the new (file+linked) "
                  f"state (file={src is not None}, linked={dst is not None})")
    elif scenario in ("symlink", "symlinkremote"):
        link = lstat_ok(path("/sl") if scenario == "symlink" else path("/slr"))
        name = "/sl" if scenario == "symlink" else "/slr"
        if link is None:
            return  # old state: the symlink was never created
        if not stat.S_ISLNK(link.st_mode):
            error(f"{name} is not a symlink")
            return
        target = os.readlink(path(name))
        if target != params["target"]:
            error(f"{name} target {target!r} != {params['target']!r}")
        if link.st_nlink != 1:
            error(f"{name} nlink {link.st_nlink} != 1")
        if link.st_size != len(params["target"]):
            error(f"{name} size {link.st_size} != {len(params['target'])}")
    else:
        error(f"unknown scenario {scenario}")


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
    failures = []
    check(case["scenario"], case.get("params", {}), mount, failures)
    if failures:
        for failure in failures:
            print(f"FAILED: {case_file}: {failure}", file=sys.stderr)
        return 1
    print(f"{case_file}: {case['scenario']} -> old or new state (legal)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
