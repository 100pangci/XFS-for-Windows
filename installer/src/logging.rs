// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use windows::Win32::System::Console::{AttachConsole, ATTACH_PARENT_PROCESS};

fn timestamp() -> String {
    let seconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or(0);
    format!("{seconds}")
}

fn default_path() -> PathBuf {
    let pid = std::process::id();
    let seconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or(0);
    std::env::temp_dir().join(format!("xfswin-setup-{pid}-{seconds}.log"))
}

fn open_console() -> Option<File> {
    unsafe {
        let _ = AttachConsole(ATTACH_PARENT_PROCESS);
    }
    OpenOptions::new().write(true).open("CONOUT$").ok()
}

pub struct Log {
    file: Option<File>,
    path: PathBuf,
    console: Option<File>,
    quiet: bool,
}

impl Log {
    pub fn new(requested: Option<&Path>, quiet: bool) -> Self {
        let path = requested
            .map(Path::to_path_buf)
            .unwrap_or_else(default_path);
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                let _ = std::fs::create_dir_all(parent);
            }
        }
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)
            .ok();
        let console = if quiet { None } else { open_console() };
        let mut log = Self {
            file,
            path,
            console,
            quiet,
        };
        log.line(format!("log: {}", log.path.display()));
        log
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn line(&mut self, message: impl AsRef<str>) {
        let line = format!("[{}] {}\r\n", timestamp(), message.as_ref());
        if let Some(file) = self.file.as_mut() {
            let _ = file.write_all(line.as_bytes());
            let _ = file.flush();
        }
        if !self.quiet {
            if let Some(console) = self.console.as_mut() {
                let _ = console.write_all(line.as_bytes());
                let _ = console.flush();
            }
        }
    }

    pub fn bytes(&mut self, label: &str, data: &[u8]) {
        let text = String::from_utf8_lossy(data);
        for line in text.lines() {
            self.line(format!("{label}: {line}"));
        }
    }
}

pub fn write_console(text: &str) {
    unsafe {
        let _ = AttachConsole(ATTACH_PARENT_PROCESS);
    }
    if let Ok(mut console) = OpenOptions::new().write(true).open("CONOUT$") {
        let _ = console.write_all(text.as_bytes());
        let _ = console.write_all(b"\r\n");
        let _ = console.flush();
    }
}

