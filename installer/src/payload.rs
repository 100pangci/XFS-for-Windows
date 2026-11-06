// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{self, Cursor, Read, Write};
use std::path::{Component, Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::Deserialize;
use zip::ZipArchive;

use crate::logging::Log;
use crate::win;

pub mod embedded {
    include!(concat!(env!("OUT_DIR"), "/payload.rs"));
}

pub use embedded::{
    PAYLOAD_ZIP, SETUP_ARCH, SETUP_GIT_SHA, SETUP_TEST_BUILD, SETUP_VERSION, WINFSP_SHA256,
};
const MANIFEST_NAME: &str = "payload-manifest.json";

#[derive(Debug, Deserialize)]
struct PayloadManifest {
    schema_version: u32,
    version: String,
    git_sha: String,
    architecture: String,
    files: BTreeMap<String, String>,
}

#[derive(Debug)]
pub struct ExtractedPayload {
    pub app: PathBuf,
    pub xfswin: PathBuf,
    pub winfsp_msi: PathBuf,
}

fn failure(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message.into())
}

fn random_suffix() -> String {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or(0);
    format!("{:x}{:x}", std::process::id(), nanos)
}

pub fn new_staging_directory(install_dir: &Path) -> io::Result<PathBuf> {
    let parent = install_dir
        .parent()
        .ok_or_else(|| failure("install directory has no parent"))?;
    let name = install_dir
        .file_name()
        .ok_or_else(|| failure("install directory has no name"))?
        .to_string_lossy();
    for _ in 0..8 {
        let candidate = parent.join(format!("{name}.xfswin-setup-{}", random_suffix()));
        if !candidate.exists() {
            fs::create_dir(&candidate)?;
            return Ok(candidate);
        }
    }
    Err(io::Error::new(
        io::ErrorKind::AlreadyExists,
        "cannot allocate a unique setup staging directory",
    ))
}

pub fn new_backup_directory(install_dir: &Path) -> io::Result<PathBuf> {
    let parent = install_dir
        .parent()
        .ok_or_else(|| failure("install directory has no parent"))?;
    let name = install_dir
        .file_name()
        .ok_or_else(|| failure("install directory has no name"))?
        .to_string_lossy();
    for _ in 0..8 {
        let candidate = parent.join(format!("{name}.xfswin-backup-{}", random_suffix()));
        if !candidate.exists() {
            return Ok(candidate);
        }
    }
    Err(io::Error::new(
        io::ErrorKind::AlreadyExists,
        "cannot allocate a unique upgrade backup directory",
    ))
}

fn safe_relative(name: &str) -> io::Result<PathBuf> {
    let path = Path::new(name);
    if path.components().any(|component| {
        matches!(
            component,
            Component::ParentDir | Component::RootDir | Component::Prefix(_)
        )
    }) {
        return Err(failure(format!("payload contains an unsafe path: {name}")));
    }
    Ok(path.to_path_buf())
}

fn is_symlink_mode(mode: Option<u32>) -> bool {
    mode.map(|mode| mode & 0o170000 == 0o120000)
        .unwrap_or(false)
}

