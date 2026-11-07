// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::args::Options;
use crate::logging::Log;
use crate::payload::{self, ExtractedPayload, SETUP_ARCH, SETUP_VERSION};
use crate::ui;
use crate::win;

const ERROR_INSTALL_FAILURE: u32 = 1603;
const ERROR_INSTALL_ALREADY_RUNNING: u32 = 1618;
const ERROR_INSTALL_USER_EXIT: u32 = 1602;
const ERROR_INSTALL_PLATFORM_UNSUPPORTED: u32 = 1633;
const ERROR_INSTALL_VERSION_MISMATCH: u32 = 1638;
const ERROR_ACCESS_DENIED: u32 = 5;
const PINNED_WINFSP_VERSION: &str = "2.1.25156";

pub struct Outcome {
    pub code: u32,
    pub message: String,
    pub show_ui: bool,
}

pub struct Failure {
    pub code: u32,
    pub message: String,
}

impl Failure {
    fn new(code: u32, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }

    fn io(context: &str, error: std::io::Error) -> Self {
        Self::new(ERROR_INSTALL_FAILURE, format!("{context}: {error}"))
    }
}

fn setup_guard() -> Result<win::MutexGuard, Failure> {
    match win::acquire_mutex() {
        Ok(handle) => Ok(win::MutexGuard(handle)),
        Err(error) => Err(Failure::new(
            ERROR_INSTALL_ALREADY_RUNNING,
            format!("another setup is running: {error}"),
        )),
    }
}

fn preflight() -> Result<(), Failure> {
    match win::is_elevated() {
        Ok(true) => {}
        Ok(false) => {
            return Err(Failure::new(
                ERROR_ACCESS_DENIED,
                "Administrator privileges are required to install XFS for Windows.",
            ))
        }
        Err(error) => {
            return Err(Failure::io(
                "cannot determine whether the setup is elevated",
                error,
            ))
        }
    }
    if let Err(error) = win::assert_native_os() {
        return Err(Failure::new(
            ERROR_INSTALL_PLATFORM_UNSUPPORTED,
            error.to_string(),
        ));
    }
    if payload::PAYLOAD_ZIP.is_empty() {
        return Err(Failure::new(
            ERROR_INSTALL_FAILURE,
            "this development setup has no embedded payload",
        ));
    }
    Ok(())
}

pub fn run(options: &Options, log: &mut Log) -> Result<Outcome, Failure> {
    if options.uninstall {
        run_uninstall(options, log)
    } else {
        run_install(options, log)
    }
}

