// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Embed the architecture-specific payload archive and the official WinFsp MSI
// into the setup executable.  The release build script sets all XFSWIN_SETUP_*
// variables; a plain `cargo check` without them gets an empty payload so the
// crate still type-checks, but the resulting binary refuses to install.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn required(name: &str) -> Option<String> {
    match env::var(name) {
        Ok(value) if !value.trim().is_empty() => Some(value),
        _ => None,
    }
}

fn copy_into_out_dir(source: &Path, name: &str) -> PathBuf {
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let destination = out_dir.join(name);
    fs::copy(source, &destination).unwrap_or_else(|error| {
        panic!(
            "cannot copy payload input {} to {}: {error}",
            source.display(),
            destination.display()
        )
    });
    destination
}

fn main() {
    println!("cargo:rerun-if-changed=installer.manifest");
    println!("cargo:rerun-if-changed=test.manifest");
    for name in [
        "XFSWIN_SETUP_PAYLOAD_ZIP",
        "XFSWIN_SETUP_VERSION",
        "XFSWIN_SETUP_GIT_SHA",
        "XFSWIN_SETUP_ARCH",
        "XFSWIN_SETUP_WINFSP_SHA256",
    ] {
        println!("cargo:rerun-if-env-changed={name}");
    }
    println!("cargo:rerun-if-env-changed=PROFILE");

    let payload = required("XFSWIN_SETUP_PAYLOAD_ZIP");
    let version = required("XFSWIN_SETUP_VERSION").unwrap_or_else(|| "0.0.0".to_string());
    let git_sha = required("XFSWIN_SETUP_GIT_SHA").unwrap_or_else(|| "unknown".to_string());
    let arch = required("XFSWIN_SETUP_ARCH").unwrap_or_else(|| "unknown".to_string());
    let winfsp_sha256 =
        required("XFSWIN_SETUP_WINFSP_SHA256").unwrap_or_else(|| String::new());
    let test_build = env::var_os("CARGO_FEATURE_TEST_FAIL").is_some();

    let payload_path = match payload {
        Some(payload) => copy_into_out_dir(Path::new(&payload), "payload.zip"),
        None => {
            println!(
                "cargo:warning=XFSWIN_SETUP_PAYLOAD_ZIP is not set; \
                 building a non-installable development setup"
            );
            let empty = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR")).join("empty.bin");
            fs::write(&empty, b"").expect("write empty payload");
            empty
        }
    };

    let generated = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR")).join("payload.rs");
    let source = format!(
        "pub const PAYLOAD_ZIP: &[u8] = include_bytes!({payload:?});\n\
         pub const SETUP_VERSION: &str = {version:?};\n\
         pub const SETUP_GIT_SHA: &str = {git_sha:?};\n\
         pub const SETUP_ARCH: &str = {arch:?};\n\
         pub const WINFSP_SHA256: &str = {winfsp_sha256:?};\n\
         pub const SETUP_TEST_BUILD: bool = {test_build};\n",
        payload = payload_path.to_string_lossy().into_owned(),
        version = version,
        git_sha = git_sha,
        arch = arch,
        winfsp_sha256 = winfsp_sha256,
        test_build = if test_build { "true" } else { "false" },
    );
    fs::write(&generated, source).expect("write generated payload constants");

    // Version resources and the requireAdministrator manifest.  The file
    // version is derived from the numeric product version; a non-numeric
    // fallback keeps development builds compiling.
    let mut numeric = version
        .split('.')
        .map(|part| part.parse::<u16>().unwrap_or(0))
        .collect::<Vec<_>>();
    while numeric.len() < 4 {
        numeric.push(0);
    }
    let file_version = format!(
        "{}.{}.{}.{}",
        numeric[0], numeric[1], numeric[2], numeric[3]
    );
    // Debug builds are the unit-test harness; they must be executable without
    // elevation.  Release builds carry the requireAdministrator manifest.
    // The manifest's assembly identity version comes from the same CMake
    // version as the PE version resources, never from a second hard-coded
    // product version.
    let manifest_source = if env::var("PROFILE").as_deref() == Ok("release") {
        "installer.manifest"
    } else {
        "test.manifest"
    };
    let manifest_text = fs::read_to_string(manifest_source)
        .unwrap_or_else(|error| panic!("cannot read {manifest_source}: {error}"));
    let manifest_text = manifest_text.replacen(
        "version=\"0.1.0.0\"",
        &format!("version=\"{file_version}\""),
        1,
    );
    let manifest_path =
        PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR")).join("setup.manifest");
    fs::write(&manifest_path, manifest_text).expect("write versioned setup manifest");
    let mut resource = winresource::WindowsResource::new();
    resource
        .set_manifest_file(&manifest_path.to_string_lossy())
        .set("ProductName", "XFS for Windows")
        .set("FileDescription", "XFS for Windows Setup")
        .set("CompanyName", "XFS-for-Windows contributors")
        .set("LegalCopyright", "Mozilla Public License 2.0")
        .set("OriginalFilename", "xfswin-setup.exe")
        .set("InternalName", "xfswin-setup")
        .set("FileVersion", &file_version)
        .set("ProductVersion", &file_version)
        .set(
            "Comments",
            &format!(
                "git={git_sha};arch={arch};acceptance={}",
                if test_build { "test-fail" } else { "none" }
            ),
        );
    if let Err(error) = resource.compile() {
        // A host without a Windows resource compiler can still run cargo check
        // for the release build; fail only when a real setup is requested.
        if env::var("XFSWIN_SETUP_REQUIRE_RESOURCES").as_deref() == Ok("1") {
            panic!("cannot embed setup version resources/manifest: {error}");
        }
        println!("cargo:warning=cannot embed setup resources: {error}");
    }
}