pub fn extract(staging: &Path, log: &mut Log) -> io::Result<ExtractedPayload> {
    if PAYLOAD_ZIP.is_empty() {
        return Err(failure(
            "this setup was built without an embedded payload and cannot install",
        ));
    }
    log.line(format!(
        "extracting payload ({} bytes) to {}",
        PAYLOAD_ZIP.len(),
        staging.display()
    ));
    let mut archive = ZipArchive::new(Cursor::new(PAYLOAD_ZIP))
        .map_err(|error| failure(format!("cannot open the embedded payload: {error}")))?;

    let manifest_bytes = {
        let mut entry = archive
            .by_name(MANIFEST_NAME)
            .map_err(|error| failure(format!("payload is missing {MANIFEST_NAME}: {error}")))?;
        let mut bytes = Vec::new();
        entry.read_to_end(&mut bytes)?;
        bytes
    };
    let manifest: PayloadManifest = serde_json::from_slice(&manifest_bytes)
        .map_err(|error| failure(format!("invalid {MANIFEST_NAME}: {error}")))?;
    if manifest.schema_version != 1 {
        return Err(failure(format!(
            "unsupported payload manifest schema version {}",
            manifest.schema_version
        )));
    }
    if manifest.version != SETUP_VERSION {
        return Err(failure(format!(
            "payload version {} does not match setup version {}",
            manifest.version, SETUP_VERSION
        )));
    }
    if manifest.git_sha != SETUP_GIT_SHA {
        return Err(failure(format!(
            "payload git SHA {} does not match setup git SHA {}",
            manifest.git_sha, SETUP_GIT_SHA
        )));
    }
    if manifest.architecture != SETUP_ARCH {
        return Err(failure(format!(
            "payload architecture {} does not match setup architecture {}",
            manifest.architecture, SETUP_ARCH
        )));
    }

    for index in 0..archive.len() {
        let mut entry = archive
            .by_index(index)
            .map_err(|error| failure(format!("cannot read payload entry {index}: {error}")))?;
        let name = entry
            .name()
            .map_err(|error| failure(format!("payload entry {index} has an invalid name: {error}")))?
            .to_string();
        if name == MANIFEST_NAME {
            continue;
        }
        let relative = safe_relative(&name)?;
        if is_symlink_mode(entry.unix_mode()) {
            return Err(failure(format!(
                "payload contains a symbolic link, which is not allowed: {name}"
            )));
        }
        let destination = staging.join(&relative);
        if entry.is_dir() {
            fs::create_dir_all(&destination)?;
            continue;
        }
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent)?;
        }
        let mut output = File::create(&destination)?;
        io::copy(&mut entry, &mut output)?;
        output.flush()?;
    }

    for (relative, expected) in &manifest.files {
        let path = staging.join(safe_relative(relative)?);
        if !path.is_file() {
            return Err(failure(format!(
                "payload is missing a manifest file: {relative}"
            )));
        }
        let actual = win::sha256_file(&path)?;
        if !actual.eq_ignore_ascii_case(expected) {
            return Err(failure(format!(
                "payload integrity check failed for {relative}: expected {expected}, got {actual}"
            )));
        }
    }

    verify_no_unlisted_files(staging, &manifest)?;

    let app = staging.join("app");
    let xfswin = app.join("xfswin.exe");
    let winfsp_msi = staging.join("winfsp").join("winfsp-2.1.25156.msi");
    if !xfswin.is_file() {
        return Err(failure("payload is missing app/xfswin.exe"));
    }
    if !app.join("docs").join("INSTALL.md").is_file() {
        return Err(failure("payload is missing app/docs/INSTALL.md"));
    }
    if !winfsp_msi.is_file() {
        return Err(failure(
            "payload is missing the pinned WinFsp MSI at winfsp/winfsp-2.1.25156.msi",
        ));
    }

    if !WINFSP_SHA256.is_empty() {
        let actual = win::sha256_file(&winfsp_msi)?;
        if !actual.eq_ignore_ascii_case(WINFSP_SHA256) {
            return Err(failure(format!(
                "embedded WinFsp MSI hash mismatch: expected {WINFSP_SHA256}, got {actual}"
            )));
        }
    } else {
        log.line("warning: setup has no independently pinned WinFsp MSI hash");
    }

    verify_version_json(&app.join("version.json"))?;
    verify_pe(&xfswin)?;
    verify_no_app_local_winfsp(&app)?;
    log.line(format!(
        "payload verified: {} files, version {}, git {}",
        manifest.files.len(),
        manifest.version,
        manifest.git_sha
    ));
    Ok(ExtractedPayload {
        app,
        xfswin,
        winfsp_msi,
    })
}

fn verify_no_unlisted_files(staging: &Path, manifest: &PayloadManifest) -> io::Result<()> {
    let mut found = BTreeMap::new();
    for root in ["app", "winfsp"] {
        let directory = staging.join(root);
        if directory.is_dir() {
            collect_files(&directory, staging, &mut found)?;
        }
    }
    let listed: BTreeMap<String, String> = manifest
        .files
        .iter()
        .map(|(name, hash)| (name.clone(), hash.clone()))
        .collect();
    for name in found.keys() {
        if !listed.contains_key(name) {
            return Err(failure(format!(
                "payload contains an unlisted file: {name}"
            )));
        }
    }
    Ok(())
}

