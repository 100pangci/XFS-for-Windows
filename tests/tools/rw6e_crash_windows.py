#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6E namespace crash matrix.

Each namespace operation is killed at every transaction write phase.  Windows
recovery must finish with a clean log and `xfswin check` (including the
bidirectional parent audit) must pass; both the old and the new namespace are
therefore legal, and a mixed dirent/parent state is not.

usage:
  rw6e_crash_windows.py --exe xfswin.exe --base base.img --work-dir out/crash
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

CRASH_POINTS = [
    "before_log_write",
    "mid_log_record",
    "after_log_commit",
    "before_metadata_write",
    "mid_metadata_write",
    "after_metadata_write",
    "before_device_flush",
    "after_device_flush",
]
CRASH_EXIT = 0x58435348


def run(args, env=None):
    return subprocess.run(args, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args(argv)
    exe = os.path.abspath(args.exe)
    base = os.path.abspath(args.base)
    os.makedirs(args.work_dir, exist_ok=True)

    operations = [
        ("create", ["create", "{img}", "/new", "--rw-experimental"]),
        ("mkdir", ["mkdir", "{img}", "/newdir", "--rw-experimental"]),
        ("link", ["link", "{img}", "/d2/moved2", "/d1/linknew",
                  "--rw-experimental"]),
        ("unlink", ["unlink", "{img}", "/d1/x", "--rw-experimental"]),
        ("rename", ["rename", "{img}", "/d1/x", "/d2/x2",
                    "--rw-experimental"]),
        ("replace", ["rename", "{img}", "/d1/x", "/d2/victim", "--replace",
                     "--rw-experimental"]),
        ("directory_move", ["rename", "{img}", "/d1/sub", "/d2/sub2",
                            "--rw-experimental"]),
        ("rmdir", ["rmdir", "{img}", "/d1/empty", "--rw-experimental"]),
    ]
    results = []
    for label, template in operations:
        for point in CRASH_POINTS:
            image = os.path.join(args.work_dir,
                                 "%s-%s.img" % (label, point))
            shutil.copy2(base, image)
            env = dict(os.environ)
            env["XFSWIN_CRASH_AT"] = point
            argv_op = [exe] + [a.format(img=image) for a in template]
            proc = run(argv_op, env)
            if proc.returncode != CRASH_EXIT:
                print("  %s/%s: no crash (%d): %s" %
                      (label, point, proc.returncode,
                       proc.stderr.decode("utf-8", "replace")[:200]),
                      file=sys.stderr)
                return 1
            env.pop("XFSWIN_CRASH_AT", None)
            recovered = run([exe, "recover", image,
                             "--yes-i-really-mean-it"], env)
            if recovered.returncode != 0:
                print("  %s/%s: recovery failed: %s" %
                      (label, point,
                       recovered.stderr.decode("utf-8", "replace")),
                      file=sys.stderr)
                return 1
            checked = run([exe, "check", image, "--parents"], env)
            if checked.returncode != 0:
                print("  %s/%s: check failed:\n%s" %
                      (label, point,
                       checked.stdout.decode("utf-8", "replace")),
                      file=sys.stderr)
                return 1
            results.append({"operation": label, "crash_point": point,
                            "image": os.path.basename(image)})
            os.unlink(image)
    summary = {"kind": "rw6e-namespace-crash", "result": "ok",
               "cases": results}
    with open(os.path.join(args.work_dir, "crash.summary.json"), "w",
              encoding="utf-8") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write("\n")
    print("rw6e crash matrix: %d/%d cases recovered and audited" %
          (len(results), len(operations) * len(CRASH_POINTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
