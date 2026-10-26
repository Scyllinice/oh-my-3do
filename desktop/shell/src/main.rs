//! The desktop app's entry point: a Tauri window showing the web interface
//! in `desktop/ui`, which asks the engine (`oh_my_3do_library`) to do its
//! work through the commands in `bridge`.
//!
//! The same program also installs its own updates: started with
//! `--apply-desktop-update <plan>`, it replaces the app's executable instead
//! of opening a window (see `self_update`).

#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]
mod bridge;
use oh_my_3do_library::self_update;
use std::{fs, path::PathBuf, sync::Arc};
use tauri::Manager;

fn main() {
    let args: Vec<String> = std::env::args().collect();

    if let Some(index) = args.iter().position(|arg| arg == "--apply-desktop-update") {
        if let Err(error) = apply_desktop_update(args.get(index + 1)) {
            eprintln!("Update failed: {error:#}");
            std::process::exit(1);
        }
        return;
    }

    let state = Arc::new(bridge::Backend::new());
    if state.software_rendering() {
        use_software_rendering();
    }

    // Started by an update on exit: check the new version starts, without
    // showing its window.
    let mut context = tauri::generate_context!();
    if args.iter().any(|arg| arg == "--update-exit") {
        for window in &mut context.config_mut().app.windows {
            window.visible = false;
        }
    }

    let handle = state.clone();
    let result = tauri::Builder::default()
        .manage(bridge::Bridge(state))
        .invoke_handler(tauri::generate_handler![
            bridge::request,
            bridge::cancel,
            bridge::cover,
            bridge::save_library_views
        ])
        .setup(move |app| {
            let window = app.get_webview_window("main").unwrap();
            #[cfg(debug_assertions)]
            if std::env::var_os("OH3DO_DEVTOOLS").is_some() {
                window.open_devtools();
            }
            let _ = window;
            handle.configure_args();
            Ok(())
        })
        .on_window_event(|window, event| {
            // The interface decides whether closing is safe (a job may be
            // running), so it is asked first.
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                use tauri::Emitter;
                let _ = window.emit("close-requested", ());
            }
        })
        .run(context);

    if let Err(error) = result {
        show_start_error(&error.to_string());
    }
}

/// Installs a staged update from the plan file at `plan_path` (this process
/// is the installer copy the app started; see `self_update::start`). The
/// outcome is saved for the app to show. If installing fails, the old app is
/// reopened.
fn apply_desktop_update(plan_path: Option<&String>) -> anyhow::Result<()> {
    let path = PathBuf::from(plan_path.ok_or_else(|| anyhow::anyhow!("Missing update plan"))?);
    let plan: self_update::Install = serde_json::from_slice(&fs::read(path)?)?;

    let result = self_update::apply(&plan);
    if let Err(error) = self_update::record_result(&plan, &result) {
        eprintln!("Could not save update result: {error:#}");
    }

    if let Err(error) = &result {
        let _ = fs::write(&plan.log, format!("{error:#}"));
        if let Err(restart) = self_update::restart_previous(&plan) {
            let both = anyhow::anyhow!("{error:#}\n{restart:#}");
            let _ = self_update::record_result(&plan, &Err(both));
        }
    }
    result
}

/// Asks the web view to draw without the graphics card, for computers
/// where hardware drawing shows a blank or broken window.
fn use_software_rendering() {
    #[cfg(windows)]
    std::env::set_var(
        "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
        format!(
            "{} --disable-gpu",
            std::env::var("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS").unwrap_or_default()
        ),
    );
    #[cfg(target_os = "linux")]
    {
        std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
        std::env::set_var("LIBGL_ALWAYS_SOFTWARE", "1");
    }
}

/// Shows why the window could not open (usually a missing web view).
fn show_start_error(error: &str) {
    rfd::MessageDialog::new()
        .set_title("Oh My 3DO could not start")
        .set_description(format!(
            "{error}\n\nWindows requires Microsoft Edge WebView2. Linux requires WebKitGTK 4.1."
        ))
        .set_level(rfd::MessageLevel::Error)
        .show();
}
