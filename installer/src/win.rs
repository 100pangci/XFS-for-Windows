// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Small, explicit wrappers around the Windows APIs used by the setup.
//!
//! The setup deliberately uses fixed machine locations (Program Files,
//! ProgramData, the machine environment block and the 64-bit registry view)
//! and never asks the user to choose an install directory.

use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::{self, Read};
use std::os::windows::process::CommandExt;
use std::path::{Component, Path, PathBuf};
use std::process::{Command, Output};
use std::time::Duration;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use windows::core::{HSTRING, Interface, PCWSTR, PWSTR};
use windows::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_ALREADY_EXISTS, ERROR_FILE_NOT_FOUND, ERROR_MORE_DATA,
    ERROR_NO_MORE_ITEMS, ERROR_SUCCESS, HANDLE,
};
use windows::Win32::Security::{
    GetTokenInformation, TokenElevation, TOKEN_ELEVATION, TOKEN_QUERY,
};
use windows::Win32::Storage::FileSystem::{
    GetFileAttributesW, FILE_ATTRIBUTE_REPARSE_POINT, INVALID_FILE_ATTRIBUTES,
};
use windows::Win32::System::ApplicationInstallationAndServicing::{
    MsiEnumRelatedProductsW, MsiGetProductInfoW, INSTALLPROPERTY_VERSIONSTRING,
};
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoTaskMemFree, CoUninitialize, IPersistFile,
    CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED,
};
use windows::Win32::System::Registry::{
    RegCloseKey, RegCreateKeyExW, RegDeleteTreeW, RegOpenKeyExW, RegQueryValueExW,
    RegSetValueExW, HKEY, HKEY_LOCAL_MACHINE, KEY_READ, KEY_WOW64_64KEY, KEY_WRITE,
    REG_DWORD, REG_EXPAND_SZ, REG_OPTION_NON_VOLATILE, REG_SZ, REG_VALUE_TYPE,
};
use windows::Win32::System::SystemInformation::{
    GetLocalTime, GetSystemDirectoryW, IMAGE_FILE_MACHINE, IMAGE_FILE_MACHINE_AMD64,
    IMAGE_FILE_MACHINE_ARM64,
};
use windows::Win32::System::Threading::{
    CreateMutexW, GetCurrentProcess, IsWow64Process2, OpenProcessToken,
};
use windows::Win32::UI::Shell::{
    FOLDERID_CommonPrograms, FOLDERID_ProgramFiles, IShellLinkW, SHGetKnownFolderPath, ShellLink,
    KF_FLAG_DEFAULT,
};
use windows::Win32::UI::WindowsAndMessaging::{
    SendMessageTimeoutW, HWND_BROADCAST, SMTO_ABORTIFHUNG, WM_SETTINGCHANGE,
};

pub const PRODUCT_NAME: &str = "XFS for Windows";
pub const PRODUCT_KEY: &str = r"SOFTWARE\XFS-for-Windows";
pub const ARP_KEY: &str =
    r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{7C4E2D19-6B8A-4F53-A1D0-3E9B5C7A2F64}";
pub const ENVIRONMENT_KEY: &str =
    r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment";
pub const WINFSP_UPGRADE_CODE: &str = "{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}";
pub const WINFSP_PRODUCT_CODE: &str = "{C79D9B29-3AF0-45B3-9DB9-226F3C2D2204}";
pub const INSTALL_DIRECTORY_NAME: &str = "XFS-for-Windows";
pub const UNINSTALLER_NAME: &str = "uninstall.exe";
pub const EVENT_NAME: &str = "Global\\XFSWinSetup";
const LOCAL_EVENT_NAME: &str = "Local\\XFSWinSetup";

const CREATE_NO_WINDOW: u32 = 0x0800_0000;

