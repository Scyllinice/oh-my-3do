//! The bridge between the web interface (`desktop/ui`) and the engine.
//!
//! The interface sends a `request` with an action name and JSON arguments;
//! [`dispatch`] runs the matching handler on a worker thread and returns
//! JSON. Only one request runs at a time, and it can be canceled. Progress
//! and messages are sent back to the interface as events while it runs.
//!
//! Reviews are two-step: a "review" or "preview" request keeps its plan
//! here (in [`Inner`]), and the following request carries it out only if
//! nothing changed in between.
//!
//! Other commands: `cancel`, `cover` (a cover thumbnail for display) and
//! `save_library_views` (view preferences, saved without blocking the
//! interface).

use anyhow::{bail, ensure, Context, Result};
use base64::{engine::general_purpose::STANDARD, Engine};
use oh_my_3do_library::{
    self as core, backup, catalog, categories, console_config, fsutil as f, library, media,
    model::*,
    save_archive::{Archive, Entry},
    save_catalog, save_cheats, self_update, setup, updates, usb, Job,
};
use serde_json::{json, Value};
use std::{
    fs,
    io::Cursor,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
};
use tauri::{AppHandle, Emitter};

/// The backend, as Tauri keeps it for the commands.
pub struct Bridge(pub Arc<Backend>);

/// The app's state behind the interface.
pub struct Backend {
    inner: Mutex<Inner>,
    /// Set to cancel the running request.
    cancel: Arc<AtomicBool>,
    /// Whether a request is running.
    busy: AtomicBool,
    /// The app's cache folder (settings, downloads, journals).
    cache: PathBuf,
}

/// A save backup (slot or archive) open in the Save Manager.
struct Backup {
    /// Where it was opened from or last saved (None for a new one).
    path: Option<PathBuf>,
    /// Its file's contents when opened or saved, to notice outside changes.
    original: Option<Vec<u8>>,
    archive: Archive,
    /// Changed since it was opened or saved.
    dirty: bool,
}

/// Everything the requests share, behind one lock.
struct Inner {
    prefs: Preferences,
    /// A reviewed title removal, and the drive's identity token if it is on
    /// a drive rather than the library.
    removal: Option<(core::title_removal::Plan, Option<String>)>,
    /// A scanned import.
    import: Option<Plan>,
    /// A reviewed copy to a drive.
    copy: Option<usb::CopyPlan>,
    /// Reviewed game information changes, each with a token the interface
    /// must send back.
    information_restore: Option<(String, core::game_info::RestorePlan)>,
    information_import: Option<(String, core::game_info::ImportPlan)>,
    information_matches: Option<(String, core::game_info::MatchReview)>,
    /// The last change's journal, for Undo.
    last: Option<PathBuf>,
    /// Console settings loaded for editing.
    config: Option<console_config::Loaded>,
    backup: Option<Backup>,
    /// Recovery copies found for cleanup.
    cleanup: Option<core::recovery::Inventory>,
    /// A reviewed removal of the menu's files from a drive.
    drive_cleanup: Option<core::drive_cleanup::Plan>,
    /// The last update check's offers.
    release: Option<updates::Offers>,
    /// A desktop update staged to install on exit: plan, plan file, version.
    pending_update: Option<(self_update::Install, PathBuf, String)>,
    /// The cheat database being edited, as loaded from `cheats_path`.
    cheats: save_cheats::Database,
    cheats_original: Option<Vec<u8>>,
    cheats_path: Option<PathBuf>,
}

impl Backend {
    /// Loads the saved preferences (or defaults) from the cache.
    pub fn new() -> Self {
        let cache = core::cache_dir();
        let prefs = fs::read(cache.join("native-settings.json"))
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default();
        Self {
            inner: Mutex::new(Inner {
                prefs,
                removal: None,
                import: None,
                copy: None,
                information_restore: None,
                information_import: None,
                information_matches: None,
                last: None,
                config: None,
                backup: None,
                cleanup: None,
                drive_cleanup: None,
                release: None,
                pending_update: None,
                cheats: save_cheats::Database::bundled(),
                cheats_original: None,
                cheats_path: None,
            }),
            cancel: Arc::new(AtomicBool::new(false)),
            busy: AtomicBool::new(false),
            cache,
        }
    }

    /// Whether to draw without the graphics card (a preference, or the
    /// `--software-rendering` argument).
    pub fn software_rendering(&self) -> bool {
        self.inner.lock().unwrap().prefs.software_rendering
            || std::env::args().any(|arg| arg == "--software-rendering")
    }

    /// Applies command-line arguments: `--library <folder>` opens that
    /// library.
    pub fn configure_args(&self) {
        let mut inner = self.inner.lock().unwrap();
        let args: Vec<String> = std::env::args().collect();
        if let Some(index) = args.iter().position(|arg| arg == "--library") {
            if let Some(root) = args.get(index + 1) {
                inner.prefs.library = root.clone();
            }
        }
    }

    /// Saves the preferences to the cache.
    fn save_prefs(&self, prefs: &Preferences) -> Result<()> {
        f::atomic(
            &self.cache.join("native-settings.json"),
            &serde_json::to_vec_pretty(prefs)?,
        )
    }
}

// ---- Reading request arguments ----

/// A text argument.
fn text<'a>(args: &'a Value, key: &str) -> Result<&'a str> {
    args[key].as_str().with_context(|| format!("Missing {key}"))
}

/// A path argument.
fn path(args: &Value, key: &str) -> Result<PathBuf> {
    Ok(PathBuf::from(text(args, key)?))
}

/// A list of numbers argument (a selection).
fn indices(args: &Value, key: &str) -> Result<Vec<usize>> {
    args[key]
        .as_array()
        .context("Missing selection")?
        .iter()
        .map(|value| {
            value
                .as_u64()
                .map(|number| number as usize)
                .context("Invalid selection")
        })
        .collect()
}

/// A list of text argument.
fn strings(args: &Value, key: &str) -> Result<Vec<String>> {
    Ok(serde_json::from_value(args[key].clone())?)
}

/// The library folder chosen in the preferences.
fn root(inner: &Inner) -> Result<PathBuf> {
    ensure!(
        !inner.prefs.library.trim().is_empty(),
        "Choose a library folder first"
    );
    f::root(Path::new(&inner.prefs.library))
}

/// The response after a change: its journal, for Undo.
fn journal_response(inner: &Inner) -> Value {
    json!({"journal": inner.last})
}

// ---- Commands ----

/// Cancels the running request.
#[tauri::command]
pub fn cancel(state: tauri::State<'_, Bridge>) {
    state.0.cancel.store(true, Ordering::Relaxed);
}

/// Preference-only writes do not lock the GUI controls or steal search focus.
#[tauri::command]
pub async fn save_library_views(
    state: tauri::State<'_, Bridge>,
    views: std::collections::BTreeMap<String, LibraryView>,
) -> Result<(), String> {
    let backend = state.0.clone();
    tauri::async_runtime::spawn_blocking(move || -> Result<()> {
        let mut inner = backend
            .inner
            .lock()
            .map_err(|_| anyhow::anyhow!("Preferences unavailable"))?;
        let mut prefs = inner.prefs.clone();
        prefs.library_views = views;
        backend.save_prefs(&prefs)?;
        inner.prefs = prefs;
        Ok(())
    })
    .await
    .map_err(|error| error.to_string())?
    .map_err(|error| format!("{error:#}"))
}

