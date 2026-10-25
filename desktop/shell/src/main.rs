#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]
mod bridge;
use std::{fs, path::PathBuf, sync::Arc};
use tauri::Manager;
fn main() {
    let args: Vec<_> = std::env::args().collect();
    if let Some(i) = args.iter().position(|a| a == "--apply-desktop-update") {
        let result = (|| -> anyhow::Result<()> {
            let path = PathBuf::from(
                args.get(i + 1)
                    .ok_or_else(|| anyhow::anyhow!("Missing update plan"))?,
            );
            let plan: oh_my_3do_library::self_update::Install =
                serde_json::from_slice(&fs::read(path)?)?;
            let result = oh_my_3do_library::self_update::apply(&plan);
            if let Err(error) = oh_my_3do_library::self_update::record_result(&plan, &result) {
                eprintln!("Could not save update result: {error:#}");
            }
            if let Err(e) = &result {
                let _ = fs::write(&plan.log, format!("{e:#}"));
                if let Err(restart) = oh_my_3do_library::self_update::restart_previous(&plan) {
                    let _ = oh_my_3do_library::self_update::record_result(
                        &plan,
                        &Err(anyhow::anyhow!("{e:#}\n{restart:#}")),
                    );
                }
            }
            result
        })();
        if let Err(error) = result {
            eprintln!("Update failed: {error:#}");
            std::process::exit(1);
        }
        return;
    }
    let state = Arc::new(bridge::Backend::new());
    if state.software_rendering() {
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
    let handle = state.clone();
    let mut context = tauri::generate_context!();
    if args.iter().any(|a| a == "--update-exit") {
        for window in &mut context.config_mut().app.windows {
            window.visible = false;
        }
    }
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
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                use tauri::Emitter;
                let _ = window.emit("close-requested", ());
            }
        })
        .run(context);
    if let Err(error) = result {
        rfd::MessageDialog::new().set_title("Oh My 3DO could not start").set_description(format!("{error}\n\nWindows requires Microsoft Edge WebView2. Linux requires WebKitGTK 4.1.")).set_level(rfd::MessageLevel::Error).show();
    }
}
