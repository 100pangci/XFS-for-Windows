#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Reverse-interoperability check: after Linux appended to a Windows-written
file, the image must contain the append and stay structurally valid.

usage: rw2_reverse_check.py <image> <inode>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rw2_probe  # noqa: E402

TAIL = b"LINUX-AFTER-WINDOWS"


def main():
    image = rw2_probe.Image(sys.argv[1])
    ino = int(sys.argv[2])
    geo = rw2_probe.check_image(image, verbose=False)
    inode = rw2_probe.read_inode(image, geo, ino)
    data = rw2_probe.read_file_data(image, geo, inode)
    if not data.endswith(TAIL):
        raise SystemExit("the Linux append is not visible in the image")
    print(f"reverse: size {len(data)}, Linux append intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
