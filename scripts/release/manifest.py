#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Create/verify REL-1 assets. This metadata never controls runtime policy."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import sys
import zipfile

ARCHITECTURES = {"x64": 0x8664, "arm64": 0xAA64}
REQUIRED_ZIP_FILES = {"xfswin.exe", "LICENSE", "README.md", "version.json", "docs/INSTALL.md"}
OPTIONAL_ZIP_FILES = {
    "THIRD-PARTY-NOTICES.txt", "WINFSP-LICENSE.txt", "licenses/WinFsp-LICENSE.txt",
    "docs/INSTALLER.md", "docs/RELEASE.md", "docs/RW_STABILITY.md", "docs/UX.md",
    "docs/WINDOWS_BLOCK_DEVICE.md",
}
REQUIRED_CASES = {
    "clean-install", "version", "path", "service", "ro-mount", "rw-smoke",
    "active-upgrade-refusal", "upgrade", "legacy-config", "uninstall",
    "config-preservation", "reinstall", "dependency-failure", "failure-before-service",
    "failure-after-files", "failure-service-start", "upgrade-rollback",
}
MAX_PAYLOAD = 64 * 1024 * 1024


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def pe_machine(data: bytes, file_size: int | None = None) -> int:
    """Check complete PE32+ headers and section bounds, not just the MZ marker.

    This is structural validation. Actual Windows execution and installer
    behavior are separately required by the architecture's acceptance evidence.
    """
    total = len(data) if file_size is None else file_size
    require(len(data) >= 64 and data[:2] == b"MZ", "Missing PE DOS header")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    require(64 <= offset <= len(data) - 24 and data[offset:offset + 4] == b"PE\0\0", "Invalid/truncated PE header")
    machine, sections = struct.unpack_from("<HH", data, offset + 4)
    optional_size, characteristics = struct.unpack_from("<HH", data, offset + 20)
    require(1 <= sections <= 96, "Invalid PE section count")
    require(characteristics & 0x0002 and not characteristics & 0x2000, "PE is not an executable application")
    optional = offset + 24
    section_table = optional + optional_size
    require(optional_size >= 112 and section_table + sections * 40 <= len(data), "Truncated PE optional/section headers")
    require(struct.unpack_from("<H", data, optional)[0] == 0x20B, "Native 64-bit PE32+ executable required")
    entrypoint = struct.unpack_from("<I", data, optional + 16)[0]
    image_size, header_size = struct.unpack_from("<II", data, optional + 56)
    subsystem = struct.unpack_from("<H", data, optional + 68)[0]
    require(section_table + sections * 40 <= header_size <= len(data), "Invalid PE header size")
    require(0 < entrypoint < image_size and subsystem in (2, 3), "Invalid PE application entrypoint/subsystem")
    executable_entrypoint = False
    for index in range(sections):
        section = section_table + index * 40
        virtual_size, address, raw_size, raw_offset = struct.unpack_from("<IIII", data, section + 8)
        flags = struct.unpack_from("<I", data, section + 36)[0]
        require(address < image_size and virtual_size <= image_size - address, "PE section exceeds image")
        if raw_size:
            require(header_size <= raw_offset <= total and raw_size <= total - raw_offset, "PE section exceeds file")
        if flags & 0x20000000 and address <= entrypoint < address + min(virtual_size, raw_size):
            executable_entrypoint = True
    require(executable_entrypoint, "PE entrypoint is not backed by an executable section")
    return machine


def read_json(path: Path) -> dict:
    require(path.is_file() and path.stat().st_size <= 1024 * 1024, f"Missing/oversized metadata: {path.name}")
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    require(isinstance(value, dict), f"Expected JSON object: {path.name}")
    return value


def dependency(path: Path) -> dict:
    value = read_json(path)
    for key in ("version", "sha256", "url"):
        require(isinstance(value.get(key), str) and value[key], f"Dependency lock needs {key}")
    require(re.fullmatch(r"[0-9a-fA-F]{64}", value["sha256"]) is not None, "Dependency SHA-256 is invalid")
    require(value["url"].startswith("https://github.com/winfsp/winfsp/releases/download/"), "WinFsp URL must be an official pinned release asset")
    require("/latest/" not in value["url"], "Unversioned WinFsp download is forbidden")
    return {"version": value["version"], "sha256": value["sha256"].lower(), "url": value["url"],
            "minimum_version": value.get("minimum_version", value["version"])}