fn win32_error(context: &str, code: u32) -> io::Error {
    // Keep a missing registry key distinguishable from a real registry
    // failure: every clean machine has no product key yet, and read_product
    // must treat that as "not installed" rather than aborting setup.
    let kind = match code {
        2 | 3 => io::ErrorKind::NotFound, // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND
        _ => io::ErrorKind::Other,
    };
    io::Error::new(
        kind,
        format!(
            "{context}: {} (0x{code:08x})",
            io::Error::from_raw_os_error(code as i32)
        ),
    )
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

fn from_wide(buffer: &[u16]) -> String {
    let end = buffer.iter().position(|value| *value == 0).unwrap_or(buffer.len());
    String::from_utf16_lossy(&buffer[..end])
}

pub fn is_elevated() -> io::Result<bool> {
    let mut token = HANDLE::default();
    unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token)? };
    let mut elevation = TOKEN_ELEVATION::default();
    let mut returned = 0u32;
    let result = unsafe {
        GetTokenInformation(
            token,
            TokenElevation,
            Some(&mut elevation as *mut TOKEN_ELEVATION as *mut core::ffi::c_void),
            std::mem::size_of::<TOKEN_ELEVATION>() as u32,
            &mut returned,
        )
    };
    unsafe {
        let _ = CloseHandle(token);
    }
    result?;
    Ok(elevation.TokenIsElevated != 0)
}

pub fn expected_pe_machine() -> io::Result<IMAGE_FILE_MACHINE> {
    match crate::payload::SETUP_ARCH {
        "x64" => Ok(IMAGE_FILE_MACHINE_AMD64),
        "arm64" => Ok(IMAGE_FILE_MACHINE_ARM64),
        other => Err(io::Error::new(
            io::ErrorKind::Other,
            format!("unsupported setup architecture: {other}"),
        )),
    }
}

pub fn assert_native_os() -> io::Result<()> {
    let expected = expected_pe_machine()?;
    let mut process_machine = IMAGE_FILE_MACHINE(0);
    let mut native_machine = IMAGE_FILE_MACHINE(0);
    let result = unsafe {
        IsWow64Process2(
            GetCurrentProcess(),
            &mut process_machine,
            Some(&mut native_machine),
        )
    };
    match result {
        Ok(()) => {
            if native_machine != expected {
                return Err(io::Error::new(
                    io::ErrorKind::Unsupported,
                    format!(
                        "this {}-bit setup cannot install the {} payload on native architecture 0x{:04x}",
                        if std::mem::size_of::<usize>() == 8 { 64 } else { 32 },
                        crate::payload::SETUP_ARCH,
                        native_machine.0
                    ),
                ));
            }
            Ok(())
        }
        Err(error) => Err(io::Error::new(
            io::ErrorKind::Other,
            format!("cannot determine the native Windows architecture: {error}"),
        )),
    }
}

pub fn pe_machine(bytes: &[u8]) -> Option<u16> {
    if bytes.len() < 0x40 || &bytes[0..2] != b"MZ" {
        return None;
    }
    let offset = u32::from_le_bytes(bytes[0x3c..0x40].try_into().ok()?) as usize;
    if offset.checked_add(6)? > bytes.len() || &bytes[offset..offset + 4] != b"PE\0\0" {
        return None;
    }
    Some(u16::from_le_bytes(
        bytes[offset + 4..offset + 6].try_into().ok()?,
    ))
}

pub fn known_folder(folder: *const windows::core::GUID) -> io::Result<PathBuf> {
    let raw = unsafe { SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, None)? };
    let text = unsafe { raw.to_string() }.map_err(|error| {
        io::Error::new(io::ErrorKind::InvalidData, format!("known folder is not valid UTF-16: {error}"))
    });
    unsafe { CoTaskMemFree(Some(raw.as_ptr() as *const core::ffi::c_void)) };
    text.map(PathBuf::from)
}

pub fn program_files() -> io::Result<PathBuf> {
    known_folder(&FOLDERID_ProgramFiles)
}

pub fn common_programs() -> io::Result<PathBuf> {
    known_folder(&FOLDERID_CommonPrograms)
}

pub fn system_directory() -> io::Result<PathBuf> {
    let mut buffer = vec![0u16; 1024];
    let length = unsafe { GetSystemDirectoryW(Some(&mut buffer)) } as usize;
    if length == 0 || length >= buffer.len() {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!("GetSystemDirectoryW failed with length {length}"),
        ));
    }
    Ok(PathBuf::from(String::from_utf16_lossy(&buffer[..length])))
}

pub fn install_directory() -> io::Result<PathBuf> {
    Ok(program_files()?.join(INSTALL_DIRECTORY_NAME))
}

pub fn start_menu_directory() -> io::Result<PathBuf> {
    Ok(common_programs()?.join(PRODUCT_NAME))
}

pub fn is_reparse_point(path: &Path) -> bool {
    let name = HSTRING::from(path.as_os_str());
    let attributes = unsafe { GetFileAttributesW(&name) };
    attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT.0) != 0
}

