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

pub struct Bridge(pub Arc<Backend>);
pub struct Backend {
    inner: Mutex<Inner>,
    cancel: Arc<AtomicBool>,
    busy: AtomicBool,
    cache: PathBuf,
}
struct Backup {
    path: Option<PathBuf>,
    original: Option<Vec<u8>>,
    archive: Archive,
    dirty: bool,
}
struct Inner {
    prefs: Preferences,
    import: Option<Plan>,
    copy: Option<usb::CopyPlan>,
    last: Option<PathBuf>,
    config: Option<console_config::Loaded>,
    backup: Option<Backup>,
    cleanup: Option<core::recovery::Inventory>,
    release: Option<updates::Check>,
    cheats: save_cheats::Database,
    cheats_original: Option<Vec<u8>>,
    cheats_path: Option<PathBuf>,
}
impl Backend {
    pub fn new() -> Self {
        let cache = core::cache_dir();
        let prefs = fs::read(cache.join("native-settings.json"))
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok())
            .unwrap_or_default();
        Self {
            inner: Mutex::new(Inner {
                prefs,
                import: None,
                copy: None,
                last: None,
                config: None,
                backup: None,
                cleanup: None,
                release: None,
                cheats: save_cheats::Database::bundled(),
                cheats_original: None,
                cheats_path: None,
            }),
            cancel: Arc::new(AtomicBool::new(false)),
            busy: AtomicBool::new(false),
            cache,
        }
    }
    pub fn software_rendering(&self) -> bool {
        self.inner.lock().unwrap().prefs.software_rendering
            || std::env::args().any(|a| a == "--software-rendering")
    }
    pub fn configure_args(&self) {
        let mut inner = self.inner.lock().unwrap();
        let args: Vec<_> = std::env::args().collect();
        if let Some(i) = args.iter().position(|a| a == "--library") {
            if let Some(root) = args.get(i + 1) {
                inner.prefs.library = root.clone();
            }
        }
    }
}
fn text<'a>(a: &'a Value, key: &str) -> Result<&'a str> {
    a[key].as_str().with_context(|| format!("Missing {key}"))
}
fn path(a: &Value, key: &str) -> Result<PathBuf> {
    Ok(PathBuf::from(text(a, key)?))
}
fn indices(a: &Value, key: &str) -> Result<Vec<usize>> {
    a[key]
        .as_array()
        .context("Missing selection")?
        .iter()
        .map(|v| v.as_u64().map(|n| n as usize).context("Invalid selection"))
        .collect()
}
fn strings(a: &Value, key: &str) -> Result<Vec<String>> {
    Ok(serde_json::from_value(a[key].clone())?)
}
fn root(inner: &Inner) -> Result<PathBuf> {
    ensure!(
        !inner.prefs.library.trim().is_empty(),
        "Choose a library folder first"
    );
    f::root(Path::new(&inner.prefs.library))
}
fn backup_view(b: &Backup) -> Value {
    json!({"path":b.path,"dirty":b.dirty,"bytes":b.archive.encode().map(|b|b.len()).unwrap_or_default(),"entries":b.archive.entries.iter().map(|e|json!({"name":e.label(),"known":core::saves::known_title(&e.label()),"file_type":e.file_type,"size":e.data.len(),"profile":save_cheats::find(e)})).collect::<Vec<_>>()})
}
fn entry_index(a: &Value, b: &Backup) -> Result<usize> {
    let n = a["index"].as_u64().context("Choose a save")? as usize;
    ensure!(n < b.archive.entries.len(), "Save selection changed");
    Ok(n)
}
fn slots(directory: &Path, cache: &Path, job: &Job) -> Result<Value> {
    Ok(json!(save_catalog::refresh(directory,cache,job)?.iter().map(|s|json!({"id":s.id,"path":s.path,"name":s.name(),"label":s.label,"valid":s.valid,"games":s.games(),"files":s.files,"search":format!("{} {} {}",s.name(),s.games(),s.aliases.join(" "))})).collect::<Vec<_>>()))
}
fn pick(a: &Value) -> Result<Value> {
    let kind = text(a, "kind")?;
    let mut dialog = rfd::FileDialog::new();
    if let Some(start) = a["start"].as_str().filter(|p| !p.is_empty()) {
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
        "support_save" => json!(dialog.add_filter("Support report", &["txt"]).set_file_name("Oh-My-3DO-support-report.txt").save_file()),
        _ => bail!("Unknown file chooser"),
    })
}
#[tauri::command]
pub fn cancel(state: tauri::State<'_, Bridge>) {
    state.0.cancel.store(true, Ordering::Relaxed);
}