/// A cover as a PNG data URL (at most 240x300) for the interface: from the
/// library's covers folder, or from a drive (`browser_root`) whose identity
/// `token` must still match.
#[tauri::command]
pub async fn cover(
    state: tauri::State<'_, Bridge>,
    name: String,
    browser_root: Option<String>,
    token: Option<String>,
) -> Result<String, String> {
    let backend = state.0.clone();
    tauri::async_runtime::spawn_blocking(move || -> Result<String> {
        let root = {
            let inner = backend
                .inner
                .lock()
                .map_err(|_| anyhow::anyhow!("State unavailable"))?;
            match &browser_root {
                Some(drive) => {
                    let drive_root = f::root(Path::new(drive))?;
                    if let Some(token) = &token {
                        let identity = core::drive_identity::inspect(&drive_root, &inner.prefs)?;
                        ensure!(
                            identity.token == *token,
                            "Drive changed; refresh the browser"
                        );
                    }
                    drive_root
                }
                None => root(&inner)?,
            }
        };

        let relative = if browser_root.is_some() {
            name
        } else {
            format!(".ohmy3do/covers/{name}")
        };
        let file = f::safe(&root, &relative)?;
        ensure!(
            fs::metadata(&file)?.len() <= 32 * 1024 * 1024,
            "Oversized cover"
        );

        let thumbnail = media::cover_display(&file)?.thumbnail(240, 300);
        let mut png = Cursor::new(Vec::new());
        thumbnail.write_to(&mut png, image::ImageFormat::Png)?;
        Ok(format!(
            "data:image/png;base64,{}",
            STANDARD.encode(png.into_inner())
        ))
    })
    .await
    .map_err(|error| error.to_string())?
    .map_err(|error| format!("{error:#}"))
}

/// Runs one request from the interface (see the module description). A
/// file chooser ("pick") runs at any time; anything else waits for no other
/// request to be running.
#[tauri::command]
pub async fn request(
    app: AppHandle,
    state: tauri::State<'_, Bridge>,
    action: String,
    args: Value,
    request_id: Option<String>,
) -> Result<Value, String> {
    let backend = state.0.clone();
    if action == "pick" {
        return tauri::async_runtime::spawn_blocking(move || pick(&args))
            .await
            .map_err(|error| error.to_string())?
            .map_err(|error| format!("{error:#}"));
    }

    ensure_idle(&backend)?;
    backend.cancel.store(false, Ordering::Relaxed);
    let _ = app.emit("busy", true);
    let job = request_job(&app, &backend, request_id);

    let finished_app = app.clone();
    let finished_backend = backend.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let mut inner = backend
            .inner
            .lock()
            .map_err(|_| anyhow::anyhow!("State unavailable"))?;
        let request = Request {
            app: &app,
            backend: &backend,
            args: &args,
            job: &job,
        };
        dispatch(&request, &mut inner, &action)
    })
    .await;

    finished_backend.busy.store(false, Ordering::Release);
    let _ = finished_app.emit("busy", false);
    result
        .map_err(|error| error.to_string())?
        .map_err(|error| format!("{error:#}"))
}

/// The job for a request: cancelable from the interface, with messages sent
/// as "progress" events and progress as "operation-progress" events (at
/// most every 100 ms, plus every change of phase and the end).
fn request_job(app: &AppHandle, backend: &Backend, request_id: Option<String>) -> Job {
    let message_events = app.clone();
    let progress_events = app.clone();
    let last_progress = Mutex::new((std::time::Instant::now(), String::new()));
    Job {
        cancel: backend.cancel.clone(),
        log: Arc::new(move |message| {
            let _ = message_events.emit("progress", message);
        }),
        progress: Arc::new(move |progress| {
            let mut last = last_progress.lock().unwrap();
            let (last_sent, last_phase) = &mut *last;
            if progress.phase != *last_phase
                || progress.completed == progress.total
                || last_sent.elapsed() >= std::time::Duration::from_millis(100)
            {
                *last_sent = std::time::Instant::now();
                *last_phase = progress.phase.clone();
                let _ = progress_events.emit(
                    "operation-progress",
                    json!({
                        "request_id": request_id,
                        "completed": progress.completed,
                        "total": progress.total,
                        "phase": progress.phase,
                    }),
                );
            }
        }),
        ..Default::default()
    }
}

/// Marks the backend busy, or refuses if another request is running.
fn ensure_idle(state: &Backend) -> Result<(), String> {
    state
        .busy
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .map(|_| ())
        .map_err(|_| "Another operation is running. Wait or cancel it first.".into())
}

/// Shows a file or folder chooser. `kind` says what to choose (and whether
/// to open or save); `start` is the folder to start in.
fn pick(args: &Value) -> Result<Value> {
    let kind = text(args, "kind")?;
    let mut dialog = rfd::FileDialog::new();
    if let Some(start) = args["start"].as_str().filter(|start| !start.is_empty()) {
        dialog = dialog.set_directory(start);
    }

    Ok(match kind {
        "folder" => json!(dialog.pick_folder()),
        "discs" => json!(dialog.add_filter("3DO discs", &["iso", "cue"]).pick_files()),
        "archive" => json!(dialog.add_filter("Save archive", &["3ds"]).pick_file()),
        "archive_save" => json!(dialog
            .add_filter("Save archive", &["3ds"])
            .set_file_name("slot01.3ds")
            .save_file()),
        "iso" => json!(dialog.add_filter("Menu ISO", &["iso"]).pick_file()),
        "image" => json!(dialog
            .add_filter("Artwork", &["png", "jpg", "jpeg", "webp", "bmp"])
            .pick_file()),
        "video" => json!(dialog
            .add_filter("Video", &["mp4", "mkv", "avi", "mov", "webm"])
            .pick_file()),
        "raw" => json!(dialog.pick_file()),
        "raw_save" => json!(dialog.set_file_name("save.bin").save_file()),
        "json" => json!(dialog.add_filter("Cheat database", &["json"]).pick_file()),
        "json_save" => json!(dialog
            .add_filter("Cheat database", &["json"])
            .set_file_name("save-cheats.json")
            .save_file()),
        "support_save" => json!(dialog
            .add_filter("Support report", &["txt"])
            .set_file_name("Oh-My-3DO-support-report.txt")
            .save_file()),
        "metadata" => json!(dialog
            .add_filter("Custom game metadata", &["json"])
            .pick_file()),
        "metadata_save" => json!(dialog
            .add_filter("Custom game metadata", &["json"])
            .set_file_name("Oh-My-3DO-custom-metadata.json")
            .save_file()),
        _ => bail!("Unknown file chooser"),
    })
}

// ---- Requests ----

/// One request being handled.
struct Request<'a> {
    app: &'a AppHandle,
    backend: &'a Backend,
    args: &'a Value,
    job: &'a Job,
}

impl Request<'_> {
    fn cache(&self) -> &Path {
        &self.backend.cache
    }
}

