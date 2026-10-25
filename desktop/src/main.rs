#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]
mod cheat_ui;
mod save_ui;
mod theme;
mod update_ui;
use eframe::egui::{self, Color32, RichText};
use oh_my_3do_library::{
    cache_dir, catalog, console_config, disc, fsutil as f, library, media, model::*, usb, Job,
};
use serde_json::Value;
use std::{
    collections::{BTreeMap, BTreeSet, HashMap, HashSet},
    fs,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc,
    },
    time::{Duration, Instant},
};

enum Outcome {
    Downloaded { path: PathBuf, desktop: bool },
    BootUpdated { journal: PathBuf, root: PathBuf },
    DrivePrepared { journal: PathBuf, root: PathBuf },
    Scan(Plan),
    Imported(PathBuf),
    Updated(PathBuf),
    Usb(usb::CopyPlan),
    Copied(PathBuf),
    Video(PathBuf),
    ConsoleLoaded(console_config::Loaded),
    ConsoleSaved(console_config::Saved),
    ConsoleRestored(console_config::Loaded),
    Records(Vec<Value>),
    Report(PathBuf),
    Done(String),
}
enum Event {
    Log(String),
    Done(Result<Outcome, String>),
}
#[derive(Clone, PartialEq, Eq)]
enum ThumbnailSource {
    File(PathBuf),
    Library(PathBuf, String),
}