def inspect_zip(path: Path, architecture: str, version: str, git_sha: str) -> dict:
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        require(len(entries) <= 128, f"Unexpected ZIP file count: {path.name}")
        require(sum(item.file_size for item in entries) <= MAX_PAYLOAD, f"Oversized ZIP: {path.name}")
        names, folded = set(), set()
        for item in entries:
            name = item.filename
            parts = PurePosixPath(name).parts
            canonical = str(PurePosixPath(name)) + ("/" if item.is_dir() else "")
            require(name and "\\" not in name and not name.startswith("/") and ":" not in name
                    and ".." not in parts and name == canonical
                    and all(part == part.rstrip(" .") for part in parts), f"Unsafe ZIP entry: {name}")
            require(not stat.S_ISLNK(item.external_attr >> 16), f"ZIP symlink is forbidden: {name}")
            require(not item.flag_bits & 1, f"Encrypted ZIP entry is forbidden: {name}")
            require(name.casefold() not in folded, f"Duplicate ZIP entry: {name}")
            folded.add(name.casefold())
            if item.is_dir():
                require(name in ("docs/", "licenses/"), f"Unexpected ZIP directory: {name}")
                continue
            names.add(name)
            require(not re.search(r"(^|/)winfsp[^/]*\.dll$", name, re.I), "WinFsp shared DLL must not be copied into the portable payload")
            require(name in REQUIRED_ZIP_FILES | OPTIONAL_ZIP_FILES, f"Unexpected portable payload: {name}")
        require(REQUIRED_ZIP_FILES <= names, f"Incomplete portable payload: {sorted(REQUIRED_ZIP_FILES - names)}")
        require(archive.testzip() is None, f"Corrupt ZIP payload: {path.name}")
        require(archive.getinfo("version.json").file_size <= 1024 * 1024, "Oversized version metadata")
        metadata = json.loads(archive.read("version.json").decode("utf-8-sig"))
        require(isinstance(metadata, dict), "Invalid version metadata")
        require(metadata.get("version") == version, f"Portable version mismatch: {path.name}")
        require(metadata.get("git_sha") == git_sha, f"Portable Git SHA mismatch: {path.name}")
        require(metadata.get("architecture") == architecture, f"Portable architecture metadata mismatch: {path.name}")
        require(metadata.get("configuration") == "Release" and metadata.get("production_build") is True
                and metadata.get("source_tree_dirty") is False, "Portable payload is not a clean production Release build")
        executable = archive.read("xfswin.exe")
        require(pe_machine(executable) == ARCHITECTURES[architecture], f"Portable executable is not native {architecture}")
        for key in ("toolchain", "build_timestamp"):
            require(isinstance(metadata.get(key), str) and metadata[key], f"Portable metadata missing {key}")
        datetime.fromisoformat(metadata["build_timestamp"].replace("Z", "+00:00"))
        metadata["_payload_exe_sha256"] = hashlib.sha256(executable).hexdigest()
        return metadata


def checked_evidence(proof: dict, architecture: str, version: str, git_sha: str,
                     installer_hash: str, payload_hash: str) -> dict:
    """Bind the *executed* installer to the installed payload, not ZIP naming."""
    require(proof.get("architecture") == architecture and proof.get("head_sha") == git_sha, "Acceptance evidence architecture/commit mismatch")
    require(proof.get("installer_sha256") == installer_hash, "Acceptance evidence belongs to a different installer")
    require(proof.get("installed_exe_sha256") == payload_hash, "Installed executable differs from the portable payload")
    installed = proof.get("installed_metadata", {})
    require(isinstance(installed, dict) and installed.get("version") == version
            and installed.get("git_sha") == git_sha and installed.get("architecture") == architecture,
            "Installed version metadata does not match the release commit")
    require(installed.get("configuration") == "Release" and installed.get("production_build") is True
            and installed.get("source_tree_dirty") is False, "Installed binary is not a clean production Release build")
    run_id = str(proof.get("run_id", ""))
    require(re.fullmatch(r"[1-9][0-9]*", run_id) is not None, "Missing actual acceptance run ID")
    cases = proof.get("cases")
    require(isinstance(cases, dict) and REQUIRED_CASES <= cases.keys(), "Incomplete installer acceptance evidence")
    require(all(status == "passed" for status in cases.values()), "Failed/skipped installer acceptance case")
    return {"architecture": architecture, "head_sha": git_sha, "run_id": run_id,
            "installer_sha256": installer_hash, "installed_exe_sha256": payload_hash,
            "installed_metadata": installed, "cases": dict(sorted(cases.items()))}