/// Runs the handler for `action`.
fn dispatch(request: &Request, inner: &mut Inner, action: &str) -> Result<Value> {
    match action {
        // The app itself.
        "quit" => quit(request, inner),
        "ready" => ready(request),
        "init" => init(request, inner),
        "preferences" => save_preferences(request, inner),
        "video_tools" => install_video_tools(request, inner),

        // The library and importing.
        "library" => library_view(request, inner),
        "categories_read" => categories_read(request),
        "categories_save" => categories_save(request, inner),
        "scan" => scan(request, inner),
        "edit_import" => edit_import(request, inner),
        "import" => import(request, inner),
        "move" => move_to_category(request, inner),
        "titles_remove_review" => titles_remove_review(request, inner),
        "titles_remove" => titles_remove(request, inner),
        "indexes" => rebuild_indexes(request, inner),
        "undo" => undo(inner),
        "review" => artwork_review(request, inner),

        // Covers, previews and game information.
        "media_update" => media_update(request, inner),
        "media_selected" => media_selected(request, inner),
        "media_rebuild" => media_rebuild(request, inner),
        "game_information" => game_information(request, inner),
        "game_info_load" => game_info_load(request, inner),
        "game_info_matches" => game_info_matches(request, inner),
        "game_info_choose_match" => game_info_choose_match(request, inner),
        "game_info_backups" => game_info_backups(request, inner),
        "game_info_backup_preview" => game_info_backup_preview(request, inner),
        "game_info_export" => game_info_export(request, inner),
        "game_info_import_preview" => game_info_import_preview(request, inner),
        "game_info_import" => game_info_import(request, inner),
        "game_info_restore_preview" => game_info_restore_preview(request, inner),
        "game_info_save" => game_info_save(request, inner),

        // Drives.
        "drive_browser" => drive_browser(request, inner),
        "drive_inspect" => drive_inspect(request, inner),
        "drive_remember" => drive_remember(request, inner),
        "drive_status" => drive_status(request),
        "drive_check" => drive_check(request),
        "drive_setup" => drive_setup(request, inner),
        "drive_eject" => drive_eject(request, inner),
        "drive_cleanup_review" => drive_cleanup_review(request, inner),
        "drive_cleanup" => drive_cleanup(request, inner),
        "copy_preview" => copy_preview(request, inner),
        "copy" => copy(request, inner),
        "installed_versions" => installed_versions(request),
        "support_export" => support_export(request),
        "recovery_scan" => recovery_scan(request, inner),
        "recovery_remove" => recovery_remove(request, inner),

        // Console settings.
        "config_load" => config_load(request, inner),
        "config_save" => config_save(request, inner),

        // Save slots and backups.
        "slots" => slots(
            &path(request.args, "directory")?,
            request.cache(),
            request.job,
        ),
        "slot_notes_read" => slot_notes_read(request),
        "slot_notes_save" => slot_notes_save(request),
        "slot_label" => slot_label(request),
        "backup_new" => backup_new(inner),
        "backup_open" => backup_open(request, inner),
        "backup_discard" => backup_discard(inner),
        "backup_entry" => backup_entry(request, inner),
        "backup_edit" => backup_edit(request, inner),
        "backup_export" => backup_export(request, inner),
        "backup_import_raw" => backup_import_raw(request, inner),
        "backup_remove" => backup_remove(request, inner),
        "backup_inspect" => backup_inspect(request),
        "backup_merge" => backup_merge(request, inner),
        "backup_save" => backup_save(request, inner),
        "backup_compare" => backup_compare(request, inner),

        // The cheat database.
        "cheats_validate" => cheats_validate(request),
        "cheats_activate" => cheats_activate(request, inner),
        "cheats_load" => cheats_load(request, inner),
        "cheats_download" => cheats_download(inner),
        "cheats_install" => cheats_install(request, inner),
        "cheats_save" => cheats_save(request, inner),

        // Updates.
        "updates_check" => updates_check(request, inner),
        "boot_update" => boot_update(request, inner),
        "self_update" => self_update_download(request, inner),
        "self_update_apply" => self_update_apply(request, inner),
        "self_update_cancel" => self_update_cancel(inner),

        _ => bail!("Unknown desktop operation: {action}"),
    }
}

// ---- The app itself ----

/// Closes the app, first starting a staged update-on-exit (which then
/// installs without reopening the app).
fn quit(request: &Request, inner: &mut Inner) -> Result<Value> {
    if let Some((plan, plan_path, _)) = &mut inner.pending_update {
        plan.restart = false;
        f::atomic(plan_path, &serde_json::to_vec_pretty(plan)?)?;
        self_update::start(plan, plan_path)?;
        inner.pending_update = None;
    }
    request.app.exit(0);
    Ok(Value::Null)
}

/// Called by the interface once it is showing. After an update, tells the
/// installer the new version started (`--update-ready <file>`), and for an
/// update on exit, closes again.
fn ready(request: &Request) -> Result<Value> {
    let args: Vec<String> = std::env::args().collect();
    if let Some(index) = args.iter().position(|arg| arg == "--update-ready") {
        if let Some(ready_file) = args.get(index + 1) {
            fs::write(ready_file, b"ready")?;
            if args.iter().any(|arg| arg == "--update-exit") {
                request.app.exit(0);
            }
        }
    }
    Ok(Value::Null)
}

/// What the interface needs to start: preferences, version, categories,
/// command-line arguments and the last update's outcome.
fn init(request: &Request, inner: &Inner) -> Result<Value> {
    let update_report = self_update::last_report(request.cache(), &std::env::current_exe()?);
    Ok(json!({
        "prefs": inner.prefs,
        "version": env!("CARGO_PKG_VERSION"),
        "categories": CATEGORIES,
        "args": std::env::args().skip(1).collect::<Vec<_>>(),
        "update_report": update_report,
    }))
}

/// Saves new preferences. Changing developer update settings forgets the
/// last update check.
fn save_preferences(request: &Request, inner: &mut Inner) -> Result<Value> {
    let prefs: Preferences = serde_json::from_value(request.args.clone())?;
    request.backend.save_prefs(&prefs)?;
    if prefs.developer_mode != inner.prefs.developer_mode
        || prefs.developer_updates_directory != inner.prefs.developer_updates_directory
    {
        inner.release = None;
    }
    inner.prefs = prefs;
    Ok(json!(inner.prefs))
}

/// Downloads FFmpeg and remembers where it is.
fn install_video_tools(request: &Request, inner: &mut Inner) -> Result<Value> {
    ensure!(
        !inner.prefs.offline,
        "Turn off offline mode to download video tools"
    );
    inner.prefs.video_tools = media::install_video_tools(request.cache(), request.job)?
        .to_string_lossy()
        .into();
    request.backend.save_prefs(&inner.prefs)?;
    Ok(json!(inner.prefs))
}

// ---- The library and importing ----

/// The library view: every disc's record, with whether it is on the chosen
/// drive and whether its cover and preview files are missing.
fn library_view(request: &Request, inner: &Inner) -> Result<Value> {
    let job = request.job;
    job.check()?;
    let root = root(inner)?;
    let database = library::database(&root)?;
    let mut records = database["games"].as_array().cloned().unwrap_or_default();

    let destination = if inner.prefs.destination.is_empty() {
        None
    } else {
        f::root(Path::new(&inner.prefs.destination)).ok()
    };
    let mut presence = destination
        .as_ref()
        .map(|target| core::drive_browser::PresenceScan::new(&root, target))
        .transpose()?;
    let mut paths = f::ReadPaths::new(&root)?;

    let total = records.len() as u64;
    job.report(0, total, "Reading PC library");
    for (index, record) in records.iter_mut().enumerate() {
        job.check()?;
        let on_drive = record["path"]
            .as_str()
            .and_then(|path| presence.as_mut().and_then(|scan| scan.check(path).ok()));
        record["on_drive"] = json!(on_drive);

        for (field, folder) in [("cover", "covers"), ("preview", "previews")] {
            let missing = record[field].as_str().is_none_or(|name| {
                paths
                    .path(&format!(".ohmy3do/{folder}/{name}"))
                    .map_or(true, |file| !file.is_file())
            });
            record[format!("{field}_missing")] = json!(missing);
        }
        job.report(index as u64 + 1, total, "Reading PC library");
    }
    job.check()?;

    Ok(json!({
        "root": root,
        "records": records,
        "categories": categories::load(&root)?.names(),
        "destination_available": destination.is_some(),
    }))
}

/// A library's or drive's categories.
fn categories_read(request: &Request) -> Result<Value> {
    let root = f::root(&path(request.args, "root")?)?;
    Ok(json!({"root": root, "config": categories::load(&root)?}))
}

/// Saves edited categories (renaming and removing folders as needed).
/// Forgets any scanned import or reviewed copy, whose paths may change.
fn categories_save(request: &Request, inner: &mut Inner) -> Result<Value> {
    let root = f::root(&path(request.args, "root")?)?;
    let original = serde_json::from_value(request.args["original"].clone())?;
    let config = serde_json::from_value(request.args["config"].clone())?;
    inner.last = Some(categories::save(
        &root,
        &original,
        &config,
        request.cache(),
        request.job,
    )?);
    inner.import = None;
    inner.copy = None;
    Ok(journal_response(inner))
}

/// Scans the chosen files and folders for discs to import.
fn scan(request: &Request, inner: &mut Inner) -> Result<Value> {
    let inputs: Vec<PathBuf> = serde_json::from_value(request.args["paths"].clone())?;
    let catalog = catalog::load(request.cache(), inner.prefs.offline, request.job)?;
    let plan = library::scan(
        &root(inner)?,
        &inputs,
        catalog,
        inner.prefs.quick,
        request.job,
    )?;
    let response = json!(plan);
    inner.import = Some(plan);
    Ok(response)
}