pub fn validate_install_path(path: &Path) -> io::Result<()> {
    if !path.is_absolute() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "install directory must be absolute",
        ));
    }
    if path.parent().is_none() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "install directory must not be a drive root",
        ));
    }
    if path.components().any(|component| matches!(component, Component::ParentDir)) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "install directory must not contain '..'",
        ));
    }
    if is_reparse_point(path) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("refusing to install through a reparse point: {}", path.display()),
        ));
    }
    Ok(())
}

pub fn acquire_mutex() -> io::Result<HANDLE> {
    // Global\ serializes setups across terminal-server sessions.  Creating a
    // global kernel object requires SeCreateGlobalPrivilege; an elevated
    // per-machine setup normally has it, but fall back to the session-local
    // name rather than failing an otherwise valid installation.
    for name in [EVENT_NAME, LOCAL_EVENT_NAME] {
        let handle = match unsafe { CreateMutexW(None, true, &HSTRING::from(name)) } {
            Ok(handle) => handle,
            Err(error) if name == EVENT_NAME => {
                let _ = error;
                continue;
            }
            Err(error) => return Err(error.into()),
        };
        if unsafe { GetLastError() } == ERROR_ALREADY_EXISTS {
            unsafe {
                let _ = CloseHandle(handle);
            }
            return Err(win32_error(
                "another XFS for Windows setup is already running",
                1618,
            ));
        }
        return Ok(handle);
    }
    Err(io::Error::new(
        io::ErrorKind::Other,
        "cannot create the setup mutex",
    ))
}

pub struct MutexGuard(pub HANDLE);

impl Drop for MutexGuard {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

fn reg_open(subkey: &str, access: windows::Win32::System::Registry::REG_SAM_FLAGS) -> io::Result<HKEY> {
    let mut handle = HKEY::default();
    let status = unsafe {
        RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            &HSTRING::from(subkey),
            None,
            access | KEY_WOW64_64KEY,
            &mut handle,
        )
    };
    if status != ERROR_SUCCESS {
        return Err(win32_error(&format!("cannot open HKLM\\{subkey}"), status.0));
    }
    Ok(handle)
}

pub fn reg_string(subkey: &str, value: &str) -> io::Result<Option<String>> {
    let handle = match reg_open(subkey, KEY_READ) {
        Ok(handle) => handle,
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            return Ok(None)
        }
        Err(error) => return Err(error),
    };
    let result = (|| {
        let mut kind = REG_VALUE_TYPE::default();
        let mut length = 0u32;
        let status = unsafe {
            RegQueryValueExW(
                handle,
                &HSTRING::from(value),
                None,
                Some(&mut kind),
                None,
                Some(&mut length),
            )
        };
        if status == ERROR_FILE_NOT_FOUND {
            return Ok(None);
        }
        if status != ERROR_SUCCESS {
            return Err(win32_error(&format!("cannot size HKLM\\{subkey}\\{value}"), status.0));
        }
        if length == 0 {
            return Ok(Some(String::new()));
        }
        let mut buffer = vec![0u8; length as usize];
        let mut actual = length;
        let status = unsafe {
            RegQueryValueExW(
                handle,
                &HSTRING::from(value),
                None,
                Some(&mut kind),
                Some(buffer.as_mut_ptr()),
                Some(&mut actual),
            )
        };
        if status == ERROR_MORE_DATA {
            return Err(io::Error::new(
                io::ErrorKind::Other,
                format!("registry value grew while reading HKLM\\{subkey}\\{value}"),
            ));
        }
        if status != ERROR_SUCCESS {
            return Err(win32_error(&format!("cannot read HKLM\\{subkey}\\{value}"), status.0));
        }
        buffer.truncate(actual as usize);
        if kind == REG_SZ || kind == REG_EXPAND_SZ {
            let units: Vec<u16> = buffer
                .chunks_exact(2)
                .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
                .collect();
            Ok(Some(from_wide(&units)))
        } else if kind == REG_DWORD {
            if buffer.len() < 4 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("HKLM\\{subkey}\\{value} is a short DWORD"),
                ));
            }
            Ok(Some(u32::from_le_bytes(buffer[..4].try_into().unwrap()).to_string()))
        } else {
            Ok(None)
        }
    })();
    unsafe {
        let _ = RegCloseKey(handle);
    }
    result
}

