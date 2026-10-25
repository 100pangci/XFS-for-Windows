#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Assert that an RW-6D oracle dump contains every representation."""

import json
import sys


def main(argv):
    data = json.load(open(argv[1], encoding="utf-8"))
    inodes = data["inodes"]
    formats = {item["format"] for item in inodes}
    missing = {"shortform", "leaf", "node"} - formats
    if missing:
        raise SystemExit("missing representations: %s" % sorted(missing))
    if not any(item["format"] == "node" and item["root_level"] >= 2
               for item in inodes):
        raise SystemExit("no multi-level DA node")
    if not any(item["aformat"] == 3 for item in inodes):
        raise SystemExit("no attribute BMBT")
    if not any(item["remote_blocks"] > 0 for item in inodes):
        raise SystemExit("no remote attribute value")
    print("RW-6D representations OK: %s max-level=%d" %
          (",".join(sorted(formats)),
           max(item["root_level"] for item in inodes)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