/// Changes one entry of the scanned import: the release chosen for it
/// ("game"), its category, an unverified title, or whether it is selected.
fn edit_import(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let config = categories::load(&root(inner)?)?;
    let plan = inner.import.as_mut().context("Scan discs first")?;
    let index = args["index"].as_u64().context("Choose an import")? as usize;
    let entry = plan.entries.get_mut(index).context("Import changed")?;

    if let Some(game_index) = args["game"].as_u64() {
        entry.blocked = false;
        entry.confidence = "manual".into();
        entry.reason = "Explicit catalog selection".into();
        let game = plan
            .catalog
            .games
            .get(game_index as usize)
            .context("Release changed")?;
        library::assign(entry, game, Some(game_index as usize), None);
        library::apply_category_defaults(entry, &config);
    }

    if let Some(category) = args["category"].as_str() {
        ensure!(config.contains(category), "Invalid category");
        library::choose_import_category(entry, &plan.catalog, category);
    }

    if args["unverified"].as_bool() == Some(true) {
        let title = text(args, "title")?.trim();
        ensure!(!title.is_empty(), "Enter a title");
        let game = Game {
            name: title.into(),
            release: title.into(),
            region: Some(text(args, "region")?.into()),
            ..Default::default()
        };
        ensure!(config.contains(text(args, "category")?), "Invalid category");
        entry.blocked = false;
        entry.reason = "Explicit unverified import".into();
        entry.confidence = "unverified".into();
        library::assign(entry, &game, None, Some(text(args, "category")?));
        entry.selected = entry.resolvable();
    }

    if let Some(selected) = args["selected"].as_bool() {
        ensure!(
            !selected || entry.resolvable(),
            "Choose a category or identify this disc before selecting it"
        );
        entry.selected = selected;
    }

    library::collisions(plan)?;
    Ok(json!(plan))
}

/// Imports the scanned discs that are selected.
fn import(request: &Request, inner: &mut Inner) -> Result<Value> {
    let plan = inner.import.as_ref().context("Scan discs first")?;
    inner.last = Some(library::import(
        plan,
        &inner.prefs,
        request.cache(),
        request.job,
    )?);
    inner.import = None;
    Ok(journal_response(inner))
}

/// Moves games to another category.
fn move_to_category(request: &Request, inner: &mut Inner) -> Result<Value> {
    inner.last = Some(library::recategorize(
        &root(inner)?,
        &strings(request.args, "discs")?,
        text(request.args, "category")?,
        request.cache(),
        request.job,
    )?);
    Ok(journal_response(inner))
}

/// Reviews removing games, from the library or (with `browser_root` and
/// `drive_token`) from a drive.
fn titles_remove_review(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    inner.removal = None;
    let on_drive = args["browser_root"].as_str().is_some();
    let target = if on_drive {
        f::root(&path(args, "browser_root")?)?
    } else {
        root(inner)?
    };
    let token = if on_drive {
        let token = text(args, "drive_token")?;
        core::drive_identity::check(&target, &inner.prefs, token)?;
        Some(token.to_owned())
    } else {
        None
    };

    let plan = core::title_removal::review(&target, &strings(args, "discs")?, request.job)?;
    let response = json!(plan);
    inner.removal = Some((plan, token));
    Ok(response)
}

/// Carries out the reviewed removal, if the library or drive is the same.
fn titles_remove(request: &Request, inner: &mut Inner) -> Result<Value> {
    let (plan, token) = inner.removal.take().context("Review title removal first")?;
    match token {
        Some(token) => core::drive_identity::check(&plan.root, &inner.prefs, &token)?,
        None => ensure!(
            plan.root == root(inner)?,
            "Library changed. Review removal again."
        ),
    }

    let result = core::title_removal::apply(&plan, request.cache(), request.job);
    // Earlier journals may restore metadata for now-deleted game files.
    inner.last = None;
    inner.copy = None;
    inner.import = None;
    Ok(json!(result?))
}

/// Rebuilds the library's index files.
fn rebuild_indexes(request: &Request, inner: &mut Inner) -> Result<Value> {
    let root = root(inner)?;
    let mut journal = library::Journal::new(&root, request.cache())?;
    if let Err(error) = library::build_indexes(&root, &mut journal, request.job) {
        journal.undo()?;
        return Err(error);
    }
    journal.finish()?;
    inner.last = Some(journal.path);
    Ok(journal_response(inner))
}

/// Undoes the last change.
fn undo(inner: &mut Inner) -> Result<Value> {
    let journal = inner.last.as_ref().context("No change to undo")?;
    library::undo(journal)?;
    inner.last = None;
    Ok(json!(true))
}

/// Writes the artwork review page.
fn artwork_review(request: &Request, inner: &Inner) -> Result<Value> {
    let page = library::review(&root(inner)?, request.cache())?;
    Ok(json!({"path": page}))
}

// ---- Covers, previews and game information ----

/// Fills in missing media for the library, or sets one disc's chosen cover
/// and/or preview.
fn media_update(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let root = root(inner)?;
    let provided = args["disc"].as_str().map(|disc| {
        (
            disc,
            args["cover"].as_str().map(PathBuf::from),
            args["preview"].as_str().map(PathBuf::from),
        )
    });
    inner.last = Some(library::update_media(
        &root,
        &inner.prefs,
        request.cache(),
        provided,
        request.job,
    )?);
    Ok(journal_response(inner))
}

/// Fills in missing media for the chosen discs.
fn media_selected(request: &Request, inner: &mut Inner) -> Result<Value> {
    inner.last = Some(library::update_selected_media(
        &root(inner)?,
        &inner.prefs,
        request.cache(),
        &strings(request.args, "discs")?,
        request.job,
    )?);
    Ok(journal_response(inner))
}

/// Re-converts all media from the original files.
fn media_rebuild(request: &Request, inner: &mut Inner) -> Result<Value> {
    inner.last = Some(library::rebuild_media(
        &root(inner)?,
        &inner.prefs,
        request.cache(),
        request.job,
    )?);
    Ok(journal_response(inner))
}

/// Refreshes downloaded game information for the library.
fn game_information(request: &Request, inner: &mut Inner) -> Result<Value> {
    inner.last = Some(core::game_info::refresh(
        &root(inner)?,
        &inner.prefs,
        request.cache(),
        request.job,
    )?);
    Ok(journal_response(inner))
}

/// The chosen discs' information, for editing.
fn game_info_load(request: &Request, inner: &Inner) -> Result<Value> {
    Ok(json!(core::game_info::edit_view(
        &root(inner)?,
        &strings(request.args, "discs")?
    )?))
}

/// Reviews which downloaded information each disc matches.
fn game_info_matches(request: &Request, inner: &mut Inner) -> Result<Value> {
    let review =
        core::game_info::review_matches(&root(inner)?, &inner.prefs, request.cache(), request.job)?;
    let token = uuid::Uuid::new_v4().to_string();
    let response = json!({
        "token": token,
        "records": review.records,
        "choices": review.choices,
        "status": review.status,
    });
    inner.information_matches = Some((token, review));
    Ok(response)
}

/// Applies the person's choice of downloaded information for some discs.
fn game_info_choose_match(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let (token, review) = inner
        .information_matches
        .as_ref()
        .context("Review information matches first")?;
    ensure!(
        token == text(args, "token")?,
        "Information match review changed; reopen it"
    );
    inner.last = Some(core::game_info::choose_match(
        &root(inner)?,
        review,
        &strings(args, "discs")?,
        text(args, "id")?,
        request.cache(),
        request.job,
    )?);
    inner.information_matches = None;
    Ok(journal_response(inner))
}

/// The automatic backups of the person's own game information.
fn game_info_backups(request: &Request, inner: &Inner) -> Result<Value> {
    Ok(json!(core::game_info::backups(
        &root(inner)?,
        request.cache()
    )?))
}

/// Reviews restoring game information from a backup.
fn game_info_backup_preview(request: &Request, inner: &mut Inner) -> Result<Value> {
    let plan = core::game_info::preview_backup(
        &root(inner)?,
        request.cache(),
        text(request.args, "name")?,
        request.job,
    )?;
    let token = uuid::Uuid::new_v4().to_string();
    let response = json!({"token": token, "records": plan.records, "skipped": plan.skipped});
    inner.information_import = Some((token, plan));
    Ok(response)
}