pub fn reg_exists(subkey: &str) -> bool {
    match reg_open(subkey, KEY_READ) {
        Ok(handle) => {
            unsafe {
                let _ = RegCloseKey(handle);
            }
            true
        }
        Err(_) => false,
    }
}

fn create_key(subkey: &str) -> io::Result<HKEY> {
    let mut handle = HKEY::default();
    let status = unsafe {
        RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            &HSTRING::from(subkey),
            None,
            PCWSTR::null(),
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE | KEY_WOW64_64KEY,
            None,
            &mut handle,
            None,
        )
    };
    if status != ERROR_SUCCESS {
        return Err(win32_error(&format!("cannot create HKLM\\{subkey}"), status.0));
    }
    Ok(handle)
}

pub fn reg_write_string(subkey: &str, value: &str, data: &str, expand: bool) -> io::Result<()> {
    let handle = create_key(subkey)?;
    let units = wide(data);
    let bytes: Vec<u8> = units.iter().flat_map(|unit| unit.to_le_bytes()).collect();
    let status = unsafe {
        RegSetValueExW(
            handle,
            &HSTRING::from(value),
            None,
            if expand { REG_EXPAND_SZ } else { REG_SZ },
            Some(&bytes),
        )
    };
    unsafe {
        let _ = RegCloseKey(handle);
    }
    if status != ERROR_SUCCESS {
        return Err(win32_error(&format!("cannot write HKLM\\{subkey}\\{value}"), status.0));
    }
    Ok(())
}

pub fn reg_write_dword(subkey: &str, value: &str, data: u32) -> io::Result<()> {
    let handle = create_key(subkey)?;
    let bytes = data.to_le_bytes();
    let status = unsafe {
        RegSetValueExW(
            handle,
            &HSTRING::from(value),
            None,
            REG_DWORD,
            Some(&bytes),
        )
    };
    unsafe {
        let _ = RegCloseKey(handle);
    }
    if status != ERROR_SUCCESS {
        return Err(win32_error(&format!("cannot write HKLM\\{subkey}\\{value}"), status.0));
    }
    Ok(())
}

pub fn reg_delete_tree(subkey: &str) -> io::Result<()> {
    let status = unsafe { RegDeleteTreeW(HKEY_LOCAL_MACHINE, &HSTRING::from(subkey)) };
    if status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND {
        Ok(())
    } else {
        Err(win32_error(&format!("cannot delete HKLM\\{subkey}"), status.0))
    }
}

pub fn read_machine_path() -> io::Result<(bool, String, u32)> {
    let handle = match reg_open(ENVIRONMENT_KEY, KEY_READ) {
        Ok(handle) => handle,
        Err(_) => return Ok((false, String::new(), REG_EXPAND_SZ.0)),
    };
    let result = (|| {
        let mut kind = REG_VALUE_TYPE::default();
        let mut length = 0u32;
        let status = unsafe {
            RegQueryValueExW(
                handle,
                &HSTRING::from("Path"),
                None,
                Some(&mut kind),
                None,
                Some(&mut length),
            )
        };
        if status == ERROR_FILE_NOT_FOUND {
            return Ok((false, String::new(), REG_EXPAND_SZ.0));
        }
        if status != ERROR_SUCCESS {
            return Err(win32_error("cannot size the machine PATH", status.0));
        }
        let mut buffer = vec![0u8; length as usize];
        let mut actual = length;
        let status = unsafe {
            RegQueryValueExW(
                handle,
                &HSTRING::from("Path"),
                None,
                Some(&mut kind),
                Some(buffer.as_mut_ptr()),
                Some(&mut actual),
            )
        };
        if status != ERROR_SUCCESS {
            return Err(win32_error("cannot read the machine PATH", status.0));
        }
        buffer.truncate(actual as usize);
        let units: Vec<u16> = buffer
            .chunks_exact(2)
            .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
            .collect();
        let text = from_wide(&units);
        let kind_number = if kind == REG_SZ { REG_SZ.0 } else { REG_EXPAND_SZ.0 };
        Ok((true, text, kind_number))
    })();
    unsafe {
        let _ = RegCloseKey(handle);
    }
    result
}

fn normalized_path_entry(entry: &str) -> String {
    let mut value = entry.trim().trim_matches('"').trim().to_string();
    while value.ends_with('\\') || value.ends_with('/') {
        value.pop();
    }
    value
}

