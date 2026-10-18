#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Structural differential: `xfs_logprint -t` vs `xfswin journal --json`.

The text does not need to match (xfs_logprint prints details this project does
not); the *structure* does. This compares what both tools know:

  * clean/dirty classification and the head/tail blocks
    (`    log tail: T head: H state: <CLEAN|DIRTY>`),
  * the total number of log operations (`Oper (n): ...` lines against the sum
    of h_num_logops),
  * the committed transaction ids (`TRANS: tid:0x...` lines),
  * the START/COMMIT flag counts.

usage: compare_logprint.py <xfswin-journal.json> <xfs_logprint-t.txt>
"""

import json
import re
import sys

TAIL_HEAD = re.compile(r"log tail:\s*(\d+)\s+head:\s*(\d+)\s+state:\s*<(\w+)>")
OPER = re.compile(r"^Oper \((\d+)\): tid: ([0-9a-fA-F]+)\s+len: (\d+)\s+"
                  r"clientid: (\S+)\s+flags: (.*)$")
TRANS = re.compile(r"^TRANS: tid:0x([0-9a-fA-F]+)\s+#items:(\d+)")


def parse_logprint(path):
    data = {
        "tail": None, "head": None, "state": None,
        "oper_count": 0, "tids": set(), "starts": 0, "commits": 0,
    }
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            match = TAIL_HEAD.search(line)
            if match:
                data["tail"] = int(match.group(1))
                data["head"] = int(match.group(2))
                data["state"] = match.group(3)
                continue
            match = OPER.match(line)
            if match:
                data["oper_count"] += 1
                data["tids"].add(int(match.group(2), 16))
                flags = match.group(5)
                if "START" in flags:
                    data["starts"] += 1
                if "COMMIT" in flags:
                    data["commits"] += 1
                continue
            match = TRANS.match(line)
            if match:
                data["tids"].add(int(match.group(1), 16))
    return data


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    with open(argv[1], "r") as handle:
        journal = json.load(handle)
    logprint = parse_logprint(argv[2])

    problems = []

    expected_state = "DIRTY" if journal["state"] == "dirty" else "CLEAN"
    if logprint["state"] is not None and logprint["state"] != expected_state:
        problems.append("state: xfs_logprint=%s xfswin=%s" %
                        (logprint["state"], journal["state"]))
    if logprint["tail"] is not None and logprint["tail"] != journal["tail"]["block"]:
        problems.append("tail block: xfs_logprint=%d xfswin=%d" %
                        (logprint["tail"], journal["tail"]["block"]))
    if logprint["head"] is not None and logprint["head"] != journal["head"]["block"]:
        problems.append("head block: xfs_logprint=%d xfswin=%d" %
                        (logprint["head"], journal["head"]["block"]))

    record_ops = sum(record["ops"] for record in journal["records"])
    if logprint["oper_count"] != record_ops:
        problems.append("operation count: xfs_logprint=%d xfswin=%d" %
                        (logprint["oper_count"], record_ops))

    committed_tids = {trans["tid"] for trans in journal["transactions"]}
    missing = committed_tids - logprint["tids"]
    if missing:
        problems.append("transactions missing from xfs_logprint: %s" %
                        sorted(missing))
    if logprint["starts"] not in (0, len(committed_tids)):
        problems.append("START count: xfs_logprint=%d xfswin=%d" %
                        (logprint["starts"], len(committed_tids)))
    if logprint["commits"] != len(committed_tids):
        problems.append("COMMIT count: xfs_logprint=%d xfswin=%d" %
                        (logprint["commits"], len(committed_tids)))

    for problem in problems:
        print("DIFF: " + problem)
    if problems:
        return 1
    print("compare_logprint: structure matches (%d records, %d ops, %d commits)"
          % (len(journal["records"]), record_ops, len(committed_tids)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