/// Exports the chosen discs' own information to a file.
fn game_info_export(request: &Request, inner: &Inner) -> Result<Value> {
    let count = core::game_info::export_custom(
        &root(inner)?,
        &path(request.args, "target")?,
        &strings(request.args, "discs")?,
        request.job,
    )?;
    Ok(json!({"count": count}))
}

/// Reviews importing game information from a file.
fn game_info_import_preview(request: &Request, inner: &mut Inner) -> Result<Value> {
    let plan =
        core::game_info::preview_import(&root(inner)?, &path(request.args, "file")?, request.job)?;
    let token = uuid::Uuid::new_v4().to_string();
    let response = json!({"token": token, "records": plan.records, "skipped": plan.skipped});
    inner.information_import = Some((token, plan));
    Ok(response)
}

/// Imports the reviewed game information (from a file or a backup) for the
/// chosen discs.
fn game_info_import(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let (token, plan) = inner
        .information_import
        .as_ref()
        .context("Preview custom metadata before importing")?;
    ensure!(
        token == text(args, "token")?,
        "Import preview changed; review it again"
    );
    inner.last = Some(core::game_info::import_custom(
        &root(inner)?,
        plan,
        &strings(args, "discs")?,
        request.cache(),
        request.job,
    )?);
    inner.information_import = None;
    Ok(journal_response(inner))
}

/// Reviews restoring downloaded information over the person's edits.
fn game_info_restore_preview(request: &Request, inner: &mut Inner) -> Result<Value> {
    let original: Vec<core::game_info::EditRecord> =
        serde_json::from_value(request.args["original"].clone())?;
    let plan = core::game_info::preview_restore(
        &root(inner)?,
        &original,
        &inner.prefs,
        request.cache(),
        request.job,
    )?;
    let token = uuid::Uuid::new_v4().to_string();
    let response = json!({"token": token, "records": plan.records, "available": plan.available});
    inner.information_restore = Some((token, plan));
    Ok(response)
}

/// Saves edited game information, restoring downloaded values for any
/// `restore_fields` (which needs a matching restore preview).
fn game_info_save(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let expected: Vec<core::game_info::EditRecord> =
        serde_json::from_value(args["original"].clone())?;
    let restore_fields: Vec<String> = if args["restore_fields"].is_array() {
        strings(args, "restore_fields")?
    } else {
        vec![]
    };

    let journal = if restore_fields.is_empty() {
        core::game_info::save_edits(
            &root(inner)?,
            &expected,
            &args["patch"],
            request.cache(),
            request.job,
        )?
    } else {
        let (token, plan) = inner
            .information_restore
            .as_ref()
            .context("Preview downloaded information before restoring")?;
        ensure!(
            token == text(args, "restore_token")?,
            "Restore preview changed; review it again"
        );
        core::game_info::save_edits_restoring(
            &root(inner)?,
            &expected,
            &args["patch"],
            plan,
            &restore_fields,
            request.cache(),
            request.job,
        )?
    };
    inner.last = Some(journal);
    inner.information_restore = None;
    Ok(journal_response(inner))
}

// ---- Drives ----

/// What is on a drive, for the Drive view.
fn drive_browser(request: &Request, inner: &Inner) -> Result<Value> {
    core::drive_browser::scan(&path(request.args, "root")?, &inner.prefs, request.job)
}

/// A drive's identity (name and token).
fn drive_inspect(request: &Request, inner: &Inner) -> Result<Value> {
    Ok(json!(core::drive_identity::inspect(
        &path(request.args, "root")?,
        &inner.prefs
    )?))
}

/// Remembers a drive under a name.
fn drive_remember(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let identity = core::drive_identity::remember(
        &path(args, "root")?,
        text(args, "name")?,
        &mut inner.prefs,
        text(args, "token")?,
    )?;
    request.backend.save_prefs(&inner.prefs)?;
    Ok(json!({"identity": identity, "prefs": inner.prefs}))
}

/// Whether each saved drive is connected and can be ejected.
fn drive_status(request: &Request) -> Result<Value> {
    let statuses: Vec<_> = strings(request.args, "roots")?
        .iter()
        .map(|root| core::eject::status(Path::new(root)))
        .collect();
    Ok(json!(statuses))
}

/// The drive readiness check.
fn drive_check(request: &Request) -> Result<Value> {
    Ok(json!(core::readiness::check(
        &path(request.args, "root")?,
        request.job
    )?))
}

/// Installs the menu on a drive: the latest release (or a local developer
/// build), or a chosen `boot.iso`.
fn drive_setup(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let destination = path(args, "destination")?;
    core::drive_identity::check(&destination, &inner.prefs, text(args, "drive_token")?)?;
    let settings: console_config::Settings =
        serde_json::from_value(args["settings"].clone()).unwrap_or_default();
    let cache = request.cache();
    let job = request.job;

    let journal = if args["release"].as_bool() == Some(true) {
        if inner.prefs.developer_mode {
            ensure!(
                !inner.prefs.developer_updates_directory.is_empty(),
                "Choose a local update directory in Settings & tools"
            );
            setup::install_developer(
                Path::new(&inner.prefs.developer_updates_directory),
                &destination,
                cache,
                &settings,
                job,
            )?
        } else {
            ensure!(
                !inner.prefs.offline,
                "Turn off offline mode to download the menu"
            );
            setup::install_release(&destination, cache, &settings, job)?
        }
    } else {
        setup::install_local(&destination, cache, &path(args, "source")?, &settings, job)?
    };
    inner.last = Some(journal);
    Ok(journal_response(inner))
}

/// Ejects a drive. Refuses while the cache, the app or an unsaved backup is
/// on it. Says whether the library or an open backup was on the drive (the
/// backup is closed).
fn drive_eject(request: &Request, inner: &mut Inner) -> Result<Value> {
    let target = path(request.args, "root")?;
    let canonical = f::root(&target)?;
    let is_on_drive = |file: &Path| {
        file.canonicalize()
            .is_ok_and(|file| file.starts_with(&canonical))
    };
    let library_closed = is_on_drive(Path::new(&inner.prefs.library));
    let backup_path = inner
        .backup
        .as_ref()
        .and_then(|backup| backup.path.as_ref());
    let backup_closed = backup_path.is_some_and(|file| is_on_drive(file));

    let mut protected = vec![request.backend.cache.clone(), std::env::current_exe()?];
    if let Some(file) = inner
        .backup
        .as_ref()
        .filter(|backup| backup.dirty)
        .and_then(|backup| backup.path.clone())
    {
        protected.push(file);
    }

    let result = core::eject::eject(&target, &protected)?;
    if backup_closed {
        inner.backup = None;
    }
    inner.copy = None;
    inner.cleanup = None;
    Ok(json!({
        "root": result.root,
        "message": result.message,
        "backup_closed": backup_closed,
        "library_closed": library_closed,
    }))
}

/// Reviews removing the menu's files from a drive.
fn drive_cleanup_review(request: &Request, inner: &mut Inner) -> Result<Value> {
    inner.drive_cleanup = None;
    let plan = core::drive_cleanup::review(&path(request.args, "root")?, request.job)?;
    let response = json!(plan);
    inner.drive_cleanup = Some(plan);
    Ok(response)
}

/// Carries out the reviewed drive cleanup. Save backups go to the chosen PC
/// folder (`saves_to`), or are deleted only when the person explicitly chose
/// not to keep them (`skip_saves`).
fn drive_cleanup(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let plan = inner
        .drive_cleanup
        .take()
        .context("Review drive cleanup first")?;
    let saves_to = args["saves_to"]
        .as_str()
        .filter(|folder| !folder.is_empty())
        .map(PathBuf::from);
    ensure!(
        plan.saves.is_empty() || saves_to.is_some() || args["skip_saves"].as_bool() == Some(true),
        "Choose where to keep the save backups"
    );

    let open_backup_on_drive = inner
        .backup
        .as_ref()
        .and_then(|backup| backup.path.as_ref())
        .is_some_and(|file| {
            file.canonicalize()
                .is_ok_and(|file| file.starts_with(&plan.root))
        });
    ensure!(
        !(open_backup_on_drive && inner.backup.as_ref().is_some_and(|backup| backup.dirty)),
        "Save or close the open save backup from this drive first"
    );

    let cleaned = core::drive_cleanup::apply(&plan, saves_to.as_deref(), request.job)?;
    if open_backup_on_drive {
        inner.backup = None;
    }
    inner.copy = None;
    inner.cleanup = None;
    Ok(json!({"cleaned": cleaned, "backup_closed": open_backup_on_drive}))
}

