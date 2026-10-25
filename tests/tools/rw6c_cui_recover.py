#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6C Windows completion of a genuine Linux CUI/CUD log.

usage:
  rw6c_cui_recover.py --exe xfswin.exe --image cui.img
      --kernel-oracle kernel.oracle.json [--json report.json]
  rw6c_cui_recover.py --exe xfswin.exe --image cui.img
      --kernel-oracle kernel.oracle.json --crash-dir out/ [--zip]

Without --crash-dir this completes the snapshot once.  With --crash-dir it
crashes recovery at every recovery/completion injection point, restarts it and
requires convergence.  Every resulting image is checked by the independent
oracle and compared with the kernel-recovered semantic oracle.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile

CRASH_EXIT = 0x58435348


def run(exe, args, env=None):
    proc = subprocess.run([exe] + args, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env)
    text = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    return proc.returncode, text


def journal(exe, image):
    rc, text = run(exe, ["journal", image, "--json"])
    if rc != 0:
        raise SystemExit("journal scan failed (exit %d):\n%s" % (rc, text))
    return json.loads(text)


def all_active_intents(scan):
    active = {}
    pairs = {
        "XFS_LI_CUI": ("XFS_LI_CUD", "CUI"),
        "XFS_LI_BUI": ("XFS_LI_BUD", "BUI"),
    }
    done = {done_type: (start_type, label)
            for start_type, (done_type, label) in pairs.items()}
    for trans in scan.get("transactions", []):
        for item in trans.get("items", []):
            typ = item.get("type")
            if typ not in pairs and typ not in done:
                continue
            ident = item.get("id")
            if ident is None:
                raise SystemExit("%s journal item has no id" % typ)
            if typ in pairs:
                active[(typ, ident)] = item
            else:
                start_type = done[typ][0]
                active.pop((start_type, ident), None)
    return active


def active_cuis(scan):
    return {(typ, ident): item
            for (typ, ident), item in all_active_intents(scan).items()
            if typ == "XFS_LI_CUI"}