fn path_entries(value: &str) -> Vec<String> {
    value
        .split(';')
        .filter(|entry| !entry.trim().is_empty())
        .map(normalized_path_entry)
        .collect()
}

pub fn path_contains(value: &str, directory: &Path) -> bool {
    let target = normalized_path_entry(&directory.to_string_lossy());
    path_entries(value)
        .iter()
        .any(|entry| entry.eq_ignore_ascii_case(&target))
}

pub fn remove_path_entry(value: &str, directory: &Path) -> (String, bool) {
    let target = normalized_path_entry(&directory.to_string_lossy());
    let mut changed = false;
    let kept: Vec<String> = value
        .split(';')
        .filter(|entry| {
            if entry.trim().is_empty() {
                return false;
            }
            let normalized = normalized_path_entry(entry);
            let keep = !normalized.eq_ignore_ascii_case(&target);
            if !keep {
                changed = true;
            }
            keep
        })
        .map(ToString::to_string)
        .collect();
    (kept.join(";"), changed)
}

pub fn append_path_entry(value: &str, directory: &Path) -> (String, bool) {
    if path_contains(value, directory) {
        return (value.to_string(), false);
    }
    let entry = directory.to_string_lossy();
    let mut result = value.trim_end_matches(';').to_string();
    if !result.is_empty() {
        result.push(';');
    }
    result.push_str(&entry);
    (result, true)
}

pub fn write_machine_path(value: &str, kind: u32) -> io::Result<()> {
    let handle = create_key(ENVIRONMENT_KEY)?;
    let units = wide(value);
    let bytes: Vec<u8> = units.iter().flat_map(|unit| unit.to_le_bytes()).collect();
    let value_type = if kind == REG_SZ.0 { REG_SZ } else { REG_EXPAND_SZ };
    let status = unsafe {
        RegSetValueExW(
            handle,
            &HSTRING::from("Path"),
            None,
            value_type,
            Some(&bytes),
        )
    };
    unsafe {
        let _ = RegCloseKey(handle);
    }
    if status != ERROR_SUCCESS {
        return Err(win32_error("cannot write the machine PATH", status.0));
    }
    broadcast_environment_change();
    Ok(())
}