fn run_install(options: &Options, log: &mut Log) -> Result<Outcome, Failure> {
    preflight()?;
    let _guard = setup_guard()?;
    let product = win::read_product()
        .map_err(|error| Failure::io("cannot read the installed product state", error))?;

    if let Some(previous) = product.as_ref() {
        let installed = win::parse_version(&previous.version).ok_or_else(|| {
            Failure::new(
                ERROR_INSTALL_FAILURE,
                format!("the installed version '{}' is not valid", previous.version),
            )
        })?;
        let package = win::parse_version(SETUP_VERSION).ok_or_else(|| {
            Failure::new(
                ERROR_INSTALL_FAILURE,
                format!("the setup version '{SETUP_VERSION}' is not valid"),
            )
        })?;
        if installed == package {
            log.line(format!(
                "XFS for Windows {} is already installed at {}",
                previous.version, previous.install_root
            ));
            return Ok(Outcome {
                code: 0,
                message: format!("XFS for Windows {} is already installed.", previous.version),
                show_ui: true,
            });
        }
        if installed > package {
            return Err(Failure::new(
                ERROR_INSTALL_VERSION_MISMATCH,
                format!(
                    "a newer version ({}) is already installed; this setup only installs {}",
                    previous.version, SETUP_VERSION
                ),
            ));
        }
    }

    let action = if product.is_some() { "upgrade" } else { "install" };
    let install_dir = match product.as_ref() {
        Some(previous) => PathBuf::from(&previous.install_root),
        None => win::install_directory()
            .map_err(|error| Failure::io("cannot locate the Program Files directory", error))?,
    };
    if let Err(error) = win::validate_install_path(&install_dir) {
        return Err(Failure::new(
            ERROR_INSTALL_FAILURE,
            format!("unsafe install directory: {error}"),
        ));
    }
    ensure_under_program_files(&install_dir)?;
    if product.is_some() && !install_dir.join("xfswin.exe").is_file() {
        return Err(Failure::new(
            ERROR_INSTALL_FAILURE,
            format!(
                "the installed files are missing from {}; uninstall the broken installation first",
                install_dir.display()
            ),
        ));
    }

    if !options.quiet && !options.passive {
        let previous_version = product.as_ref().map(|previous| previous.version.as_str());
        if !ui::confirm_install(SETUP_VERSION, SETUP_ARCH, &install_dir, previous_version) {
            log.line("the user cancelled setup");
            return Ok(Outcome {
                code: ERROR_INSTALL_USER_EXIT,
                message: "Setup was cancelled.".to_string(),
                show_ui: false,
            });
        }
    }
    let _progress = if options.quiet {
        None
    } else {
        Some(ui::Progress::start(format!(
            "Installing XFS for Windows {SETUP_VERSION} ({SETUP_ARCH}).\r\n\r\n\
             The service, Start Menu shortcuts and machine PATH will be updated. \
             This can take several minutes."
        )))
    };

    let winfsp = win::winfsp_status()
        .map_err(|error| Failure::io("cannot determine the WinFsp state", error))?;
    let install_winfsp = match &winfsp {
        win::WinFspStatus::Missing => true,
        win::WinFspStatus::Installed(version) => {
            log.line(format!("found WinFsp {version}"));
            win::version_is_newer(PINNED_WINFSP_VERSION, version)
        }
        win::WinFspStatus::Unsupported(version) => {
            return Err(Failure::new(
                ERROR_INSTALL_FAILURE,
                format!(
                    "WinFsp {version} is not supported; XFS for Windows requires WinFsp 2.x. \
                     Uninstall the unsupported WinFsp version and run setup again."
                ),
            ))
        }
    };
    if install_winfsp {
        log.line(format!(
            "WinFsp 2.x is missing or older than {PINNED_WINFSP_VERSION}; the pinned MSI will be installed"
        ));
    }

    let staging = payload::new_staging_directory(&install_dir)
        .map_err(|error| Failure::io("cannot create the setup staging directory", error))?;
    log.line(format!("staging directory: {}", staging.display()));

    let extracted = match payload::extract(&staging, log) {
        Ok(extracted) => extracted,
        Err(error) => {
            payload::cleanup(&staging, log);
            return Err(Failure::io("cannot extract the embedded payload", error));
        }
    };

    if install_winfsp {
        let msi_log = PathBuf::from(format!("{}.winfsp.msi.log", log.path().display()));
        log.line(format!("installing the bundled WinFsp MSI: {}", extracted.winfsp_msi.display()));
        match win::install_winfsp(&extracted.winfsp_msi, &msi_log) {
            Ok(code) => {
                if code == 3010 || code == 1641 {
                    log.line("WinFsp requested a reboot; continuing without rebooting");
                } else {
                    log.line("WinFsp installation completed");
                }
            }
            Err(error) => {
                payload::cleanup(&staging, log);
                return Err(Failure::new(
                    ERROR_INSTALL_FAILURE,
                    format!(
                        "the required WinFsp dependency could not be installed: {error}. \
                         See the MSI log at {}.",
                        msi_log.display()
                    ),
                ));
            }
        }
    }

    if let Err(failure) = run_installer_step(
        &extracted.xfswin,
        "prepare",
        action,
        &install_dir,
        None,
        log,
    ) {
        log.line("prepare failed; asking the installer library to restore service state");
        let _ = run_installer_step(
            &extracted.xfswin,
            "rollback",
            "auto",
            &install_dir,
            None,
            log,
        );
        payload::cleanup(&staging, log);
        return Err(failure);
    }

    let backup = if product.is_some() {
        match payload::new_backup_directory(&install_dir) {
            Ok(path) => Some(path),
            Err(error) => {
                let failure = Failure::io("cannot allocate the upgrade backup directory", error);
                let _ = run_installer_step(
                    &extracted.xfswin,
                    "rollback",
                    "auto",
                    &install_dir,
                    None,
                    log,
                );
                payload::cleanup(&staging, log);
                return Err(failure);
            }
        }
    } else {
        None
    };

    let state_path = staging.join("setup-state.json");
    let mut state = match SetupState::capture(
        &install_dir,
        product.as_ref(),
        backup.as_deref(),
        &staging,
        action,
    ) {
        Ok(state) => state,
        Err(error) => {
            let failure = Failure::io("cannot capture the pre-install state", error);
            let _ = run_installer_step(
                &extracted.xfswin,
                "rollback",
                "auto",
                &install_dir,
                None,
                log,
            );
            payload::cleanup(&staging, log);
            return Err(failure);
        }
    };
    if let Err(error) = win::write_json(&state_path, &state) {
        let failure = Failure::io("cannot persist the setup transaction state", error);
        let failure = rollback(
            &install_dir,
            &staging,
            &extracted.xfswin,
            &state,
            log,
            failure,
        );
        payload::cleanup(&staging, log);
        return Err(failure);
    }

    log.line(format!(
        "transaction: action={action}, previous_version={}, backup={}",
        state
            .previous_version
            .as_deref()
            .unwrap_or("<none>"),
        state
            .backup_dir
            .as_deref()
            .unwrap_or("<none>")
    ));

    let result = install_transaction(
        options,
        log,
        &install_dir,
        &extracted,
        &staging,
        &state_path,
        &mut state,
        product.is_some(),
        backup.as_deref(),
        action,
    );
    match result {
        Ok(()) => {
            payload::cleanup(&staging, log);
            if let Some(backup) = backup.as_deref() {
                if let Err(error) = win::remove_directory_with_retry(backup) {
                    log.line(format!(
                        "warning: cannot remove the old installation backup {}: {error}",
                        backup.display()
                    ));
                }
            }
            log.line(format!("XFS for Windows {SETUP_VERSION} installed successfully"));
            Ok(Outcome {
                code: 0,
                message: format!(
                    "XFS for Windows {SETUP_VERSION} was installed successfully.\r\n\r\n{}",
                    install_dir.display()
                ),
                show_ui: true,
            })
        }
        Err(failure) => {
            let failure = rollback(
                &install_dir,
                &staging,
                &extracted.xfswin,
                &state,
                log,
                failure,
            );
            payload::cleanup(&staging, log);
            Err(failure)
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn install_transaction(
    options: &Options,
    log: &mut Log,
    install_dir: &Path,
    extracted: &ExtractedPayload,
    staging: &Path,
    state_path: &Path,
    state: &mut SetupState,
    upgrading: bool,
    backup: Option<&Path>,
    action: &str,
) -> Result<(), Failure> {
    if upgrading {
        let backup = backup.ok_or_else(|| {
            Failure::new(ERROR_INSTALL_FAILURE, "missing upgrade backup directory")
        })?;
        log.line(format!(
            "moving the previous installation to {}",
            backup.display()
        ));
        win::move_or_copy_directory(install_dir, backup).map_err(|error| {
            Failure::io(
                "cannot move the previous installation into its backup directory",
                error,
            )
        })?;
        state.files_moved = true;
        persist_state(state_path, state)?;
    }

    fs::create_dir_all(install_dir).map_err(|error| {
        Failure::io(
            &format!("cannot create install directory {}", install_dir.display()),
            error,
        )
    })?;
    state.new_files_installed = true;
    persist_state(state_path, state)?;
    log.line(format!(
        "copying {} product files to {}",
        extracted.app.display(),
        install_dir.display()
    ));
    win::copy_directory(&extracted.app, install_dir)
        .map_err(|error| Failure::io("cannot copy the product files", error))?;

    let installed_xfswin = install_dir.join("xfswin.exe");
    let installed_hash = win::sha256_file(&installed_xfswin)
        .map_err(|error| Failure::io("cannot hash the installed xfswin.exe", error))?;
    let staged_hash = win::sha256_file(&extracted.xfswin)
        .map_err(|error| Failure::io("cannot hash the staged xfswin.exe", error))?;
    if !installed_hash.eq_ignore_ascii_case(&staged_hash) {
        return Err(Failure::new(
            ERROR_INSTALL_FAILURE,
            "the installed xfswin.exe does not match the verified payload",
        ));
    }
    win::copy_current_executable(&install_dir.join(win::UNINSTALLER_NAME))
        .map_err(|error| Failure::io("cannot install the uninstaller", error))?;

    register(log, install_dir, state, state_path)?;

    let apply_failure = match options.test_fail.as_deref() {
        Some(stage @ ("before-service" | "after-files" | "service-start")) => Some(stage),
        _ => None,
    };
    if let Some(stage) = apply_failure {
        log.line(format!("test build: injecting apply failure at {stage}"));
    }
    run_installer_step(
        &installed_xfswin,
        "apply",
        action,
        install_dir,
        apply_failure,
        log,
    )?;

    let verify_failure = if options.test_fail.as_deref() == Some("verify") {
        Some("verify")
    } else {
        None
    };
    if let Some(stage) = verify_failure {
        log.line(format!("test build: injecting verify failure at {stage}"));
    }
    let verify_result = run_installer_step(
        &installed_xfswin,
        "verify",
        action,
        install_dir,
        verify_failure,
        log,
    );
    if verify_failure.is_some() {
        return match verify_result {
            Ok(()) => Err(Failure::new(
                ERROR_INSTALL_FAILURE,
                "the test verify failure was not injected",
            )),
            Err(failure) => Err(failure),
        };
    }
    verify_result?;

    run_installer_step(
        &installed_xfswin,
        "commit",
        "auto",
        install_dir,
        None,
        log,
    )?;
    let _ = staging;
    Ok(())
}

fn register(
    log: &mut Log,
    install_dir: &Path,
    state: &mut SetupState,
    state_path: &Path,
) -> Result<(), Failure> {
    let (new_path, changed) = win::append_path_entry(&state.path_value, install_dir);
    if changed {
        log.line("adding the product directory to the machine PATH");
        win::write_machine_path(&new_path, state.path_type)
            .map_err(|error| Failure::io("cannot update the machine PATH", error))?;
    }
    log.line("writing product and Add/Remove Programs registration");
    win::write_product(install_dir, SETUP_VERSION, SETUP_ARCH)
        .map_err(|error| Failure::io("cannot write product registration", error))?;
    log.line("creating Start Menu shortcuts");
    win::create_shortcuts(install_dir)
        .map_err(|error| Failure::io("cannot create Start Menu shortcuts", error))?;
    state.registration_written = true;
    persist_state(state_path, state)
}

fn persist_state(path: &Path, state: &SetupState) -> Result<(), Failure> {
    win::write_json(path, state)
        .map_err(|error| Failure::io("cannot persist the setup transaction state", error))
}

fn run_installer_step(
    exe: &Path,
    verb: &str,
    action: &str,
    install_dir: &Path,
    failure_point: Option<&str>,
    log: &mut Log,
) -> Result<(), Failure> {
    let install_path = install_dir.join("xfswin.exe");
    let install_text = install_path.to_string_lossy().into_owned();
    let mut args: Vec<&str> = vec![
        "installer",
        verb,
        "--action",
        action,
        "--install-path",
        &install_text,
    ];
    if let Some(point) = failure_point {
        args.push("--fail");
        args.push(point);
    }
    log.line(format!(
        "running: \"{}\" {}",
        exe.display(),
        args.join(" ")
    ));
    let output = win::run_cli(exe, &args).map_err(|error| {
        Failure::io(
            &format!("cannot run the installer helper {}", exe.display()),
            error,
        )
    })?;
    log.bytes("installer stdout", output.stdout.as_bytes());
    log.bytes("installer stderr", output.stderr.as_bytes());
    if output.code == 0 {
        return Ok(());
    }
    let detail = output
        .stderr
        .lines()
        .chain(output.stdout.lines())
        .find(|line| !line.trim().is_empty())
        .unwrap_or("no diagnostic output");
    Err(Failure::new(
        ERROR_INSTALL_FAILURE,
        format!(
            "installer {verb} failed with exit code {}: {}",
            output.code,
            detail.trim()
        ),
    ))
}

fn rollback(
    install_dir: &Path,
    staging: &Path,
    staged_xfswin: &Path,
    state: &SetupState,
    log: &mut Log,
    original: Failure,
) -> Failure {
    log.line(format!("rolling back after: {}", original.message));
    let mut problems: Vec<String> = Vec::new();

    let installed_exe = install_dir.join("xfswin.exe");
    let quiesce_helper = if installed_exe.is_file() {
        installed_exe.as_path()
    } else {
        staged_xfswin
    };
    if let Err(failure) = run_installer_step(
        quiesce_helper,
        "rollback-quiesce",
        "auto",
        install_dir,
        None,
        log,
    ) {
        problems.push(format!("rollback-quiesce: {}", failure.message));
    }

    if state.product_present {
        if state.files_moved {
            match win::remove_directory_with_retry(install_dir) {
                Ok(()) => {}
                Err(error) => problems.push(format!(
                    "cannot remove the partial new installation {}: {error}",
                    install_dir.display()
                )),
            }
            if let Some(backup) = state.backup_dir.as_deref() {
                match win::move_or_copy_directory(Path::new(backup), install_dir) {
                    Ok(()) => log.line("restored the previous installation files"),
                    Err(error) => problems.push(format!(
                        "cannot restore the previous installation from {backup}: {error}"
                    )),
                }
            } else {
                problems.push("the upgrade state has no backup directory".to_string());
            }
        } else if let Some(backup) = state.backup_dir.as_deref() {
            if Path::new(backup).exists() {
                if let Err(error) = win::remove_directory_with_retry(Path::new(backup)) {
                    log.line(format!(
                        "warning: cannot remove partial backup {backup}: {error}"
                    ));
                }
            }
        }
    } else if state.new_files_installed {
        if let Err(error) = win::remove_directory_with_retry(install_dir) {
            problems.push(format!(
                "cannot remove the new installation {}: {error}",
                install_dir.display()
            ));
        }
    }

    if state.product_present {
        if let (Some(root), Some(version), Some(architecture)) = (
            state.previous_install_root.as_deref(),
            state.previous_version.as_deref(),
            state.previous_architecture.as_deref(),
        ) {
            if let Err(error) = win::write_product(Path::new(root), version, architecture) {
                problems.push(format!("cannot restore product registration: {error}"));
            }
        } else {
            problems.push("the upgrade state is missing the previous product registration".to_string());
        }
    } else if let Err(error) = win::remove_product_registration() {
        problems.push(format!("cannot remove product registration: {error}"));
    }

    if let Err(error) = win::write_machine_path(&state.path_value, state.path_type) {
        problems.push(format!("cannot restore the machine PATH: {error}"));
    }
    if !state.start_menu_present {
        if let Err(error) = win::remove_shortcuts() {
            problems.push(format!("cannot remove Start Menu shortcuts: {error}"));
        }
    } else if install_dir.join("xfswin.exe").is_file() {
        if let Err(error) = win::create_shortcuts(install_dir) {
            log.line(format!("warning: cannot refresh Start Menu shortcuts: {error}"));
        }
    }

    let rollback_helper = if install_dir.join("xfswin.exe").is_file() {
        install_dir.join("xfswin.exe")
    } else {
        staged_xfswin.to_path_buf()
    };
    if let Err(failure) = run_installer_step(
        &rollback_helper,
        "rollback",
        "auto",
        install_dir,
        None,
        log,
    ) {
        problems.push(format!("rollback: {}", failure.message));
    }

    let _ = staging;
    if problems.is_empty() {
        log.line("rollback completed");
        Failure::new(
            original.code,
            format!("{} (the installation was rolled back)", original.message),
        )
    } else {
        for problem in &problems {
            log.line(format!("rollback problem: {problem}"));
        }
        Failure::new(
            original.code,
            format!(
                "{} (rollback was incomplete: {})",
                original.message,
                problems.join("; ")
            ),
        )
    }
}

fn ensure_under_program_files(path: &Path) -> Result<(), Failure> {
    let program_files = win::program_files()
        .map_err(|error| Failure::io("cannot locate the Program Files directory", error))?;
    let base = normalize_for_compare(&program_files);
    let candidate = normalize_for_compare(path);
    if !candidate.starts_with(&(base + "\\")) {
        return Err(Failure::new(
            ERROR_INSTALL_FAILURE,
            format!(
                "refusing to install outside Program Files: {}",
                path.display()
            ),
        ));
    }
    Ok(())
}

fn normalize_for_compare(path: &Path) -> String {
    let mut text = path.to_string_lossy().replace('/', "\\").to_ascii_lowercase();
    while text.len() > 3 && text.ends_with('\\') {
        text.pop();
    }
    text
}

fn current_installed_location() -> Option<PathBuf> {
    let current = std::env::current_exe().ok()?;
    let parent = current.parent()?;
    if parent.join("xfswin.exe").is_file() {
        Some(parent.to_path_buf())
    } else {
        None
    }
}

fn new_temp_name() -> String {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or(0);
    format!("{}-{nanos:x}", win::current_pid())
}

fn run_uninstall(options: &Options, log: &mut Log) -> Result<Outcome, Failure> {
    if !options.quiet && !options.passive && !options.confirmed {
        if !ui::confirm_uninstall(true) {
            log.line("the user cancelled the uninstall");
            return Ok(Outcome {
                code: ERROR_INSTALL_USER_EXIT,
                message: "Uninstall was cancelled.".to_string(),
                show_ui: false,
            });
        }
    }

    if !options.from_copy {
        if let Some(install_dir) = current_installed_location() {
            return relaunch_uninstall(options, &install_dir, log);
        }
    } else if let Some(parent) = options.parent_pid {
        log.line(format!(
            "waiting for the original uninstaller process {parent} to exit"
        ));
        if !win::wait_for_process_exit(parent, Duration::from_secs(30)) {
            log.line("warning: the original uninstaller did not exit within 30 seconds");
        }
    }

    preflight()?;
    let _guard = setup_guard()?;
    let product = win::read_product()
        .map_err(|error| Failure::io("cannot read the installed product state", error))?;
    let install_dir = match product.as_ref() {
        Some(product) => PathBuf::from(&product.install_root),
        None => match current_installed_location() {
            Some(path) => path,
            None => {
                log.line("XFS for Windows is not installed");
                return Ok(Outcome {
                    code: 0,
                    message: "XFS for Windows is not installed.".to_string(),
                    show_ui: true,
                });
            }
        },
    };
    if let Err(error) = win::validate_install_path(&install_dir) {
        return Err(Failure::new(
            ERROR_INSTALL_FAILURE,
            format!("unsafe install directory: {error}"),
        ));
    }
    ensure_under_program_files(&install_dir)?;

    let _progress = if options.quiet {
        None
    } else {
        Some(ui::Progress::start(
            "Uninstalling XFS for Windows.\r\n\r\n\
             The service is stopped cleanly and mounted sessions are released before \
             files and registration are removed.",
        ))
    };

    let installed_exe = install_dir.join("xfswin.exe");
    if installed_exe.is_file() {
        log.line("preparing the uninstall transaction");
        run_installer_step(
            &installed_exe,
            "prepare",
            "uninstall",
            &install_dir,
            None,
            log,
        )?;
        if let Err(failure) = run_installer_step(
            &installed_exe,
            "remove",
            "uninstall",
            &install_dir,
            None,
            log,
        ) {
            log.line("remove failed; restoring the service state");
            let _ = run_installer_step(
                &installed_exe,
                "rollback",
                "auto",
                &install_dir,
                None,
                log,
            );
            return Err(failure);
        }
        if let Err(failure) = run_installer_step(
            &installed_exe,
            "commit",
            "auto",
            &install_dir,
            None,
            log,
        ) {
            log.line("commit failed; restoring the service state");
            let _ = run_installer_step(
                &installed_exe,
                "rollback",
                "auto",
                &install_dir,
                None,
                log,
            );
            return Err(failure);
        }
    } else {
        log.line("the installed xfswin.exe is missing; cleaning up registration only");
    }

    let (path_present, path_value, path_type) = win::read_machine_path()
        .map_err(|error| Failure::io("cannot read the machine PATH", error))?;
    if path_present {
        let (new_path, changed) = win::remove_path_entry(&path_value, &install_dir);
        if changed {
            log.line("removing the product directory from the machine PATH");
            win::write_machine_path(&new_path, path_type)
                .map_err(|error| Failure::io("cannot update the machine PATH", error))?;
        }
    }
    log.line("removing product and Add/Remove Programs registration");
    win::remove_product_registration()
        .map_err(|error| Failure::io("cannot remove product registration", error))?;
    log.line("removing Start Menu shortcuts");
    win::remove_shortcuts().map_err(|error| Failure::io("cannot remove Start Menu shortcuts", error))?;

    log.line(format!("removing {}", install_dir.display()));
    win::remove_directory_with_retry(&install_dir)
        .map_err(|error| Failure::io("cannot remove the installed product files", error))?;

    log.line("uninstall completed; %ProgramData%\\xfswin and WinFsp were preserved");
    Ok(Outcome {
        code: 0,
        message: "XFS for Windows was uninstalled. Your automount configuration and WinFsp were preserved."
            .to_string(),
        show_ui: true,
    })
}

fn relaunch_uninstall(
    options: &Options,
    install_dir: &Path,
    log: &mut Log,
) -> Result<Outcome, Failure> {
    let temp = std::env::temp_dir().join(format!("xfswin-uninstall-{}.exe", new_temp_name()));
    win::copy_current_executable(&temp)
        .map_err(|error| Failure::io("cannot copy the uninstaller to a temporary directory", error))?;
    let mut command = Command::new(&temp);
    command
        .arg("/uninstall")
        .arg("/from-copy")
        .arg("/parent-pid")
        .arg(win::current_pid().to_string());
    if options.quiet {
        command.arg("/quiet");
    }
    if options.passive {
        command.arg("/passive");
    }
    if !options.quiet && !options.passive {
        // The original process already asked for confirmation; the temporary
        // copy must not ask a second time.
        command.arg("/confirmed");
    }
    if let Some(path) = options.log_path.as_deref() {
        command.arg("/log").arg(path);
    }
    let child = command
        .spawn()
        .map_err(|error| Failure::io("cannot relaunch the uninstaller", error))?;
    log.line(format!(
        "relaunched the uninstaller from {} as process {} for {}",
        temp.display(),
        child.id(),
        install_dir.display()
    ));
    Ok(Outcome {
        code: 0,
        message: "The uninstaller is restarting from a temporary directory to remove the running files."
            .to_string(),
        show_ui: false,
    })
}

#[derive(Clone, Debug, Serialize, Deserialize)]
struct SetupState {
    install_dir: String,
    action: String,
    previous_install_root: Option<String>,
    previous_version: Option<String>,
    previous_architecture: Option<String>,
    path_present: bool,
    path_value: String,
    path_type: u32,
    arp_present: bool,
    product_present: bool,
    start_menu_present: bool,
    backup_dir: Option<String>,
    staging_dir: String,
    files_moved: bool,
    new_files_installed: bool,
    registration_written: bool,
}

impl SetupState {
    fn capture(
        install_dir: &Path,
        previous: Option<&win::ProductState>,
        backup: Option<&Path>,
        staging: &Path,
        action: &str,
    ) -> std::io::Result<Self> {
        let (path_present, path_value, path_type) = win::read_machine_path()?;
        Ok(Self {
            install_dir: install_dir.to_string_lossy().into_owned(),
            action: action.to_string(),
            previous_install_root: previous.map(|product| product.install_root.clone()),
            previous_version: previous.map(|product| product.version.clone()),
            previous_architecture: previous.map(|product| product.architecture.clone()),
            path_present,
            path_value,
            path_type,
            arp_present: win::reg_exists(win::ARP_KEY),
            product_present: previous.is_some(),
            start_menu_present: win::shortcut_directory_exists(),
            backup_dir: backup.map(|path| path.to_string_lossy().into_owned()),
            staging_dir: staging.to_string_lossy().into_owned(),
            files_moved: false,
            new_files_installed: false,
            registration_written: false,
        })
    }
}
