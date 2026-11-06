# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Negative publication-gate checks; all binaries/evidence here are synthetic."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "release" / "manifest.py"
SPEC = importlib.util.spec_from_file_location("release_manifest", SCRIPT)
manifest = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(manifest)


class ReleaseGateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xfswin-release-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.evidence = self.directory / "evidence"
        self.evidence.mkdir()
        self.version, self.sha = "0.1.0", "ab" * 20
        self.lock = self.directory / "dependency.json"
        self.lock.write_text(json.dumps({"version": "2.1.0", "sha256": "0" * 64,
                                         "url": "https://github.com/winfsp/winfsp/releases/download/v2.1/fixture.msi"}), encoding="utf-8")
        for architecture, machine in manifest.ARCHITECTURES.items():
            executable = self.pe(machine)
            metadata = {"version": self.version, "git_sha": self.sha, "architecture": architecture,
                        "configuration": "Release", "production_build": True, "source_tree_dirty": False,
                        "toolchain": "synthetic fixture", "build_timestamp": "2026-11-06T00:00:00Z"}
            with zipfile.ZipFile(self.portable(architecture), "w") as archive:
                for name in manifest.REQUIRED_ZIP_FILES:
                    content = executable if name == "xfswin.exe" else json.dumps(metadata).encode() if name == "version.json" else b"Test fixture\n"
                    archive.writestr(name, content)
            self.installer(architecture).write_bytes(executable)
            checksum = hashlib.sha256(executable).hexdigest()
            self.proof_path(architecture).write_text(json.dumps({
                "architecture": architecture, "head_sha": self.sha, "run_id": "1",
                "installer_sha256": checksum, "installed_exe_sha256": checksum,
                "installed_metadata": metadata,
                "cases": {case: "passed" for case in manifest.REQUIRED_CASES},
            }), encoding="utf-8")

    @staticmethod
    def pe(machine):
        data = bytearray(1024)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3C, 128)
        data[128:132] = b"PE\0\0"
        struct.pack_into("<HH", data, 132, machine, 1)
        struct.pack_into("<HH", data, 148, 240, 0x22)
        optional = 152
        struct.pack_into("<H", data, optional, 0x20B)
        struct.pack_into("<I", data, optional + 16, 0x1000)
        struct.pack_into("<II", data, optional + 56, 0x2000, 512)
        struct.pack_into("<H", data, optional + 68, 3)
        section = optional + 240
        data[section:section + 8] = b".text\0\0\0"
        struct.pack_into("<IIII", data, section + 8, 4, 0x1000, 512, 512)
        struct.pack_into("<I", data, section + 36, 0x60000020)
        data[512:516] = b"\xc0\x03\x5f\xd6" if machine == 0xAA64 else b"\x31\xc0\xc3\x90"
        return bytes(data)

    def portable(self, architecture):
        return self.directory / f"xfswin-{self.version}-windows-{architecture}.zip"

    def installer(self, architecture):
        return self.directory / f"xfswin-{self.version}-windows-{architecture}-setup.exe"

    def proof_path(self, architecture):
        return self.evidence / f"qualification-{architecture}.json"

    def edit_proof(self, architecture, change):
        path = self.proof_path(architecture)
        proof = manifest.read_json(path)
        change(proof)
        path.write_text(json.dumps(proof), encoding="utf-8")

    def edit_portable_metadata(self, change):
        path = self.portable("x64")
        with zipfile.ZipFile(path) as archive:
            members = [(item.filename, archive.read(item)) for item in archive.infolist()]
        with zipfile.ZipFile(path, "w") as archive:
            for name, content in members:
                if name == "version.json":
                    metadata = json.loads(content)
                    change(metadata)
                    content = json.dumps(metadata).encode()
                archive.writestr(name, content)

    def create(self):
        manifest.create(self.directory, self.version, self.sha, self.lock, self.evidence)

    def verify(self):
        manifest.verify(self.directory, self.version, self.sha, self.lock)

    def test_complete_two_architecture_release(self):
        self.create()
        self.verify()
        self.assertEqual(len(manifest.read_json(self.directory / "release-manifest.json")["artifacts"]), 4)
        self.assertEqual(len((self.directory / "SHA256SUMS.txt").read_text().splitlines()), 5)

    def test_missing_arm64_installer_blocks_release(self):
        self.installer("arm64").unlink()
        with self.assertRaisesRegex(ValueError, "Missing.*arm64-setup"):
            self.create()

    def test_emulated_x64_is_not_an_arm64_installer(self):
        self.installer("arm64").write_bytes(self.pe(0x8664))
        with self.assertRaisesRegex(ValueError, "not native arm64"):
            self.create()

    def test_arm64ec_is_not_native_arm64(self):
        self.installer("arm64").write_bytes(self.pe(0xA641))
        with self.assertRaisesRegex(ValueError, "not native arm64"):
            self.create()

    def test_changed_installer_invalidates_qualified_identity(self):
        self.create()
        with self.installer("x64").open("ab") as stream:
            stream.write(b"changed after qualification")
        with self.assertRaisesRegex(ValueError, "different installer"):
            self.verify()

    def test_wrong_tag_commit_blocks_release(self):
        self.create()
        with self.assertRaisesRegex(ValueError, "expected tag/commit"):
            manifest.verify(self.directory, self.version, "cd" * 20, self.lock)

    def test_shared_winfsp_dll_cannot_leak_into_portable(self):
        with zipfile.ZipFile(self.portable("x64"), "a") as archive:
            archive.writestr("winfsp-x64.dll", b"not redistributable this way")
        with self.assertRaisesRegex(ValueError, "shared DLL"):
            self.create()

    def test_zip_path_traversal_blocks_release(self):
        with zipfile.ZipFile(self.portable("x64"), "a") as archive:
            archive.writestr("../outside.txt", b"unsafe")
        with self.assertRaisesRegex(ValueError, "Unsafe ZIP entry"):
            self.create()

    def test_checksum_omission_blocks_release(self):
        self.create()
        sums = self.directory / "SHA256SUMS.txt"
        sums.write_text("\n".join(sums.read_text().splitlines()[:-1]) + "\n", encoding="ascii")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.verify()

    def test_unversioned_dependency_is_rejected(self):
        lock = manifest.read_json(self.lock)
        lock["url"] = "https://github.com/winfsp/winfsp/releases/latest/download/winfsp.msi"
        self.lock.write_text(json.dumps(lock), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "official pinned"):
            self.create()

    def test_dot_alias_cannot_overwrite_checked_executable(self):
        with zipfile.ZipFile(self.portable("x64"), "a") as archive:
            archive.writestr("./xfswin.exe", self.pe(0xAA64))
        with self.assertRaisesRegex(ValueError, "Unsafe ZIP entry"):
            self.create()

    def test_unapproved_dll_is_rejected(self):
        with zipfile.ZipFile(self.portable("x64"), "a") as archive:
            archive.writestr("unrelated.dll", b"not an approved dependency")
        with self.assertRaisesRegex(ValueError, "Unexpected portable payload"):
            self.create()

    def test_truncated_pe_markers_are_not_an_executable(self):
        data = bytearray(70)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3C, 64)
        data[64:68] = b"PE\0\0"
        struct.pack_into("<H", data, 68, 0x8664)
        self.installer("x64").write_bytes(data)
        with self.assertRaisesRegex(ValueError, "truncated PE"):
            self.create()

    def test_corrupt_required_document_is_rejected(self):
        path = self.portable("x64")
        with zipfile.ZipFile(path) as archive:
            offset = archive.getinfo("README.md").header_offset
        data = bytearray(path.read_bytes())
        name_size, extra_size = struct.unpack_from("<HH", data, offset + 26)
        data[offset + 30 + name_size + extra_size] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "Corrupt ZIP payload"):
            self.create()

    def test_missing_clean_install_evidence_is_rejected(self):
        self.proof_path("arm64").unlink()
        with self.assertRaisesRegex(ValueError, "Missing/oversized metadata"):
            self.create()

    def test_installed_executable_must_match_portable(self):
        self.edit_proof("x64", lambda proof: proof.update(installed_exe_sha256="0" * 64))
        with self.assertRaisesRegex(ValueError, "differs from the portable"):
            self.create()

    def test_stale_installer_cannot_borrow_zip_version(self):
        self.edit_proof("x64", lambda proof: proof["installed_metadata"].update(version="0.0.1"))
        with self.assertRaisesRegex(ValueError, "Installed version metadata"):
            self.create()

    def test_failed_acceptance_case_blocks_release(self):
        self.edit_proof("x64", lambda proof: proof["cases"].update(upgrade="failed"))
        with self.assertRaisesRegex(ValueError, "Failed/skipped"):
            self.create()

    def test_skipped_acceptance_case_blocks_release(self):
        self.edit_proof("arm64", lambda proof: proof["cases"].update(uninstall="skipped"))
        with self.assertRaisesRegex(ValueError, "Failed/skipped"):
            self.create()

    def test_missing_required_acceptance_case_blocks_release(self):
        self.edit_proof("x64", lambda proof: proof["cases"].pop("rw-smoke"))
        with self.assertRaisesRegex(ValueError, "Incomplete installer acceptance"):
            self.create()

    def test_debug_payload_is_not_a_production_release(self):
        self.edit_portable_metadata(lambda metadata: metadata.update(configuration="Debug"))
        with self.assertRaisesRegex(ValueError, "clean production Release"):
            self.create()

    def test_dirty_source_payload_is_not_attributed_to_a_commit(self):
        self.edit_portable_metadata(lambda metadata: metadata.update(source_tree_dirty=True))
        with self.assertRaisesRegex(ValueError, "clean production Release"):
            self.create()

    def test_architecture_evidence_cannot_mix_runs(self):
        self.edit_proof("arm64", lambda proof: proof.update(run_id="2"))
        with self.assertRaisesRegex(ValueError, "same qualification run"):
            self.create()


if __name__ == "__main__":
    unittest.main()