/// Preference-only writes do not lock the GUI controls or steal search focus.
#[tauri::command]
pub async fn save_library_views(state: tauri::State<'_, Bridge>, views: std::collections::BTreeMap<String, LibraryView>) -> Result<(), String> {
    let backend=state.0.clone();
    tauri::async_runtime::spawn_blocking(move || -> Result<()> {
        let mut inner=backend.inner.lock().map_err(|_|anyhow::anyhow!("Preferences unavailable"))?;
        let mut prefs=inner.prefs.clone();
        prefs.library_views=views;
        f::atomic(&backend.cache.join("native-settings.json"),&serde_json::to_vec_pretty(&prefs)?)?;
        inner.prefs=prefs;
        Ok(())
    }).await.map_err(|e|e.to_string())?.map_err(|e|format!("{e:#}"))
}
#[tauri::command]
pub async fn cover(state: tauri::State<'_, Bridge>, name: String) -> Result<String, String> {
    let backend = state.0.clone();
    tauri::async_runtime::spawn_blocking(move || -> Result<String> {
        let root = {
            let inner = backend
                .inner
                .lock()
                .map_err(|_| anyhow::anyhow!("State unavailable"))?;
            root(&inner)?
        };
        let p = f::safe(&root, &format!(".ohmy3do/covers/{name}"))?;
        ensure!(
            fs::metadata(&p)?.len() <= 32 * 1024 * 1024,
            "Oversized cover"
        );
        let image = media::cover_display(&p)?.thumbnail(240, 300);
        let mut out = Cursor::new(Vec::new());
        image.write_to(&mut out, image::ImageFormat::Png)?;
        Ok(format!(
            "data:image/png;base64,{}",
            STANDARD.encode(out.into_inner())
        ))
    })
    .await
    .map_err(|e| e.to_string())?
    .map_err(|e| format!("{e:#}"))
}
#[tauri::command]
pub async fn request(
    app: AppHandle,
    state: tauri::State<'_, Bridge>,
    action: String,
    args: Value,
) -> Result<Value, String> {
    let backend = state.0.clone();
    if action == "quit" {
        app.exit(0);
        return Ok(Value::Null);
    }
    if action == "pick" {
        return tauri::async_runtime::spawn_blocking(move || pick(&args))
            .await
            .map_err(|e| e.to_string())?
            .map_err(|e| format!("{e:#}"));
    }
    ensure_idle(&backend)?;
    backend.cancel.store(false, Ordering::Relaxed);
    let _ = app.emit("busy", true);
    let events = app.clone();
    let job = Job {
        cancel: backend.cancel.clone(),
        log: Arc::new(move |message| {
            let _ = events.emit("progress", message);
        }),
        ..Default::default()
    };
    let done = app.clone();
    let finished_state = backend.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let result = (|| {
            let mut inner = backend
                .inner
                .lock()
                .map_err(|_| anyhow::anyhow!("State unavailable"))?;
            dispatch(&app, &backend, &mut inner, &action, &args, &job)
        })();
        result
    })
    .await;
    finished_state.busy.store(false, Ordering::Release);
    let _ = done.emit("busy", false);
    result
        .map_err(|e| e.to_string())?
        .map_err(|e| format!("{e:#}"))
}
fn ensure_idle(state: &Backend) -> Result<(), String> {
    state
        .busy
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .map(|_| ())
        .map_err(|_| "Another operation is running. Wait or cancel it first.".into())
}
fn dispatch(
    app: &AppHandle,
    state: &Backend,
    s: &mut Inner,
    action: &str,
    a: &Value,
    job: &Job,
) -> Result<Value> {
    match action {
        "ready" => {
            let args: Vec<_> = std::env::args().collect();
            if let Some(i) = args.iter().position(|a| a == "--update-ready") {
                if let Some(path) = args.get(i + 1) {
                    fs::write(path, b"ready")?;
                }
            }
            Ok(Value::Null)
        }
        "init" => Ok(
            json!({"prefs":s.prefs,"version":env!("CARGO_PKG_VERSION"),"categories":CATEGORIES,"args":std::env::args().skip(1).collect::<Vec<_>>()}),
        ),
        "preferences" => {
            let prefs: Preferences = serde_json::from_value(a.clone())?;
            f::atomic(
                &state.cache.join("native-settings.json"),
                &serde_json::to_vec_pretty(&prefs)?,
            )?;
            s.prefs = prefs;
            Ok(json!(s.prefs))
        }
        "library" => {
            let root = root(s)?;
            let db = library::database(&root)?;
            Ok(
                json!({"root":root,"records":db["games"],"categories":categories::load(&root)?.names()}),
            )
        }
        "categories_read" => {
            let root = f::root(&path(a, "root")?)?;
            Ok(json!({"root":root,"config":categories::load(&root)?}))
        }
        "categories_save" => {
            let root = f::root(&path(a, "root")?)?;
            let original = serde_json::from_value(a["original"].clone())?;
            let config = serde_json::from_value(a["config"].clone())?;
            s.last = Some(categories::save(
                &root,
                &original,
                &config,
                &state.cache,
                job,
            )?);
            s.import = None;
            s.copy = None;
            Ok(json!({"journal":s.last}))
        }
        "scan" => {
            let inputs: Vec<PathBuf> = serde_json::from_value(a["paths"].clone())?;
            let catalog = catalog::load(&state.cache, s.prefs.offline, job)?;
            let plan = library::scan(&root(s)?, &inputs, catalog, s.prefs.quick, job)?;
            let response = json!(plan);
            s.import = Some(plan);
            Ok(response)
        }
        "edit_import" => {
            let config = categories::load(&root(s)?)?;
            let plan = s.import.as_mut().context("Scan discs first")?;
            let i = a["index"].as_u64().context("Choose an import")? as usize;
            let entry = plan.entries.get_mut(i).context("Import changed")?;
            if let Some(v) = a["selected"].as_bool() {
                entry.selected = v;
            }
            if let Some(n) = a["game"].as_u64() {
                entry.blocked = false;
                entry.confidence = "manual".into();
                entry.reason = "Explicit catalog selection".into();
                let g = plan
                    .catalog
                    .games
                    .get(n as usize)
                    .context("Release changed")?;
                library::assign(entry, g, Some(n as usize), None);
                library::apply_category_defaults(entry, &config);
            }
            if let Some(category) = a["category"].as_str() {
                ensure!(config.contains(category), "Invalid category");
                if let Some(n) = entry.game {
                    library::assign(entry, &plan.catalog.games[n], Some(n), Some(category));
                }
            }
            if a["unverified"].as_bool() == Some(true) {
                let title = text(a, "title")?.trim();
                ensure!(!title.is_empty(), "Enter a title");
                let game = Game {
                    name: title.into(),
                    release: title.into(),
                    region: Some(text(a, "region")?.into()),
                    ..Default::default()
                };
                ensure!(config.contains(text(a, "category")?), "Invalid category");
                entry.blocked = false;
                entry.reason = "Explicit unverified import".into();
                entry.confidence = "unverified".into();
                library::assign(entry, &game, None, Some(text(a, "category")?));
                entry.selected = entry.resolvable();
            }
            library::collisions(plan)?;
            Ok(json!(plan))
        }
        "import" => {
            let plan = s.import.as_ref().context("Scan discs first")?;
            s.last = Some(library::import(plan, &s.prefs, &state.cache, job)?);
            s.import = None;
            Ok(json!({"journal":s.last}))
        }
        "move" => {
            s.last = Some(library::recategorize(
                &root(s)?,
                &strings(a, "discs")?,
                text(a, "category")?,
                &state.cache,
                job,
            )?);
            Ok(json!({"journal":s.last}))
        }
        "copy_preview" => {
            let plan = usb::plan_selected(
                &root(s)?,
                &path(a, "destination")?,
                &state.cache,
                a["verify"].as_bool().unwrap_or(false),
                &strings(a, "discs")?,
                job,
            )?;
            let value = json!({"summary":usb::summary(&plan),"bytes":plan.bytes,"games":plan.games,"files":plan.files,"destination":plan.destination,"storage":usb::storage_estimate(&plan)?});
            s.copy = Some(plan);
            Ok(value)
        }
        "copy" => {
            let p = s.copy.as_ref().context("Preview the copy first")?;
            ensure!(
                f::root(&path(a, "destination")?)? == p.destination && root(s)? == p.source,
                "Folders changed; preview again"
            );
            s.last = Some(usb::apply(p, &state.cache, job)?);
            s.copy = None;
            Ok(json!({"journal":s.last}))
        }
        "drive_check" => Ok(json!(core::readiness::check(&path(a,"root")?,job)?)),
        "support_export" => {
            core::support_report::export(&path(a,"root")?, &path(a,"target")?, &strings(a,"activity")?, job)?;
            Ok(json!(true))
        }
        "recovery_scan" => {
            let inventory=core::recovery::scan(&path(a,"directory")?,job)?;
            let result=json!(inventory);
            s.cleanup=Some(inventory);
            Ok(result)
        }
        "recovery_remove" => {
            let selected=strings(a,"selected")?;
            let review=s.cleanup.as_ref().context("Scan recovery copies first")?;
            if let Some(open)=s.backup.as_ref().and_then(|b|b.path.as_ref()) {
                let open=open.canonicalize()?;
                ensure!(!selected.iter().any(|n|review.directory.join(n)==open),"Close the open recovery copy before deleting it");
            }
            let removed=core::recovery::remove(review,&selected,job)?;
            s.cleanup=None;
            Ok(json!(removed))
        }
        "drive_setup" => {
            let dest = path(a, "destination")?;
            let settings: console_config::Settings =
                serde_json::from_value(a["settings"].clone()).unwrap_or_default();
            s.last = Some(if a["release"].as_bool() == Some(true) {
                ensure!(
                    !s.prefs.offline,
                    "Turn off offline mode to download the menu"
                );
                setup::install_release(&dest, &state.cache, &settings, job)?
            } else {
                setup::install_local(&dest, &state.cache, &path(a, "source")?, &settings, job)?
            });
            Ok(json!({"journal":s.last}))
        }
        "indexes" => {
            let root = root(s)?;
            let mut tx = library::Journal::new(&root, &state.cache)?;
            if let Err(e) = library::build_indexes(&root, &mut tx, job) {
                tx.undo()?;
                return Err(e);
            }
            tx.finish()?;
            s.last = Some(tx.path);
            Ok(json!({"journal":s.last}))
        }
        "undo" => {
            let path = s.last.as_ref().context("No change to undo")?;
            library::undo(path)?;
            s.last = None;
            Ok(json!(true))
        }
        "review" => Ok(json!({"path":library::review(&root(s)?,&state.cache)?})),
        "media_update" => {
            let root = root(s)?;
            let provided = a["disc"].as_str().map(|disc| {
                (
                    disc,
                    a["cover"].as_str().map(PathBuf::from),
                    a["preview"].as_str().map(PathBuf::from),
                )
            });
            s.last = Some(library::update_media(
                &root,
                &s.prefs,
                &state.cache,
                provided,
                job,
            )?);
            Ok(json!({"journal":s.last}))
        }
        "media_rebuild" => {
            s.last = Some(library::rebuild_media(
                &root(s)?,
                &s.prefs,
                &state.cache,
                job,
            )?);
            Ok(json!({"journal":s.last}))
        }
        "video_tools" => {
            ensure!(
                !s.prefs.offline,
                "Turn off offline mode to download video tools"
            );
            s.prefs.video_tools = media::install_video_tools(&state.cache, job)?
                .to_string_lossy()
                .into();
            f::atomic(
                &state.cache.join("native-settings.json"),
                &serde_json::to_vec_pretty(&s.prefs)?,
            )?;
            Ok(json!(s.prefs))
        }
        "config_load" => {
            let loaded = console_config::load(&path(a, "root")?)?;
            let value =
                json!({"root":loaded.root,"settings":loaded.settings,"exists":loaded.exists()});
            s.config = Some(loaded);
            Ok(value)
        }
        "config_save" => {
            let loaded = s.config.as_ref().context("Load settings first")?;
            let settings: console_config::Settings = serde_json::from_value(a["settings"].clone())?;
            let saved = console_config::save(loaded, &settings, &state.cache, job)?;
            s.last = Some(saved.journal);
            s.config = Some(console_config::load(&loaded.root)?);
            Ok(json!(true))
        }
        "slots" => slots(&path(a, "directory")?, &state.cache, job),
        "slot_label" => {
            let p = path(a, "path")?;
            let (page, index) = backup::slot(&p).context("Choose a numbered slot")?;
            let (mut labels, original) = backup::Labels::read(&page)?;
            labels.0[index] = text(a, "label")?.into();
            labels.save(&page, original.as_deref())?;
            slots(
                p.parent().context("Missing slot directory")?,
                &state.cache,
                job,
            )
        }
        "backup_new" => {
            ensure!(
                s.backup.as_ref().is_none_or(|b| !b.dirty),
                "Save or discard the current backup first"
            );
            s.backup = Some(Backup {
                path: None,
                original: None,
                archive: Archive::default(),
                dirty: false,
            });
            Ok(backup_view(s.backup.as_ref().unwrap()))
        }
        "backup_open" => {
            ensure!(
                s.backup.as_ref().is_none_or(|b| !b.dirty),
                "Save or discard the current backup first"
            );
            let path = path(a, "path")?;
            let (archive, original) = Archive::read_with_bytes(&path)?;
            save_cheats::activate(save_cheats::for_backup(&path)?);
            s.backup = Some(Backup {
                path: Some(path),
                original: Some(original),
                archive,
                dirty: false,
            });
            Ok(backup_view(s.backup.as_ref().unwrap()))
        }
        "backup_discard" => {
            s.backup = None;
            Ok(Value::Null)
        }
        "backup_entry" => {
            let b = s.backup.as_ref().context("Open a backup")?;
            let entry = &b.archive.entries[entry_index(a, b)?];
            let profile = save_cheats::find(entry);
            let values = profile.as_ref().map(|p| {
                (0..p.fields.len())
                    .map(|i| p.value(entry, i))
                    .collect::<Vec<_>>()
            });
            Ok(
                json!({"name":entry.label(),"hex":entry.data.iter().map(|v|format!("{v:02X}")).collect::<Vec<_>>().join(" "),"data":entry.data,"file_type":entry.file_type,"profile":profile,"values":values}),
            )
        }
        "backup_edit" => {
            let b = s.backup.as_mut().context("Open a backup")?;
            let i = entry_index(a, b)?;
            let mut next = b.archive.clone();
            let e = &mut next.entries[i];
            if let Some(hex) = a["hex"].as_str() {
                e.data = core::save_archive::decode_hex(hex)?;
            }
            if let Some(field) = a["field"].as_u64() {
                let p = save_cheats::find(e).context("No known format for this save")?;
                p.set(
                    e,
                    field as usize,
                    a["value"].as_u64().context("Missing value")?.try_into()?,
                )?;
            }
            if let Some(preset) = a["preset"].as_u64() {
                save_cheats::find(e)
                    .context("No known format for this save")?
                    .apply_preset(e, preset as usize)?;
            }
            next.encode()?;
            b.archive = next;
            b.dirty = true;
            Ok(backup_view(b))
        }
        "backup_export" => {
            let b = s.backup.as_ref().context("Open a backup")?;
            let entry = b.archive.entries[entry_index(a, b)?].clone();
            let bytes = if a["raw"].as_bool() == Some(true) {
                entry.data
            } else {
                Archive {
                    entries: vec![entry],
                }
                .encode()?
            };
            let target = path(a, "path")?;
            let expected = core::save_archive::existing_bytes(&target)?;
            ensure!(
                expected.is_none() || a["overwrite"].as_bool() == Some(true),
                "Confirm replacing the destination export"
            );
            let saved = core::save_archive::save_bytes(&target, &bytes, expected.as_deref())?;
            Ok(json!({"recovery":saved.backup}))
        }
        "backup_import_raw" => {
            let b = s.backup.as_mut().context("Open a backup")?;
            let i = entry_index(a, b)?;
            let bytes = core::save_archive::read_bytes(&path(a, "path")?)?;
            let mut next = b.archive.clone();
            next.entries[i].data = bytes;
            next.encode()?;
            b.archive = next;
            b.dirty = true;
            Ok(backup_view(b))
        }
        "backup_remove" => {
            let b = s.backup.as_mut().context("Open a backup")?;
            let chosen = indices(a, "indices")?;
            ensure!(
                chosen.iter().all(|i| *i < b.archive.entries.len()),
                "Selection changed"
            );
            b.archive.entries = b
                .archive
                .entries
                .iter()
                .enumerate()
                .filter(|(i, _)| !chosen.contains(i))
                .map(|(_, e)| e.clone())
                .collect();
            b.dirty = true;
            Ok(backup_view(b))
        }
        "backup_inspect" => {
            let archive = Archive::read(&path(a, "path")?)?;
            Ok(json!(archive.entries.iter().enumerate().map(|(i,e)|json!({"index":i,"name":e.label(),"size":e.data.len(),"known":core::saves::known_title(&e.label())})).collect::<Vec<_>>()))
        }
        "backup_merge" => {
            let archive = Archive::read(&path(a, "path")?)?;
            let entries = indices(a, "indices")?
                .iter()
                .map(|i| {
                    archive
                        .entries
                        .get(*i)
                        .cloned()
                        .context("Source save changed")
                })
                .collect::<Result<Vec<Entry>>>()?;
            let b = s.backup.as_mut().context("Create or open a backup first")?;
            b.archive.merge_entries(&entries)?;
            b.dirty = true;
            Ok(backup_view(b))
        }
        "backup_save" => {
            let b = s.backup.as_mut().context("Open a backup")?;
            let target = path(a, "path")?;
            let expected = if b.path.as_ref() == Some(&target) {
                b.original.clone()
            } else {
                core::save_archive::existing_bytes(&target)?
            };
            ensure!(
                expected.is_none() || a["overwrite"].as_bool() == Some(true),
                "Confirm replacing the entire destination slot/archive"
            );
            let saved = b.archive.save(&target, expected.as_deref())?;
            b.original = Some(core::save_archive::read_bytes(&target)?);
            b.path = Some(target.clone());
            b.dirty = false;
            let catalog = if backup::slot(&target).is_some() {
                Some(slots(target.parent().unwrap(), &state.cache, job))
            } else {
                None
            };
            Ok(
                json!({"backup":backup_view(b),"recovery":saved.backup,"catalog_warning":catalog.and_then(|r|r.err()).map(|e|format!("{e:#}"))}),
            )
        }
        "backup_compare" => {
            let before = Archive::read(&path(a, "path")?)?;
            let after = &s.backup.as_ref().context("Open a backup")?.archive;
            Ok(json!(backup::comparison(&before,after)))
        }
        "cheats_validate" => {
            let db = save_cheats::Database::parse(text(a, "json")?.as_bytes())?;
            Ok(json!(db))
        }
        "cheats_activate" => {
            let db = save_cheats::Database::parse(text(a, "json")?.as_bytes())?;
            save_cheats::activate(Some(db.clone()));
            s.cheats = db;
            Ok(Value::Null)
        }
        "cheats_load" => {
            let path = a["path"].as_str().map(PathBuf::from);
            let (db, raw) = if let Some(p) = &path {
                let bytes = fs::read(p)?;
                (save_cheats::Database::parse(&bytes)?, Some(bytes))
            } else {
                (save_cheats::Database::bundled(), None)
            };
            s.cheats = db;
            s.cheats_path = path;
            s.cheats_original = raw;
            Ok(json!(s.cheats))
        }
        "cheats_download" => {
            ensure!(
                !s.prefs.offline,
                "Turn off offline mode to download definitions"
            );
            s.cheats = save_cheats::merge_missing(&s.cheats, &save_cheats::download()?)?;
            Ok(json!(s.cheats))
        }
        "cheats_install" => {
            let db = save_cheats::Database::parse(text(a, "json")?.as_bytes())?;
            s.last = Some(save_cheats::install(&path(a, "root")?, &db, &state.cache)?);
            s.cheats = db;
            Ok(json!({"journal":s.last}))
        }
        "cheats_save" => {
            let db = save_cheats::Database::parse(text(a, "json")?.as_bytes())?;
            let target = path(a, "path")?;
            let expected = if s.cheats_path.as_ref() == Some(&target) {
                s.cheats_original.clone()
            } else {
                if target.exists() {
                    Some(fs::read(&target)?)
                } else {
                    None
                }
            };
            save_cheats::save_json(&target, &db, expected.as_deref(), &state.cache)?;
            s.cheats_original = Some(fs::read(&target)?);
            s.cheats_path = Some(target);
            s.cheats = db;
            Ok(json!(true))
        }
        "updates_check" => {
            ensure!(
                !s.prefs.offline,
                "Turn off offline mode to check for updates"
            );
            let check = updates::check(
                &[
                    PathBuf::from(&s.prefs.library),
                    PathBuf::from(&s.prefs.destination),
                ],
                job,
            )?;
            let newer = updates::is_newer(&check.release.version, env!("CARGO_PKG_VERSION"))?;
            let targets = check
                .targets
                .iter()
                .filter(|t| t.digest.as_deref() != Some(check.boot_digest.as_str()))
                .map(|t| json!({"root":t.root,"installed":t.digest.is_some()}))
                .collect::<Vec<_>>();
            let out = json!({"tag":check.release.tag,"version":check.release.version,"notes":check.release.notes,"desktop":newer,"targets":targets,"warnings":check.warnings});
            s.release = Some(check);
            Ok(out)
        }
        "boot_update" => {
            let check = s.release.as_ref().context("Check for updates first")?;
            let target = path(a, "root")?;
            let expected = check
                .targets
                .iter()
                .find(|t| t.root == target)
                .context("Check this drive for updates first")?;
            s.last = Some(updates::install_boot(
                &check.release,
                expected,
                &check.boot_digest,
                &state.cache,
                job,
            )?);
            Ok(json!({"journal":s.last}))
        }
        "self_update" => {
            let release = &s
                .release
                .as_ref()
                .context("Check for updates first")?
                .release;
            ensure!(
                updates::is_newer(&release.version, env!("CARGO_PKG_VERSION"))?,
                "The latest desktop version is already installed"
            );
            let work = state.cache.join("self-update");
            fs::create_dir_all(&work)?;
            let zip = work.join(format!("download-{}.zip", uuid::Uuid::new_v4()));
            updates::download_desktop(
                release,
                updates::platform().context("No desktop package for this platform")?,
                &zip,
                job,
            )?;
            let (plan, path) =
                self_update::stage(&zip, &std::env::current_exe()?, &state.cache, job)?;
            let _ = fs::remove_file(zip);
            job.check()?;
            self_update::start(&plan, &path)?;
            app.exit(0);
            Ok(Value::Null)
        }
        _ => bail!("Unknown desktop operation: {action}"),
    }
}