pub fn broadcast_environment_change() {
    let name = wide("Environment");
    unsafe {
        let _ = SendMessageTimeoutW(
            HWND_BROADCAST,
            WM_SETTINGCHANGE,
            windows::Win32::Foundation::WPARAM(0),
            windows::Win32::Foundation::LPARAM(name.as_ptr() as isize),
            SMTO_ABORTIFHUNG,
            5000,
            None,
        );
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ProductState {
    pub install_root: String,
    pub version: String,
    pub architecture: String,
}

pub fn read_product() -> io::Result<Option<ProductState>> {
    let root = reg_string(PRODUCT_KEY, "InstallRoot")?;
    if root.is_none() {
        return Ok(None);
    }
    let version = reg_string(PRODUCT_KEY, "Version")?.unwrap_or_else(|| "0.0.0".to_string());
    let architecture =
        reg_string(PRODUCT_KEY, "Architecture")?.unwrap_or_else(|| crate::payload::SETUP_ARCH.to_string());
    Ok(Some(ProductState {
        install_root: root.unwrap(),
        version,
        architecture,
    }))
}

pub fn write_product(install_root: &Path, version: &str, architecture: &str) -> io::Result<()> {
    reg_write_string(PRODUCT_KEY, "InstallRoot", &install_root.to_string_lossy(), true)?;
    reg_write_string(PRODUCT_KEY, "Version", version, false)?;
    reg_write_string(PRODUCT_KEY, "Architecture", architecture, false)?;

    let root = install_root.to_string_lossy();
    let uninstaller = install_root.join(UNINSTALLER_NAME);
    let quoted_uninstaller = format!("\"{}\"", uninstaller.display());
    reg_write_string(ARP_KEY, "DisplayName", PRODUCT_NAME, false)?;
    reg_write_string(ARP_KEY, "DisplayVersion", version, false)?;
    reg_write_string(
        ARP_KEY,
        "Publisher",
        "XFS-for-Windows contributors",
        false,
    )?;
    reg_write_string(ARP_KEY, "InstallLocation", &root, false)?;
    reg_write_string(
        ARP_KEY,
        "DisplayIcon",
        &install_root.join("xfswin.exe").to_string_lossy(),
        false,
    )?;
    reg_write_string(ARP_KEY, "UninstallString", &format!("{quoted_uninstaller} /uninstall"), false)?;
    reg_write_string(
        ARP_KEY,
        "QuietUninstallString",
        &format!("{quoted_uninstaller} /uninstall /quiet"),
        false,
    )?;
    reg_write_string(
        ARP_KEY,
        "URLInfoAbout",
        "https://github.com/100pangci/XFS-for-Windows",
        false,
    )?;
    reg_write_dword(ARP_KEY, "NoModify", 1)?;
    reg_write_dword(ARP_KEY, "NoRepair", 1)?;
    reg_write_dword(
        ARP_KEY,
        "EstimatedSize",
        directory_size_kb(install_root).min(u32::MAX as u64) as u32,
    )?;
    let now = unsafe { GetLocalTime() };
    reg_write_string(
        ARP_KEY,
        "InstallDate",
        &format!("{:04}{:02}{:02}", now.wYear, now.wMonth, now.wDay),
        false,
    )?;
    Ok(())
}

pub fn remove_product_registration() -> io::Result<()> {
    reg_delete_tree(ARP_KEY)?;
    reg_delete_tree(PRODUCT_KEY)?;
    Ok(())
}

fn directory_size_kb(path: &Path) -> u64 {
    fn walk(path: &Path, total: &mut u64) {
        let Ok(entries) = fs::read_dir(path) else {
            return;
        };
        for entry in entries.flatten() {
            let Ok(metadata) = entry.metadata() else {
                continue;
            };
            if metadata.is_dir() {
                walk(&entry.path(), total);
            } else {
                *total = total.saturating_add(metadata.len());
            }
        }
    }
    let mut bytes = 0u64;
    walk(path, &mut bytes);
    (bytes.saturating_add(1023)) / 1024
}

pub fn create_shortcuts(install_root: &Path) -> io::Result<()> {
    let menu = start_menu_directory()?;
    fs::create_dir_all(&menu)?;
    let system = system_directory()?;
    let cmd = system.join("cmd.exe");
    let uninstaller = install_root.join("xfswin.exe");
    let install_text = install_root.to_string_lossy().to_string();
    create_shortcut(
        &menu.join("XFS for Windows Command Prompt.lnk"),
        &cmd,
        "",
        &install_text,
        "Open a command prompt for the installed xfswin",
        &cmd,
    )?;
    create_shortcut(
        &menu.join("XFS for Windows Documentation.lnk"),
        &install_root.join("docs").join("INSTALL.md"),
        "",
        &install_text,
        "Install, upgrade and automount guide",
        &uninstaller,
    )?;
    Ok(())
}

fn create_shortcut(
    link_path: &Path,
    target: &Path,
    arguments: &str,
    working_directory: &str,
    description: &str,
    icon: &Path,
) -> io::Result<()> {
    if let Some(parent) = link_path.parent() {
        fs::create_dir_all(parent)?;
    }
    let initialized = unsafe { CoInitializeEx(None, COINIT_APARTMENTTHREADED) };
    let should_uninitialize = initialized.is_ok();
    if !should_uninitialize && initialized.0 != 0x8001_0106u32 as i32 {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!("CoInitializeEx failed: 0x{:08x}", initialized.0),
        ));
    }
    let result = (|| {
        let link: IShellLinkW =
            unsafe { CoCreateInstance(&ShellLink, None::<&windows::core::IUnknown>, CLSCTX_INPROC_SERVER)? };
        unsafe {
            link.SetPath(&HSTRING::from(target.as_os_str()))?;
            if !arguments.is_empty() {
                link.SetArguments(&HSTRING::from(arguments))?;
            }
            link.SetWorkingDirectory(&HSTRING::from(working_directory))?;
            link.SetDescription(&HSTRING::from(description))?;
            link.SetIconLocation(&HSTRING::from(icon.as_os_str()), 0)?;
            let persist: IPersistFile = link.cast()?;
            persist.Save(&HSTRING::from(link_path.as_os_str()), true)?;
        }
        Ok(())
    })();
    if should_uninitialize {
        unsafe { CoUninitialize() };
    }
    result
}