/// Reviews copying the chosen games to a drive. With `existing_only`, only
/// checks games already on the drive (refusing if any game file is
/// missing or folders would move).
fn copy_preview(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let drive = core::drive_identity::inspect(&path(args, "destination")?, &inner.prefs)?;
    ensure!(
        !drive.mismatch,
        "Different drive at this location. Inspect and remember it in Drive setup first."
    );
    let plan = usb::plan_selected(
        &root(inner)?,
        &path(args, "destination")?,
        request.cache(),
        args["verify"].as_bool().unwrap_or(false),
        &strings(args, "discs")?,
        request.job,
    )?;

    if args["existing_only"].as_bool().unwrap_or(false) {
        if let Some(expected) = args["drive_token"].as_str() {
            ensure!(
                drive.token == expected,
                "Drive changed; refresh and review again"
            );
        }
        ensure!(
            plan.moves.is_empty(),
            "Drive categories differ. Use the normal copy review to reconcile folders first."
        );
        let games_complete = plan.files.iter().all(|file| {
            let name = file.path.to_ascii_lowercase();
            let support =
                name == "boot.iso" || name.starts_with(".ohmy3do/") || name.contains("/.ohmy3do/");
            support || file.status == "unchanged"
        });
        ensure!(
            games_complete,
            "Some game files are missing. Use Copy selected to USB to repair them first."
        );
    }

    let response = json!({
        "summary": usb::summary(&plan),
        "bytes": plan.bytes,
        "games": plan.games,
        "folders_to_move": plan.moves.len(),
        "files": plan.files,
        "destination": plan.destination,
        "storage": usb::storage_estimate(&plan)?,
        "drive": drive,
    });
    inner.copy = Some(plan);
    Ok(response)
}

/// Carries out the reviewed copy, if the library and drive are the same.
fn copy(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let plan = inner.copy.as_ref().context("Preview the copy first")?;
    ensure!(
        f::root(&path(args, "destination")?)? == plan.destination && root(inner)? == plan.source,
        "Folders changed; preview again"
    );
    core::drive_identity::check(&plan.destination, &inner.prefs, text(args, "drive_token")?)?;
    inner.last = Some(usb::apply(plan, request.cache(), request.job)?);
    inner.copy = None;
    Ok(journal_response(inner))
}

/// Whether a drive has a menu, and its version if known.
fn installed_versions(request: &Request) -> Result<Value> {
    updates::installed_menu(&path(request.args, "root")?, request.cache(), request.job)
}

/// Writes a support report for a drive.
fn support_export(request: &Request) -> Result<Value> {
    let args = request.args;
    core::support_report::export(
        &path(args, "root")?,
        &path(args, "target")?,
        &strings(args, "activity")?,
        request.job,
    )?;
    Ok(json!(true))
}

/// Finds recovery copies (left by earlier saves) in a folder.
fn recovery_scan(request: &Request, inner: &mut Inner) -> Result<Value> {
    let inventory = core::recovery::scan(&path(request.args, "directory")?, request.job)?;
    let response = json!(inventory);
    inner.cleanup = Some(inventory);
    Ok(response)
}

/// Deletes the chosen recovery copies (never the open backup).
fn recovery_remove(request: &Request, inner: &mut Inner) -> Result<Value> {
    let selected = strings(request.args, "selected")?;
    let review = inner
        .cleanup
        .as_ref()
        .context("Scan recovery copies first")?;
    if let Some(open) = inner
        .backup
        .as_ref()
        .and_then(|backup| backup.path.as_ref())
    {
        let open = open.canonicalize()?;
        ensure!(
            !selected
                .iter()
                .any(|name| review.directory.join(name) == open),
            "Close the open recovery copy before deleting it"
        );
    }
    let removed = core::recovery::remove(review, &selected, request.job)?;
    inner.cleanup = None;
    Ok(json!(removed))
}

// ---- Console settings ----

/// Loads a drive's console settings for editing.
fn config_load(request: &Request, inner: &mut Inner) -> Result<Value> {
    let loaded = console_config::load(&path(request.args, "root")?)?;
    let response = json!({
        "root": loaded.root,
        "settings": loaded.settings,
        "exists": loaded.exists(),
    });
    inner.config = Some(loaded);
    Ok(response)
}

/// Saves the edited console settings.
fn config_save(request: &Request, inner: &mut Inner) -> Result<Value> {
    let loaded = inner.config.as_ref().context("Load settings first")?;
    let settings: console_config::Settings =
        serde_json::from_value(request.args["settings"].clone())?;
    let saved = console_config::save(loaded, &settings, request.cache(), request.job)?;
    inner.last = Some(saved.journal);
    inner.config = Some(console_config::load(&loaded.root)?);
    Ok(json!(true))
}

// ---- Save slots and backups ----

/// The save slots in a folder, refreshing its catalog, as the Save Manager
/// lists them.
fn slots(directory: &Path, cache: &Path, job: &Job) -> Result<Value> {
    let slots = save_catalog::refresh(directory, cache, job)?;
    let listed: Vec<Value> = slots
        .iter()
        .map(|slot| {
            // Everything the Save Manager's search box matches against.
            let search = format!(
                "{} {} {} {}",
                slot.name(),
                slot.games(),
                slot.aliases.join(" "),
                slot.notes
            );
            json!({
                "id": slot.id,
                "path": slot.path,
                "name": slot.name(),
                "label": slot.label,
                "notes": slot.notes,
                "valid": slot.valid,
                "games": slot.games(),
                "files": slot.files,
                "search": search,
            })
        })
        .collect();
    Ok(json!(listed))
}

/// A slot's notes.
fn slot_notes_read(request: &Request) -> Result<Value> {
    let notes = core::slot_notes::read(&path(request.args, "path")?)?;
    Ok(json!({"notes": notes}))
}

/// Saves a slot's notes (refusing if they changed since `original`).
fn slot_notes_save(request: &Request) -> Result<Value> {
    let args = request.args;
    let slot = path(args, "path")?;
    core::slot_notes::save(&slot, text(args, "notes")?, args["original"].as_str())?;
    Ok(json!(true))
}

/// Sets a numbered slot's label, then lists the slots again.
fn slot_label(request: &Request) -> Result<Value> {
    let slot = path(request.args, "path")?;
    let (page, index) = backup::slot(&slot).context("Choose a numbered slot")?;
    let (mut labels, original) = backup::Labels::read(&page)?;
    labels.0[index] = text(request.args, "label")?.into();
    labels.save(&page, original.as_deref())?;
    slots(
        slot.parent().context("Missing slot directory")?,
        request.cache(),
        request.job,
    )
}

/// An open backup as the Save Manager shows it: its saves, with each one's
/// known game and cheat profile.
fn backup_view(backup: &Backup) -> Value {
    let entries: Vec<Value> = backup
        .archive
        .entries
        .iter()
        .map(|entry| {
            json!({
                "name": entry.label(),
                "known": core::saves::known_title(&entry.label()),
                "file_type": entry.file_type,
                "size": entry.data.len(),
                "profile": save_cheats::find(entry),
            })
        })
        .collect();
    json!({
        "path": backup.path,
        "dirty": backup.dirty,
        "bytes": backup.archive.encode().map(|bytes| bytes.len()).unwrap_or_default(),
        "entries": entries,
    })
}

/// The save chosen by the `index` argument.
fn entry_index(args: &Value, backup: &Backup) -> Result<usize> {
    let index = args["index"].as_u64().context("Choose a save")? as usize;
    ensure!(
        index < backup.archive.entries.len(),
        "Save selection changed"
    );
    Ok(index)
}

/// Refuses to replace the open backup while it has unsaved changes.
fn ensure_backup_saved(inner: &Inner) -> Result<()> {
    ensure!(
        inner.backup.as_ref().is_none_or(|backup| !backup.dirty),
        "Save or discard the current backup first"
    );
    Ok(())
}

