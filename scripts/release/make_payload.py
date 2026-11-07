#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Create the deterministic payload archive embedded in the Rust setup.

The archive layout is:

  payload-manifest.json
  app/<the CMake install tree>
  winfsp/winfsp-<version>.msi

The Rust setup verifies payload-manifest.json and every file hash before it
copies anything into Program Files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import struct
import sys
import zipfile
from pathlib import Path

REQUIRED_APP_FILES = {
    "xfswin.exe",
    "LICENSE",
    "README.md",
    "version.json",
    "docs/INSTALL.md",
}
ARCHITECTURES = {"x64": 0x8664, "arm64": 0xAA64}
FIXED_TIME = (1980, 1, 1, 0, 0, 0)


class PayloadError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pe_machine(path: Path) -> int:
    with path.open("rb") as stream:
        header = stream.read(0x40)
        if len(header) < 0x40 or header[:2] != b"MZ":
            raise PayloadError(f"not a PE file: {path}")
        offset = struct.unpack_from("<I", header, 0x3C)[0]
        stream.seek(offset)
        signature = stream.read(6)
        if len(signature) < 6 or signature[:4] != b"PE\0\0":
            raise PayloadError(f"not a PE file: {path}")
        return struct.unpack_from("<H", signature, 4)[0]


def collect_app_files(app_dir: Path) -> list[tuple[str, Path]]:
    files: list[tuple[str, Path]] = []
    for path in sorted(app_dir.rglob("*")):
        if path.is_symlink():
            raise PayloadError(f"symbolic links are not allowed in the payload: {path}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise PayloadError(f"unsupported payload entry: {path}")
        relative = path.relative_to(app_dir).as_posix()
        if relative.lower().startswith("winfsp") and relative.lower().endswith(".dll"):
            raise PayloadError(f"app-local WinFsp DLL is forbidden: {relative}")
        files.append((f"app/{relative}", path))
    names = {name.removeprefix("app/") for name, _ in files}
    missing = sorted(REQUIRED_APP_FILES - names)
    if missing:
        raise PayloadError(f"app payload is missing: {', '.join(missing)}")
    return files


def validate_version_json(app_dir: Path, version: str, git_sha: str, architecture: str) -> None:
    path = app_dir / "version.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PayloadError(f"invalid version.json: {error}") from error
    expected = {
        "product": "XFS-for-Windows",
        "version": version,
        "git_sha": git_sha,
        "architecture": architecture,
    }
    for field, wanted in expected.items():
        if value.get(field) != wanted:
            raise PayloadError(
                f"version.json {field} is {value.get(field)!r}, expected {wanted!r}"
            )
    if value.get("production_build") is not True:
        raise PayloadError("version.json is not from a production build")
    if value.get("source_tree_dirty") is not False:
        raise PayloadError("version.json records a dirty source tree")


def write_entry(archive: zipfile.ZipFile, name: str, data: bytes) -> None:
    info = zipfile.ZipInfo(name, date_time=FIXED_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (stat.S_IFREG | 0o644) << 16
    info.create_system = 3
    archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def build_payload(args: argparse.Namespace) -> None:
    app_dir = Path(args.app_dir).resolve()
    winfsp_msi = Path(args.winfsp_msi).resolve()
    output = Path(args.output).resolve()
    if not app_dir.is_dir():
        raise PayloadError(f"app directory does not exist: {app_dir}")
    if not winfsp_msi.is_file():
        raise PayloadError(f"WinFsp MSI does not exist: {winfsp_msi}")
    if f"winfsp-{args.winfsp_version}.msi" != winfsp_msi.name:
        raise PayloadError(
            f"WinFsp MSI name {winfsp_msi.name!r} does not match "
            f"winfsp-{args.winfsp_version}.msi"
        )
    expected_machine = ARCHITECTURES[args.architecture]
    actual_machine = pe_machine(app_dir / "xfswin.exe")
    if actual_machine != expected_machine:
        raise PayloadError(
            f"xfswin.exe machine 0x{actual_machine:04x} does not match "
            f"{args.architecture} (0x{expected_machine:04x})"
        )
    validate_version_json(app_dir, args.version, args.git_sha, args.architecture)
    winfsp_hash = sha256_file(winfsp_msi)
    if args.winfsp_sha256 and winfsp_hash.lower() != args.winfsp_sha256.lower():
        raise PayloadError(
            f"WinFsp MSI hash {winfsp_hash} does not match the pinned "
            f"{args.winfsp_sha256}"
        )

    files = collect_app_files(app_dir)
    winfsp_name = f"winfsp/winfsp-{args.winfsp_version}.msi"
    files.append((winfsp_name, winfsp_msi))

    manifest = {
        "schema_version": 1,
        "version": args.version,
        "git_sha": args.git_sha,
        "architecture": args.architecture,
        "files": {name: sha256_file(path) for name, path in files},
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", allowZip64=True) as archive:
        write_entry(
            archive,
            "payload-manifest.json",
            (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"),
        )
        for name, path in files:
            write_entry(archive, name, path.read_bytes())
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise PayloadError(f"payload archive is corrupt: {output}")
    print(
        json.dumps(
            {
                "output": str(output),
                "size_bytes": output.stat().st_size,
                "sha256": sha256_file(output),
                "file_count": len(files) + 1,
                "winfsp_sha256": winfsp_hash,
            },
            indent=2,
        )
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-dir", required=True)
    parser.add_argument("--winfsp-msi", required=True)
    parser.add_argument("--winfsp-version", required=True)
    parser.add_argument("--winfsp-sha256", default="")
    parser.add_argument("--output", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--git-sha", required=True)
    parser.add_argument("--architecture", required=True, choices=sorted(ARCHITECTURES))
    args = parser.parse_args(argv)
    try:
        build_payload(args)
    except PayloadError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