pub fn remove_shortcuts() -> io::Result<()> {
    let menu = start_menu_directory()?;
    for name in [
        "XFS for Windows Command Prompt.lnk",
        "XFS for Windows Documentation.lnk",
    ] {
        match fs::remove_file(menu.join(name)) {
            Ok(()) => {}
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => return Err(error),
        }
    }
    match fs::remove_dir(&menu) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        // Leave the folder in place if it contains anything the setup did not
        // create; never recursively delete a user-visible Start Menu folder.
        Err(error) if error.raw_os_error() == Some(145) => Ok(()),
        Err(error) => Err(error),
    }
}

pub fn shortcut_directory_exists() -> bool {
    start_menu_directory().map(|path| path.exists()).unwrap_or(false)
}

#[derive(Clone, Debug)]
pub enum WinFspStatus {
    Missing,
    Installed(String),
    Unsupported(String),
}

pub fn winfsp_status() -> io::Result<WinFspStatus> {
    if let Some(version) = winfsp_version_from_msi()? {
        return Ok(classify_winfsp(version));
    }
    if let Some(version) = winfsp_version_from_registry()? {
        return Ok(classify_winfsp(version));
    }
    Ok(WinFspStatus::Missing)
}

fn classify_winfsp(version: String) -> WinFspStatus {
    let major = version
        .split('.')
        .next()
        .and_then(|part| part.parse::<u32>().ok())
        .unwrap_or(0);
    if major == 2 {
        WinFspStatus::Installed(version)
    } else {
        WinFspStatus::Unsupported(version)
    }
}

fn winfsp_version_from_msi() -> io::Result<Option<String>> {
    let upgrade = HSTRING::from(WINFSP_UPGRADE_CODE);
    let mut buffer = [0u16; 39];
    let mut index = 0u32;
    loop {
        let status = unsafe {
            MsiEnumRelatedProductsW(
                &upgrade,
                None,
                index,
                PWSTR::from_raw(buffer.as_mut_ptr()),
            )
        };
        if status == ERROR_NO_MORE_ITEMS.0 {
            break;
        }
        if status != ERROR_SUCCESS.0 {
            return Ok(None);
        }
        let product = HSTRING::from(from_wide(&buffer));
        if let Some(version) = msi_product_version(&product) {
            return Ok(Some(version));
        }
        index += 1;
    }
    let direct = HSTRING::from(WINFSP_PRODUCT_CODE);
    if let Some(version) = msi_product_version(&direct) {
        return Ok(Some(version));
    }
    Ok(None)
}

fn msi_product_version(product: &HSTRING) -> Option<String> {
    let mut buffer = [0u16; 128];
    let mut length = buffer.len() as u32;
    let status = unsafe {
        MsiGetProductInfoW(
            product,
            INSTALLPROPERTY_VERSIONSTRING,
            Some(PWSTR::from_raw(buffer.as_mut_ptr())),
            Some(&mut length),
        )
    };
    if status == ERROR_SUCCESS.0 && length > 0 {
        Some(from_wide(&buffer))
    } else {
        None
    }
}

fn winfsp_version_from_registry() -> io::Result<Option<String>> {
    let key64 = format!(
        r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{WINFSP_PRODUCT_CODE}"
    );
    if let Some(version) = reg_string(&key64, "DisplayVersion")? {
        return Ok(Some(version));
    }
    let key32 = format!(
        r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\{WINFSP_PRODUCT_CODE}"
    );
    reg_string(&key32, "DisplayVersion")
}

pub fn install_winfsp(msi: &Path, log_path: &Path) -> io::Result<u32> {
    let msiexec = system_directory()?.join("msiexec.exe");
    let mut last_code = 0u32;
    for attempt in 0..3 {
        let output = Command::new(&msiexec)
            .args([
                OsStr::new("/i"),
                msi.as_os_str(),
                OsStr::new("/qn"),
                OsStr::new("/norestart"),
                OsStr::new("/l*v"),
                log_path.as_os_str(),
            ])
            .creation_flags(CREATE_NO_WINDOW)
            .output()?;
        let code = output.status.code().unwrap_or(-1) as u32;
        last_code = code;
        if code == 0 || code == 3010 || code == 1641 {
            return Ok(code);
        }
        if code != 1618 || attempt == 2 {
            break;
        }
        std::thread::sleep(Duration::from_secs(5));
    }
    Err(win32_error(
        "WinFsp installation failed",
        if last_code == 0 { 1603 } else { last_code },
    ))
}

pub struct CliOutput {
    pub code: i32,
    pub stdout: String,
    pub stderr: String,
}