/// Starts a new, empty backup.
fn backup_new(inner: &mut Inner) -> Result<Value> {
    ensure_backup_saved(inner)?;
    let backup = inner.backup.insert(Backup {
        path: None,
        original: None,
        archive: Archive::default(),
        dirty: false,
    });
    Ok(backup_view(backup))
}

/// Opens a backup, and uses the cheat database that belongs with it.
fn backup_open(request: &Request, inner: &mut Inner) -> Result<Value> {
    ensure_backup_saved(inner)?;
    let file = path(request.args, "path")?;
    let (archive, original) = Archive::read_with_bytes(&file)?;
    save_cheats::activate(save_cheats::for_backup(&file)?);
    let backup = inner.backup.insert(Backup {
        path: Some(file),
        original: Some(original),
        archive,
        dirty: false,
    });
    Ok(backup_view(backup))
}

/// Closes the open backup without saving.
fn backup_discard(inner: &mut Inner) -> Result<Value> {
    inner.backup = None;
    Ok(Value::Null)
}

/// One save's contents (as hex and bytes), and its cheat profile and
/// current values if its format is known.
fn backup_entry(request: &Request, inner: &Inner) -> Result<Value> {
    let backup = inner.backup.as_ref().context("Open a backup")?;
    let entry = &backup.archive.entries[entry_index(request.args, backup)?];
    let profile = save_cheats::find(entry);
    let values = profile.as_ref().map(|profile| {
        (0..profile.fields.len())
            .map(|field| profile.value(entry, field))
            .collect::<Vec<_>>()
    });
    let hex = entry
        .data
        .iter()
        .map(|byte| format!("{byte:02X}"))
        .collect::<Vec<_>>()
        .join(" ");
    Ok(json!({
        "name": entry.label(),
        "hex": hex,
        "data": entry.data,
        "file_type": entry.file_type,
        "profile": profile,
        "values": values,
    }))
}

/// Edits one save: new contents as hex, a cheat field's value, or a cheat
/// preset. Checked on a copy, so a bad edit changes nothing.
fn backup_edit(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let backup = inner.backup.as_mut().context("Open a backup")?;
    let index = entry_index(args, backup)?;
    let mut edited = backup.archive.clone();
    let entry = &mut edited.entries[index];

    if let Some(hex) = args["hex"].as_str() {
        entry.data = core::save_archive::decode_hex(hex)?;
    }
    if let Some(field) = args["field"].as_u64() {
        let profile = save_cheats::find(entry).context("No known format for this save")?;
        let value = args["value"]
            .as_u64()
            .context("Missing value")?
            .try_into()?;
        profile.set(entry, field as usize, value)?;
    }
    if let Some(preset) = args["preset"].as_u64() {
        save_cheats::find(entry)
            .context("No known format for this save")?
            .apply_preset(entry, preset as usize)?;
    }

    edited.encode()?;
    backup.archive = edited;
    backup.dirty = true;
    Ok(backup_view(backup))
}

/// Exports one save, as its raw contents or as a one-save archive. Asks
/// before replacing an existing file.
fn backup_export(request: &Request, inner: &Inner) -> Result<Value> {
    let args = request.args;
    let backup = inner.backup.as_ref().context("Open a backup")?;
    let entry = backup.archive.entries[entry_index(args, backup)?].clone();
    let bytes = if args["raw"].as_bool() == Some(true) {
        entry.data
    } else {
        Archive {
            entries: vec![entry],
        }
        .encode()?
    };

    let target = path(args, "path")?;
    let expected = core::save_archive::existing_bytes(&target)?;
    ensure!(
        expected.is_none() || args["overwrite"].as_bool() == Some(true),
        "Confirm replacing the destination export"
    );
    let saved = core::save_archive::save_bytes(&target, &bytes, expected.as_deref())?;
    Ok(json!({"recovery": saved.backup}))
}

/// Replaces one save's contents with a raw file's.
fn backup_import_raw(request: &Request, inner: &mut Inner) -> Result<Value> {
    let backup = inner.backup.as_mut().context("Open a backup")?;
    let index = entry_index(request.args, backup)?;
    let bytes = core::save_archive::read_bytes(&path(request.args, "path")?)?;
    let mut edited = backup.archive.clone();
    edited.entries[index].data = bytes;
    edited.encode()?;
    backup.archive = edited;
    backup.dirty = true;
    Ok(backup_view(backup))
}

/// Removes the chosen saves.
fn backup_remove(request: &Request, inner: &mut Inner) -> Result<Value> {
    let backup = inner.backup.as_mut().context("Open a backup")?;
    let chosen = indices(request.args, "indices")?;
    ensure!(
        chosen
            .iter()
            .all(|index| *index < backup.archive.entries.len()),
        "Selection changed"
    );
    backup.archive.entries = backup
        .archive
        .entries
        .iter()
        .enumerate()
        .filter(|(index, _)| !chosen.contains(index))
        .map(|(_, entry)| entry.clone())
        .collect();
    backup.dirty = true;
    Ok(backup_view(backup))
}

/// Lists another backup's saves, for choosing some to merge.
fn backup_inspect(request: &Request) -> Result<Value> {
    let archive = Archive::read(&path(request.args, "path")?)?;
    let listed: Vec<Value> = archive
        .entries
        .iter()
        .enumerate()
        .map(|(index, entry)| {
            json!({
                "index": index,
                "name": entry.label(),
                "size": entry.data.len(),
                "known": core::saves::known_title(&entry.label()),
            })
        })
        .collect();
    Ok(json!(listed))
}

/// Merges the chosen saves from another backup into the open one.
fn backup_merge(request: &Request, inner: &mut Inner) -> Result<Value> {
    let archive = Archive::read(&path(request.args, "path")?)?;
    let entries = indices(request.args, "indices")?
        .iter()
        .map(|index| {
            archive
                .entries
                .get(*index)
                .cloned()
                .context("Source save changed")
        })
        .collect::<Result<Vec<Entry>>>()?;
    let backup = inner
        .backup
        .as_mut()
        .context("Create or open a backup first")?;
    backup.archive.merge_entries(&entries)?;
    backup.dirty = true;
    Ok(backup_view(backup))
}

/// Saves the open backup to `path`. Asks before replacing a file (and checks
/// the file it was opened from did not change). Saving a numbered slot also
/// refreshes the slot catalog; a catalog problem is reported as a warning.
fn backup_save(request: &Request, inner: &mut Inner) -> Result<Value> {
    let args = request.args;
    let backup = inner.backup.as_mut().context("Open a backup")?;
    let target = path(args, "path")?;
    let expected = if backup.path.as_ref() == Some(&target) {
        backup.original.clone()
    } else {
        core::save_archive::existing_bytes(&target)?
    };
    ensure!(
        expected.is_none() || args["overwrite"].as_bool() == Some(true),
        "Confirm replacing the entire destination slot/archive"
    );

    let saved = backup.archive.save(&target, expected.as_deref())?;
    backup.original = Some(core::save_archive::read_bytes(&target)?);
    backup.path = Some(target.clone());
    backup.dirty = false;

    let catalog = if backup::slot(&target).is_some() {
        Some(slots(
            target.parent().unwrap(),
            request.cache(),
            request.job,
        ))
    } else {
        None
    };
    let catalog_warning = catalog
        .and_then(|result| result.err())
        .map(|error| format!("{error:#}"));
    Ok(json!({
        "backup": backup_view(backup),
        "recovery": saved.backup,
        "catalog_warning": catalog_warning,
    }))
}

/// Compares a backup file with the open backup.
fn backup_compare(request: &Request, inner: &Inner) -> Result<Value> {
    let before = Archive::read(&path(request.args, "path")?)?;
    let after = &inner.backup.as_ref().context("Open a backup")?.archive;
    Ok(json!(backup::comparison(&before, after)))
}

// ---- The cheat database ----

/// Checks edited cheat database JSON.
fn cheats_validate(request: &Request) -> Result<Value> {
    let database = save_cheats::Database::parse(text(request.args, "json")?.as_bytes())?;
    Ok(json!(database))
}