def inventory(directory: Path, version: str, git_sha: str, winfsp: dict,
              evidence_directory: Path | None = None, saved_evidence: dict | None = None) -> tuple[list[dict], dict]:
    require(re.fullmatch(r"\d+\.\d+\.\d+", version) is not None, "Version must be the numeric CMake project version")
    require(re.fullmatch(r"[0-9a-f]{40}", git_sha) is not None, "A full source Git SHA is required")
    artifacts, expected, proofs = [], set(), {}
    for architecture in ARCHITECTURES:
        prefix = f"xfswin-{version}-windows-{architecture}"
        portable, installer = directory / f"{prefix}.zip", directory / f"{prefix}-setup.exe"
        require(portable.is_file(), f"Missing {portable.name}")
        require(installer.is_file(), f"Missing {installer.name}")
        require(installer.stat().st_size <= 256 * 1024 * 1024, f"Oversized installer: {installer.name}")
        metadata = inspect_zip(portable, architecture, version, git_sha)
        with installer.open("rb") as stream:
            require(pe_machine(stream.read(1024 * 1024), installer.stat().st_size) == ARCHITECTURES[architecture], f"Installer is not native {architecture}")
        installer_hash = digest(installer)
        if evidence_directory is not None:
            proof = read_json(evidence_directory / f"qualification-{architecture}.json")
        else:
            require(isinstance(saved_evidence, dict), "Clean-machine installer acceptance evidence is required")
            proof = saved_evidence.get(architecture, {})
        proofs[architecture] = checked_evidence(proof, architecture, version, git_sha,
                                               installer_hash, metadata["_payload_exe_sha256"])
        for path, kind in ((portable, "portable"), (installer, "installer")):
            expected.add(path.name)
            artifacts.append({"name": path.name, "kind": kind, "architecture": architecture,
                              "version": version, "git_sha": git_sha, "sha256": digest(path),
                              "size_bytes": path.stat().st_size, "toolchain": metadata["toolchain"],
                              "build_timestamp": metadata["build_timestamp"],
                              "winfsp_dependency_version": winfsp["version"]})
    require(len({proof["run_id"] for proof in proofs.values()}) == 1, "Architecture evidence must come from the same qualification run")
    actual = {p.name for p in directory.iterdir() if p.is_file() and p.name.startswith("xfswin-")
              and p.name.endswith((".zip", "-setup.exe"))}
    require(actual == expected, f"Unexpected release assets: {sorted(actual - expected)}")
    return sorted(artifacts, key=lambda item: item["name"]), proofs


def create(directory: Path, version: str, git_sha: str, lock: Path, evidence_directory: Path) -> None:
    winfsp = dependency(lock)
    artifacts, proofs = inventory(directory, version, git_sha, winfsp, evidence_directory)
    manifest = {"schema_version": 1, "product": "XFS-for-Windows", "version": version,
                "git_sha": git_sha, "build_timestamp": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
                "architectures": list(ARCHITECTURES), "winfsp_dependency": winfsp,
                "qualification": proofs, "artifacts": artifacts}
    manifest_path = directory / "release-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    sums = {item["name"]: item["sha256"] for item in artifacts}
    sums[manifest_path.name] = digest(manifest_path)
    (directory / "SHA256SUMS.txt").write_text("".join(f"{sums[name]}  {name}\n" for name in sorted(sums)), encoding="ascii", newline="\n")
    verify(directory, version, git_sha, lock, evidence_directory)


def verify(directory: Path, version: str, git_sha: str, lock: Path, evidence_directory: Path | None = None) -> None:
    manifest_path = directory / "release-manifest.json"
    manifest, winfsp = read_json(manifest_path), dependency(lock)
    require(manifest.get("schema_version") == 1 and manifest.get("product") == "XFS-for-Windows", "Unsupported release manifest")
    require(manifest.get("version") == version and manifest.get("git_sha") == git_sha, "Release provenance does not match expected tag/commit")
    require(manifest.get("architectures") == list(ARCHITECTURES), "Incomplete architecture coverage")
    require(manifest.get("winfsp_dependency") == winfsp, "Release dependency does not match the reviewed lock")
    datetime.fromisoformat(manifest["build_timestamp"].replace("Z", "+00:00"))
    artifacts, proofs = inventory(directory, version, git_sha, winfsp, evidence_directory, manifest.get("qualification"))
    require(manifest.get("qualification") == proofs, "Installer acceptance evidence differs from the manifest")
    require(manifest.get("artifacts") == artifacts, "Release manifest differs from actual artifact contents/hashes")
    sums_path = directory / "SHA256SUMS.txt"
    require(sums_path.is_file() and sums_path.stat().st_size <= 16 * 1024, "Missing/oversized SHA256SUMS.txt")
    sums = {}
    for line in sums_path.read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9_.-]+)", line)
        require(match is not None, "Malformed SHA256SUMS.txt line")
        checksum, name = match.groups()
        require(name not in sums, f"Duplicate checksum: {name}")
        sums[name] = checksum
    expected = {item["name"]: item["sha256"] for item in artifacts}
    expected[manifest_path.name] = digest(manifest_path)
    require(sums == expected, "SHA256SUMS.txt is incomplete or does not match the release")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--git-sha", required=True)
    parser.add_argument("--dependency-lock", required=True, type=Path)
    parser.add_argument("--evidence-directory", type=Path)
    args = parser.parse_args()
    if args.action == "create" and args.evidence_directory is None:
        parser.error("create requires --evidence-directory from actual clean-machine acceptance")
    try:
        (create if args.action == "create" else verify)(args.directory, args.version, args.git_sha, args.dependency_lock, args.evidence_directory)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"REL-1 artifact validation failed: {error}", file=sys.stderr)
        return 1
    print(f"REL-1 artifact validation passed: {args.version} {args.git_sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