pub fn run_cli(exe: &Path, args: &[&str]) -> io::Result<CliOutput> {
    if !exe.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("missing executable: {}", exe.display()),
        ));
    }
    let mut command = Command::new(exe);
    command.args(args).creation_flags(CREATE_NO_WINDOW);
    if let Some(parent) = exe.parent() {
        command.current_dir(parent);
    }
    let output: Output = command.output()?;
    Ok(CliOutput {
        code: output.status.code().unwrap_or(-1),
        stdout: String::from_utf8_lossy(&output.stdout).into_owned(),
        stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
    })
}

pub fn sha256_file(path: &Path) -> io::Result<String> {
    let mut file = File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(hex_digest(&hasher.finalize()))
}

fn hex_digest(digest: &[u8]) -> String {
    use std::fmt::Write as _;
    let mut text = String::with_capacity(digest.len() * 2);
    for byte in digest {
        let _ = write!(text, "{byte:02x}");
    }
    text
}

pub fn copy_directory(source: &Path, destination: &Path) -> io::Result<()> {
    fs::create_dir_all(destination)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let file_type = entry.file_type()?;
        if file_type.is_symlink() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("refusing to copy symbolic link: {}", entry.path().display()),
            ));
        }
        let target = destination.join(entry.file_name());
        if file_type.is_dir() {
            copy_directory(&entry.path(), &target)?;
        } else if file_type.is_file() {
            fs::copy(entry.path(), &target)?;
        }
    }
    Ok(())
}

pub fn move_or_copy_directory(source: &Path, destination: &Path) -> io::Result<()> {
    if destination.exists() {
        return Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            format!("backup path already exists: {}", destination.display()),
        ));
    }
    match fs::rename(source, destination) {
        Ok(()) => Ok(()),
        Err(_) => {
            copy_directory(source, destination)?;
            fs::remove_dir_all(source)
        }
    }
}

pub fn remove_directory_with_retry(path: &Path) -> io::Result<()> {
    if !path.exists() {
        return Ok(());
    }
    let mut last = None;
    for attempt in 0..10 {
        match fs::remove_dir_all(path) {
            Ok(()) => return Ok(()),
            Err(error) => {
                last = Some(error);
                std::thread::sleep(Duration::from_millis(if attempt < 5 { 250 } else { 1000 }));
            }
        }
    }
    Err(last.unwrap())
}

pub fn copy_current_executable(destination: &Path) -> io::Result<()> {
    let current = std::env::current_exe()?;
    fs::copy(current, destination)?;
    Ok(())
}

pub fn write_json<T: Serialize>(path: &Path, value: &T) -> io::Result<()> {
    let bytes = serde_json::to_vec_pretty(value)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
    fs::write(path, bytes)
}

pub fn parse_version(value: &str) -> Option<(u64, u64, u64)> {
    let mut parts = value.split('.');
    let major = parts.next()?.parse::<u64>().ok()?;
    let minor = parts.next().unwrap_or("0").parse::<u64>().ok()?;
    let patch = parts.next().unwrap_or("0").parse::<u64>().ok()?;
    Some((major, minor, patch))
}

pub fn version_is_newer(candidate: &str, current: &str) -> bool {
    match (parse_version(candidate), parse_version(current)) {
        (Some(candidate), Some(current)) => candidate > current,
        _ => false,
    }
}

pub fn current_pid() -> u32 {
    std::process::id()
}

pub fn wait_for_process_exit(pid: u32, timeout: Duration) -> bool {
    use windows::Win32::Foundation::WAIT_OBJECT_0;
    use windows::Win32::System::Threading::{
        OpenProcess, WaitForSingleObject, PROCESS_SYNCHRONIZE,
    };
    let handle = match unsafe { OpenProcess(PROCESS_SYNCHRONIZE, false, pid) } {
        Ok(handle) => handle,
        Err(_) => return true,
    };
    let result = unsafe { WaitForSingleObject(handle, timeout.as_millis() as u32) } == WAIT_OBJECT_0;
    unsafe {
        let _ = CloseHandle(handle);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn win32_error_marks_missing_keys_as_not_found() {
        assert_eq!(win32_error("open", 2).kind(), io::ErrorKind::NotFound);
        assert_eq!(win32_error("open", 3).kind(), io::ErrorKind::NotFound);
        assert_eq!(win32_error("open", 5).kind(), io::ErrorKind::Other);
    }
}