fn collect_files(
    directory: &Path,
    base: &Path,
    output: &mut BTreeMap<String, String>,
) -> io::Result<()> {
    let mut entries: Vec<_> = fs::read_dir(directory)?.collect::<Result<_, _>>()?;
    entries.sort_by_key(|entry| entry.file_name());
    for entry in entries {
        let file_type = entry.file_type()?;
        if file_type.is_symlink() {
            return Err(failure(format!(
                "payload contains a symbolic link: {}",
                entry.path().display()
            )));
        }
        if file_type.is_dir() {
            collect_files(&entry.path(), base, output)?;
        } else if file_type.is_file() {
            let relative = entry
                .path()
                .strip_prefix(base)
                .unwrap_or(&entry.path())
                .to_string_lossy()
                .replace('\\', "/");
            output.insert(relative, String::new());
        }
    }
    Ok(())
}

fn verify_version_json(path: &Path) -> io::Result<()> {
    let text = fs::read_to_string(path)
        .map_err(|error| failure(format!("cannot read {}: {error}", path.display())))?;
    let value: serde_json::Value = serde_json::from_str(&text)
        .map_err(|error| failure(format!("invalid version.json: {error}")))?;
    let expected = [
        ("product", "XFS-for-Windows"),
        ("version", SETUP_VERSION),
        ("git_sha", SETUP_GIT_SHA),
        ("architecture", SETUP_ARCH),
    ];
    for (field, wanted) in expected {
        if value.get(field).and_then(|value| value.as_str()) != Some(wanted) {
            return Err(failure(format!(
                "version.json field {field} does not match the setup"
            )));
        }
    }
    if value
        .get("production_build")
        .and_then(|value| value.as_bool())
        != Some(true)
    {
        return Err(failure("version.json is not from a production build"));
    }
    if value
        .get("source_tree_dirty")
        .and_then(|value| value.as_bool())
        != Some(false)
    {
        return Err(failure("version.json records a dirty source tree"));
    }
    Ok(())
}

fn verify_pe(xfswin: &Path) -> io::Result<()> {
    let mut file = File::open(xfswin)?;
    let mut header = vec![0u8; 1024 * 1024];
    let read = file.read(&mut header)?;
    header.truncate(read);
    let machine = win::pe_machine(&header)
        .ok_or_else(|| failure("payload xfswin.exe is not a valid PE file"))?;
    let expected = win::expected_pe_machine()?;
    if machine != expected.0 {
        return Err(failure(format!(
            "payload xfswin.exe machine 0x{machine:04x} does not match setup architecture {} (0x{:04x})",
            SETUP_ARCH, expected.0
        )));
    }
    Ok(())
}

fn verify_no_app_local_winfsp(app: &Path) -> io::Result<()> {
    let mut stack = vec![app.to_path_buf()];
    while let Some(directory) = stack.pop() {
        for entry in fs::read_dir(&directory)? {
            let entry = entry?;
            let file_type = entry.file_type()?;
            if file_type.is_dir() {
                stack.push(entry.path());
            } else if file_type.is_file() {
                let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
                if name.starts_with("winfsp") && name.ends_with(".dll") {
                    return Err(failure(format!(
                        "payload contains an app-local WinFsp DLL: {}",
                        entry.path().display()
                    )));
                }
            } else {
                return Err(failure(format!(
                    "payload contains a non-file entry: {}",
                    entry.path().display()
                )));
            }
        }
    }
    Ok(())
}

pub fn cleanup(staging: &Path, log: &mut Log) {
    match fs::remove_dir_all(staging) {
        Ok(()) => log.line(format!("removed staging directory {}", staging.display())),
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => log.line(format!(
            "warning: cannot remove staging directory {}: {error}",
            staging.display()
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn embedded_payload_extracts_and_verifies() {
        if PAYLOAD_ZIP.is_empty() {
            return;
        }
        let staging = std::env::temp_dir().join(format!(
            "xfswin-payload-test-{}-{:x}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|duration| duration.as_nanos())
                .unwrap_or(0)
        ));
        fs::create_dir_all(&staging).expect("create test staging");
        let mut log = Log::new(None, true);
        let extracted = extract(&staging, &mut log).expect("extract embedded payload");
        assert!(extracted.xfswin.is_file());
        assert!(extracted.winfsp_msi.is_file());
        assert!(extracted.app.join("version.json").is_file());
        fs::remove_dir_all(&staging).ok();
    }
}