def oracle(python, script, image):
    out = image + ".oracle.json"
    proc = subprocess.run([python, script, image, "--json", out],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        text = (proc.stdout + proc.stderr).decode("utf-8", "replace")
        raise SystemExit("refcount oracle failed:\n" + text)
    return json.load(open(out)), out


def compare_oracle(actual, expected, label):
    keys = ("shared_records", "shared_blocks", "cow_records", "fdblocks")
    bad = []
    for key in keys:
        if actual.get(key) != expected.get(key):
            bad.append("%s: windows=%r kernel=%r" %
                       (key, actual.get(key), expected.get(key)))
    if bad:
        raise SystemExit("%s: recovery does not match the kernel oracle:\n%s"
                         % (label, "\n".join(bad)))


def validate(exe, python, probe, image, kernel, label):
    scan = journal(exe, image)
    if scan.get("state") != "clean":
        raise SystemExit("%s: log is not clean (%s)" %
                         (label, scan.get("state")))
    if active_cuis(scan):
        raise SystemExit("%s: a CUI intent survived recovery" % label)
    rc, text = run(exe, ["check", image])
    if rc != 0:
        raise SystemExit("%s: check failed:\n%s" % (label, text))
    actual, oracle_path = oracle(python, probe, image)
    compare_oracle(actual, kernel, label)
    return oracle_path


def recover_once(exe, image, env=None):
    return run(exe, ["recover", image, "--yes-i-really-mean-it"], env=env)


def crash_points(scan):
    cuis = active_cuis(scan)
    adjust_extents = 0
    for item in cuis.values():
        for extent in item.get("extents") or []:
            if int(extent.get("op", 0)) in (1, 2):
                adjust_extents += 1
    intents = all_active_intents(scan)
    points = [
        "recovery_scan:1",
        "recovery_before_pass2:1",
        "recovery_after_transaction:1",
        "recovery_mid_buffer:1",
        "recovery_after_metadata_flush:1",
        "recovery_before_intents:1",
        "recovery_before_iunlinks:1",
        "recovery_mid_iunlinks:1",
        "recovery_before_log_clean:1",
        "recovery_after_log_clean:1",
    ]
    for hit in range(1, min(3, len(intents)) + 1):
        points.append("recovery_mid_intents:%d" % hit)
        points.append("before_intent_done:%d" % hit)
        points.append("after_intent_done:%d" % hit)
    for hit in range(1, min(3, adjust_extents) + 1):
        points.append("after_refcount_update:%d" % hit)
    return points


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--kernel-oracle", required=True)
    ap.add_argument("--crash-dir")
    ap.add_argument("--zip", action="store_true")
    ap.add_argument("--json")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    image = os.path.abspath(args.image)
    kernel = json.load(open(args.kernel_oracle, encoding="utf-8"))
    if kernel.get("result") != "ok":
        raise SystemExit("kernel oracle image was not accepted")
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = os.path.join(repo, "tools", "rw6c_refcount_probe.py")
    python = sys.executable

    before = journal(exe, image)
    cuis = active_cuis(before)
    if not cuis:
        raise SystemExit("snapshot has no surviving CUI")
    total_extents = sum(int(item.get("nextents") or 0)
                        for item in cuis.values())
    summary = {
        "image": image,
        "surviving_cuis": len(cuis),
        "surviving_extents": total_extents,
        "runs": [],
    }

    if not args.crash_dir:
        rc, text = recover_once(exe, image)
        if rc != 0:
            raise SystemExit("CUI recovery failed (exit %d):\n%s" %
                             (rc, text))
        if "intents completed" not in text:
            print(text)
            raise SystemExit("recovery did not report intent completion")
        validate(exe, python, probe, image, kernel, "complete")
        print("RW-6C CUI recovery OK: %s (%d intents, %d extents)" %
              (image, len(cuis), total_extents))
        if args.json:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(summary, f, indent=2, sort_keys=True)
        return 0

    os.makedirs(args.crash_dir, exist_ok=True)
    failures = []
    crashed = 0
    for point in crash_points(before):
        name = point.replace(":", "-")
        target = os.path.join(args.crash_dir, "cui-%s.img" % name)
        shutil.copyfile(image, target)
        env = dict(os.environ)
        env["XFSWIN_CRASH_AT"] = point
        first_rc, first_text = recover_once(exe, target, env=env)
        did_crash = first_rc == CRASH_EXIT
        if did_crash:
            crashed += 1
            marker = "CRASH INJECTION at %s" % point.split(":")[0]
            if marker not in first_text:
                failures.append("%s: crash marker missing" % point)
                continue
        elif first_rc != 0:
            failures.append("%s: initial recovery failed:\n%s" %
                            (point, first_text))
            continue
        else:
            failures.append("%s: injection point was not reached" % point)
            continue

        second_rc, second_text = recover_once(exe, target)
        if second_rc != 0:
            failures.append("%s: restarted recovery failed:\n%s" %
                            (point, second_text))
            continue
        try:
            oracle_path = validate(exe, python, probe, target, kernel, point)
        except SystemExit as exc:
            failures.append("%s: %s" % (point, exc))
            continue
        summary["runs"].append({"point": point, "oracle": oracle_path})
        archive = target + ".zip"
        if args.zip:
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
                z.write(target, os.path.basename(target))
            os.unlink(target)
        else:
            os.unlink(target)
        print("ok: genuine CUI recovery crash at %s" % point)

    summary.update({"crashes": crashed, "failures": failures,
                    "result": "ok" if not failures else "failed"})
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    if crashed == 0:
        print("FAIL: no recovery crash point was reached", file=sys.stderr)
        return 1
    print("RW-6C CUI crash matrix OK: %d crashes, %d images" %
          (crashed, len(summary["runs"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
