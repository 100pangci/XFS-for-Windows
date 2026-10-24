#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Real-image rmap split/borrow/merge/root stress through ordinary file APIs."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    out = Path(args.work_dir)
    out.mkdir(parents=True, exist_ok=True)
    image = out / "rmap-tree-windows.img"
    assert image.resolve() != Path(args.source).resolve()
    shutil.copyfile(args.source, image)
    stats = {k: 0 for k in ("split_leaf", "split_node", "merge_leaf", "merge_node",
                            "borrow_leaf", "borrow_node", "root_grow", "root_collapse")}

    def cli(command, *operands, write=False):
        argv = [args.exe, command, str(image), *map(str, operands)]
        if write:
            argv += ["--rw-experimental", "--verbose"]
        proc = subprocess.run(argv, capture_output=True, text=True)
        assert proc.returncode == 0, " ".join(argv) + "\n" + proc.stdout + proc.stderr
        for line in (proc.stdout + proc.stderr).splitlines():
            if "rmap edits:" in line:
                for key, value in re.findall(r"(\w+)=(\d+)", line):
                    if key in stats:
                        stats[key] += int(value)
        return proc.stdout

    def verify():
        cli("check")
        subprocess.run([sys.executable, str(Path(__file__).with_name("rw6b_rmap_probe.py")),
                        str(image), "--quiet"], check=True)

    def height():
        # AG0 file allocations: read the actual AGF height, not writer stats.
        with image.open("rb") as stream:
            sb = stream.read(512)
            sector = int.from_bytes(sb[102:104], "big")
            assert int.from_bytes(sb[4:8], "big") == 4096
            stream.seek(sector + 36)
            return int.from_bytes(stream.read(4), "big")

    verify()
    before = height()
    assert before == 1
    cli("create", "/tree-a", write=True)
    cli("create", "/tree-b", write=True)
    # Alternating owners prevent physical adjacency from merging the rmap
    # records. Removing B then removes every other mapping, forcing borrow.
    # max_leaf=168, max_node=91 at 4 KiB. 10,000 nonmergeable data records
    # force internal growth, not just more leaves under an unchanged root.
    for i in range(5000):
        for name in ("/tree-a", "/tree-b"):
            cli("fallocate", name, i * 4096, 4096, write=True)
        if (i + 1) % 500 == 0:
            print("allocated pairs:", i + 1, flush=True)
    verify()
    peak = height()
    assert peak >= 3, "no real internal rmap split"
    shutil.copyfile(image, out / "rmap-tree-grown.img")
    cli("unlink", "/tree-b", write=True)
    verify()
    cli("unlink", "/tree-a", write=True)
    verify()
    after = height()
    # Freed tree blocks stay on the active AGFL as OWN_AG. Those scattered
    # records can legitimately require a two-level tree after file deletion.
    assert after < peak, "rmap root did not collapse"
    for key in ("split_leaf", "split_node", "merge_leaf", "merge_node",
                "root_grow", "root_collapse"):
        assert stats[key] > 0, "missing real-image coverage: " + key
    assert stats["borrow_leaf"] + stats["borrow_node"] > 0, "no real-image borrow"
    evidence = {"before": before, "peak": peak, "after": after, "edits": stats}
    (out / "tree.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print("real-image rmap tree:", evidence, flush=True)


if __name__ == "__main__":
    main()
