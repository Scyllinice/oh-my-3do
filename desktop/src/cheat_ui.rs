use eframe::egui::{self, RichText};
use oh_my_3do_library::{
    cache_dir,
    save_cheats::{self, Database},
};
use std::{
    fs,
    path::{Path, PathBuf},
    sync::mpsc,
};
pub struct CheatDatabase {
    open: bool,
    db: Database,
    baseline: Vec<u8>,
    path: Option<PathBuf>,
    original: Option<Vec<u8>>,
    target: String,
    selected: usize,
    message: String,
    discard: bool,
    pending_open: Option<PathBuf>,
    incoming: Option<Database>,
    receiver: Option<mpsc::Receiver<Result<Database, String>>>,
}
impl Default for CheatDatabase {
    fn default() -> Self {
        let db = Database::bundled();
        let baseline = db.json().unwrap();
        Self {
            open: false,
            db,
            baseline,
            path: None,
            original: None,
            target: String::new(),
            selected: 0,
            message: String::new(),
            discard: false,
            pending_open: None,
            incoming: None,
            receiver: None,
        }
    }
}
impl CheatDatabase {
    pub fn button(&mut self, ui: &mut egui::Ui, root: &str) {
        if ui.button("Edit / update cheat database…").clicked() {
            self.open = true;
            if self.target.is_empty() {
                self.target = root.into();
            }
        }
    }
    fn dirty(&self) -> bool {
        serde_json::to_vec_pretty(&self.db).ok().as_deref() != Some(self.baseline.as_slice())
    }
    fn result<T>(&mut self, r: anyhow::Result<T>, message: &str) {
        self.message = match r {
            Ok(_) => message.into(),
            Err(e) => format!("{e:#}"),
        };
    }
    fn load(&mut self, path: &Path) -> anyhow::Result<()> {
        let db = Database::read(path)?;
        self.original = Some(fs::read(path)?);
        self.baseline = db.json()?;
        self.db = db;
        self.path = Some(path.to_owned());
        self.selected = 0;
        Ok(())
    }
    pub fn open_file(&mut self, path: &Path) -> anyhow::Result<()> {
        self.load(path)?;
        self.open = true;
        Ok(())
    }
    fn request_open(&mut self, path: PathBuf) {
        if self.dirty() {
            self.pending_open = Some(path);
            self.discard = true;
        } else {
            let r = self.load(&path);
            self.result(r, "Database opened. Install changes to update the console.");
        }
    }
    fn open_dialog(&mut self) {
        if let Some(path) = rfd::FileDialog::new()
            .add_filter("Cheat database", &["json"])
            .pick_file()
        {
            self.request_open(path);
        }
    }
    fn save(&mut self, as_new: bool) {
        let path = if as_new || self.path.is_none() {
            rfd::FileDialog::new()
                .add_filter("Cheat database", &["json"])
                .set_file_name("save-cheats.json")
                .save_file()
        } else {
            self.path.clone()
        };
        if let Some(path) = path {
            let r = (|| -> anyhow::Result<()> {
                let expected = if self.path.as_ref() == Some(&path) {
                    self.original.clone()
                } else {
                    save_cheats::source_bytes(&path)?
                };
                save_cheats::save_json(&path, &self.db, expected.as_deref(), &cache_dir())?;
                self.original = Some(fs::read(&path)?);
                self.baseline = self.db.json()?;
                self.path = Some(path);
                Ok(())
            })();
            self.result(
                r,
                "Database saved and verified. Install it to update the console.",
            );
        }
    }
    pub fn window(&mut self, ctx: &egui::Context, offline: bool) {
        if let Some(rx) = &self.receiver {
            match rx.try_recv() {
                Ok(result) => {
                    self.receiver = None;
                    match result {
                        Ok(db) => {
                            self.incoming = Some(db);
                            self.message =
                                "Download ready. Review and add the definitions below.".into();
                        }
                        Err(e) => self.message = e,
                    }
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.receiver = None;
                    self.message = "Database download ended unexpectedly.".into();
                }
                _ => {
                    ctx.request_repaint_after(std::time::Duration::from_millis(100));
                }
            }
        }
        if !self.open {
            return;
        }
        let mut visible = true;
        egui::Window::new("Cheat database").open(&mut visible).default_size(egui::vec2(920.,700.)).show(ctx,|ui|{
            ui.label("Edit known save offsets, ranges and presets. Install to a library or USB folder to update the console without replacing boot.iso.");
            ui.label(self.path.as_ref().map(|p|p.display().to_string()).unwrap_or_else(||"Bundled definitions (editable copy)".into()));
            ui.horizontal(|ui|{
                if ui.button("Open JSON…").clicked(){self.open_dialog();}
                if ui.button("Save JSON").clicked(){self.save(false);}
                if ui.button("Save JSON as…").clicked(){self.save(true);}
                if ui.button("Validate").clicked(){let r=self.db.validate();self.result(r,"Database is valid.");}
                if ui.button("Use in Save Manager").clicked(){let r=self.db.validate().map(|_|save_cheats::activate(Some(self.db.clone())));self.result(r,"Save Manager now uses these definitions.");}
            });
            if self.dirty(){ui.small("Unsaved database changes.");}
            ui.horizontal(|ui|{
                if ui.add_enabled(!offline && self.receiver.is_none(),egui::Button::new("Download official definitions")).clicked(){let(tx,rx)=mpsc::channel();self.receiver=Some(rx);std::thread::spawn(move||{let _=tx.send(save_cheats::download().map_err(|e|format!("{e:#}")));});}
                if ui.button("Add missing bundled definitions").clicked(){let r=save_cheats::merge_missing(&self.db,&Database::bundled()).map(|db|self.db=db);self.result(r,"Added missing profiles; existing definitions preserved.");}
            });
            if let Some(upstream)=&self.incoming{
                ui.label(format!("Downloaded {} profiles. Adding missing profiles preserves your existing IDs and values.",upstream.profiles.len()));
                let mut action=0;
                ui.horizontal(|ui|{if ui.button("Add missing profiles").clicked(){action=1;}if ui.button("Replace matching IDs and add new profiles").clicked(){action=2;}if ui.button("Dismiss download").clicked(){action=3;}});
                if action!=0{let upstream=self.incoming.take().unwrap();if action==1{let r=save_cheats::merge_missing(&self.db,&upstream).map(|db|self.db=db);self.result(r,"Missing profiles added.");}else if action==2{let mut candidate=self.db.clone();for p in upstream.profiles{if let Some(old)=candidate.profiles.iter_mut().find(|old|old.id==p.id){*old=p;}else{candidate.profiles.push(p);}}let r=candidate.validate().map(|_|self.db=candidate);self.result(r,"Downloaded profiles merged. Review and save or install your changes.");}}
            }
            ui.separator();
            ui.horizontal(|ui|{
                ui.label("Library / USB root");ui.text_edit_singleline(&mut self.target);
                if ui.button("Browse…").clicked(){if let Some(p)=rfd::FileDialog::new().pick_folder(){self.target=p.display().to_string();}}
                if ui.button("Load installed database").clicked(){let p=PathBuf::from(&self.target).join(save_cheats::JSON_PATH);self.request_open(p);}
            });
            if ui.button("Install database to selected folder").clicked(){let r=save_cheats::install(Path::new(&self.target),&self.db,&cache_dir());self.result(r,"Database source and console file installed and verified. Start the console again to load them.");}
            if !self.message.is_empty(){ui.label(RichText::new(&self.message).color(crate::theme::LIME));}
            ui.separator();
            egui::ScrollArea::vertical().max_height(420.).show(ui,|ui|self.form(ui));
        });
        if !visible {
            if self.dirty() {
                self.discard = true;
            } else {
                self.open = false;
            }
        }
        if self.discard {
            egui::Modal::new(egui::Id::new("unsaved_cheat_database")).show(ctx, |ui| {
                ui.label("Save your database changes before continuing?");
                if ui.button("Save and continue").clicked() {
                    self.save(false);
                    if !self.dirty() {
                        self.continue_open();
                    }
                }
                if ui.button("Discard and continue").clicked() {
                    self.continue_open();
                }
                if ui.button("Cancel").clicked() {
                    self.discard = false;
                    self.pending_open = None;
                }
            });
        }
    }
    fn continue_open(&mut self) {
        self.discard = false;
        if let Some(path) = self.pending_open.take() {
            let r = self.load(&path);
            self.result(r, "Database opened.");
        } else {
            if let Ok(db) = Database::parse(&self.baseline) {
                self.db = db;
            }
            self.open = false;
        }
    }
    fn form(&mut self, ui: &mut egui::Ui) {
        self.selected = self.selected.min(self.db.profiles.len() - 1);
        egui::ComboBox::from_id_salt("cheat_profile")
            .selected_text(&self.db.profiles[self.selected].game)
            .show_ui(ui, |ui| {
                for (i, p) in self.db.profiles.iter().enumerate() {
                    ui.selectable_value(&mut self.selected, i, &p.game);
                }
            });
        ui.horizontal(|ui| {
            if ui.button("Add game profile").clicked() {
                let mut p = Database::bundled().profiles.remove(0);
                p.id = format!("custom-{}", uuid::Uuid::new_v4());
                p.game = "New game".into();
                p.filename = "NewSave".into();
                p.source.clear();
                p.notes = "Describe the verified layout and supported variants.".into();
                self.db.profiles.push(p);
                self.selected = self.db.profiles.len() - 1;
            }
            if ui
                .add_enabled(
                    self.db.profiles.len() > 1,
                    egui::Button::new("Remove profile"),
                )
                .clicked()
            {
                self.db.profiles.remove(self.selected);
                self.selected = 0;
            }
        });
        let p = &mut self.db.profiles[self.selected];
        ui.horizontal(|ui| {
            ui.label("Game");
            ui.add(egui::TextEdit::singleline(&mut p.game).desired_width(200.));
            ui.label("Save filename");
            ui.add(egui::TextEdit::singleline(&mut p.filename).desired_width(200.));
        });
        ui.collapsing("Profile details and format source", |ui| {
            egui::Grid::new("cheat_metadata")
                .spacing(egui::vec2(10., 4.))
                .show(ui, |ui| {
                    for (label, value) in [
                        ("Profile ID", &mut p.id),
                        ("Format source URL", &mut p.source),
                        ("Notes", &mut p.notes),
                    ] {
                        ui.label(label);
                        ui.add(egui::TextEdit::singleline(value).desired_width(580.).min_size(egui::vec2(580.,0.)));
                        ui.end_row();
                    }
                });
        });
        ui.collapsing("Layout and checksum", |ui| {
            ui.label(
                "Supported: aligned 32-bit big-endian fields; sum of preceding words plus seed.",
            );
            egui::Grid::new("cheat_layout").show(ui, |ui| {
                for (label, value) in [
                    ("Save size", &mut p.size),
                    ("Signature offset", &mut p.signature.offset),
                    ("Checksum offset", &mut p.checksum.offset),
                ] {
                    ui.label(label);
                    ui.add(egui::DragValue::new(value).range(0..=32768));
                    ui.end_row();
                }
                ui.label("Signature value");
                ui.add(egui::DragValue::new(&mut p.signature.value).hexadecimal(8, false, true));
                ui.end_row();
                ui.label("Checksum seed");
                ui.add(egui::DragValue::new(&mut p.checksum.seed));
                ui.end_row();
            });
        });
        ui.label(
            RichText::new("Fields: offsets are hexadecimal; values are decimal")
                .strong()
                .color(crate::theme::TEXT),
        );
        let mut remove = None;
        egui::Grid::new("cheat_fields")
            .spacing(egui::vec2(10., 4.))
            .show(ui, |ui| {
                ui.label("Name");
                ui.label("Offset");
                ui.label("Minimum");
                ui.label("Maximum");
                ui.end_row();
                for (i, f) in p.fields.iter_mut().enumerate() {
                    ui.add(egui::TextEdit::singleline(&mut f.label).desired_width(210.).min_size(egui::vec2(210.,0.)));
                    ui.add(egui::DragValue::new(&mut f.offset).hexadecimal(4, false, true));
                    ui.add(egui::DragValue::new(&mut f.min));
                    ui.add(egui::DragValue::new(&mut f.max));
                    if ui.small_button("Remove").clicked() {
                        remove = Some(i);
                    }
                    ui.end_row();
                }
            });
        if let Some(i) = remove {
            p.fields.remove(i);
            p.presets.retain(|preset| preset.field != i);
            for preset in &mut p.presets {
                if preset.field > i {
                    preset.field -= 1;
                }
            }
        }
        if ui.button("Add field").clicked() {
            p.fields.push(save_cheats::Field {
                label: "New field".into(),
                offset: 4,
                min: 0,
                max: 255,
            });
        }
        ui.label(RichText::new("Presets").strong().color(crate::theme::TEXT));
        let mut remove = None;
        egui::Grid::new("cheat_presets").show(ui, |ui| {
            for (i, preset) in p.presets.iter_mut().enumerate() {
                ui.add(egui::TextEdit::singleline(&mut preset.label).desired_width(250.).min_size(egui::vec2(250.,0.)));
                egui::ComboBox::from_id_salt(("preset_field", i))
                    .selected_text(
                        p.fields
                            .get(preset.field)
                            .map(|f| f.label.as_str())
                            .unwrap_or("Select field"),
                    )
                    .show_ui(ui, |ui| {
                        for (j, f) in p.fields.iter().enumerate() {
                            ui.selectable_value(&mut preset.field, j, &f.label);
                        }
                    });
                ui.add(egui::DragValue::new(&mut preset.value));
                if ui.small_button("Remove").clicked() {
                    remove = Some(i);
                }
                ui.end_row();
            }
        });
        if let Some(i) = remove {
            p.presets.remove(i);
        }
        if ui.button("Add preset").clicked() {
            p.presets.push(save_cheats::Preset {
                label: "New preset".into(),
                field: 0,
                value: p.fields.first().map(|f| f.min).unwrap_or(0),
            });
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn file_switching_preserves_edits_until_discard_and_failed_open_keeps_them() {
        let dir = tempfile::tempdir().unwrap();
        let first = dir.path().join("first.json");
        let second = dir.path().join("second.json");
        let original = Database::bundled();
        let mut other = original.clone();
        other.profiles[0].game = "Other definitions".into();
        fs::write(&first, original.json().unwrap()).unwrap();
        fs::write(&second, other.json().unwrap()).unwrap();
        let mut editor = CheatDatabase::default();
        editor.open_file(&first).unwrap();
        editor.db.profiles[0].presets[0].value = 7;
        editor.request_open(second.clone());
        assert!(editor.discard && editor.dirty());
        assert_eq!(editor.db.profiles[0].presets[0].value, 7);
        editor.discard = false;
        editor.pending_open = None;
        assert!(editor.dirty());
        editor.request_open(second);
        editor.continue_open();
        assert!(!editor.dirty());
        assert_eq!(editor.db.profiles[0].game, "Other definitions");
        assert_eq!(fs::read(&first).unwrap(), original.json().unwrap());
        editor.db.profiles[0].presets[0].value = 8;
        editor.request_open(dir.path().join("missing.json"));
        editor.continue_open();
        assert!(editor.dirty());
        assert_eq!(editor.db.profiles[0].presets[0].value, 8);
    }
    #[test]
    fn editor_renders_incomplete_records_without_mutating_installed_files() {
        let ctx = egui::Context::default();
        let mut editor = CheatDatabase::default();
        editor.open = true;
        let _ = ctx.run(egui::RawInput::default(), |ctx| editor.window(ctx, true));
        editor.db.profiles[0].fields.clear();
        editor.db.profiles[0].presets[0].field = 99;
        let _ = ctx.run(egui::RawInput::default(), |ctx| editor.window(ctx, true));
        assert!(editor.db.validate().is_err());
    }
}
