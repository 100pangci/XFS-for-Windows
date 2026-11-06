// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Minimal Win32 user interface.
//!
//! The setup only needs three things from the UI: an explicit confirmation
//! before it changes the machine, an indeterminate progress window while it
//! works, and a final success/error message.  It deliberately has no custom
//! install directory, feature selection or per-user options.

use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicIsize, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;

use windows::core::PCWSTR;
use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::Graphics::Gdi::UpdateWindow;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GetMessageW,
    GetSystemMetrics, GetWindowLongPtrW, LoadCursorW, MessageBoxW, PostMessageW, PostQuitMessage,
    RegisterClassW, SetTimer, SetWindowLongPtrW, SetWindowTextW, ShowWindow, TranslateMessage,
    GWLP_USERDATA, IDC_ARROW, IDOK, MB_DEFBUTTON2, MB_ICONERROR, MB_ICONINFORMATION,
    MB_ICONWARNING, MB_OK, MB_OKCANCEL, MB_SETFOREGROUND, MESSAGEBOX_STYLE, MSG, SM_CXSCREEN,
    SM_CYSCREEN, SW_SHOW, WINDOW_EX_STYLE, WM_APP, WM_DESTROY, WM_TIMER, WNDCLASSW, WS_CAPTION,
    WS_CHILD, WS_EX_APPWINDOW, WS_EX_TOPMOST, WS_POPUP, WS_VISIBLE,
};

const WM_PROGRESS_CLOSE: u32 = WM_APP + 1;

const PROGRESS_CLASS: &str = "XFSWinSetupProgress";
const PROGRESS_TITLE: &str = "XFS for Windows Setup";
const TIMER_ID: usize = 1;

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

fn message(text: &str, caption: &str, flags: MESSAGEBOX_STYLE) -> i32 {
    let text = wide(text);
    let caption = wide(caption);
    unsafe {
        MessageBoxW(
            None,
            PCWSTR(text.as_ptr()),
            PCWSTR(caption.as_ptr()),
            flags,
        )
        .0
    }
}

pub fn error(log_path: Option<&Path>, text: &str) {
    let _ = log_path;
    let _ = message(
        text,
        PROGRESS_TITLE,
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND,
    );
}

pub fn info(text: &str) {
    let _ = message(
        text,
        PROGRESS_TITLE,
        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND,
    );
}

/// Ask for explicit consent before installing or upgrading.  The default
/// button is Cancel so pressing Enter cannot accept the license or start a
/// machine-wide change by accident.
pub fn confirm_install(
    version: &str,
    architecture: &str,
    install_dir: &Path,
    previous: Option<&str>,
) -> bool {
    let action = match previous {
        Some(previous) => format!("upgrade XFS for Windows from {previous} to {version}"),
        None => format!("install XFS for Windows {version} ({architecture})"),
    };
    let previous_note = if previous.is_some() {
        "The existing installation will be upgraded in place.\r\n"
    } else {
        ""
    };
    let text = format!(
        "This will {action} into:\r\n\r\n    {path}\r\n\r\n\
         {previous_note}\
         The per-machine XFSWinAutomount service will be installed and started. \
         The pinned WinFsp 2.1.25156 runtime is installed only when it is missing \
         or older.\r\n\r\n\
         Your %ProgramData%\\xfswin configuration, UUIDs, identity pins and \
         read-write enrollment are preserved and are never rewritten by setup.\r\n\r\n\
         XFS for Windows is licensed under the Mozilla Public License 2.0. \
         The bundled WinFsp runtime is licensed under GPLv3 with the FLOSS \
         exception; its notice and license are installed with the product.\r\n\r\n\
         Do you want to continue?",
        action = action,
        path = install_dir.display(),
        previous_note = previous_note
    );
    let result = message(
        &text,
        PROGRESS_TITLE,
        MB_OKCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON2 | MB_SETFOREGROUND,
    );
    result == IDOK.0
}

pub fn confirm_uninstall(preserve_config: bool) -> bool {
    let text = format!(
        "This will uninstall XFS for Windows and remove its per-machine service, \
         Start Menu shortcuts and machine PATH entry.\r\n\r\n\
         {}\
         The shared WinFsp runtime is not removed.\r\n\r\n\
         Do you want to continue?",
        if preserve_config {
            "Your %ProgramData%\\xfswin configuration and device enrollment are preserved.\r\n\r\n"
        } else {
            ""
        }
    );
    let result = message(
        &text,
        PROGRESS_TITLE,
        MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND,
    );
    result == IDOK.0
}

struct ProgressState {
    stop: Arc<AtomicBool>,
    window: Arc<AtomicIsize>,
}

static PROGRESS_FRAME: AtomicUsize = AtomicUsize::new(0);

unsafe extern "system" fn progress_proc(
    window: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    match message {
        WM_TIMER => {
            let bar_label = HWND(GetWindowLongPtrW(window, GWLP_USERDATA) as *mut core::ffi::c_void);
            if !bar_label.0.is_null() {
                let frame = PROGRESS_FRAME.fetch_add(1, Ordering::Relaxed) % 12;
                let filled = if frame <= 6 { frame } else { 12 - frame };
                let text = format!("[{}{}]", "#".repeat(filled), " ".repeat(12 - filled));
                let text = wide(&text);
                let _ = SetWindowTextW(bar_label, PCWSTR(text.as_ptr()));
            }
            LRESULT(0)
        }
        WM_PROGRESS_CLOSE => {
            let _ = DestroyWindow(window);
            LRESULT(0)
        }
        WM_DESTROY => {
            PostQuitMessage(0);
            LRESULT(0)
        }
        _ => DefWindowProcW(window, message, wparam, lparam),
    }
}

