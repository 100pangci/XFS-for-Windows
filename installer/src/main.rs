// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#![windows_subsystem = "windows"]

mod args;
mod install;
mod logging;
mod payload;
mod ui;
mod win;

fn main() {
    match args::parse(std::env::args().skip(1)) {
        args::ParseResult::Help => {
            logging::write_console(args::USAGE);
            std::process::exit(0);
        }
        args::ParseResult::Version => {
            logging::write_console(&format!(
                "XFS for Windows Setup {} ({}){}",
                payload::SETUP_VERSION,
                payload::SETUP_ARCH,
                if payload::SETUP_TEST_BUILD {
                    " [acceptance test build]"
                } else {
                    ""
                }
            ));
            std::process::exit(0);
        }
        args::ParseResult::Error(message) => {
            logging::write_console(&format!("error: {message}\r\n\r\n{}", args::USAGE));
            ui::error(None, &message);
            std::process::exit(2);
        }
        args::ParseResult::Run(options) => {
            let mut log = logging::Log::new(options.log_path.as_deref(), options.quiet);
            log.line(format!(
                "XFS for Windows Setup {} ({}) git {}",
                payload::SETUP_VERSION,
                payload::SETUP_ARCH,
                payload::SETUP_GIT_SHA
            ));
            let result = install::run(&options, &mut log);
            match result {
                Ok(outcome) => {
                    log.line(format!(
                        "completed with exit code {}: {}",
                        outcome.code, outcome.message
                    ));
                    if !options.quiet && !options.passive && outcome.show_ui {
                        ui::info(&outcome.message);
                    }
                    std::process::exit(outcome.code as i32);
                }
                Err(failure) => {
                    log.line(format!("failed with exit code {}: {}", failure.code, failure.message));
                    log.line(format!("log file: {}", log.path().display()));
                    if !options.quiet {
                        ui::error(
                            Some(log.path()),
                            &format!("{}\r\n\r\nLog: {}", failure.message, log.path().display()),
                        );
                    }
                    std::process::exit(failure.code as i32);
                }
            }
        }
    }
}
