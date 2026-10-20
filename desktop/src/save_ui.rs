use eframe::egui::{self, RichText};
use oh_my_3do_library::{
    backup::{self, Labels, State},
    save_archive::{self, Archive},
};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
};
#[derive(Default)]
pub struct SaveBrowser {
    open: bool,
    catalog_directory: Option<PathBuf>,
    catalog_pending: Option<PathBuf>,
    catalog_query: String,
    catalog_slots: Vec<oh_my_3do_library::save_catalog::Slot>,
    catalog_worker:
        Option<std::thread::JoinHandle<anyhow::Result<Vec<oh_my_3do_library::save_catalog::Slot>>>>,
    catalog_status: String,
    catalog_visible: bool,
    has_document: bool,
    pending_new: bool,
    incoming: Option<(PathBuf, Archive, Vec<bool>)>,
    labels: Labels,
    labels_path: Option<PathBuf>,
    labels_original: Option<Vec<u8>>,
    label_slot: Option<usize>,
    label_edit: String,
    comparison: Option<(PathBuf, Archive)>,
    difference_selected: usize,
    difference_page: usize,
    path: Option<PathBuf>,
    original: Archive,
    original_bytes: Vec<u8>,
    pending_open: Option<PathBuf>,
    message_error: bool,
    archive: Archive,
    selected: usize,
    page: usize,
    hex: String,
    pending: bool,
    show_hex: bool,
    message: String,
}
impl SaveBrowser {
    pub fn open_file(&mut self, path: &Path) -> anyhow::Result<()> {
        let path = path.canonicalize()?;
        let (archive, bytes) = Archive::read_with_bytes(&path)?;
        let database_error = match oh_my_3do_library::save_cheats::for_backup(&path) {
            Ok(db) => {
                oh_my_3do_library::save_cheats::activate(db);
                None
            }
            Err(error) => {
                oh_my_3do_library::save_cheats::activate(None);
                Some(format!(
                    "Cheat database invalid; bundled definitions are in use: {error:#}"
                ))
            }
        };
        self.original_bytes = bytes;
        self.pending_open = None;
        self.pending_new = false;
        self.incoming = None;
        self.has_document = true;
        self.original = archive.clone();
        self.archive = archive;
        self.path = Some(path.to_owned());
        self.comparison = None;
        self.read_label();
        self.selected = 0;
        self.page = 0;
        self.refresh();
        self.message_error = database_error.is_some();
        self.message = database_error.unwrap_or_default();
        self.open = true;
        Ok(())
    }
    pub fn show_button(&mut self, ui: &mut egui::Ui) {
        if ui.button("Save Manager…").clicked() {
            self.open = true;
        }
        ui.label("Inspect .3ds backups, export individual saves, edit bytes or use a supported game editor.");
    }
    fn refresh(&mut self) {
        self.selected = self
            .selected
            .min(self.archive.entries.len().saturating_sub(1));
        let bytes = self
            .archive
            .entries
            .get(self.selected)
            .map(|e| e.data.as_slice())
            .unwrap_or_default();
        self.page = self.page.min(bytes.len().saturating_sub(1) / 256);
        self.hex = bytes
            .iter()
            .skip(self.page * 256)
            .take(256)
            .enumerate()
            .map(|(i, b)| format!("{b:02X}{}", if i % 16 == 15 { "\n" } else { " " }))
            .collect();
        self.pending = false;
    }
    fn dirty(&self) -> bool {
        self.archive != self.original || self.pending
    }
    fn apply_hex_page(&mut self) -> anyhow::Result<()> {
        if !self.pending {
            return Ok(());
        }
        let bytes = save_archive::decode_hex(&self.hex)?;
        let entry = self
            .archive
            .entries
            .get_mut(self.selected)
            .ok_or_else(|| anyhow::anyhow!("Select a save first"))?;
        let start = self.page * 256;
        let end = (start + 256).min(entry.data.len());
        anyhow::ensure!(
            bytes.len() == end - start,
            "Keep this page at {} bytes; use Import to replace a save of a different size",
            end - start
        );
        entry.data[start..end].copy_from_slice(&bytes);
        self.refresh();
        Ok(())
    }
    fn save_to(&mut self, path: &Path) -> anyhow::Result<()> {
        self.apply_hex_page()?;
        let same_file = self
            .path
            .as_ref()
            .is_some_and(|current| path.canonicalize().ok().as_ref() == Some(current));
        let expected = if same_file {
            Some(self.original_bytes.clone())
        } else {
            save_archive::existing_bytes(path)?
        };
        let saved = self.archive.save(path, expected.as_deref())?;
        self.path = Some(path.canonicalize()?);
        self.original_bytes = saved.bytes;
        self.original = self.archive.clone();
        self.read_label();
        if backup::slot(path).is_some()
            || path
                .file_name()
                .is_some_and(|n| n == "recovery.3ds" || n == "recovery2.3ds")
        {
            self.start_catalog(path.parent().unwrap().to_path_buf());
        }
        self.message_error = false;
        self.message = match saved.backup {
            Some(backup) => format!("Saved and verified. Recovery copy: {}", backup.display()),
            None => format!("Saved and verified: {}", path.display()),
        };
        Ok(())
    }
    fn save_current(&mut self) -> anyhow::Result<()> {
        if let Some(path) = self.path.clone() {
            self.save_to(&path)
        } else {
            let path = rfd::FileDialog::new()
                .set_file_name("slot01.3ds")
                .add_filter("Save backup", &["3ds"])
                .save_file()
                .ok_or_else(|| anyhow::anyhow!("Save cancelled; draft kept"))?;
            self.save_to(&path)
        }
    }
    fn save_as_dialog(&mut self) {
        let mut dialog = rfd::FileDialog::new().add_filter("Save backup", &["3ds"]);
        if let Some(path) = &self.path {
            if let Some(parent) = path.parent() {
                dialog = dialog.set_directory(parent);
            }
            if let Some(name) = path.file_name() {
                dialog = dialog.set_file_name(name.to_string_lossy());
            }
        } else {
            dialog = dialog.set_file_name("slot01.3ds");
        }
        if let Some(path) = dialog.save_file() {
            let result = self.save_to(&path);
            self.report_error(result);
        }
    }
    fn new_backup(&mut self) {
        self.path = None;
        self.archive = Archive::default();
        self.original = Archive::default();
        self.original_bytes.clear();
        self.pending_open = None;
        self.pending_new = false;
        self.incoming = None;
        self.has_document = true;
        self.comparison = None;
        self.selected = 0;
        self.page = 0;
        self.read_label();
        self.refresh();
        self.message = "Empty backup draft. Add saves from other backups, then Save as.".into();
        self.message_error = false;
    }
    fn import_window(&mut self, ctx: &egui::Context) {
        let Some((path, archive, checked)) = &mut self.incoming else {
            return;
        };
        let mut apply = false;
        let mut cancel = false;
        egui::Modal::new(egui::Id::new("copy_saves_from_backup")).show(ctx,|ui|{
            ui.set_max_width(640.);ui.heading("Add saves to this backup");ui.label(path.display().to_string());
            ui.label("Checked saves will add or replace matching filenames. Other destination saves stay intact. Nothing is written until you save the draft.");
            ui.horizontal(|ui|{if ui.button("Select all").clicked(){checked.fill(true);}if ui.button("Clear selection").clicked(){checked.fill(false);}});
            egui::ScrollArea::vertical().max_height(300.).show(ui,|ui|{for (i,e) in archive.entries.iter().enumerate(){
                let exists=self.archive.entries.iter().any(|old|old.name.eq_ignore_ascii_case(&e.name));
                ui.checkbox(&mut checked[i],format!("{} · {} bytes{}",e.label(),e.data.len(),if exists{" · replaces matching save"}else{" · adds save"}));
            }});
            if self.message_error{ui.colored_label(crate::theme::RED,&self.message);}
            ui.horizontal(|ui|{apply=ui.add_enabled(checked.iter().any(|v|*v),egui::Button::new("Add / replace selected saves")).clicked();cancel=ui.button("Cancel").clicked();});
        });
        if cancel {
            self.incoming = None;
        } else if apply {
            let selected = archive
                .entries
                .iter()
                .zip(checked.iter())
                .filter(|(_, yes)| **yes)
                .map(|(e, _)| e.clone())
                .collect::<Vec<_>>();
            match self.archive.merge_entries(&selected) {
                Ok(()) => {
                    self.incoming = None;
                    self.refresh();
                    self.message_error = false;
                    self.message = format!(
                        "Added/replaced {} save(s) in the draft. Save to write the slot.",
                        selected.len()
                    );
                }
                Err(e) => {
                    self.message_error = true;
                    self.message = format!("Cannot merge saves: {e:#}");
                }
            }
        }
    }
    fn read_label(&mut self) {
        self.labels_path = None;
        self.labels_original = None;
        self.label_slot = None;
        self.label_edit.clear();
        let Some((path, slot)) = self.path.as_deref().and_then(backup::slot) else {
            return;
        };
        match Labels::read(&path) {
            Ok((labels, original)) => {
                self.label_edit = labels.0[slot].clone();
                self.labels = labels;
                self.labels_original = original;
                self.labels_path = Some(path);
                self.label_slot = Some(slot);
            }
            Err(e) => {
                self.message_error = true;
                self.message = format!("Slot names unavailable: {e:#}");
            }
        }
    }
    fn save_label(&mut self) -> anyhow::Result<()> {
        let slot = self
            .label_slot
            .ok_or_else(|| anyhow::anyhow!("Open a numbered backup slot first"))?;
        let path = self.labels_path.as_ref().unwrap();
        let mut labels = self.labels.clone();
        labels.0[slot] = self.label_edit.clone();
        labels.save(path, self.labels_original.as_deref())?;
        let (labels, original) = Labels::read(path)?;
        self.labels = labels;
        self.labels_original = original;
        if let Some(path) = self.path.clone() {
            self.start_catalog(path.parent().unwrap().to_path_buf());
        }
        self.message = "Slot name saved and verified; save archive unchanged".into();
        self.message_error = false;
        Ok(())
    }
    pub fn compare_file(&mut self, path: &Path) -> anyhow::Result<()> {
        anyhow::ensure!(
            self.has_document,
            "Open or create a backup to compare first"
        );
        self.apply_hex_page()?;
        self.comparison = Some((path.to_owned(), Archive::read(path)?));
        self.difference_selected = 0;
        self.difference_page = 0;
        Ok(())
    }
    fn backup_tools(&mut self, ui: &mut egui::Ui) {
        if self.label_slot.is_some() {
            ui.horizontal(|ui| {
                ui.label(format!(
                    "{} name",
                    self.path
                        .as_ref()
                        .and_then(|p| p.file_stem())
                        .unwrap()
                        .to_string_lossy()
                ));
                ui.add(
                    egui::TextEdit::singleline(&mut self.label_edit)
                        .char_limit(backup::LABEL_BYTES)
                        .desired_width(280.),
                );
                if ui.button("Save slot name").clicked() {
                    let r = self.save_label();
                    self.report_error(r);
                }
            });
            ui.small("24 printable ASCII characters; blank restores the slot filename. Names are stored separately from save data.");
        }
        ui.horizontal(|ui| {
            if ui.button("Add saves from backup…").clicked() {
                if let Some(path) = rfd::FileDialog::new()
                    .add_filter("Save backup", &["3ds"])
                    .pick_file()
                {
                    let r = (|| -> anyhow::Result<()> {
                        self.apply_hex_page()?;
                        let archive = Archive::read(&path)?;
                        let checked = vec![false; archive.entries.len()];
                        self.incoming = Some((path, archive, checked));
                        Ok(())
                    })();
                    self.report_error(r);
                }
            }
            if ui
                .add_enabled(
                    !self.pending && !self.archive.entries.is_empty() && self.comparison.is_none(),
                    egui::Button::new("Remove selected save"),
                )
                .clicked()
            {
                self.archive.entries.remove(self.selected);
                self.refresh();
                self.message = "Removed from draft; save the backup to write changes.".into();
                self.message_error = false;
            }
            if ui.button("Compare with backup…").clicked() {
                if let Some(path) = rfd::FileDialog::new()
                    .add_filter("Save backup", &["3ds"])
                    .pick_file()
                {
                    let r = self.compare_file(&path);
                    self.report_error(r);
                }
            }
            if self.comparison.is_some() && ui.button("Close comparison").clicked() {
                self.comparison = None;
            }
        });
        let dirty = self.dirty();
        let Some((path, other)) = &self.comparison else {
            return;
        };
        egui::CollapsingHeader::new("Backup comparison (read only)")
            .default_open(true)
            .show(ui, |ui| {
                ui.label(format!(
                    "Before: current open backup{}",
                    if dirty { " (including edits)" } else { "" }
                ));
                ui.label(format!("After: {}", path.display()));
                let diffs = backup::compare(&self.archive, other);
                let summary = [
                    State::Added,
                    State::Removed,
                    State::Changed,
                    State::Unchanged,
                ]
                .map(|state| diffs.iter().filter(|d| d.state == state).count());
                ui.label(format!(
                    "{} added · {} removed · {} changed · {} identical",
                    summary[0], summary[1], summary[2], summary[3]
                ));
                if diffs.is_empty() {
                    ui.label("Both backups are empty.");
                    return;
                }
                self.difference_selected = self.difference_selected.min(diffs.len() - 1);
                egui::ScrollArea::vertical()
                    .id_salt("backup_compare_saves")
                    .max_height(240.)
                    .show(ui, |ui| {
                        egui::Grid::new("backup_diff_table")
                            .striped(true)
                            .show(ui, |ui| {
                                for (i, d) in diffs.iter().enumerate() {
                                    if ui
                                        .selectable_label(
                                            self.difference_selected == i,
                                            RichText::new(d.name(&self.archive, other).label())
                                                .color(if self.difference_selected == i {
                                                    crate::theme::INK
                                                } else {
                                                    crate::theme::TEXT
                                                }),
                                        )
                                        .clicked()
                                    {
                                        self.difference_selected = i;
                                        self.difference_page = 0;
                                    }
                                    ui.label(format!("{:?}", d.state));
                                    ui.label(format!("{} bytes", d.changed_bytes));
                                    ui.label(if d.type_changed {
                                        "File type changed"
                                    } else if d.name_changed {
                                        "Filename case changed"
                                    } else {
                                        ""
                                    });
                                    ui.end_row();
                                }
                            });
                    });
                let d = &diffs[self.difference_selected];
                let bytes = d.byte_changes(&self.archive, other);
                self.difference_page = self.difference_page.min(bytes.len().saturating_sub(1) / 32);
                if bytes.is_empty() {
                    ui.label(if d.type_changed || d.name_changed {
                        "Only filename/type differs."
                    } else {
                        "Save bytes are identical."
                    });
                    return;
                }
                ui.horizontal(|ui| {
                    if ui
                        .add_enabled(
                            self.difference_page > 0,
                            egui::Button::new("Previous differences"),
                        )
                        .clicked()
                    {
                        self.difference_page -= 1;
                    }
                    ui.label(format!(
                        "Page {} / {}",
                        self.difference_page + 1,
                        bytes.len().div_ceil(32)
                    ));
                    if ui
                        .add_enabled(
                            (self.difference_page + 1) * 32 < bytes.len(),
                            egui::Button::new("Next differences"),
                        )
                        .clicked()
                    {
                        self.difference_page += 1;
                    }
                });
                let mut text = String::from("Offset  Before  After\n");
                for &(offset, a, b) in bytes.iter().skip(self.difference_page * 32).take(32) {
                    let hex = |v: Option<u8>| {
                        v.map(|n| format!("{n:02X}")).unwrap_or_else(|| "--".into())
                    };
                    text.push_str(&format!("{offset:04X}      {}     {}\n", hex(a), hex(b)));
                }
                egui::ScrollArea::vertical()
                    .id_salt("backup_compare_bytes")
                    .max_height(200.)
                    .show(ui, |ui| {
                        ui.monospace(text);
                    });
            });
    }
    fn report_error(&mut self, result: anyhow::Result<()>) {
        if let Err(error) = result {
            self.message = format!("{error:#}");
            self.message_error = true;
        }
    }
    fn run(&mut self, result: anyhow::Result<()>) {
        self.message_error = result.is_err();
        self.message = match result {
            Ok(()) => "Done".into(),
            Err(e) => format!("{e:#}"),
        };
    }
    fn request_open(&mut self, path: PathBuf) -> anyhow::Result<()> {
        if self.dirty() {
            self.pending_new = false;
            self.pending_open = Some(path);
            Ok(())
        } else {
            self.open_file(&path)
        }
    }
    fn finish_open(&mut self, save: Option<bool>) -> anyhow::Result<()> {
        let path = self.pending_open.clone();
        if path.is_none() && !self.pending_new {
            return Ok(());
        }
        match save {
            None => {
                self.pending_open = None;
                self.pending_new = false;
                return Ok(());
            }
            Some(true) => self.save_current()?,
            Some(false) => (),
        }
        if let Some(path) = path {
            self.open_file(&path)
        } else {
            self.new_backup();
            Ok(())
        }
    }
    fn confirm_open(&mut self, ctx: &egui::Context) {
        if self.pending_open.is_none() && !self.pending_new {
            return;
        }
        let response = egui::Modal::new(egui::Id::new("save_unsaved_open")).show(ctx, |ui| {
            ui.set_max_width(620.);
            ui.label(
                RichText::new("Save changes before opening another backup?")
                    .strong()
                    .color(crate::theme::TEXT),
            );
            if self.message_error {
                ui.colored_label(crate::theme::RED, &self.message);
            }
            let mut choice = None;
            ui.horizontal(|ui| {
                if ui.button("Save & open").clicked() {
                    choice = Some(Some(true));
                }
                if ui.button("Discard & open").clicked() {
                    choice = Some(Some(false));
                }
                if ui.button("Cancel").clicked() {
                    choice = Some(None);
                }
            });
            choice
        });
        if let Some(choice) = response.inner {
            let result = self.finish_open(choice);
            self.report_error(result);
        } else if response.should_close() {
            self.pending_open = None;
        }
    }
    pub fn window(&mut self, ctx: &egui::Context, directory: &str) {
        if !self.open {
            return;
        }
        let mut visible = self.open;
        egui::Window::new("Save Manager")
            .open(&mut visible)
            .default_size(egui::vec2(840., 720.))
            .min_width(720.)
            .default_pos(egui::pos2(160., 70.))
            .resizable(true)
            .show(ctx, |ui| {
                egui::ScrollArea::vertical()
                    .max_height(ui.available_height().min(680.))
                    .show(ui, |ui| self.contents(ui, directory));
            });
        // Keep unfinished changes available when this window is reopened.
        self.open = visible;
        self.confirm_open(ctx);
        self.import_window(ctx);
    }
    pub fn browse_slots(&mut self, directory: PathBuf) {
        self.open = true;
        self.catalog_visible = true;
        self.start_catalog(directory);
    }
    fn start_catalog(&mut self, directory: PathBuf) {
        if self.catalog_worker.is_some() {
            self.catalog_pending = Some(directory);
            return;
        }
        self.catalog_directory = Some(directory.clone());
        self.catalog_status = "Reading backups and refreshing console search catalog…".into();
        self.catalog_worker = Some(std::thread::spawn(move || {
            oh_my_3do_library::save_catalog::refresh(
                &directory,
                &oh_my_3do_library::cache_dir(),
                &Default::default(),
            )
        }));
    }
    fn catalog_ui(&mut self, ui: &mut egui::Ui, directory: &str) {
        if self
            .catalog_worker
            .as_ref()
            .is_some_and(|w| w.is_finished())
        {
            match self.catalog_worker.take().unwrap().join() {
                Ok(Ok(slots)) => {
                    self.catalog_status = format!(
                        "{} occupied backups indexed. Search game names, filenames or slot labels.",
                        slots.len()
                    );
                    self.catalog_slots = slots;
                }
                Ok(Err(e)) => {
                    self.catalog_slots.clear();
                    self.catalog_status = format!("Catalog refresh failed: {e:#}");
                }
                Err(_) => {
                    self.catalog_slots.clear();
                    self.catalog_status = "Catalog refresh stopped unexpectedly".into();
                }
            }
        }
        if self.catalog_worker.is_none() {
            if let Some(path) = self.catalog_pending.take() {
                self.start_catalog(path)
            }
        }
        ui.horizontal(|ui| {
            if ui
                .add_enabled(
                    self.catalog_worker.is_none(),
                    egui::Button::new("Find saves across slots…"),
                )
                .clicked()
            {
                let start = self
                    .catalog_directory
                    .clone()
                    .or_else(|| {
                        self.path
                            .as_ref()
                            .and_then(|p| p.parent().map(Path::to_path_buf))
                    })
                    .unwrap_or_else(|| Path::new(directory).join(".ohmy3do/saves"));
                if let Some(path) = rfd::FileDialog::new()
                    .set_directory(start)
                    .set_title("Select the saves folder (.ohmy3do/saves)")
                    .pick_folder()
                {
                    self.catalog_visible = true;
                    self.start_catalog(path)
                }
            }
            if self.catalog_directory.is_some()
                && ui
                    .add_enabled(
                        self.catalog_worker.is_none(),
                        egui::Button::new("Refresh slot catalog"),
                    )
                    .clicked()
            {
                self.start_catalog(self.catalog_directory.clone().unwrap())
            }
            if self.catalog_directory.is_some() {
                ui.checkbox(&mut self.catalog_visible, "Show occupied slots");
            }
        });
        if self.catalog_worker.is_some() {
            ui.ctx()
                .request_repaint_after(std::time::Duration::from_millis(100));
        }
        if !self.catalog_status.is_empty() {
            ui.label(&self.catalog_status);
        }
        if !self.catalog_visible {
            return;
        }
        if let Some(path) = &self.catalog_directory {
            ui.small(path.display().to_string());
        }
        ui.horizontal(|ui| {
            ui.label("Find game / slot:");
            ui.text_edit_singleline(&mut self.catalog_query);
            if ui.button("Clear").clicked() {
                self.catalog_query.clear();
            }
        });
        let mut open = None;
        egui::ScrollArea::vertical()
            .id_salt("occupied-save-slots")
            .max_height(180.)
            .show(ui, |ui| {
                let mut matches = 0;
                for slot in &self.catalog_slots {
                    if !slot.matches(&self.catalog_query) {
                        continue;
                    }
                    matches += 1;
                    ui.horizontal(|ui| {
                        if ui
                            .add_enabled(slot.valid, egui::Button::new(slot.name()))
                            .clicked()
                        {
                            open = Some(slot.path.clone())
                        }
                        ui.label(slot.games());
                    });
                }
                if matches == 0 && self.catalog_worker.is_none() {
                    ui.label("No matching occupied slots.");
                }
            });
        if let Some(path) = open {
            let r = self.request_open(path);
            self.report_error(r);
        }
        ui.separator();
    }
    fn contents(&mut self, ui: &mut egui::Ui, directory: &str) {
        self.catalog_ui(ui, directory);
        let dirty = self.dirty();
        ui.horizontal(|ui| {
            if ui.add(egui::Button::new("Open .3ds backup…")).clicked() {
                let start = self
                    .path
                    .as_ref()
                    .and_then(|p| p.parent())
                    .map(Path::to_path_buf)
                    .unwrap_or_else(|| Path::new(directory).join(".ohmy3do/saves"));
                if let Some(path) = rfd::FileDialog::new()
                    .set_directory(start)
                    .add_filter("Oh My 3DO save backup", &["3ds"])
                    .pick_file()
                {
                    let result = self.request_open(path);
                    self.report_error(result);
                }
            }
            if ui.button("New empty backup").clicked() {
                if self.dirty() {
                    self.pending_open = None;
                    self.pending_new = true;
                } else {
                    self.new_backup();
                }
            }
            if self.has_document {
                if ui.add_enabled(dirty, egui::Button::new("Save")).clicked() {
                    let result = self.save_current();
                    self.report_error(result);
                }
                if ui.button("Save as…").clicked() {
                    self.save_as_dialog();
                }
            }
            if dirty && ui.button("Discard edits").clicked() {
                self.archive = self.original.clone();
                self.refresh();
                self.message = "Edits discarded; file unchanged".into();
                self.message_error = false;
            }
            if dirty
                && !self.pending
                && self.archive.entries.get(self.selected).is_some_and(|e| {
                    self.original
                        .entries
                        .iter()
                        .any(|old| old.name.eq_ignore_ascii_case(&e.name))
                })
                && ui.button("Revert selected save").clicked()
            {
                let name = &self.archive.entries[self.selected].name;
                self.archive.entries[self.selected] = self
                    .original
                    .entries
                    .iter()
                    .find(|old| old.name.eq_ignore_ascii_case(name))
                    .unwrap()
                    .clone();
                self.refresh();
            }
        });
        if let Some(path) = &self.path {
            ui.label(path.display().to_string());
        } else if self.has_document {
            ui.label("New backup (unsaved)");
        } else {
            return;
        }
        self.backup_tools(ui);
        if !self.message.is_empty() {
            ui.label(RichText::new(&self.message).color(if self.message_error {
                crate::theme::RED
            } else {
                crate::theme::OSD
            }));
        }
        if self.comparison.is_some() {
            return;
        }
        ui.label(format!(
            "{} saves · {} bytes{}",
            self.archive.entries.len(),
            self.archive.encode().map_or(0, |b| b.len()),
            if dirty { " · edited" } else { "" }
        ));
        let mut selected = self.selected;
        ui.add_enabled_ui(!self.pending, |ui| {
            egui::ComboBox::from_id_salt("save_entry")
                .selected_text(self.archive.entries.get(selected).map_or(
                    "Empty backup".into(),
                    |e| {
                        let raw = e.label();
                        oh_my_3do_library::saves::known_title(&raw)
                            .unwrap_or(&raw)
                            .to_owned()
                    },
                ))
                .show_ui(ui, |ui| {
                    for (i, entry) in self.archive.entries.iter().enumerate() {
                        let raw = entry.label();
                        let title = oh_my_3do_library::saves::known_title(&raw);
                        ui.selectable_value(
                            &mut selected,
                            i,
                            title.map_or(raw.clone(), |t| format!("{t} ({raw})")),
                        );
                    }
                });
        });
        if selected != self.selected {
            self.selected = selected;
            self.page = 0;
            self.refresh();
        }
        if let Some(entry) = self.archive.entries.get(self.selected) {
            ui.label(format!(
                "{} · {} bytes · file type {:08X}",
                entry.label(),
                entry.data.len(),
                entry.file_type
            ));
            let supported = oh_my_3do_library::save_cheats::find(entry).is_some();
            if supported {
                ui.add_enabled_ui(!self.pending, |ui| {
                    ui.horizontal(|ui| {
                        let hex_selected = self.show_hex;
                        ui.selectable_value(
                            &mut self.show_hex,
                            false,
                            RichText::new("Game editor").color(if hex_selected {
                                crate::theme::TEXT
                            } else {
                                crate::theme::INK
                            }),
                        );
                        ui.selectable_value(
                            &mut self.show_hex,
                            true,
                            RichText::new("Hex editor").color(if hex_selected {
                                crate::theme::INK
                            } else {
                                crate::theme::TEXT
                            }),
                        );
                    });
                });
            }
            if let Some(profile) = oh_my_3do_library::save_cheats::find(entry) {
                if !self.show_hex {
                    ui.separator();
                    ui.label(
                        RichText::new(format!("{} known values & cheats", profile.game))
                            .strong()
                            .color(crate::theme::TEXT),
                    );
                    ui.label(if profile.checksum.algorithm=="none" {"Save-file offsets · masked big-endian values · neighboring flags preserved"} else {"Save-file offsets · big-endian values · game checksum updated automatically"});
                    ui.hyperlink_to("Format source", &profile.source);
                    ui.small(&profile.notes);
                    let mut change = None;
                    let mut preset = None;
                    ui.add_enabled_ui(!self.pending, |ui| {
                        for (i, p) in profile.presets.iter().enumerate() {
                            if ui.button(&p.label).clicked() {
                                preset = Some(i);
                            }
                        }
                        egui::ScrollArea::vertical()
                            .id_salt("game_editor_fields")
                            .max_height(250.)
                            .show(ui, |ui| {
                                egui::Grid::new("known_save_values").num_columns(3).show(
                                    ui,
                                    |ui| {
                                        for (i, field) in profile.fields.iter().enumerate() {
                                            ui.label(&field.label);
                                            ui.monospace(format!("0x{:04X}", field.offset));
                                            let mut value = profile.value(entry, i).unwrap();
                                            if ui
                                                .add(
                                                    egui::DragValue::new(&mut value)
                                                        .range(field.min..=field.max),
                                                )
                                                .changed()
                                            {
                                                change = Some((i, value));
                                            }
                                            ui.end_row();
                                        }
                                    },
                                );
                            });
                    });
                    if let Some((field, value)) = change {
                        let result =
                            profile.set(&mut self.archive.entries[self.selected], field, value);
                        self.run(result);
                        self.refresh();
                    } else if let Some(preset) = preset {
                        let result =
                            profile.apply_preset(&mut self.archive.entries[self.selected], preset);
                        self.run(result);
                        self.refresh();
                    }
                }
            } else {
                ui.label("No verified cheat profile matches this save's name, layout, values and checksum. Raw editing is available below.");
            }
            if self.show_hex || !supported {
                ui.separator();
                ui.label(
                    RichText::new("Hex editor")
                        .strong()
                        .color(crate::theme::TEXT),
                );
                let size = self.archive.entries[self.selected].data.len();
                ui.horizontal(|ui| {
                    if ui
                        .add_enabled(
                            !self.pending && self.page > 0,
                            egui::Button::new("Previous 256 bytes"),
                        )
                        .clicked()
                    {
                        self.page -= 1;
                        self.refresh();
                    }
                    ui.label(format!(
                        "Offset {:04X} · page {} / {}",
                        self.page * 256,
                        self.page + 1,
                        size.div_ceil(256).max(1)
                    ));
                    if ui
                        .add_enabled(
                            !self.pending && (self.page + 1) * 256 < size,
                            egui::Button::new("Next 256 bytes"),
                        )
                        .clicked()
                    {
                        self.page += 1;
                        self.refresh();
                    }
                });
                ui.horizontal_top(|ui| {
                    let bytes = &self.archive.entries[self.selected].data;
                    let mut offsets = String::new();
                    let mut ascii = String::new();
                    for (i, row) in bytes.chunks(16).enumerate().skip(self.page * 16).take(16) {
                        offsets.push_str(&format!("{:04X}\n", i * 16));
                        ascii.extend(row.iter().map(|b| {
                            if (32..127).contains(b) {
                                *b as char
                            } else {
                                '.'
                            }
                        }));
                        ascii.push('\n');
                    }
                    ui.monospace(offsets);
                    let font = egui::TextStyle::Monospace.resolve(ui.style());
                    let hex_width = 48. * ui.fonts_mut(|fonts| fonts.glyph_width(&font, '0')) + 8.;
                    self.pending |= ui
                        .add(
                            egui::TextEdit::multiline(&mut self.hex)
                                .font(egui::TextStyle::Monospace)
                                .desired_width(hex_width)
                                .desired_rows(
                                    (size.saturating_sub(self.page * 256).min(256))
                                        .div_ceil(16)
                                        .max(3),
                                ),
                        )
                        .changed();
                    ui.monospace(ascii);
                });
                ui.horizontal(|ui| {
                    if ui
                        .add_enabled(self.pending, egui::Button::new("Apply hex page"))
                        .clicked()
                    {
                        let result = self.apply_hex_page();
                        self.run(result);
                    }
                    if self.pending && ui.button("Discard hex page").clicked() {
                        self.refresh();
                    }
                    if ui
                        .add_enabled(!self.pending, egui::Button::new("Revert selected save"))
                        .clicked()
                    {
                        self.archive.entries[self.selected] =
                            self.original.entries[self.selected].clone();
                        self.refresh();
                    }
                });
                ui.label(RichText::new("Raw edits update backup checksums. Any internal game checksum must also be correct for the game to accept the save.").small());
            }
            ui.add_enabled_ui(!self.pending, |ui| {
                ui.horizontal(|ui| {
                    if ui.button("Export raw save…").clicked() {
                        if let Some(path) =
                            rfd::FileDialog::new().set_file_name("save.bin").save_file()
                        {
                            let result = (|| -> anyhow::Result<()> {
                                let expected = save_archive::existing_bytes(&path)?;
                                let saved = save_archive::save_bytes(
                                    &path,
                                    &self.archive.entries[self.selected].data,
                                    expected.as_deref(),
                                )?;
                                self.message = match saved.backup {
                                    Some(p) => format!("Exported. Recovery copy: {}", p.display()),
                                    None => format!("Exported: {}", path.display()),
                                };
                                self.message_error = false;
                                Ok(())
                            })();
                            self.report_error(result);
                        }
                    }
                    if ui.button("Export single-save backup…").clicked() {
                        if let Some(path) = rfd::FileDialog::new()
                            .set_file_name("save-export.3ds")
                            .add_filter("Save backup", &["3ds"])
                            .save_file()
                        {
                            let result = (|| -> anyhow::Result<()> {
                                let expected = save_archive::existing_bytes(&path)?;
                                let saved = Archive {
                                    entries: vec![self.archive.entries[self.selected].clone()],
                                }
                                .save(&path, expected.as_deref())?;
                                self.message = match saved.backup {
                                    Some(p) => format!("Exported. Recovery copy: {}", p.display()),
                                    None => format!("Exported: {}", path.display()),
                                };
                                self.message_error = false;
                                Ok(())
                            })();
                            self.report_error(result);
                        }
                    }
                    if ui.button("Import replacement bytes…").clicked() {
                        if let Some(path) = rfd::FileDialog::new().pick_file() {
                            let result = (|| -> anyhow::Result<()> {
                                let mut bytes = Vec::new();
                                fs::File::open(path)?.take(65537).read_to_end(&mut bytes)?;
                                let mut candidate = self.archive.clone();
                                candidate.entries[self.selected].data = bytes;
                                candidate.encode()?;
                                self.archive = candidate;
                                Ok(())
                            })();
                            self.run(result);
                            self.refresh();
                        }
                    }
                });
            });
        }
        ui.separator();
        ui.label("Save replaces this backup after keeping a recovery copy beside it. Save as switches to the chosen file. Restore edited .3ds archives through Tools > Save Manager > USB backups on the console (see the saves guide).");
        if self.pending {
            ui.label("Save includes this hex page. Apply or discard it before switching entries or exporting one save.");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn archive(value: u8) -> Archive {
        Archive {
            entries: vec![save_archive::Entry {
                name: b"TestSave".to_vec(),
                file_type: 0x44415441,
                data: vec![value; 4],
            }],
        }
    }
    fn files() -> (tempfile::TempDir, PathBuf, PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let first = dir.path().join("slot01.3ds");
        let second = dir.path().join("slot02.3ds");
        archive(1).save_copy(&first).unwrap();
        archive(2).save_copy(&second).unwrap();
        (dir, first, second)
    }
    #[test]
    fn saving_pending_hex_updates_the_existing_file_and_allows_opening_another() {
        let (_dir, first, second) = files();
        let mut ui = SaveBrowser::default();
        ui.open_file(&first).unwrap();
        ui.hex = "AA BB CC DD".into();
        ui.pending = true;
        ui.save_current().unwrap();
        assert_eq!(
            Archive::read(&first).unwrap().entries[0].data,
            [0xaa, 0xbb, 0xcc, 0xdd]
        );
        assert!(!ui.dirty());
        assert!(!ui.pending);
        ui.request_open(second.clone()).unwrap();
        assert_eq!(ui.path, Some(second.canonicalize().unwrap()));
        assert!(ui.pending_open.is_none());
        assert_eq!(ui.archive, archive(2));
    }
    #[test]
    fn save_as_replaces_chosen_backup_and_switches_the_saved_baseline() {
        let (_dir, first, second) = files();
        let mut ui = SaveBrowser::default();
        ui.open_file(&first).unwrap();
        ui.archive.entries[0].data = vec![7; 4];
        ui.save_to(&second).unwrap();
        assert_eq!(Archive::read(&first).unwrap(), archive(1));
        assert_eq!(Archive::read(&second).unwrap(), archive(7));
        assert_eq!(ui.path, Some(second.canonicalize().unwrap()));
        assert!(!ui.dirty());
        ui.archive.entries[0].data = vec![8; 4];
        ui.save_current().unwrap();
        assert_eq!(Archive::read(&second).unwrap(), archive(8));
        ui.request_open(first.clone()).unwrap();
        assert!(ui.pending_open.is_none());
    }
    #[test]
    fn changing_files_can_save_discard_or_cancel_without_closing_the_app() {
        for choice in [None, Some(false), Some(true)] {
            let (_dir, first, second) = files();
            let mut ui = SaveBrowser::default();
            ui.open_file(&first).unwrap();
            ui.archive.entries[0].data = vec![9; 4];
            ui.request_open(second.clone()).unwrap();
            assert_eq!(ui.pending_open, Some(second.clone()));
            assert_eq!(ui.path, Some(first.canonicalize().unwrap()));
            ui.finish_open(choice).unwrap();
            assert!(ui.pending_open.is_none());
            if choice.is_none() {
                assert_eq!(ui.archive, archive(9));
                assert!(ui.dirty());
                assert_eq!(ui.path, Some(first.canonicalize().unwrap()));
            } else {
                assert_eq!(ui.archive, archive(2));
                assert!(!ui.dirty());
            }
            assert_eq!(
                Archive::read(&first).unwrap(),
                archive(if choice == Some(true) { 9 } else { 1 })
            );
        }
    }
    #[test]
    fn invalid_pending_hex_and_external_file_changes_keep_edits_and_block_save_and_open() {
        let (_dir, first, second) = files();
        let mut ui = SaveBrowser::default();
        ui.open_file(&first).unwrap();
        ui.hex = "not hex".into();
        ui.pending = true;
        ui.request_open(second.clone()).unwrap();
        assert!(ui.finish_open(Some(true)).is_err());
        assert!(ui.pending_open.is_some());
        assert!(ui.pending);
        assert_eq!(Archive::read(&first).unwrap(), archive(1));
        ui.finish_open(None).unwrap();
        ui.hex = "01 02 03 04".into();
        fs::write(&first, archive(3).encode().unwrap()).unwrap();
        assert!(ui.save_current().is_err());
        assert_eq!(Archive::read(&first).unwrap(), archive(3));
        assert!(ui.dirty());
        ui.request_open(second.clone()).unwrap();
        ui.finish_open(Some(false)).unwrap();
        assert_eq!(ui.archive, archive(2));
    }
    #[test]
    fn new_draft_copy_and_comparison_leave_sources_unchanged_and_guard_discard() {
        let (_dir, first, second) = files();
        let first_before = fs::read(&first).unwrap();
        let second_before = fs::read(&second).unwrap();
        let mut ui = SaveBrowser::default();
        ui.new_backup();
        assert!(ui.has_document && ui.path.is_none());
        let source = Archive::read(&first).unwrap();
        ui.archive.merge_entries(&source.entries).unwrap();
        ui.refresh();
        assert!(ui.dirty());
        ui.compare_file(&second).unwrap();
        assert!(ui.comparison.is_some());
        assert_eq!(ui.archive, source);
        ui.pending_new = true;
        ui.finish_open(None).unwrap();
        assert!(ui.dirty());
        ui.pending_new = true;
        ui.finish_open(Some(false)).unwrap();
        assert!(ui.archive.entries.is_empty() && !ui.dirty());
        assert_eq!(fs::read(first).unwrap(), first_before);
        assert_eq!(fs::read(second).unwrap(), second_before);
    }
}