fn load_thumbnail(source: &ThumbnailSource) -> anyhow::Result<egui::ColorImage> {
    let path = match source {
        ThumbnailSource::File(path) => path.clone(),
        ThumbnailSource::Library(root, name) => {
            f::safe(&f::root(root)?, &format!(".ohmy3do/covers/{name}"))?
        }
    };
    if fs::metadata(&path)?.len() > 32 * 1024 * 1024 {
        anyhow::bail!("Oversized thumbnail")
    }
    let image = media::cover_display(&path)?.thumbnail(168, 210).to_rgba8();
    Ok(egui::ColorImage::from_rgba_unmultiplied(
        [image.width() as usize, image.height() as usize],
        image.as_raw(),
    ))
}
/// One game folder in the library (Category/Region/Game), with all its discs.
struct Group {
    folder: String,
    title: String,
    region: String,
    category: String,
    genre: String,
    publisher: String,
    cover: Option<String>,
    discs: Vec<String>,
    /// Laid out as Category/Region/Game, so it can change category.
    movable: bool,
}
fn groups(records: &[Value]) -> Vec<Group> {
    let disc = regex::Regex::new(r"(?i) \((disc|cd) \d+\)").unwrap();
    let mut out: BTreeMap<String, Group> = BTreeMap::new();
    for r in records {
        let path = r["path"].as_str().unwrap_or("");
        let parts: Vec<&str> = path.split('/').collect();
        let movable = parts.len() >= 4 && CATEGORIES.contains(&parts[0]);
        let folder = if movable {
            parts[..3].join("/")
        } else {
            path.to_owned()
        };
        let text = |v: &Value| v.as_str().unwrap_or("").to_owned();
        let group = out.entry(f::fold(&folder)).or_insert_with(|| Group {
            folder: folder.clone(),
            title: disc.replace(&text(&r["title"]), "").into_owned(),
            region: text(&r["region"]),
            category: if movable {
                parts[0].to_owned()
            } else {
                text(&r["category"])
            },
            genre: text(&r["metadata"]["genre"]),
            publisher: text(&r["metadata"]["publisher"]),
            cover: None,
            discs: vec![],
            movable,
        });
        if group.cover.is_none() {
            group.cover = r["cover"].as_str().map(str::to_owned);
        }
        group.discs.push(path.to_owned());
    }
    let mut list: Vec<Group> = out.into_values().collect();
    list.sort_by_key(|g| (g.title.to_lowercase(), g.folder.clone()));
    list
}
/// A title without trailing (Region) / (Rev 1) groups, as the menu lists it.
fn short_title(title: &str) -> String {
    let group = regex::Regex::new(r" \([^()]*\)$").unwrap();
    let mut title = title.to_owned();
    while let Some(m) = group.find(&title) {
        if m.start() == 0 {
            break;
        }
        title.truncate(m.start());
    }
    title
}
/// Titles that are adult content in the user's library, offered for review.
fn adult_suggestion(g: &Group) -> bool {
    let t = g.title.to_lowercase();
    g.publisher == "Vivid Interactive"
        || t.starts_with("virtual cameraman")
        || t.starts_with("oneesan to issho")
        || t.starts_with("idol janshi suchie-pai")
        || t.starts_with("idol mahjong final romance")
        || t.starts_with("super real mahjong")
}
#[derive(Clone, Copy, PartialEq)]
enum Tab {
    Library,
    Import,
    Media,
    Usb,
    Settings,
}
struct App {
    updates: update_ui::Updates,
    save_browser: save_ui::SaveBrowser,
    cheat_database: cheat_ui::CheatDatabase,
    setup_visible: bool,
    setup_settings: console_config::Settings,
    prefs: Preferences,
    cache: PathBuf,
    inputs: Vec<PathBuf>,
    plan: Option<Plan>,
    tab: Tab,
    selected: Option<usize>,
    sender: mpsc::Sender<Event>,
    receiver: mpsc::Receiver<Event>,
    busy: bool,
    cancel: Arc<AtomicBool>,
    status: String,
    logs: Vec<String>,
    release_dialog: bool,
    release_search: String,
    release_choice: Option<usize>,
    edit_title: String,
    edit_category: String,
    edit_region: String,
    usb_plan: Option<usb::CopyPlan>,
    library_copy_open: bool,
    library_copy_root: Option<PathBuf>,
    library_copy_discs: Vec<String>,
    library_copy_titles: Vec<String>,
    verify: bool,
    last_journal: Option<PathBuf>,
    console_root: String,
    console_loaded: Option<console_config::Loaded>,
    console_edit: console_config::Settings,
    console_error: Option<String>,
    console_pending: bool,
    console_initialized: bool,
    console_journal: Option<(PathBuf, PathBuf)>,
    records: Vec<Value>,
    selected_record: Option<usize>,
    supplied_cover: Option<PathBuf>,
    supplied_preview: Option<PathBuf>,
    filter: String,
    thumbnail: Option<egui::TextureHandle>,
    thumb_source: Option<ThumbnailSource>,
    thumb_sender: mpsc::Sender<ThumbnailSource>,
    thumb_receiver: mpsc::Receiver<(ThumbnailSource, Option<egui::ColorImage>)>,
    screenshot: Option<PathBuf>,
    frames: u32,
    profile: Option<UiProfile>,
    logo: egui::TextureHandle,
    screenshot_requested: bool,
    settled_frames: u32,
    groups: Vec<Group>,
    records_stale: bool,
    lib_category: Option<String>,
    lib_region: Option<String>,
    lib_adult: bool,
    lib_search: String,
    lib_selected: BTreeSet<String>,
    lib_anchor: Option<usize>,
    move_to: String,
    covers: HashMap<String, egui::TextureHandle>,
    covers_pending: HashSet<String>,
    cover_sender: mpsc::Sender<(String, ThumbnailSource)>,
    cover_receiver: mpsc::Receiver<(String, Option<egui::ColorImage>)>,
}
struct UiProfile {
    path: PathBuf,
    previous: Instant,
    intervals: Vec<f64>,
    cpu: Vec<f64>,
}
impl App {
    fn new(cc: &eframe::CreationContext<'_>, screenshot: Option<PathBuf>) -> Self {
        let cache = cache_dir();
        let _ = fs::create_dir_all(&cache);
        let prefs = fs::read(cache.join("native-settings.json"))
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok())
            .unwrap_or(Preferences {
                download_covers: true,
                ..Default::default()
            });
        theme::install(&cc.egui_ctx);
        let logo = image::load_from_memory(include_bytes!("../../assets/logo-desktop.png"))
            .map(|i| i.to_rgba8())
            .unwrap_or_default();
        let logo = cc.egui_ctx.load_texture(
            "logo",
            egui::ColorImage::from_rgba_unmultiplied(
                [logo.width() as usize, logo.height() as usize],
                logo.as_raw(),
            ),
            egui::TextureOptions::LINEAR,
        );
        // Grid covers load in request order; the single-cover loader below
        // instead skips to the latest request.
        let (cover_sender, cover_requests) = mpsc::channel::<(String, ThumbnailSource)>();
        let (cover_images, cover_receiver) = mpsc::channel();
        let cover_context = cc.egui_ctx.clone();
        std::thread::spawn(move || {
            while let Ok((key, source)) = cover_requests.recv() {
                if cover_images
                    .send((key, load_thumbnail(&source).ok()))
                    .is_err()
                {
                    break;
                }
                cover_context.request_repaint();
            }
        });
        let (sender, receiver) = mpsc::channel();
        let (thumb_sender, requests) = mpsc::channel::<ThumbnailSource>();
        let (images, thumb_receiver) = mpsc::channel();
        let context = cc.egui_ctx.clone();
        std::thread::spawn(move || {
            while let Ok(mut source) = requests.recv() {
                // Scrolling quickly should load the latest selection, rather than
                // queueing every cover passed on the way there.
                while let Ok(latest) = requests.try_recv() {
                    source = latest;
                }
                let image = load_thumbnail(&source).ok();
                if images.send((source, image)).is_err() {
                    break;
                }
                context.request_repaint();
            }
        });
        Self {
            prefs,
            cache,
            inputs: vec![],
            plan: None,
            tab: Tab::Library,
            selected: None,
            sender,
            receiver,
            busy: false,
            cancel: Arc::new(AtomicBool::new(false)),
            status: "Choose a library to manage games, or use Prepare USB to set up a drive."
                .into(),
            logs: vec![],
            release_dialog: false,
            release_search: String::new(),
            release_choice: None,
            edit_title: String::new(),
            edit_category: "Games".into(),
            edit_region: "USA".into(),
            updates: Default::default(),
            save_browser: Default::default(),
            cheat_database: Default::default(),
            setup_visible: false,
            setup_settings: Default::default(),
            usb_plan: None,
            library_copy_open: false,
            library_copy_root: None,
            library_copy_discs: vec![],
            library_copy_titles: vec![],
            verify: false,
            last_journal: None,
            console_root: String::new(),
            console_loaded: None,
            console_edit: console_config::Settings::default(),
            console_error: None,
            console_pending: false,
            console_initialized: false,
            console_journal: None,
            records: vec![],
            selected_record: None,
            supplied_cover: None,
            supplied_preview: None,
            filter: String::new(),
            thumbnail: None,
            thumb_source: None,
            thumb_sender,
            thumb_receiver,
            screenshot,
            frames: 0,
            profile: None,
            logo,
            screenshot_requested: false,
            settled_frames: 0,
            groups: vec![],
            records_stale: true,
            lib_category: Some("Games".into()),
            lib_region: None,
            lib_adult: false,
            lib_search: String::new(),
            lib_selected: BTreeSet::new(),
            lib_anchor: None,
            move_to: "Multimedia".into(),
            covers: HashMap::new(),
            covers_pending: HashSet::new(),
            cover_sender,
            cover_receiver,
        }
    }
    fn error(&mut self, error: impl std::fmt::Display) {
        self.status = error.to_string();
        self.logs.push(self.status.clone());
    }
    fn save_preferences(&self) {
        let _ = f::atomic(
            &self.cache.join("native-settings.json"),
            &serde_json::to_vec_pretty(&self.prefs).unwrap_or_default(),
        );
    }
    fn start(
        &mut self,
        label: &str,
        work: impl FnOnce(Job) -> anyhow::Result<Outcome> + Send + 'static,
    ) {
        if self.busy {
            return;
        }
        self.save_preferences();
        self.busy = true;
        self.status = label.into();
        self.cancel = Arc::new(AtomicBool::new(false));
        let cancel = self.cancel.clone();
        let sender = self.sender.clone();
        let logger = sender.clone();
        std::thread::spawn(move || {
            let job = Job {
                cancel,
                log: Arc::new(move |s| {
                    let _ = logger.send(Event::Log(s));
                }),
                ..Default::default()
            };
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| work(job)))
                .map_err(|_| "Operation failed unexpectedly; keep the journal and log".to_owned())
                .and_then(|r| r.map_err(|e| format!("{e:#}")));
            let _ = sender.send(Event::Done(result));
        });
    }
    fn root(&self) -> anyhow::Result<PathBuf> {
        f::root(Path::new(&self.prefs.library))
    }
    fn scan(&mut self) {
        let root = match self.root() {
            Ok(r) => r,
            Err(e) => {
                self.error(e);
                return;
            }
        };
        let inputs = self.inputs.clone();
        if inputs.is_empty() {
            self.error("Add ISO/CUE files or a directory first");
            return;
        }
        let prefs = self.prefs.clone();
        let cache = self.cache.clone();
        self.start("Loading catalog and scanning selected files", move |job| {
            let catalog = catalog::load(&cache, prefs.offline, &job)?;
            let plan = library::scan(&root, &inputs, catalog, prefs.quick, &job)?;
            f::atomic(
                &cache.join("native-import-plan.json"),
                &serde_json::to_vec_pretty(&plan)?,
            )?;
            Ok(Outcome::Scan(plan))
        });
    }
    fn import(&mut self) {
        let Some(plan) = self.plan.clone() else {
            return;
        };
        if self.root().ok().as_ref() != Some(&plan.root) {
            self.error("Library folder changed; scan again");
            return;
        }
        let prefs = self.prefs.clone();
        let cache = self.cache.clone();
        self.start("Importing checked discs", move |job| {
            Ok(Outcome::Imported(library::import(
                &plan, &prefs, &cache, &job,
            )?))
        });
    }
    fn load_records(&mut self) {
        let root = match self.root() {
            Ok(r) => r,
            Err(e) => {
                self.error(e);
                return;
            }
        };
        self.start("Reading library", move |_| {
            let db = library::database(&root)?;
            Ok(Outcome::Records(
                db["games"].as_array().cloned().unwrap_or_default(),
            ))
        });
    }
    fn poll(&mut self) {
        for _ in 0..100 {
            let Ok(event) = self.receiver.try_recv() else {
                break;
            };
            match event {
                Event::Log(line) => {
                    self.status = line.clone();
                    self.logs.push(line);
                    if self.logs.len() > 1500 {
                        self.logs.drain(..500);
                    }
                }
                Event::Done(result) => {
                    self.busy = false;
                    let console_pending = self.console_pending;
                    self.console_pending = false;
                    match result {
                        Err(e) => {
                            if console_pending {
                                self.console_error = Some(e.clone());
                            }
                            self.error(e);
                        }
                        Ok(outcome) => match outcome {
                            Outcome::Downloaded { path, desktop } => {
                                self.status = if desktop {
                                    format!("Verified download saved to {}. Close this app and extract the ZIP to update it.", path.display())
                                } else {
                                    format!("Verified boot.iso saved to {}", path.display())
                                };
                                if let Some(parent) = path.parent() {
                                    open(parent);
                                }
                            }
                            Outcome::DrivePrepared { journal, root } => {
                                self.last_journal = Some(journal);
                                self.usb_plan = None;
                                self.records_stale = true;
                                self.status=format!("Drive ready: {}. Menu and save support installed; no games copied.",root.display());
                            }
                            Outcome::BootUpdated { journal, root } => {
                                self.last_journal = Some(journal);
                                self.usb_plan = None;
                                self.updates.installed_boot(&root);
                                self.status = format!(
                                    "Updated boot.iso in {}. Cold boot the console to use it.",
                                    root.display()
                                );
                            }
                            Outcome::Scan(plan) => {
                                self.status=format!("{} disc entries scanned. Review names and locations, then check entries to import.",plan.entries.len());
                                self.plan = Some(plan);
                                self.selected = None;
                            }
                            Outcome::Imported(path) => {
                                self.last_journal = Some(path);
                                self.records_stale = true;
                                self.plan = None;
                                self.inputs.clear();
                                self.status =
                                    "Import complete. Original files were preserved.".into();
                            }
                            Outcome::Updated(path) => {
                                self.last_journal = Some(path);
                                self.status =
                                    "Library updated. See the log for anything skipped.".into();
                                self.records.clear();
                                self.records_stale = true;
                                self.covers.clear();
                                self.covers_pending.clear();
                                self.selected_record = None;
                            }
                            Outcome::Usb(plan) => {
                                self.status = usb::summary(&plan);
                                self.usb_plan = Some(plan)
                            }
                            Outcome::Copied(path) => {
                                self.library_copy_open = false;
                                self.last_journal = Some(path);
                                self.usb_plan = None;
                                self.status =
                                    "USB copy complete. Settings and saves were preserved.".into();
                            }
                            Outcome::Video(path) => {
                                self.prefs.video_tools = path.to_string_lossy().into_owned();
                                self.save_preferences();
                                self.status = "Video tools installed and ready".into();
                            }
                            Outcome::ConsoleLoaded(loaded) => {
                                self.console_root = loaded.root.to_string_lossy().into_owned();
                                self.console_edit = loaded.settings.clone();
                                self.status = if loaded.exists() {
                                    "Console settings loaded".into()
                                } else {
                                    "No console settings file yet. Defaults are ready to save."
                                        .into()
                                };
                                self.console_loaded = Some(loaded);
                                self.console_error = None;
                            }
                            Outcome::ConsoleSaved(saved) => {
                                self.console_journal =
                                    Some((saved.loaded.root.clone(), saved.journal.clone()));
                                self.last_journal = Some(saved.journal);
                                self.console_edit = saved.loaded.settings.clone();
                                self.console_loaded = Some(saved.loaded);
                                self.console_error = None;
                                self.usb_plan = None;
                                self.status = "Console settings saved and verified.".into();
                            }
                            Outcome::ConsoleRestored(loaded) => {
                                if self.console_journal.as_ref().is_some_and(|(_, path)| {
                                    self.last_journal.as_ref() == Some(path)
                                }) {
                                    self.last_journal = None;
                                }
                                self.console_journal = None;
                                self.console_edit = loaded.settings.clone();
                                self.console_loaded = Some(loaded);
                                self.console_error = None;
                                self.usb_plan = None;
                                self.status = "Previous console settings restored".into();
                            }
                            Outcome::Records(records) => {
                                self.groups = groups(&records);
                                self.records = records;
                                self.records_stale = false;
                                self.lib_selected.clear();
                                self.lib_anchor = None;
                                self.selected_record = None;
                                self.status =
                                    format!("{} disc entries in the library", self.records.len());
                            }
                            Outcome::Report(path) => {
                                self.status = format!("Report saved: {}", path.display());
                                open(&path);
                            }
                            Outcome::Done(s) => {
                                self.console_loaded = None;
                                self.console_initialized = false;
                                self.console_journal = None;
                                self.status = s;
                                self.records_stale = true;
                            }
                        },
                    }
                }
            }
        }
    }
    fn show_thumbnail(&mut self, ui: &mut egui::Ui, source: Option<ThumbnailSource>) {
        if source != self.thumb_source {
            self.thumbnail = None;
            self.thumb_source = source.clone();
            if let Some(source) = source {
                let _ = self.thumb_sender.send(source);
            }
        }
        while let Ok((source, image)) = self.thumb_receiver.try_recv() {
            if Some(&source) == self.thumb_source.as_ref() {
                self.thumbnail = image.map(|image| {
                    ui.ctx()
                        .load_texture("cover-preview", image, egui::TextureOptions::LINEAR)
                });
            }
        }
        if let Some(texture) = &self.thumbnail {
            ui.add(egui::Image::new(texture).max_size(egui::vec2(168., 210.)));
        }
    }
    /// Requests covers for the given groups and returns any that are ready.
    fn cover_texture(&mut self, ctx: &egui::Context, name: &str) -> Option<egui::TextureHandle> {
        while let Ok((key, image)) = self.cover_receiver.try_recv() {
            self.covers_pending.remove(&key);
            if let Some(image) = image {
                self.covers.insert(
                    key.clone(),
                    ctx.load_texture(format!("cover:{key}"), image, egui::TextureOptions::NEAREST),
                );
            }
        }
        if let Some(texture) = self.covers.get(name) {
            return Some(texture.clone());
        }
        if self.covers_pending.insert(name.to_owned()) {
            let _ = self.cover_sender.send((
                name.to_owned(),
                ThumbnailSource::Library(PathBuf::from(&self.prefs.library), name.to_owned()),
            ));
        }
        None
    }
    fn visible_groups(&self) -> Vec<usize> {
        let search = self.lib_search.to_lowercase();
        (0..self.groups.len())
            .filter(|&i| {
                let g = &self.groups[i];
                (self.lib_adult && adult_suggestion(g) && g.category != "Adult"
                    || !self.lib_adult
                        && self.lib_category.as_ref().is_none_or(|c| *c == g.category)
                        && self.lib_region.as_ref().is_none_or(|r| *r == g.region))
                    && (search.is_empty() || g.title.to_lowercase().contains(&search))
            })
            .collect()
    }
    fn library_ui(&mut self, ui: &mut egui::Ui) {
        if self.groups.is_empty() && ui.button("Set up a PC library…").clicked() {
            self.setup_visible = true;
            self.tab = Tab::Settings;
        }
        if self.records_stale && !self.busy && !self.prefs.library.is_empty() {
            self.records_stale = false;
            self.load_records();
        }
        let mut categories: BTreeMap<&str, usize> = BTreeMap::new();
        let mut regions: BTreeMap<&str, usize> = BTreeMap::new();
        for g in &self.groups {
            *categories.entry(g.category.as_str()).or_default() += 1;
            *regions.entry(g.region.as_str()).or_default() += 1;
        }
        let adult = self
            .groups
            .iter()
            .filter(|g| adult_suggestion(g) && g.category != "Adult")
            .count();
        let mut pick_category = None;
        let mut pick_region = None;
        let mut pick_adult = false;
        egui::SidePanel::left("library_filters")
            .exact_width(210.)
            .resizable(false)
            .frame(egui::Frame::new().fill(theme::HEADER).inner_margin(14.))
            .show_inside(ui, |ui| {
                let row = |ui: &mut egui::Ui, selected: bool, label: &str, count: usize| {
                    let text = RichText::new(label).color(if selected {
                        theme::INK
                    } else {
                        theme::MUTED
                    });
                    let button = egui::Button::new("").fill(if selected {
                        theme::LIME
                    } else {
                        egui::Color32::TRANSPARENT
                    });
                    let response = ui.add_sized([ui.available_width(), 28.], button);
                    let ink = if selected { theme::INK } else { theme::MUTED };
                    let font = if selected {
                        theme::bold(15.)
                    } else {
                        egui::FontId::proportional(15.)
                    };
                    ui.painter().text(
                        response.rect.left_center() + egui::vec2(10., 0.),
                        egui::Align2::LEFT_CENTER,
                        text.text(),
                        font.clone(),
                        ink,
                    );
                    ui.painter().text(
                        response.rect.right_center() - egui::vec2(10., 0.),
                        egui::Align2::RIGHT_CENTER,
                        count.to_string(),
                        font,
                        ink,
                    );
                    response.clicked()
                };
                egui::ScrollArea::vertical()
                    .id_salt("filters")
                    .show(ui, |ui| {
                        theme::section(ui, "Categories");
                        if row(
                            ui,
                            !self.lib_adult && self.lib_category.is_none(),
                            "All",
                            self.groups.len(),
                        ) {
                            pick_category = Some(None);
                        }
                        for c in CATEGORIES {
                            if let Some(&n) = categories.get(c) {
                                if row(
                                    ui,
                                    !self.lib_adult && self.lib_category.as_deref() == Some(c),
                                    c,
                                    n,
                                ) {
                                    pick_category = Some(Some(c.to_owned()));
                                }
                            }
                        }
                        ui.add_space(10.);
                        theme::section(ui, "Region");
                        if row(
                            ui,
                            self.lib_region.is_none(),
                            "All regions",
                            self.groups.len(),
                        ) {
                            pick_region = Some(None);
                        }
                        let mut by_count: Vec<_> =
                            regions.iter().filter(|(r, _)| !r.is_empty()).collect();
                        by_count.sort_by_key(|(r, n)| (std::cmp::Reverse(**n), r.to_string()));
                        for (r, n) in by_count.into_iter().take(8) {
                            if row(ui, self.lib_region.as_deref() == Some(*r), r, *n) {
                                pick_region = Some(Some(r.to_string()));
                            }
                        }
                        if adult > 0 {
                            ui.add_space(10.);
                            theme::section(ui, "Review");
                            if row(ui, self.lib_adult, "Possibly adult", adult) {
                                pick_adult = true;
                            }
                        }
                    });
            });
        if let Some(c) = pick_category {
            self.lib_category = c;
            self.lib_adult = false;
            self.lib_anchor = None;
        }
        if let Some(r) = pick_region {
            self.lib_region = r;
            self.lib_anchor = None;
        }
        if pick_adult {
            self.lib_adult = true;
            self.move_to = "Adult".into();
            self.lib_anchor = None;
        }
        let visible = self.visible_groups();
        let title = if self.lib_adult {
            "Possibly adult".to_owned()
        } else {
            self.lib_category
                .clone()
                .unwrap_or_else(|| "All discs".into())
        };
        egui::Frame::new().inner_margin(egui::Margin { left: 18, right: 4, top: 6, bottom: 0 }).show(ui, |ui| {
            ui.horizontal(|ui| {
                theme::heading(ui, &title);
                ui.label(RichText::new(format!("{} games", visible.len())).color(theme::DIM));
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.add(egui::TextEdit::singleline(&mut self.lib_search).hint_text("Find a title").desired_width(200.));
                });
            });
            ui.horizontal_wrapped(|ui| {
                if ui.button("Add discs…").clicked() { self.tab=Tab::Import; }
                let count=self.lib_selected.len();
                if ui.add_enabled(count>0,theme::primary(format!("Copy selected to USB ({count})…"))).clicked() { self.open_library_copy(); }
                if ui.button("Select all shown").clicked() {
                    for &i in &visible { self.lib_selected.insert(self.groups[i].folder.clone()); }
                }
            });
            if self.lib_adult {
                ui.label(RichText::new("Vivid Interactive titles, gravure and strip-mahjong discs found in your library. Pick the ones to move; nothing moves until you confirm.").color(theme::MUTED));
            }
            if self.groups.is_empty() {
                ui.add_space(30.);
                ui.label(RichText::new(if self.prefs.library.is_empty() { "Choose a library folder above to see its games." } else { "Reading the library…" }).color(theme::MUTED));
                return;
            }
            let bar = if self.lib_selected.is_empty() { 0. } else { 64. };
            let gap = 14.;
            let width = ui.available_width() - 16.;
            let columns = ((width + gap) / (132. + gap)).floor().max(1.) as usize;
            // Tiles share the row's width; the art keeps the console cover's 4:5 shape.
            let tile_w = ((width - gap * (columns - 1) as f32) / columns as f32).floor();
            let art_h = (tile_w * 1.25).floor();
            let tile_h = art_h + 41.;
            let rows = visible.len().div_ceil(columns);
            let shift = ui.input(|i| i.modifiers.shift);
            let mut clicked = None;
            egui::ScrollArea::vertical()
                .id_salt("library_grid")
                .max_height((ui.available_height() - bar).max(120.))
                .auto_shrink([false, false])
                .show_rows(ui, tile_h + gap, rows, |ui, range| {
                    for row in range {
                        ui.horizontal(|ui| {
                            ui.spacing_mut().item_spacing.x = gap;
                            for column in 0..columns {
                                let Some(&index) = visible.get(row * columns + column) else { break };
                                let (rect, response) = ui.allocate_exact_size(egui::vec2(tile_w, tile_h), egui::Sense::click());
                                let cover = self.groups[index].cover.clone();
                                let texture = cover.as_deref().and_then(|name| self.cover_texture(ui.ctx(), name));
                                let g = &self.groups[index];
                                let selected = self.lib_selected.contains(&g.folder);
                                let art = egui::Rect::from_min_size(rect.min, egui::vec2(tile_w, art_h));
                                let painter = ui.painter();
                                painter.rect_filled(art, 0., theme::PANEL);
                                if let Some(texture) = texture {
                                    let size = texture.size_vec2();
                                    let scale = (art.width() / size.x).min(art.height() / size.y);
                                    painter.image(texture.id(), egui::Rect::from_center_size(art.center(), size * scale), egui::Rect::from_min_max(egui::pos2(0., 0.), egui::pos2(1., 1.)), egui::Color32::WHITE);
                                } else {
                                    painter.text(art.center(), egui::Align2::CENTER_CENTER, if cover.is_some() { "…" } else { "No cover" }, egui::FontId::proportional(13.), theme::DIM);
                                }
                                if selected {
                                    painter.rect_stroke(art.shrink(1.5), 0., egui::Stroke::new(3., theme::LIME), egui::StrokeKind::Inside);
                                    let mark = egui::Rect::from_min_size(art.min + egui::vec2(6., 6.), egui::vec2(22., 22.));
                                    painter.rect_filled(mark, 0., theme::LIME);
                                    painter.text(mark.center(), egui::Align2::CENTER_CENTER, "✔", theme::bold(15.), theme::INK);
                                } else if response.hovered() {
                                    painter.rect_stroke(art.shrink(1.), 0., egui::Stroke::new(2., theme::LINE), egui::StrokeKind::Inside);
                                }
                                let caption = egui::Rect::from_min_max(egui::pos2(rect.left(), art.bottom() + 4.), rect.max);
                                let title = painter.layout(short_title(&g.title), theme::bold(14.), theme::TEXT, tile_w);
                                let first = title.rows.first().map(|r| r.text()).unwrap_or_default();
                                let shown = if title.rows.len() > 1 { format!("{}…", first.trim_end()) } else { first };
                                painter.text(caption.min, egui::Align2::LEFT_TOP, shown, theme::bold(14.), theme::TEXT);
                                let detail = [g.region.as_str(), g.genre.split(';').next().unwrap_or("").trim()]
                                    .into_iter().filter(|s| !s.is_empty()).collect::<Vec<_>>().join(" · ");
                                painter.text(caption.min + egui::vec2(0., 18.), egui::Align2::LEFT_TOP, detail, egui::FontId::proportional(13.), theme::DIM);
                                let response = response.on_hover_text(format!("{}\n{}", g.title, g.folder));
                                if response.clicked() {
                                    clicked = Some(index);
                                }
                            }
                        });
                    }
                });
            if let Some(index) = clicked {
                let position = visible.iter().position(|&i| i == index).unwrap_or(0);
                match (shift, self.lib_anchor) {
                    (true, Some(anchor)) => {
                        let (a, b) = (anchor.min(position), anchor.max(position));
                        for &i in &visible[a..=b] {
                            self.lib_selected.insert(self.groups[i].folder.clone());
                        }
                    }
                    _ => {
                        let folder = self.groups[index].folder.clone();
                        if !self.lib_selected.remove(&folder) {
                            self.lib_selected.insert(folder);
                        }
                    }
                }
                self.lib_anchor = Some(position);
            }
            if !self.lib_selected.is_empty() {
                self.move_bar(ui);
            }
        });
    }
    fn move_bar(&mut self, ui: &mut egui::Ui) {
        let count = self.lib_selected.len();
        let mut confirm = false;
        let movable = self
            .groups
            .iter()
            .filter(|g| self.lib_selected.contains(&g.folder))
            .all(|g| g.movable);
        egui::Frame::new()
            .fill(theme::HEADER)
            .stroke(egui::Stroke::new(2., theme::LIME))
            .inner_margin(10.)
            .show(ui, |ui| {
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if ui.add_enabled(movable,theme::primary(format!("Move {count}"))).on_hover_text("Category moves require Category/Region/Game folders. Loose discs can still be copied to USB.").clicked() {
                        confirm = true;
                    }
                    if ui.add(theme::danger("Clear")).clicked() {
                        self.lib_selected.clear();
                        self.lib_anchor = None;
                    }
                    ui.with_layout(egui::Layout::left_to_right(egui::Align::Center), |ui| {
                        ui.label(RichText::new(format!("{count} selected")).font(theme::pixel_small(20.)));
                        ui.add_space(8.);
                        ui.label(RichText::new("Move to").color(theme::MUTED));
                        egui::ComboBox::from_id_salt("move_to")
                            .selected_text(RichText::new(&self.move_to).font(theme::bold(15.)).color(theme::LIME))
                            .width(170.)
                            .show_ui(ui, |ui| {
                                for c in CATEGORIES {
                                    ui.selectable_value(&mut self.move_to, c.to_owned(), c);
                                }
                            });
                    });
                });
            });
        if confirm {
            let discs: Vec<String> = self
                .groups
                .iter()
                .filter(|g| self.lib_selected.contains(&g.folder) && g.category != self.move_to)
                .flat_map(|g| g.discs.clone())
                .collect();
            if discs.is_empty() {
                self.error(format!("Those games are already in {}", self.move_to));
                return;
            }
            let (root, cache, category) = (self.root(), self.cache.clone(), self.move_to.clone());
            match root {
                Ok(root) => {
                    self.start(&format!("Moving {count} games to {category}"), move |job| {
                        Ok(Outcome::Updated(library::recategorize(
                            &root, &discs, &category, &cache, &job,
                        )?))
                    })
                }
                Err(e) => self.error(e),
            }
        }
    }
    fn import_ui(&mut self, ui: &mut egui::Ui) {
        theme::heading(ui, "Add discs");
        ui.label("Select discs or directories. Originals stay in place; imports copy into region and game folders.");
        ui.horizontal(|ui| {
            if ui.button("Add ISO / CUE files…").clicked() {
                if let Some(paths) = rfd::FileDialog::new()
                    .add_filter("Disc images", &["iso", "cue", "ISO", "CUE"])
                    .pick_files()
                {
                    for p in paths {
                        if !self.inputs.contains(&p) {
                            self.inputs.push(p)
                        }
                    }
                    self.plan = None;
                }
            }
            if ui.button("Add directory…").clicked() {
                if let Some(p) = rfd::FileDialog::new().pick_folder() {
                    if !self.inputs.contains(&p) {
                        self.inputs.push(p)
                    }
                    self.plan = None;
                }
            }
            if ui.button("Clear sources").clicked() {
                self.inputs.clear();
                self.plan = None;
            }
            ui.checkbox(&mut self.prefs.quick, "Fast name-only scan")
                .on_hover_text(
                    "Skips disc checksums. Filename matches require explicit acceptance.",
                );
            if ui
                .add_enabled(!self.inputs.is_empty(), theme::primary("Scan selection"))
                .clicked()
            {
                self.scan();
            }
        });
        egui::ScrollArea::vertical()
            .id_salt("sources")
            .max_height(85.)
            .show(ui, |ui| {
                for p in &self.inputs {
                    ui.label(
                        RichText::new(p.to_string_lossy())
                            .small()
                            .color(Color32::from_gray(180)),
                    );
                }
            });
        let mut selected = self.selected;
        let mut do_import = false;
        let mut resolve = false;
        if let Some(plan) = &mut self.plan {
            ui.separator();
            let count = plan.entries.iter().filter(|r| r.selected).count();
            ui.horizontal(|ui| {
                ui.strong(format!("{} entries · {count} checked", plan.entries.len()));
                if ui.button("Check resolved").clicked() {
                    for e in &mut plan.entries {
                        e.selected = e.resolvable()
                    }
                }
                if ui.button("Uncheck all").clicked() {
                    for e in &mut plan.entries {
                        e.selected = false
                    }
                }
                if ui
                    .add_enabled(
                        count > 0,
                        theme::primary(format!("Import checked ({count})")),
                    )
                    .clicked()
                {
                    do_import = true
                }
            });
            egui::ScrollArea::both()
                .id_salt("import_rows")
                .max_height(280.)
                .show_rows(ui, 32., plan.entries.len() + 1, |ui, rows| {
                    ui.set_min_width(1080.);
                    for row in rows {
                        ui.horizontal(|ui| {
                            if row == 0 {
                                for (width, label) in [
                                    (35., "Add"),
                                    (240., "Game / disc"),
                                    (175., "Identification"),
                                    (100., "Category"),
                                    (460., "Destination"),
                                ] {
                                    ui.add_sized(
                                        [width, 32.],
                                        egui::Label::new(RichText::new(label).strong()).truncate(),
                                    );
                                }
                            } else {
                                let index = row - 1;
                                let e = &mut plan.entries[index];
                                let enabled = e.resolvable();
                                ui.add_enabled_ui(enabled, |ui| {
                                    ui.add_sized(
                                        [35., 32.],
                                        egui::Checkbox::without_text(&mut e.selected),
                                    );
                                });
                                if ui
                                    .add_sized(
                                        [240., 32.],
                                        egui::Button::selectable(selected == Some(index), &e.title)
                                            .truncate(),
                                    )
                                    .on_hover_text(&e.title)
                                    .clicked()
                                {
                                    selected = Some(index)
                                }
                                let confidence = match e.confidence.as_str() {
                                    "checksum" => "Verified disc",
                                    "converted-checksum" => "Verified converted ISO",
                                    "data-track" => "Partial data match",
                                    "filename" => "Name only: review",
                                    "manual" => "Chosen release",
                                    "unverified" => "Unverified title",
                                    _ => "Needs a match",
                                };
                                ui.add_sized([175., 32.], egui::Label::new(confidence).truncate());
                                let category = if e.category.is_empty() {
                                    "Choose…"
                                } else {
                                    &e.category
                                };
                                ui.add_sized([100., 32.], egui::Label::new(category).truncate());
                                ui.add_sized(
                                    [460., 32.],
                                    egui::Label::new(RichText::new(&e.target).small()).truncate(),
                                )
                                .on_hover_text(&e.target);
                            }
                        });
                    }
                });
            if let Some(index) = selected {
                if let Some(e) = plan.entries.get_mut(index) {
                    ui.separator();
                    ui.label(&e.reason);
                    ui.label(RichText::new(format!("Source: {}", e.source.display())).small());
                    ui.horizontal(|ui| {
                        if ui.button("Choose release / edit entry…").clicked() {
                            resolve = true;
                            self.edit_title = e.title.clone();
                            self.edit_category = if e.category.is_empty() {
                                "Games".into()
                            } else {
                                e.category.clone()
                            };
                            self.edit_region = e.region.clone();
                            self.release_choice = e.game;
                            self.release_search = String::new();
                        }
                        if ui.button("Provide cover…").clicked() {
                            if let Some(p) = rfd::FileDialog::new()
                                .add_filter(
                                    "Images",
                                    &["png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff", "gif"],
                                )
                                .pick_file()
                            {
                                e.cover = Some(p)
                            }
                        }
                        if ui.button("Provide preview…").clicked() {
                            if let Some(p) = rfd::FileDialog::new()
                                .add_filter("Videos", &["mp4", "mkv", "avi", "mov", "webm"])
                                .pick_file()
                            {
                                e.preview = Some(p)
                            }
                        }
                    });
                    if let Some(p) = &e.cover {
                        ui.label(format!("Cover: {}", p.display()));
                    }
                    if let Some(p) = &e.preview {
                        ui.label(format!("Preview: {}", p.display()));
                    }
                }
            }
        }
        self.selected = selected;
        let thumb = self
            .plan
            .as_ref()
            .and_then(|p| self.selected.and_then(|n| p.entries.get(n)))
            .and_then(|e| e.cover.clone());
        self.show_thumbnail(ui, thumb.map(ThumbnailSource::File));
        if resolve {
            self.release_dialog = true
        }
        if do_import {
            self.import()
        }
    }
    fn resolve_window(&mut self, ctx: &egui::Context) {
        if !self.release_dialog {
            return;
        }
        let mut action = None;
        egui::Window::new("Resolve selected disc").collapsible(false).default_width(650.).show(ctx,|ui|{
   ui.label("Choose the exact catalog release, or enter a title to import without a verified identity.");
   ui.text_edit_singleline(&mut self.release_search);let filter=self.release_search.to_lowercase();
   if let Some(plan)=&self.plan{
       let matches=plan.catalog.games.iter().enumerate().filter_map(|(i,g)|(filter.is_empty()||g.release.to_lowercase().contains(&filter)).then_some(i)).collect::<Vec<_>>();
       let row_height=ui.text_style_height(&egui::TextStyle::Button)+2.*ui.spacing().button_padding.y;
       egui::ScrollArea::vertical().id_salt("releases").max_height(230.).show_rows(ui,row_height,matches.len(),|ui,rows|{
           for row in rows{let index=matches[row];let release=&plan.catalog.games[index].release;
               if ui.add_sized([ui.available_width(),row_height],egui::Button::selectable(self.release_choice==Some(index),release).truncate()).on_hover_text(release).clicked(){self.release_choice=Some(index);}
           }
       });
   }
   egui::ComboBox::from_label("Category").selected_text(&self.edit_category).show_ui(ui,|ui|{for c in CATEGORIES{ui.selectable_value(&mut self.edit_category,c.into(),c);}});
   if ui.add_enabled(self.release_choice.is_some(),egui::Button::new("Use chosen release (manual identification)")).clicked(){action=Some(false)}
   ui.separator();ui.label("Unverified title");ui.text_edit_singleline(&mut self.edit_title);ui.horizontal(|ui|{ui.label("Region");ui.text_edit_singleline(&mut self.edit_region);});
   ui.label(RichText::new("This keeps the title you provide and records the copied disc's fingerprint. It does not claim a catalog checksum match.").small());
   if ui.button("Use title as unverified").clicked(){action=Some(true)}if ui.button("Cancel").clicked(){self.release_dialog=false}
  });
        if let Some(unverified) = action {
            if let (Some(plan), Some(index)) = (&mut self.plan, self.selected) {
                let entry = &mut plan.entries[index];
                entry.blocked = false;
                entry.reason = if unverified {
                    "Explicit unverified import"
                } else {
                    "Explicit catalog selection; checksum identity remains manual"
                }
                .into();
                entry.confidence = if unverified { "unverified" } else { "manual" }.into();
                if unverified {
                    if self.edit_title.trim().is_empty() {
                        self.error("Enter a title first");
                        return;
                    }
                    let title = if disc::regions(&self.edit_title).is_empty()
                        && self.edit_region != "Unknown"
                    {
                        format!("{} ({})", self.edit_title.trim(), self.edit_region.trim())
                    } else {
                        self.edit_title.clone()
                    };
                    let game = Game {
                        name: title.clone(),
                        release: title,
                        region: Some(self.edit_region.clone()),
                        ..Default::default()
                    };
                    library::assign(entry, &game, None, Some(&self.edit_category));
                } else if let Some(g) = self.release_choice {
                    library::assign(
                        entry,
                        &plan.catalog.games[g],
                        Some(g),
                        Some(&self.edit_category),
                    );
                }
                entry.selected = entry.resolvable();
                if let Err(e) = library::collisions(plan) {
                    self.error(e)
                }
                self.release_dialog = false;
            }
        }
    }
    fn media_ui(&mut self, ui: &mut egui::Ui) {
        theme::heading(ui, "Covers & previews");
        ui.label("Search local EmuMovies downloads, provide files for an entry, or download missing front covers.");
        directory_row(ui, "EmuMovies downloads", &mut self.prefs.emumovies);
        ui.checkbox(
            &mut self.prefs.download_covers,
            "Download missing covers when a safe match is available",
        );
        ui.horizontal(|ui| {
            if ui.button("Read library").clicked() {
                self.load_records()
            }
            if ui.button("Update missing media").clicked() {
                let (root, prefs, cache) = (self.root(), self.prefs.clone(), self.cache.clone());
                match root {
                    Ok(root) => self.start("Finding missing media", move |job| {
                        Ok(Outcome::Updated(library::update_media(
                            &root, &prefs, &cache, None, &job,
                        )?))
                    }),
                    Err(e) => self.error(e),
                }
            }
            if ui.button("Rebuild media quality").on_hover_text("Reconvert the original covers and videos for the current menu (15-second previews need this release's boot.iso). Existing media stays available for undo. Unavailable originals are skipped.").clicked() {
                let (root, prefs, cache) = (self.root(), self.prefs.clone(), self.cache.clone());
                match root {
                    Ok(root) => self.start("Rebuilding media from originals", move |job| {
                        Ok(Outcome::Updated(library::rebuild_media(&root, &prefs, &cache, &job)?))
                    }),
                    Err(e) => self.error(e),
                }
            }
            if ui.button("Artwork review page").clicked() {
                let (root, cache) = (self.root(), self.cache.clone());
                match root {
                    Ok(root) => self.start("Building artwork review", move |_| {
                        Ok(Outcome::Report(library::review(&root, &cache)?))
                    }),
                    Err(e) => self.error(e),
                }
            }
        });
        ui.text_edit_singleline(&mut self.filter);
        let filter = self.filter.to_lowercase();
        let visible = self
            .records
            .iter()
            .enumerate()
            .filter_map(|(index, r)| {
                let title = r["title"].as_str().unwrap_or("Untitled");
                let path = r["path"].as_str().unwrap_or("");
                (filter.is_empty() || format!("{title} {path}").to_lowercase().contains(&filter))
                    .then_some(index)
            })
            .collect::<Vec<_>>();
        let row_height =
            ui.text_style_height(&egui::TextStyle::Button) + 2. * ui.spacing().button_padding.y;
        egui::ScrollArea::vertical()
            .id_salt("library_rows")
            .max_height(260.)
            .show_rows(ui, row_height, visible.len(), |ui, rows| {
                for row in rows {
                    let index = visible[row];
                    let r = &self.records[index];
                    let title = r["title"].as_str().unwrap_or("Untitled");
                    let cover = if r["cover"].is_string() {
                        "cover"
                    } else {
                        "no cover"
                    };
                    let preview = if r["preview"].is_string() {
                        "preview"
                    } else {
                        "no preview"
                    };
                    if ui
                        .add_sized(
                            [ui.available_width(), row_height],
                            egui::Button::selectable(
                                self.selected_record == Some(index),
                                format!("{title}   ·   {cover}   ·   {preview}"),
                            )
                            .truncate(),
                        )
                        .on_hover_text(title)
                        .clicked()
                    {
                        self.selected_record = Some(index);
                        self.supplied_cover = None;
                        self.supplied_preview = None;
                    }
                }
            });
        if let Some(index) = self.selected_record {
            if let Some(record) = self.records.get(index) {
                let path = record["path"].as_str().unwrap_or("").to_owned();
                ui.label(&path);
                ui.horizontal(|ui| {
                    if ui.button("Choose cover…").clicked() {
                        self.supplied_cover = rfd::FileDialog::new()
                            .add_filter(
                                "Images",
                                &["png", "jpg", "jpeg", "webp", "bmp", "tiff", "gif"],
                            )
                            .pick_file();
                    }
                    if ui.button("Choose preview…").clicked() {
                        self.supplied_preview = rfd::FileDialog::new()
                            .add_filter("Video", &["mp4", "mkv", "avi", "mov", "webm"])
                            .pick_file();
                    }
                    if ui
                        .add_enabled(
                            self.supplied_cover.is_some() || self.supplied_preview.is_some(),
                            egui::Button::new("Apply supplied media"),
                        )
                        .clicked()
                    {
                        let (root, mut prefs, cache, cover, preview) = (
                            self.root(),
                            self.prefs.clone(),
                            self.cache.clone(),
                            self.supplied_cover.clone(),
                            self.supplied_preview.clone(),
                        );
                        prefs.emumovies.clear();
                        prefs.download_covers = false;
                        match root {
                            Ok(root) => self.start("Applying supplied media", move |job| {
                                Ok(Outcome::Updated(library::update_media(
                                    &root,
                                    &prefs,
                                    &cache,
                                    Some((&path, cover, preview)),
                                    &job,
                                )?))
                            }),
                            Err(e) => self.error(e),
                        }
                    }
                });
                if let Some(p) = &self.supplied_cover {
                    ui.label(format!("Cover: {}", p.display()));
                }
                if let Some(p) = &self.supplied_preview {
                    ui.label(format!("Preview: {}", p.display()));
                }
            }
        }
        let thumb = self
            .supplied_cover
            .clone()
            .map(ThumbnailSource::File)
            .or_else(|| {
                self.selected_record
                    .and_then(|n| self.records.get(n))
                    .and_then(|r| r["cover"].as_str())
                    .map(|name| {
                        ThumbnailSource::Library(PathBuf::from(&self.prefs.library), name.into())
                    })
            });
        self.show_thumbnail(ui, thumb);
    }
    fn install_drive_from(&mut self, source: PathBuf) {
        let (target, cache, settings) = (
            f::root(Path::new(&self.prefs.destination)),
            self.cache.clone(),
            self.setup_settings.clone(),
        );
        match target {
            Ok(root) => self.start("Preparing drive without copying games", move |job| {
                let journal = oh_my_3do_library::setup::install_local(
                    &root, &cache, &source, &settings, &job,
                )?;
                Ok(Outcome::DrivePrepared { journal, root })
            }),
            Err(e) => self.error(e),
        }
    }
    fn drive_setup_ui(&mut self, ui: &mut egui::Ui) {
        ui.group(|ui| {
            ui.label(RichText::new("Set up this drive without copying games").strong().color(theme::TEXT));
            ui.label("Install the menu and everything needed for Save Manager. No game library is required.");
            ui.collapsing("Initial menu settings", |ui| {
                ui.checkbox(&mut self.setup_settings.hide_adult,"Hide Adult");
                ui.checkbox(&mut self.setup_settings.video_previews,"Enable video previews");
                ui.label("Applied only when creating settings; existing settings are preserved.");
            });
            let target=!self.prefs.destination.trim().is_empty();
            let staged=Path::new(&self.prefs.library).join("boot.iso");
            ui.horizontal_wrapped(|ui| {
                if ui.add_enabled(target && !self.prefs.library.trim().is_empty() && staged.is_file(),theme::primary("Use library boot.iso")).clicked(){self.install_drive_from(staged);}
                if ui.add_enabled(target,egui::Button::new("Choose boot.iso…")).clicked(){
                    if let Some(source)=rfd::FileDialog::new().add_filter("Menu boot ISO",&["iso"]).pick_file(){self.install_drive_from(source);}
                }
                if ui.add_enabled(target && !self.prefs.offline,egui::Button::new("Download released menu")).clicked(){
                    let (root,cache,settings)=(f::root(Path::new(&self.prefs.destination)),self.cache.clone(),self.setup_settings.clone());
                    match root {
                        Ok(root)=>self.start("Downloading and preparing drive",move |job|{
                            let journal=oh_my_3do_library::setup::install_release(&root,&cache,&settings,&job)?;
                            Ok(Outcome::DrivePrepared{journal,root})
                        }),
                        Err(e)=>self.error(e),
                    }
                }
            });
            let installed=Path::new(&self.prefs.destination).join("boot.iso");
            if ui.add_enabled(target && installed.is_file(),egui::Button::new("Repair installed menu support")).on_hover_text("Create missing support files and folders using the boot.iso already on this drive. No download or game-library copy.").clicked(){self.install_drive_from(installed);}
            ui.label("A drive with no games can still boot the menu and use Save Manager. Downloads use the published release; choose a local ISO to test an unreleased build.");
        });
    }
    fn staging_setup_ui(&mut self, ui: &mut egui::Ui) {
        ui.checkbox(
            &mut self.setup_visible,
            "Set up a PC staging library (optional)",
        );
        if self.setup_visible {
            ui.group(|ui| {
                ui.label(RichText::new("1. Choose an existing folder for your staging library").strong().color(theme::TEXT));
                directory_row(ui, "Library folder", &mut self.prefs.library);
                ui.label("Use a separate folder on your PC. Add discs from the Add discs tab after installing the menu.");
                ui.label(RichText::new("2. Install the matching menu and initial settings").strong().color(theme::TEXT));
                ui.checkbox(&mut self.setup_settings.hide_adult, "Hide Adult in the console menu");
                ui.checkbox(&mut self.setup_settings.video_previews, "Play video previews");
                ui.label("Existing console settings are preserved. The menu image is verified against the release's published SHA-256 checksum. Internet access is required.");
                if ui.add_enabled(!self.prefs.offline, egui::Button::new("Download & install matching menu")).clicked() {
                    let (root, cache, settings) = (self.root(), self.cache.clone(), self.setup_settings.clone());
                    match root {
                        Ok(root) => self.start("Installing matching menu release", move |job| {
                            Ok(Outcome::Updated(oh_my_3do_library::setup::install_release(&root, &cache, &settings, &job)?))
                        }),
                        Err(e) => self.error(e),
                    }
                }
                ui.label(RichText::new("3. Add discs, then select games in Library to copy to USB").strong().color(theme::TEXT));
                if ui.button("Add discs").clicked() { self.tab = Tab::Import; }
            });
        }
    }
    fn usb_ui(&mut self, ui: &mut egui::Ui) {
        theme::heading(ui, "Set up / repair USB");
        directory_row(ui, "USB destination", &mut self.prefs.destination);
        self.drive_setup_ui(ui);
        ui.add_space(12.);
        ui.label("To copy games, select them in Library and choose Copy selected to USB.");
        if ui.button("Go to Library").clicked() {
            self.tab = Tab::Library;
        }
    }
    fn open_library_copy(&mut self) {
        self.library_copy_discs = self
            .groups
            .iter()
            .filter(|g| self.lib_selected.contains(&g.folder))
            .flat_map(|g| g.discs.clone())
            .collect();
        self.library_copy_titles = self
            .groups
            .iter()
            .filter(|g| self.lib_selected.contains(&g.folder))
            .map(|g| g.title.clone())
            .collect();
        self.library_copy_root = self.root().ok();
        self.usb_plan = None;
        self.library_copy_open = true;
    }
    fn library_copy_window(&mut self, ctx: &egui::Context) {
        if !self.library_copy_open {
            return;
        }
        let mut open = true;
        egui::Window::new("Copy selected games to USB").id(egui::Id::new("library_usb_copy"))
            .open(&mut open).collapsible(false).default_width(690.).max_height(620.).vscroll(true).show(ctx,|ui| {
                ui.add_enabled_ui(!self.busy,|ui| {
                    ui.label(format!("Selected games: {} · Disc entries: {}",self.library_copy_titles.len(),self.library_copy_discs.len()));
                    ui.collapsing("Selected games",|ui| { for title in &self.library_copy_titles { ui.label(title); } });
                    let old_target=self.prefs.destination.clone();
                    directory_row(ui,"USB destination",&mut self.prefs.destination);
                    if old_target!=self.prefs.destination { self.usb_plan=None; }
                    ui.label("Includes every disc, cover, preview and required menu file. Games, settings and saves already on the USB are preserved.");
                    let current=self.library_copy_root.is_some() && self.root().ok()==self.library_copy_root;
                    if !current { ui.colored_label(theme::RED,"Library changed. Close this window and select games again."); }
                    let selected=self.library_copy_discs.clone();
                    self.usb_copy_review_ui(ui,&selected,current);
                });
            });
        self.library_copy_open = open;
    }
    fn usb_copy_review_ui(
        &mut self,
        ui: &mut egui::Ui,
        selected: &[String],
        selection_current: bool,
    ) {
        ui.collapsing("Copy options & details", |ui| {
            if ui.checkbox(&mut self.verify,"Verify existing disc contents (slower)").changed() { self.usb_plan=None; }
            ui.label("Settings, favorites, history and saves on the drive are preserved. Selected games follow category changes. Different existing disc contents block copying; nothing is deleted.");
        });
        if ui
            .add_enabled(selection_current, theme::primary("Validate & preview copy"))
            .clicked()
        {
            let selected = selected.to_vec();
            let (root, target, cache, verify) = (
                self.root(),
                PathBuf::from(&self.prefs.destination),
                self.cache.clone(),
                self.verify,
            );
            match root {
                Ok(root) => self.start("Validating USB copy", move |job| {
                    Ok(Outcome::Usb(usb::plan_selected(
                        &root, &target, &cache, verify, &selected, &job,
                    )?))
                }),
                Err(e) => self.error(e),
            }
        }
        if let Some(plan) = &self.usb_plan {
            ui.separator();
            ui.label(
                RichText::new(usb::summary(plan))
                    .strong()
                    .color(theme::TEXT),
            );
            let mut apply = false;
            ui.collapsing("Show file details", |ui| {
                egui::ScrollArea::vertical()
                    .id_salt("usb_rows")
                    .max_height(320.)
                    .show(ui, |ui| {
                        for item in &plan.files {
                            ui.label(format!("{:10} {}", item.status, item.path));
                        }
                    });
            });
            if ui
                .add_enabled(
                    selection_current,
                    theme::primary("Copy reviewed files to USB"),
                )
                .clicked()
            {
                apply = true
            }
            if apply {
                let plan = plan.clone();
                let cache = self.cache.clone();
                if self.root().ok().as_ref() != Some(&plan.source)
                    || f::root(Path::new(&self.prefs.destination)).ok().as_ref()
                        != Some(&plan.destination)
                {
                    self.error("Folders changed; preview the copy again");
                } else {
                    self.start("Copying to USB", move |job| {
                        Ok(Outcome::Copied(usb::apply(&plan, &cache, &job)?))
                    })
                }
            }
        }
    }
    fn load_console_settings(&mut self) {
        if self.busy {
            return;
        }
        let root = PathBuf::from(self.console_root.trim());
        self.console_error = None;
        self.console_pending = true;
        self.start("Reading console settings", move |_| {
            Ok(Outcome::ConsoleLoaded(console_config::load(&root)?))
        });
    }
    fn console_settings_ui(&mut self, ui: &mut egui::Ui) {
        if !self.console_initialized {
            self.console_initialized = true;
            if self.console_root.is_empty() {
                self.console_root = self.prefs.library.clone();
            }
            if !self.console_root.is_empty() {
                self.load_console_settings();
            }
        }
        ui.label(
            RichText::new("Console settings")
                .strong()
                .color(theme::TEXT),
        );
        ui.label("Choose the library or USB root folder. These settings control the 3DO menu.");
        ui.horizontal(|ui| {
            if ui
                .add_enabled(
                    !self.prefs.library.is_empty(),
                    egui::Button::new("Use library folder"),
                )
                .clicked()
            {
                self.console_root = self.prefs.library.clone();
                self.load_console_settings();
            }
            if ui
                .add_enabled(
                    !self.prefs.destination.is_empty(),
                    egui::Button::new("Use USB destination"),
                )
                .clicked()
            {
                self.console_root = self.prefs.destination.clone();
                self.load_console_settings();
            }
        });
        directory_row(ui, "Settings folder", &mut self.console_root);
        ui.horizontal(|ui| {
            if ui
                .add_enabled(
                    !self.console_root.trim().is_empty(),
                    egui::Button::new("Load / reload settings"),
                )
                .clicked()
            {
                self.load_console_settings();
            }
            ui.label(
                RichText::new(console_config::PATH)
                    .small()
                    .color(theme::MUTED),
            );
        });
        if let Some(error) = &self.console_error {
            ui.colored_label(theme::RED, error);
        }
        let loaded_here = self
            .console_loaded
            .as_ref()
            .is_some_and(|loaded| loaded.root.to_string_lossy() == self.console_root);
        if !loaded_here {
            ui.label(
                RichText::new("Load this folder before editing its console settings.")
                    .color(theme::MUTED),
            );
        } else if self
            .console_loaded
            .as_ref()
            .is_some_and(|loaded| !loaded.exists())
        {
            ui.label("No INI file yet. Save to create it with these defaults.");
        }
        ui.add_enabled_ui(loaded_here && !self.busy, |ui| {
            egui::Grid::new("console_settings_values").num_columns(2).spacing([32., 8.]).show(ui, |ui| {
                ui.checkbox(&mut self.console_edit.usb_covers, "Show covers");
                ui.checkbox(&mut self.console_edit.video_previews, "Play video previews");
                ui.end_row();
                ui.checkbox(&mut self.console_edit.preview_sound, "Play preview sound")
                    .on_hover_text("Game audio plays when video previews are enabled.");
                ui.checkbox(&mut self.console_edit.large_titles, "Larger game titles");
                ui.end_row();
                ui.checkbox(&mut self.console_edit.remember_position, "Remember browsing position");
                ui.checkbox(&mut self.console_edit.recent_games, "Record recent games");
                ui.end_row();
                ui.checkbox(&mut self.console_edit.hide_adult, "Hide adult games")
                    .on_hover_text("Hide the Adult category from the console browser, Find, Recent and Favorites.");
                ui.end_row();
            });
            ui.add(egui::Slider::new(&mut self.console_edit.screen_margin, 0..=12).text("Screen margin (pixels)"));
            ui.horizontal(|ui| {
                ui.label("Folder compatibility");
                egui::ComboBox::from_id_salt("console_folder_workaround")
                    .selected_text(match self.console_edit.folder_workaround { 1 => "Always", 2 => "Off", _ => "Automatic (FCare)" })
                    .show_ui(ui, |ui| {
                        ui.selectable_value(&mut self.console_edit.folder_workaround, 0, "Automatic (FCare)");
                        ui.selectable_value(&mut self.console_edit.folder_workaround, 1, "Always");
                        ui.selectable_value(&mut self.console_edit.folder_workaround, 2, "Off");
                    });
            }).response.on_hover_text("Works around repeated folder IDs on FCare optical drive emulators. Automatic is recommended.");
            let dirty = self.console_loaded.as_ref().is_some_and(|loaded| loaded.settings != self.console_edit || !loaded.exists());
            ui.horizontal(|ui| {
                if ui.add_enabled(dirty, theme::primary("Save console settings")).clicked() {
                    if let Some(loaded) = self.console_loaded.clone() {
                        let values = self.console_edit.clone();
                        let cache = self.cache.clone();
                        self.console_pending = true;
                        self.console_error = None;
                        self.start("Saving console settings", move |job| {
                            Ok(Outcome::ConsoleSaved(console_config::save(&loaded, &values, &cache, &job)?))
                        });
                    }
                }
                if ui.button("Restore defaults").clicked() {
                    self.console_edit = console_config::Settings::default();
                }
                let undo = self.console_journal.as_ref().filter(|(root, _)| {
                    self.console_loaded.as_ref().is_some_and(|loaded| loaded.root == *root)
                }).cloned();
                if ui.add_enabled(!dirty && undo.is_some(), egui::Button::new("Undo settings save")).clicked() {
                    if let Some((root, journal)) = undo {
                        self.console_pending = true;
                        self.console_error = None;
                        self.start("Restoring console settings", move |_| {
                            library::undo(&journal)?;
                            Ok(Outcome::ConsoleRestored(console_config::load(&root)?))
                        });
                    }
                }
                if dirty { ui.label(RichText::new("Unsaved changes").color(theme::AMBER)); }
            });
        });
        ui.label(RichText::new("Changes apply the next time you start the console with this drive. Restore defaults changes the form until you save.").small());
        ui.label(RichText::new("USB preparation preserves an existing drive's INI. Load the USB folder here to change that drive's settings.").small().color(theme::MUTED));
    }
    fn settings_ui(&mut self, ui: &mut egui::Ui) {
        theme::heading(ui, "Tools & settings");
        self.staging_setup_ui(ui);
        ui.separator();
        ui.label(RichText::new("Updates").strong().color(theme::TEXT));
        let mut automatic = !self.prefs.disable_update_checks;
        if ui
            .checkbox(&mut automatic, "Check for updates on startup")
            .changed()
        {
            self.prefs.disable_update_checks = !automatic;
            self.save_preferences();
        }
        self.updates.controls(
            ui,
            self.prefs.offline,
            vec![
                PathBuf::from(&self.prefs.library),
                PathBuf::from(&self.prefs.destination),
            ],
        );
        ui.separator();
        self.save_browser.show_button(ui);
        self.cheat_database.button(ui, &self.prefs.destination);
        ui.separator();
        self.console_settings_ui(ui);
        ui.separator();
        ui.label(
            RichText::new("Desktop preferences")
                .strong()
                .color(theme::TEXT),
        );
        if let Some(state) = ui
            .ctx()
            .data(|data| data.get_temp::<String>(egui::Id::new("graphics")))
        {
            ui.label(state);
        }
        if ui
            .checkbox(
                &mut self.prefs.software_rendering,
                "Use software graphics (restart required)",
            )
            .changed()
        {
            self.save_preferences();
        }
        ui.checkbox(
            &mut self.prefs.offline,
            "Offline: use the cached catalog and local media only",
        );
        directory_row(ui, "FFmpeg / ffprobe folder", &mut self.prefs.video_tools);
        ui.horizontal(|ui| {
            let ready = media::video_exes(&self.prefs).is_ok();
            ui.label(if ready {
                "Video tools ready"
            } else {
                "Video tools not found"
            });
            if ui.button("Get video tools").clicked() {
                let cache = self.cache.clone();
                self.start("Downloading FFmpeg and ffprobe from BtbN", move |job| {
                    Ok(Outcome::Video(media::install_video_tools(&cache, &job)?))
                })
            }
        });
        ui.label("Video tools download separately from their provider and are verified against its published SHA-256 digest. They are not bundled with the app.");
        ui.separator();
        if ui.button("Rebuild Find index").clicked() {
            let (root, cache) = (self.root(), self.cache.clone());
            match root {
                Ok(root) => self.start("Rebuilding Find", move |job| {
                    let mut tx = library::Journal::new(&root, &cache)?;
                    if let Err(e) = library::build_indexes(&root, &mut tx, &job) {
                        tx.undo()?;
                        return Err(e);
                    }
                    tx.finish()?;
                    Ok(Outcome::Updated(tx.path))
                }),
                Err(e) => self.error(e),
            }
        }
        ui.horizontal(|ui| {
            if ui
                .add_enabled(
                    self.last_journal.is_some(),
                    egui::Button::new("Undo last change"),
                )
                .clicked()
            {
                if let Some(path) = self.last_journal.clone() {
                    self.start("Undoing last change", move |_| {
                        library::undo(&path)?;
                        Ok(Outcome::Done("Previous library state restored".into()))
                    });
                    self.last_journal = None;
                }
            }
            if ui.button("Open undo journal…").clicked() {
                if let Some(path) = rfd::FileDialog::new()
                    .set_directory(self.cache.join("native-transactions"))
                    .add_filter("Native undo journal", &["json"])
                    .pick_file()
                {
                    self.start("Restoring transaction", move |_| {
                        library::undo(&path)?;
                        Ok(Outcome::Done("Previous state restored".into()))
                    });
                }
            }
        });
        ui.label(RichText::new("Undo refuses to overwrite files edited since the operation. Original import sources always stay in place.").small());
        if ui.button("Install menu boot.iso…").clicked() {
            if let Some(source) = rfd::FileDialog::new()
                .add_filter("Menu boot ISO", &["iso"])
                .pick_file()
            {
                let (root, cache) = (self.root(), self.cache.clone());
                match root {
                    Ok(root) => self.start("Installing menu boot image and support", move |job| {
                        let journal = oh_my_3do_library::setup::install_local(
                            &root,
                            &cache,
                            &source,
                            &Default::default(),
                            &job,
                        )?;
                        Ok(Outcome::DrivePrepared { journal, root })
                    }),
                    Err(e) => self.error(e),
                }
            }
        }
        ui.separator();
        ui.label(concat!("Oh My 3DO Library ", env!("CARGO_PKG_VERSION")));
        ui.label("Catalogs, media conversion cache and undo journals stay on the PC.");
        if ui.button("Open app data / reports").clicked() {
            open(&self.cache)
        }
    }
}
fn directory_row(ui: &mut egui::Ui, label: &str, value: &mut String) {
    ui.horizontal(|ui| {
        ui.label(label);
        ui.add(
            egui::TextEdit::singleline(value)
                .desired_width((ui.available_width() - 120.).max(150.)),
        );
        if ui.button("Browse…").clicked() {
            let mut dialog = rfd::FileDialog::new();
            if !value.is_empty() {
                dialog = dialog.set_directory(Path::new(value));
            }
            if let Some(p) = dialog.pick_folder() {
                *value = p.to_string_lossy().into_owned();
            }
        }
    });
}
fn open(path: &Path) {
    #[cfg(windows)]
    let mut command = {
        let mut c = std::process::Command::new("explorer.exe");
        c.arg(path);
        c
    };
    #[cfg(target_os = "macos")]
    let mut command = {
        let mut c = std::process::Command::new("open");
        c.arg(path);
        c
    };
    #[cfg(all(not(windows), not(target_os = "macos")))]
    let mut command = {
        let mut c = std::process::Command::new("xdg-open");
        c.arg(path);
        c
    };
    let _ = command.spawn();
}
impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let frame_started = Instant::now();
        self.poll();
        let update_roots = vec![
            PathBuf::from(&self.prefs.library),
            PathBuf::from(&self.prefs.destination),
        ];
        self.updates.tick(
            ctx,
            !self.prefs.offline
                && !self.prefs.disable_update_checks
                && self.screenshot.is_none()
                && self.profile.is_none(),
            update_roots.clone(),
        );
        if let Some(action) =
            self.updates
                .window(ctx, !self.busy && !self.prefs.offline, update_roots)
        {
            match action {
                update_ui::Action::Desktop {
                    release,
                    platform,
                    path,
                } => self.start("Downloading desktop update", move |job| {
                    oh_my_3do_library::updates::download_desktop(&release, &platform, &path, &job)?;
                    Ok(Outcome::Downloaded {
                        path,
                        desktop: true,
                    })
                }),
                update_ui::Action::BootDownload {
                    release,
                    digest,
                    path,
                } => self.start("Downloading boot.iso", move |job| {
                    oh_my_3do_library::updates::download_boot(&release, &digest, &path, &job)?;
                    Ok(Outcome::Downloaded {
                        path,
                        desktop: false,
                    })
                }),
                update_ui::Action::Boot {
                    release,
                    target,
                    digest,
                } => {
                    let cache = self.cache.clone();
                    self.start("Updating boot.iso", move |job| {
                        let journal = oh_my_3do_library::updates::install_boot(
                            &release, &target, &digest, &cache, &job,
                        )?;
                        Ok(Outcome::BootUpdated {
                            journal,
                            root: target.root,
                        })
                    });
                }
            }
        }
        self.save_browser.window(ctx, &self.prefs.library);
        self.cheat_database.window(ctx, self.prefs.offline);
        self.frames += 1;
        if self.busy && ctx.input(|i| i.viewport().close_requested()) {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            self.cancel.store(true, Ordering::Relaxed);
            self.status = "Cancelling safely; waiting for rollback to finish".into();
        }
        egui::TopBottomPanel::top("header")
            .frame(
                egui::Frame::new()
                    .fill(theme::HEADER)
                    .inner_margin(egui::Margin::symmetric(20, 10)),
            )
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.add(egui::Image::new(&self.logo).fit_to_exact_size(egui::vec2(
                        self.logo.size_vec2().x * 40. / self.logo.size_vec2().y,
                        40.,
                    )));
                    ui.add_space(12.);
                    ui.add_enabled_ui(!self.busy, |ui| {
                        for (n, (tab, label)) in [
                            (Tab::Library, "Library"),
                            (Tab::Import, "Add discs"),
                            (Tab::Media, "Media"),
                            (Tab::Usb, "USB"),
                            (Tab::Settings, "Tools"),
                        ]
                        .into_iter()
                        .enumerate()
                        {
                            let on = self.tab == tab;
                            let mut job = egui::text::LayoutJob::default();
                            job.append(
                                &format!("CH{} ", n + 1),
                                0.,
                                egui::TextFormat::simple(
                                    theme::pixel(12.),
                                    if on { theme::INK } else { theme::OSD },
                                ),
                            );
                            job.append(
                                label,
                                0.,
                                egui::TextFormat::simple(
                                    theme::pixel_small(20.),
                                    if on { theme::INK } else { theme::TEXT },
                                ),
                            );
                            if ui
                                .add(
                                    egui::Button::new(job)
                                        .fill(if on { theme::LIME } else { theme::PANEL })
                                        .min_size(egui::vec2(0., 36.)),
                                )
                                .clicked()
                            {
                                self.tab = tab;
                                self.filter.clear();
                                if tab == Tab::Media
                                    && self.records.is_empty()
                                    && !self.prefs.library.is_empty()
                                {
                                    self.load_records();
                                }
                            }
                        }
                    });
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        ui.add_enabled_ui(!self.busy, |ui| {
                            let label = if self.prefs.library.is_empty() {
                                "Choose library…".to_owned()
                            } else {
                                format!("{}   Change…", self.prefs.library)
                            };
                            if ui
                                .add(
                                    egui::Button::new(RichText::new(label).color(theme::MUTED))
                                        .fill(theme::INK)
                                        .stroke(egui::Stroke::new(2., theme::LINE)),
                                )
                                .on_hover_text("Library folder")
                                .clicked()
                            {
                                let mut dialog = rfd::FileDialog::new();
                                if !self.prefs.library.is_empty() {
                                    dialog = dialog.set_directory(Path::new(&self.prefs.library));
                                }
                                if let Some(p) = dialog.pick_folder() {
                                    self.prefs.library = p.to_string_lossy().into_owned();
                                    self.save_preferences();
                                    self.records.clear();
                                    self.groups.clear();
                                    self.covers.clear();
                                    self.covers_pending.clear();
                                    self.records_stale = true;
                                }
                            }
                        });
                    });
                });
            });
        egui::TopBottomPanel::bottom("status")
            .frame(
                egui::Frame::new()
                    .fill(theme::HEADER)
                    .inner_margin(egui::Margin::symmetric(20, 6)),
            )
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    if self.busy {
                        ui.spinner();
                        if ui.button("Cancel safely").clicked() {
                            self.cancel.store(true, Ordering::Relaxed);
                            self.status = "Cancelling safely…".into();
                        }
                    }
                    ui.label(RichText::new(&self.status).color(theme::MUTED));
                });
                egui::CollapsingHeader::new("Activity log").show(ui, |ui| {
                    egui::ScrollArea::vertical()
                        .id_salt("logs")
                        .max_height(130.)
                        .stick_to_bottom(true)
                        .show_rows(
                            ui,
                            ui.text_style_height(&egui::TextStyle::Small),
                            self.logs.len(),
                            |ui, rows| {
                                for row in rows {
                                    ui.add(
                                        egui::Label::new(RichText::new(&self.logs[row]).small())
                                            .truncate(),
                                    );
                                }
                            },
                        );
                });
            });
        egui::CentralPanel::default()
            .frame(egui::Frame::new().fill(theme::BG))
            .show(ctx, |ui| {
                ui.add_enabled_ui(!self.busy && !self.library_copy_open, |ui| {
                    if self.tab == Tab::Library {
                        return self.library_ui(ui);
                    }
                    egui::Frame::new()
                        .inner_margin(egui::Margin::symmetric(24, 14))
                        .show(ui, |ui| {
                            egui::ScrollArea::vertical()
                                .id_salt(("page", self.tab as usize))
                                .auto_shrink([false, false])
                                .show(ui, |ui| match self.tab {
                                    Tab::Library => {}
                                    Tab::Import => self.import_ui(ui),
                                    Tab::Media => self.media_ui(ui),
                                    Tab::Usb => self.usb_ui(ui),
                                    Tab::Settings => self.settings_ui(ui),
                                });
                        });
                });
            });
        self.library_copy_window(ctx);
        if !self.busy {
            self.resolve_window(ctx)
        }
        if self.busy {
            ctx.request_repaint_after(Duration::from_millis(100));
        }
        // Capture only this app's framebuffer for repeatable packaging smoke tests.
        // Wait for the library and its visible covers before capturing.
        let records_ready = !matches!(self.tab, Tab::Library | Tab::Media) || !self.records_stale;
        if !self.busy && !self.updates.checking() && records_ready && self.covers_pending.is_empty()
        {
            self.settled_frames += 1;
        } else {
            self.settled_frames = 0;
        }
        if self.screenshot.is_some()
            && self.frames >= 3
            && (self.settled_frames >= 10 || self.frames == 900)
            && !self.screenshot_requested
        {
            self.screenshot_requested = true;
            ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(Default::default()));
        }
        if let Some(path) = self.screenshot.clone() {
            let screenshot = ctx.input(|i| {
                i.events.iter().find_map(|e| {
                    if let egui::Event::Screenshot { image, .. } = e {
                        Some(image.clone())
                    } else {
                        None
                    }
                })
            });
            if let Some(img) = screenshot {
                let rgba = img
                    .pixels
                    .iter()
                    .flat_map(|p| p.to_array())
                    .collect::<Vec<_>>();
                let result = image::save_buffer(
                    &path,
                    &rgba,
                    img.width() as u32,
                    img.height() as u32,
                    image::ColorType::Rgba8,
                );
                if let Err(e) = result {
                    self.error(e)
                }
                self.screenshot = None;
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            } else {
                ctx.request_repaint_after(Duration::from_millis(100));
            }
        }
        if let Some(profile) = &mut self.profile {
            let now = Instant::now();
            if self.frames > 15 {
                profile
                    .intervals
                    .push((frame_started - profile.previous).as_secs_f64() * 1000.);
                profile
                    .cpu
                    .push((now - frame_started).as_secs_f64() * 1000.);
            }
            profile.previous = frame_started;
            if profile.cpu.len() >= 120 {
                profile.cpu.sort_by(f64::total_cmp);
                profile.intervals.sort_by(f64::total_cmp);
                let graphics = ctx.data(|data| data.get_temp::<String>(egui::Id::new("graphics")));
                let report = serde_json::json!({"rows":self.records.len(), "frames":profile.cpu.len(), "ui_cpu_median_ms":profile.cpu[60], "ui_cpu_p95_ms":profile.cpu[114], "frame_median_ms":profile.intervals[60], "frame_p95_ms":profile.intervals[114], "graphics":graphics});
                let _ = fs::write(&profile.path, serde_json::to_vec_pretty(&report).unwrap());
                self.profile = None;
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            } else {
                ctx.request_repaint();
            }
        }
    }
    fn on_exit(&mut self, _: Option<&eframe::glow::Context>) {
        self.save_preferences()
    }
}
fn main() -> eframe::Result<()> {
    let args = std::env::args().collect::<Vec<_>>();
    let screenshot = args
        .windows(2)
        .find(|w| w[0] == "--screenshot")
        .map(|w| PathBuf::from(&w[1]));
    let compare_backup = args
        .windows(2)
        .find(|w| w[0] == "--compare-backup")
        .map(|w| PathBuf::from(&w[1]));
    let save_slots = args
        .windows(2)
        .find(|w| w[0] == "--save-slots")
        .map(|w| PathBuf::from(&w[1]));
    let save_backup = args
        .windows(2)
        .find(|w| w[0] == "--save-backup")
        .map(|w| PathBuf::from(&w[1]));
    let cheat_database = args
        .windows(2)
        .find(|w| w[0] == "--cheat-database")
        .map(|w| PathBuf::from(&w[1]));
    let guided_setup = args.iter().any(|a| a == "--guided-setup");
    let check_updates = args.iter().any(|a| a == "--check-updates");
    let profile = args
        .windows(2)
        .find(|w| w[0] == "--profile-ui")
        .map(|w| PathBuf::from(&w[1]));
    // Starting channel, 1-5, for screenshots of each page.
    let channel = args
        .windows(2)
        .find(|w| w[0] == "--tab")
        .and_then(|w| w[1].parse::<usize>().ok());
    let library = args
        .windows(2)
        .find(|w| w[0] == "--library")
        .map(|w| w[1].clone());
    let mut options = eframe::NativeOptions {
        renderer: eframe::Renderer::Wgpu,
        viewport: egui::ViewportBuilder::default()
            .with_icon(
                eframe::icon_data::from_png_bytes(include_bytes!("../../assets/app-icon.png"))
                    .expect("Bundled app icon must be a valid PNG"),
            )
            .with_inner_size([1220., 850.])
            .with_min_inner_size([980., 680.]),
        ..Default::default()
    };
    // Try hardware devices first. A failed driver must not prevent startup:
    // keep a working software device as the final compatibility fallback.
    let mut setup = eframe::egui_wgpu::WgpuSetupCreateNew::default();
    // Pre-created GL devices are tied to a context without our window's
    // surface. Use native backends so the selected device can present later.
    setup.instance_descriptor.backends = wgpu::Backends::PRIMARY;
    #[cfg(windows)]
    {
        setup.instance_descriptor.backends = wgpu::Backends::DX12;
    }
    let prefs = fs::read(cache_dir().join("native-settings.json"))
        .ok()
        .and_then(|b| serde_json::from_slice::<Preferences>(&b).ok())
        .unwrap_or_default();
    let software = prefs.software_rendering || args.iter().any(|a| a == "--software-rendering");
    let instance = wgpu::Instance::new(&setup.instance_descriptor);
    let mut adapters = instance.enumerate_adapters(setup.instance_descriptor.backends);
    adapters.sort_by_key(|adapter| {
        let cpu = adapter.get_info().device_type == wgpu::DeviceType::Cpu;
        if software {
            !cpu
        } else {
            cpu
        }
    });
    if software {
        adapters.retain(|adapter| adapter.get_info().device_type == wgpu::DeviceType::Cpu);
    }
    let mut attempts = Vec::new();
    let mut graphics = None;
    for adapter in adapters {
        let info = adapter.get_info();
        let descriptor = (setup.device_descriptor)(&adapter);
        match pollster::block_on(adapter.request_device(&descriptor)) {
            Ok((device, queue)) => {
                let label = format!(
                    "Graphics: {} ({:?}, {:?})",
                    info.name, info.device_type, info.backend
                );
                attempts.push(label.clone());
                graphics = Some((
                    label,
                    eframe::egui_wgpu::WgpuSetupExisting {
                        instance: instance.clone(),
                        adapter,
                        device,
                        queue,
                    },
                ));
                break;
            }
            Err(error) => attempts.push(format!("{} ({:?}): {error}", info.name, info.device_type)),
        }
    }
    let cache = cache_dir();
    let _ = fs::create_dir_all(&cache);
    let _ = fs::write(cache.join("native-graphics.log"), attempts.join("\n"));
    let Some((graphics_label, graphics_setup)) = graphics else {
        return Err(eframe::Error::AppCreation(Box::new(std::io::Error::other(
            format!("No usable graphics device. {}", attempts.join("; ")),
        ))));
    };
    options.wgpu_options.wgpu_setup = graphics_setup.into();
    options.wgpu_options.desired_maximum_frame_latency = Some(1);
    eframe::run_native(
        "Oh My 3DO Library",
        options,
        Box::new(move |cc| {
            cc.egui_ctx
                .data_mut(|data| data.insert_temp(egui::Id::new("graphics"), graphics_label));
            let mut app = App::new(cc, screenshot);
            if let Some(library) = library {
                app.prefs.library = library;
            }
            if let Some(n) = channel {
                app.tab = [
                    Tab::Library,
                    Tab::Import,
                    Tab::Media,
                    Tab::Usb,
                    Tab::Settings,
                ][n.clamp(1, 5) - 1];
            }
            if guided_setup {
                app.tab = Tab::Usb;
            }
            if check_updates && !app.prefs.offline {
                app.updates.check(
                    &cc.egui_ctx,
                    vec![
                        PathBuf::from(&app.prefs.library),
                        PathBuf::from(&app.prefs.destination),
                    ],
                );
            }
            if let Some(path) = cheat_database {
                if let Err(error) = app.cheat_database.open_file(&path) {
                    app.error(error);
                }
            }
            if let Some(path) = save_backup {
                if let Err(error) = app.save_browser.open_file(&path) {
                    app.error(error);
                }
            }
            if let Some(path) = save_slots {
                app.save_browser.browse_slots(path);
            }
            if let Some(path) = compare_backup {
                if let Err(error) = app.save_browser.compare_file(&path) {
                    app.error(error);
                }
            }
            if let Some(path) = profile {
                // Explicit diagnostic mode: exercise a large list without
                // importing files or reading/changing the library.
                app.records = (0..5000).map(|i| serde_json::json!({"title":format!("Game {i:05}"),"path":format!("Games/USA/Game {i:05}/Disc 1.iso")})).collect();
                app.tab = Tab::Media;
                app.profile = Some(UiProfile {
                    path,
                    previous: Instant::now(),
                    intervals: vec![],
                    cpu: vec![],
                });
            }
            Ok(Box::new(app))
        }),
    )
}