fn run_progress(message: String, state: ProgressState) {
    let class_name = wide(PROGRESS_CLASS);
    let title = wide(PROGRESS_TITLE);
    let instance = match unsafe { GetModuleHandleW(None) } {
        Ok(instance) => instance,
        Err(_) => return,
    };
    let cursor = unsafe { LoadCursorW(None, IDC_ARROW) }.unwrap_or_default();
    let class = WNDCLASSW {
        lpfnWndProc: Some(progress_proc),
        hInstance: instance.into(),
        hCursor: cursor,
        lpszClassName: PCWSTR(class_name.as_ptr()),
        ..Default::default()
    };
    if unsafe { RegisterClassW(&class) } == 0 {
        return;
    }

    let width = 520;
    let height = 210;
    let x = (unsafe { GetSystemMetrics(SM_CXSCREEN) } - width) / 2;
    let y = (unsafe { GetSystemMetrics(SM_CYSCREEN) } - height) / 2;
    let window = match unsafe {
        CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_APPWINDOW,
            PCWSTR(class_name.as_ptr()),
            PCWSTR(title.as_ptr()),
            WS_POPUP | WS_CAPTION | WS_VISIBLE,
            x.max(0),
            y.max(0),
            width,
            height,
            None,
            None,
            Some(instance.into()),
            None,
        )
    } {
        Ok(window) => window,
        Err(_) => return,
    };
    state.window.store(window.0 as isize, Ordering::SeqCst);

    let static_class = wide("STATIC");
    let message_text = wide(&message);
    let message_label = unsafe {
        CreateWindowExW(
            WINDOW_EX_STYLE(0),
            PCWSTR(static_class.as_ptr()),
            PCWSTR(message_text.as_ptr()),
            WS_CHILD | WS_VISIBLE,
            24,
            20,
            width - 48,
            height - 100,
            Some(window),
            None,
            Some(instance.into()),
            None,
        )
    };
    let message_label = match message_label {
        Ok(label) => label,
        Err(_) => {
            let _ = unsafe { DestroyWindow(window) };
            state.window.store(0, Ordering::SeqCst);
            return;
        }
    };
    let bar_text = wide("");
    let bar_label = unsafe {
        CreateWindowExW(
            WINDOW_EX_STYLE(0),
            PCWSTR(static_class.as_ptr()),
            PCWSTR(bar_text.as_ptr()),
            WS_CHILD | WS_VISIBLE,
            24,
            height - 70,
            width - 48,
            24,
            Some(window),
            None,
            Some(instance.into()),
            None,
        )
    };
    let bar_label = match bar_label {
        Ok(label) => label,
        Err(_) => {
            let _ = unsafe { DestroyWindow(window) };
            state.window.store(0, Ordering::SeqCst);
            return;
        }
    };
    let _ = message_label;
    unsafe {
        SetWindowLongPtrW(window, GWLP_USERDATA, bar_label.0 as isize);
        let _ = ShowWindow(window, SW_SHOW);
        let _ = UpdateWindow(window);
        let _ = SetTimer(Some(window), TIMER_ID, 120, None);
    }

    if state.stop.load(Ordering::SeqCst) {
        unsafe {
            let _ = DestroyWindow(window);
        }
        state.window.store(0, Ordering::SeqCst);
        return;
    }

    let mut msg = MSG::default();
    loop {
        let result = unsafe { GetMessageW(&mut msg, None, 0, 0) };
        if result.0 <= 0 {
            break;
        }
        if state.stop.load(Ordering::SeqCst) {
            unsafe {
                let _ = DestroyWindow(window);
            }
            break;
        }
        unsafe {
            let _ = TranslateMessage(&msg);
            let _ = DispatchMessageW(&msg);
        }
    }
    state.window.store(0, Ordering::SeqCst);
}

/// Indeterminate progress window.  Dropping the value closes the window and
/// waits for its UI thread, including on every error path.
pub struct Progress {
    state: Option<ProgressState>,
    thread: Option<JoinHandle<()>>,
}

impl Progress {
    pub fn start(message: impl Into<String>) -> Self {
        let message: String = message.into();
        let state = ProgressState {
            stop: Arc::new(AtomicBool::new(false)),
            window: Arc::new(AtomicIsize::new(0)),
        };
        let thread_state = ProgressState {
            stop: Arc::clone(&state.stop),
            window: Arc::clone(&state.window),
        };
        let thread = std::thread::spawn(move || run_progress(message, thread_state));
        // Give the window a moment to appear before the installer starts
        // doing work; failure to show UI never blocks the installation.
        for _ in 0..50 {
            if state.window.load(Ordering::SeqCst) != 0 {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        Self {
            state: Some(state),
            thread: Some(thread),
        }
    }
}

impl Drop for Progress {
    fn drop(&mut self) {
        if let Some(state) = self.state.take() {
            state.stop.store(true, Ordering::SeqCst);
            let window = state.window.load(Ordering::SeqCst);
            if window != 0 {
                unsafe {
                    let _ = PostMessageW(
                        Some(HWND(window as *mut core::ffi::c_void)),
                        WM_PROGRESS_CLOSE,
                        WPARAM(0),
                        LPARAM(0),
                    );
                }
            }
            for _ in 0..50 {
                if state.window.load(Ordering::SeqCst) == 0 {
                    break;
                }
                std::thread::sleep(Duration::from_millis(10));
            }
        }
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}