/// Uses edited cheat database JSON for recognizing saves.
fn cheats_activate(request: &Request, inner: &mut Inner) -> Result<Value> {
    let database = save_cheats::Database::parse(text(request.args, "json")?.as_bytes())?;
    save_cheats::activate(Some(database.clone()));
    inner.cheats = database;
    Ok(Value::Null)
}

/// Loads a cheat database file for editing, or the built-in one.
fn cheats_load(request: &Request, inner: &mut Inner) -> Result<Value> {
    let file = request.args["path"].as_str().map(PathBuf::from);
    let (database, raw) = match &file {
        Some(file) => {
            let bytes = fs::read(file)?;
            (save_cheats::Database::parse(&bytes)?, Some(bytes))
        }
        None => (save_cheats::Database::bundled(), None),
    };
    inner.cheats = database;
    inner.cheats_path = file;
    inner.cheats_original = raw;
    Ok(json!(inner.cheats))
}

/// Adds the official database's new profiles to the one being edited.
fn cheats_download(inner: &mut Inner) -> Result<Value> {
    ensure!(
        !inner.prefs.offline,
        "Turn off offline mode to download definitions"
    );
    inner.cheats = save_cheats::merge_missing(&inner.cheats, &save_cheats::download()?)?;
    Ok(json!(inner.cheats))
}

/// Installs a cheat database on a drive.
fn cheats_install(request: &Request, inner: &mut Inner) -> Result<Value> {
    let database = save_cheats::Database::parse(text(request.args, "json")?.as_bytes())?;
    inner.last = Some(save_cheats::install(
        &path(request.args, "root")?,
        &database,
        request.cache(),
    )?);
    inner.cheats = database;
    Ok(journal_response(inner))
}

/// Saves the edited cheat database to a file (refusing if the file changed
/// since it was loaded).
fn cheats_save(request: &Request, inner: &mut Inner) -> Result<Value> {
    let database = save_cheats::Database::parse(text(request.args, "json")?.as_bytes())?;
    let target = path(request.args, "path")?;
    let expected = if inner.cheats_path.as_ref() == Some(&target) {
        inner.cheats_original.clone()
    } else if target.exists() {
        Some(fs::read(&target)?)
    } else {
        None
    };
    save_cheats::save_json(&target, &database, expected.as_deref(), request.cache())?;
    inner.cheats_original = Some(fs::read(&target)?);
    inner.cheats_path = Some(target);
    inner.cheats = database;
    Ok(json!(true))
}

// ---- Updates ----

/// Checks for menu and desktop updates (on GitHub, or in the local build
/// folder in developer mode), and which drives' menus differ from the
/// offered one.
fn updates_check(request: &Request, inner: &mut Inner) -> Result<Value> {
    let job = request.job;
    ensure!(
        inner.prefs.developer_mode || !inner.prefs.offline,
        "Turn off offline mode to check for updates"
    );
    let roots = [
        PathBuf::from(&inner.prefs.library),
        PathBuf::from(&inner.prefs.destination),
        request.args["root"]
            .as_str()
            .map(PathBuf::from)
            .unwrap_or_default(),
    ];

    let check: updates::Offers = if inner.prefs.developer_mode {
        ensure!(
            !inner.prefs.developer_updates_directory.is_empty(),
            "Choose a local update directory in Settings & tools"
        );
        updates::check_local(
            Path::new(&inner.prefs.developer_updates_directory),
            &roots,
            job,
        )?
    } else {
        updates::check(&roots, job)?.into()
    };
    if let (Some(menu), Some(digest)) = (&check.menu, &check.boot_digest) {
        updates::remember_menu(request.cache(), digest, &menu.version)?;
    }

    let desktop_newer = check
        .desktop
        .as_ref()
        .map(|release| {
            release.desktop_available(&std::env::current_exe()?, env!("CARGO_PKG_VERSION"), job)
        })
        .transpose()?
        .unwrap_or(false);
    let targets: Vec<Value> = check
        .targets
        .iter()
        .filter(|target| target.digest != check.boot_digest)
        .map(|target| json!({"root": target.root, "installed": target.digest.is_some()}))
        .collect();
    let describe = |release: &updates::Release| {
        json!({
            "tag": release.tag,
            "version": release.version,
            "notes": release.notes,
            "url": release.page(),
            "local": release.is_local(),
        })
    };
    let response = json!({
        "desktop": desktop_newer,
        "local": inner.prefs.developer_mode,
        "menu_release": check.menu.as_ref().map(describe),
        "desktop_release": check.desktop.as_ref().map(describe),
        "targets": targets,
        "warnings": check.warnings,
    });
    inner.release = Some(check);
    Ok(response)
}

/// Installs the offered menu on a drive that was part of the update check.
fn boot_update(request: &Request, inner: &mut Inner) -> Result<Value> {
    let check = inner.release.as_ref().context("Check for updates first")?;
    let target = path(request.args, "root")?;
    core::drive_identity::check(&target, &inner.prefs, text(request.args, "drive_token")?)?;
    let expected = check
        .targets
        .iter()
        .find(|state| state.root == target)
        .context("Check this drive for updates first")?;
    inner.last = Some(updates::install_boot(
        check.menu.as_ref().context("No menu update was found")?,
        expected,
        check
            .boot_digest
            .as_deref()
            .context("Missing menu checksum")?,
        request.cache(),
        request.job,
    )?);
    Ok(journal_response(inner))
}

/// Throws away a desktop update staged to install on exit.
fn self_update_cancel(inner: &mut Inner) -> Result<Value> {
    if let Some((plan, plan_path, _)) = &inner.pending_update {
        self_update::discard(plan, plan_path)?;
    }
    inner.pending_update = None;
    Ok(Value::Null)
}

/// Installs the staged desktop update now, restarting the app.
fn self_update_apply(request: &Request, inner: &mut Inner) -> Result<Value> {
    let (plan, plan_path, _) = inner
        .pending_update
        .as_mut()
        .context("No update is waiting to install")?;
    plan.restart = true;
    f::atomic(plan_path, &serde_json::to_vec_pretty(plan)?)?;
    self_update::start(plan, plan_path)?;
    inner.pending_update = None;
    request.app.exit(0);
    Ok(Value::Null)
}

/// Downloads and stages the offered desktop update. Installs it now (the
/// app closes and restarts), or with `on_exit`, keeps it to install when
/// the app closes.
fn self_update_download(request: &Request, inner: &mut Inner) -> Result<Value> {
    let job = request.job;
    ensure!(
        inner.pending_update.is_none(),
        "An update is already ready to install"
    );
    let release = &inner
        .release
        .as_ref()
        .context("Check for updates first")?
        .desktop
        .as_ref()
        .context("No desktop update was found")?;
    ensure!(
        release.desktop_available(&std::env::current_exe()?, env!("CARGO_PKG_VERSION"), job)?,
        "The latest desktop version is already installed"
    );

    let work = request.cache().join("self-update");
    fs::create_dir_all(&work)?;
    let zip = work.join(format!("download-{}.zip", uuid::Uuid::new_v4()));
    updates::download_desktop(
        release,
        updates::platform().context("No desktop package for this platform")?,
        &zip,
        job,
    )?;
    let (plan, plan_path) =
        self_update::stage(&zip, &std::env::current_exe()?, request.cache(), job)?;
    let _ = fs::remove_file(zip);

    let wrong_local_build = release
        .binary_digest()
        .is_some_and(|expected| expected != plan.new_sha256);
    if wrong_local_build {
        self_update::discard(&plan, &plan_path)?;
        bail!(
            "Local executable checksum does not match the offered build. Check local builds again."
        );
    }
    if let Err(error) = job.check() {
        self_update::discard(&plan, &plan_path)?;
        return Err(error);
    }

    if request.args["on_exit"].as_bool().unwrap_or(false) {
        let version = release.version.clone();
        let local = release.is_local();
        inner.pending_update = Some((plan, plan_path, version.clone()));
        return Ok(json!({"version": version, "local": local}));
    }
    self_update::start(&plan, &plan_path)?;
    request.app.exit(0);
    Ok(Value::Null)
}
