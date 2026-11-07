// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

use std::path::PathBuf;

pub const USAGE: &str = "\
XFS for Windows Setup

Usage:
  xfswin-setup.exe [options]

Options:
  /quiet, /silent       Install or uninstall without any UI.
  /passive              Show progress but do not ask questions.
  /uninstall, /remove   Uninstall instead of install.
  /log <path>           Write the setup log to <path>.
  /norestart            Accepted for compatibility; the setup never reboots.
  /help, /?             Show this help.
  /version              Show the setup version.

The setup is a native per-machine installer.  It installs to
%ProgramFiles%\\XFS-for-Windows, keeps %ProgramData%\\xfswin unchanged,
and never uninstalls WinFsp.
";

#[derive(Clone, Debug, Default)]
pub struct Options {
    pub quiet: bool,
    pub passive: bool,
    pub uninstall: bool,
    pub log_path: Option<PathBuf>,
    pub test_fail: Option<String>,
    pub help: bool,
    pub version: bool,
    pub from_copy: bool,
    pub parent_pid: Option<u32>,
    pub confirmed: bool,
}

pub enum ParseResult {
    Run(Options),
    Help,
    Version,
    Error(String),
}

fn take_value(
    parts: &mut std::slice::Iter<'_, String>,
    inline: Option<String>,
) -> Result<String, String> {
    if let Some(value) = inline {
        if value.is_empty() {
            return Err("missing value after '='".to_string());
        }
        return Ok(value);
    }
    match parts.next() {
        Some(value) if !value.starts_with('/') && !value.starts_with('-') => Ok(value.clone()),
        _ => Err("missing option value".to_string()),
    }
}

pub fn parse<I>(args: I) -> ParseResult
where
    I: IntoIterator<Item = String>,
{
    let mut options = Options::default();
    let args: Vec<String> = args.into_iter().collect();
    let mut parts = args.iter();
    while let Some(raw) = parts.next() {
        if raw.is_empty() {
            return ParseResult::Error("empty argument".to_string());
        }
        let mut token = raw.as_str();
        if let Some(rest) = token.strip_prefix("--") {
            token = rest;
        } else if let Some(rest) = token.strip_prefix('/') {
            token = rest;
        } else if let Some(rest) = token.strip_prefix('-') {
            token = rest;
        } else {
            return ParseResult::Error(format!("unexpected argument: {raw}"));
        }
        let (name, inline) = match token.split_once('=') {
            Some((name, value)) => (name, Some(value.to_string())),
            None => (token, None),
        };
        match name.to_ascii_lowercase().as_str() {
            "quiet" | "silent" | "q" => options.quiet = true,
            "passive" => options.passive = true,
            "uninstall" | "remove" => options.uninstall = true,
            "log" | "logfile" => match take_value(&mut parts, inline) {
                Ok(value) => options.log_path = Some(PathBuf::from(value)),
                Err(error) => return ParseResult::Error(format!("/log: {error}")),
            },
            "norestart" => {}
            "help" | "h" | "?" => options.help = true,
            "version" => options.version = true,
            "from-copy" => options.from_copy = true,
            "confirmed" => options.confirmed = true,
            "parent-pid" => {
                let value = match take_value(&mut parts, inline) {
                    Ok(value) => value,
                    Err(error) => return ParseResult::Error(format!("/parent-pid: {error}")),
                };
                match value.parse::<u32>() {
                    Ok(pid) => options.parent_pid = Some(pid),
                    Err(_) => {
                        return ParseResult::Error(format!("/parent-pid is not a PID: {value}"))
                    }
                }
            }
            "test-fail" => {
                if cfg!(feature = "test-fail") {
                    match take_value(&mut parts, inline) {
                        Ok(value) => options.test_fail = Some(value),
                        Err(error) => return ParseResult::Error(format!("/test-fail: {error}")),
                    }
                } else {
                    return ParseResult::Error(
                        "/test-fail is only available in the acceptance test build".to_string(),
                    );
                }
            }
            other => return ParseResult::Error(format!("unknown option: {other}")),
        }
    }

    if options.help {
        return ParseResult::Help;
    }
    if options.version {
        return ParseResult::Version;
    }
    if options.quiet && options.passive {
        return ParseResult::Error("/quiet and /passive are mutually exclusive".to_string());
    }
    if let Some(stage) = options.test_fail.as_deref() {
        let valid = [
            "before-service",
            "after-files",
            "service-start",
            "verify",
        ];
        if !valid.contains(&stage) {
            return ParseResult::Error(format!("unknown /test-fail stage: {stage}"));
        }
        if options.uninstall {
            return ParseResult::Error("/test-fail is not supported for uninstall".to_string());
        }
        if !options.quiet && !options.passive {
            return ParseResult::Error(
                "/test-fail requires /quiet or /passive".to_string(),
            );
        }
    }
    if options.from_copy && !options.uninstall {
        return ParseResult::Error("/from-copy is only valid with /uninstall".to_string());
    }
    if options.confirmed && !options.uninstall {
        return ParseResult::Error("/confirmed is only valid with /uninstall".to_string());
    }
    ParseResult::Run(options)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(args: &[&str]) -> Options {
        match parse(args.iter().map(|value| value.to_string())) {
            ParseResult::Run(options) => options,
            _ => panic!("expected successful parse"),
        }
    }

    #[test]
    fn parses_quiet_install() {
        let options = run(&["/quiet", "/log", "C:\\temp\\setup.log"]);
        assert!(options.quiet);
        assert!(!options.uninstall);
        assert_eq!(
            options.log_path.as_deref(),
            Some(std::path::Path::new("C:\\temp\\setup.log"))
        );
    }

    #[test]
    fn parses_equals_and_double_dash_forms() {
        let options = run(&["--quiet", "/log=C:\\temp\\a.log", "/parent-pid=42"]);
        assert!(options.quiet);
        assert_eq!(options.parent_pid, Some(42));
    }

    #[test]
    fn rejects_unknown_option() {
        match parse(["/bogus".to_string()]) {
            ParseResult::Error(message) => assert!(message.contains("unknown option")),
            _ => panic!("expected an error"),
        }
    }

    #[test]
    fn test_fail_requires_test_build_and_quiet() {
        if cfg!(feature = "test-fail") {
            let options = run(&["/quiet", "/test-fail", "verify"]);
            assert_eq!(options.test_fail.as_deref(), Some("verify"));
        } else {
            match parse([
                "/quiet".to_string(),
                "/test-fail".to_string(),
                "verify".to_string(),
            ]) {
                ParseResult::Error(message) => assert!(message.contains("test build")),
                _ => panic!("expected an error"),
            }
        }
    }
}
