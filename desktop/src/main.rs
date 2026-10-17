#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]
use eframe::egui::{self, Color32, RichText};
use oh_my_3do_library::{
    cache_dir, catalog, disc, fsutil as f, library, media, model::*, usb, Job,
};
use serde_json::Value;
use std::{
    fs,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc,
    },
    time::{Duration, Instant},
};

enum Outcome {
    Scan(Plan),
    Imported(PathBuf),
    Updated(PathBuf),
    Usb(usb::CopyPlan),
    Copied(PathBuf),
    Video(PathBuf),
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
    let mut reader = image::ImageReader::open(path)?.with_guessed_format()?;
    let mut limits = image::Limits::default();
    limits.max_alloc = Some(64 * 1024 * 1024);
    reader.limits(limits);
    let image = reader.decode()?.thumbnail(168, 210).to_rgba8();
    Ok(egui::ColorImage::from_rgba_unmultiplied(
        [image.width() as usize, image.height() as usize],
        image.as_raw(),
    ))
}
#[derive(Clone, Copy, PartialEq)]
enum Tab {
    Import,
    Media,
    Usb,
    Settings,
}
struct App {
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
    verify: bool,
    last_journal: Option<PathBuf>,
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
        let mut visuals = egui::Visuals::dark();
        visuals.panel_fill = Color32::from_rgb(16, 25, 38);
        visuals.window_fill = Color32::from_rgb(23, 35, 50);
        visuals.selection.bg_fill = Color32::from_rgb(25, 94, 107);
        visuals.selection.stroke.color = Color32::from_rgb(180, 245, 235);
        cc.egui_ctx.set_visuals(visuals);
        cc.egui_ctx.style_mut(|s| {
            s.spacing.item_spacing = egui::vec2(10., 9.);
            s.spacing.button_padding = egui::vec2(12., 7.);
            s.text_styles
                .insert(egui::TextStyle::Body, egui::FontId::proportional(16.));
            s.text_styles
                .insert(egui::TextStyle::Button, egui::FontId::proportional(15.));
            s.text_styles
                .insert(egui::TextStyle::Heading, egui::FontId::proportional(25.));
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
            tab: Tab::Import,
            selected: None,
            sender,
            receiver,
            busy: false,
            cancel: Arc::new(AtomicBool::new(false)),
            status: "Choose a library and add source files to begin".into(),
            logs: vec![],
            release_dialog: false,
            release_search: String::new(),
            release_choice: None,
            edit_title: String::new(),
            edit_category: "Games".into(),
            edit_region: "USA".into(),
            usb_plan: None,
            verify: false,
            last_journal: None,
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
                    match result {
                        Err(e) => self.error(e),
                        Ok(outcome) => match outcome {
                            Outcome::Scan(plan) => {
                                self.status=format!("{} disc entries scanned. Review names and locations, then check entries to import.",plan.entries.len());
                                self.plan = Some(plan);
                                self.selected = None;
                            }
                            Outcome::Imported(path) => {
                                self.last_journal = Some(path);
                                self.plan = None;
                                self.inputs.clear();
                                self.status =
                                    "Import complete. Original files were preserved.".into();
                            }
                            Outcome::Updated(path) => {
                                self.last_journal = Some(path);
                                self.status="Media update complete. See the log for missing or skipped media.".into();
                                self.records.clear();
                                self.selected_record = None;
                            }
                            Outcome::Usb(plan) => {
                                self.status = usb::summary(&plan);
                                self.usb_plan = Some(plan)
                            }
                            Outcome::Copied(path) => {
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
                            Outcome::Records(records) => {
                                self.records = records;
                                self.selected_record = None;
                                self.status =
                                    format!("{} disc entries in the library", self.records.len());
                            }
                            Outcome::Report(path) => {
                                self.status = format!("Report saved: {}", path.display());
                                open(&path);
                            }
                            Outcome::Done(s) => self.status = s,
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
    fn import_ui(&mut self, ui: &mut egui::Ui) {
        ui.heading("Add to the library");
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
                .add_enabled(!self.inputs.is_empty(), egui::Button::new("Scan selection"))
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
                        egui::Button::new(format!("Import checked ({count})")),
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
        ui.heading("Artwork & previews");
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
    fn usb_ui(&mut self, ui: &mut egui::Ui) {
        ui.heading("Prepare a USB drive");
        ui.label("Validate the staging library, review the copy, then transfer only the files the menu needs.");
        directory_row(ui, "USB destination", &mut self.prefs.destination);
        ui.checkbox(
            &mut self.verify,
            "Verify all existing disc contents (slower)",
        );
        ui.label("Existing destination settings, favorites, history and saves are preserved. Different existing discs block copying; no destination files are deleted.");
        if ui.button("Validate & preview copy").clicked() {
            let (root, target, cache, verify) = (
                self.root(),
                PathBuf::from(&self.prefs.destination),
                self.cache.clone(),
                self.verify,
            );
            match root {
                Ok(root) => self.start("Validating USB copy", move |job| {
                    Ok(Outcome::Usb(usb::plan(
                        &root, &target, &cache, verify, &job,
                    )?))
                }),
                Err(e) => self.error(e),
            }
        }
        if let Some(plan) = &self.usb_plan {
            ui.separator();
            ui.strong(usb::summary(plan));
            let mut apply = false;
            egui::ScrollArea::vertical()
                .id_salt("usb_rows")
                .max_height(320.)
                .show(ui, |ui| {
                    for item in &plan.files {
                        ui.label(format!("{:10} {}", item.status, item.path));
                    }
                });
            if ui.button("Copy reviewed files to USB").clicked() {
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
    fn settings_ui(&mut self, ui: &mut egui::Ui) {
        ui.heading("Tools & settings");
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
                    Ok(root) => self.start("Installing menu boot image", move |job| {
                        disc::issue(&source)?;
                        if fs::metadata(&source)?.len() > 16 * 1024 * 1024 {
                            anyhow::bail!("Choose a menu boot image smaller than 16 MiB")
                        }
                        let data = fs::read(&source)?;
                        if data.len() < 84
                            || data.len() > 16 * 1024 * 1024
                            || data[..7] != [1, 90, 90, 90, 90, 90, 1]
                        {
                            anyhow::bail!("Choose a 3DO menu boot image")
                        };
                        let mut tx = library::Journal::new(&root, &cache)?;
                        let result = (|| -> anyhow::Result<()> {
                            job.check()?;
                            tx.write("boot.iso", &data)?;
                            tx.finish()
                        })();
                        if let Err(error) = result {
                            tx.undo()?;
                            return Err(error);
                        }
                        Ok(Outcome::Updated(tx.path))
                    }),
                    Err(e) => self.error(e),
                }
            }
        }
        ui.separator();
        ui.label(concat!(
            "Oh My 3DO Library ",
            env!("CARGO_PKG_VERSION")
        ));
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
        self.frames += 1;
        if self.busy && ctx.input(|i| i.viewport().close_requested()) {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            self.cancel.store(true, Ordering::Relaxed);
            self.status = "Cancelling safely; waiting for rollback to finish".into();
        }
        egui::TopBottomPanel::top("header").show(ctx, |ui| {
            ui.add_space(8.);
            ui.horizontal(|ui| {
                ui.heading(RichText::new("OH MY 3DO").color(Color32::from_rgb(124, 223, 212)));
                ui.label(RichText::new("LIBRARY").color(Color32::from_gray(170)));
            });
            ui.add_enabled_ui(!self.busy, |ui| {
                directory_row(ui, "Library folder", &mut self.prefs.library);
            });
            ui.add_space(5.);
        });
        egui::TopBottomPanel::bottom("status").show(ctx, |ui| {
            ui.horizontal(|ui| {
                if self.busy {
                    ui.spinner();
                    if ui.button("Cancel safely").clicked() {
                        self.cancel.store(true, Ordering::Relaxed);
                        self.status = "Cancelling safely…".into();
                    }
                }
                ui.label(&self.status);
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
        egui::SidePanel::left("navigation")
            .exact_width(170.)
            .show(ctx, |ui| {
                ui.add_space(15.);
                ui.add_enabled_ui(!self.busy, |ui| {
                    for (tab, label) in [
                        (Tab::Import, "Add discs"),
                        (Tab::Media, "Covers & previews"),
                        (Tab::Usb, "Prepare USB"),
                        (Tab::Settings, "Tools & settings"),
                    ] {
                        if ui.selectable_label(self.tab == tab, label).clicked() {
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
                ui.with_layout(egui::Layout::bottom_up(egui::Align::LEFT), |ui| {
                    ui.label(
                        RichText::new("Original files\nstay in place.")
                            .small()
                            .color(Color32::from_gray(145)),
                    );
                });
            });
        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_enabled_ui(!self.busy, |ui| {
                egui::ScrollArea::vertical()
                    .id_salt("page")
                    .show(ui, |ui| match self.tab {
                        Tab::Import => self.import_ui(ui),
                        Tab::Media => self.media_ui(ui),
                        Tab::Usb => self.usb_ui(ui),
                        Tab::Settings => self.settings_ui(ui),
                    });
            });
        });
        if !self.busy {
            self.resolve_window(ctx)
        }
        if self.busy {
            ctx.request_repaint_after(Duration::from_millis(100));
        }
        // Capture only this app's framebuffer for repeatable packaging smoke tests.
        if self.screenshot.is_some() && self.frames == 3 {
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
    let profile = args
        .windows(2)
        .find(|w| w[0] == "--profile-ui")
        .map(|w| PathBuf::from(&w[1]));
    let mut options = eframe::NativeOptions {
        renderer: eframe::Renderer::Wgpu,
        viewport: egui::ViewportBuilder::default()
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
