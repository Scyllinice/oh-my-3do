use eframe::egui::{self, RichText};
use oh_my_3do_library::{
    updates::{self, BootState, Check, Release},
    Job,
};
use std::{path::PathBuf, sync::mpsc};
pub enum Action {
    Desktop {
        release: Release,
        platform: String,
        path: PathBuf,
    },
    Boot {
        release: Release,
        target: BootState,
        digest: String,
    },
    BootDownload {
        release: Release,
        digest: String,
        path: PathBuf,
    },
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
struct Pending { desktop: bool, boot: bool }
impl Pending {
    fn for_report(report: &Check) -> Self {
        Self {
            desktop: updates::is_newer(&report.release.version, env!("CARGO_PKG_VERSION")).unwrap_or(false),
            boot: report.targets.iter().any(|t| t.digest.as_deref() != Some(&report.boot_digest)),
        }
    }
    fn any(self) -> bool { self.desktop || self.boot }
    fn title(self) -> &'static str {
        match (self.desktop, self.boot) {
            (true, true) => "Desktop and boot image updates",
            (true, false) => "Desktop app update",
            (false, true) => "Boot image update",
            _ => "Updates",
        }
    }
}
#[derive(Default)]
pub struct Updates {
    report: Option<Check>,
    receiver: Option<mpsc::Receiver<Result<Check, String>>>,
    attempted: bool,
    pub visible: bool,
    status: String,
}
impl Updates {
    pub fn checking(&self) -> bool {
        self.receiver.is_some()
    }
    pub fn check(&mut self, ctx: &egui::Context, roots: Vec<PathBuf>) {
        if self.receiver.is_some() {
            return;
        }
        self.attempted = true;
        self.status = "Checking the latest release…".into();
        let (sender, receiver) = mpsc::channel();
        self.receiver = Some(receiver);
        let ctx = ctx.clone();
        std::thread::spawn(move || {
            let result = updates::check(&roots, &Job::default()).map_err(|e| format!("{e:#}"));
            let _ = sender.send(result);
            ctx.request_repaint();
        });
    }
    pub fn tick(&mut self, ctx: &egui::Context, automatic: bool, roots: Vec<PathBuf>) {
        if automatic && !self.attempted {
            self.check(ctx, roots)
        }
        if let Some(receiver) = &self.receiver {
            if let Ok(result) = receiver.try_recv() {
                self.receiver = None;
                match result {
                    Ok(report) => {
                        let pending = Pending::for_report(&report);
                        self.visible = pending.any();
                        self.status = if pending.any() {
                            format!("{} available: {}", pending.title(), report.release.tag)
                        } else {
                            format!("Everything checked is up to date. Latest release: {}", report.release.tag)
                        };
                        self.report = Some(report);
                    }
                    Err(e) => {
                        self.status = format!("Could not check for updates: {e}");
                        self.report = None;
                    }
                }
            } else {
                ctx.request_repaint_after(std::time::Duration::from_millis(100));
            }
        }
    }
    pub fn installed_boot(&mut self, root: &std::path::Path) {
        if let Some(report) = &mut self.report {
            for target in &mut report.targets {
                if target.root == root {
                    target.digest = Some(report.boot_digest.clone())
                }
            }
        }
        if let Some(report) = &self.report {
            self.visible &= Pending::for_report(report).any();
        }
        self.status = "boot.iso updated and verified. Cold boot the console to use it.".into();
    }
    pub fn controls(&mut self, ui: &mut egui::Ui, offline: bool, roots: Vec<PathBuf>) {
        ui.horizontal(|ui| {
            if ui
                .add_enabled(
                    !offline && self.receiver.is_none(),
                    egui::Button::new("Check for updates"),
                )
                .clicked()
            {
                self.check(ui.ctx(), roots.clone());
            }
            if self.report.as_ref().is_some_and(|r| Pending::for_report(r).any()) && ui.button("View update details…").clicked() {
                self.visible = true;
            }
            if ui.add_enabled(!offline && !self.checking(), egui::Button::new("Check another library / USB folder…")).clicked() {
                if let Some(root) = rfd::FileDialog::new().pick_folder() {
                    let mut all=roots; all.push(root); self.check(ui.ctx(),all);
                }
            }
        });
        if !self.status.is_empty() {
            ui.label(&self.status);
        }
        if let Some(report) = &self.report {
            for warning in &report.warnings {
                ui.label(RichText::new(warning).color(crate::theme::AMBER));
            }
        }
        if offline {
            ui.label("Update checks and downloads are disabled while Offline is enabled.");
        }
    }
    pub fn window(
        &mut self,
        ctx: &egui::Context,
        enabled: bool,
        _roots: Vec<PathBuf>,
    ) -> Option<Action> {
        let report = self.report.clone()?;
        let pending = Pending::for_report(&report);
        if !self.visible || !pending.any() { return None; }
        let mut visible = self.visible;
        let mut action = None;
        let release = &report.release;
        egui::Window::new(pending.title())
            .id(egui::Id::new("updates_downloads"))
            .open(&mut visible)
            .default_size(egui::vec2(760., 500.))
            .default_pos(egui::pos2(200., 70.))
            .show(ctx, |ui| {
                egui::ScrollArea::vertical().id_salt("updates_window")
                    .max_height(ui.available_height().min(600.)).show(ui, |ui| {
                    ui.hyperlink_to(format!("Release notes for {}",release.tag),release.page());
                    if pending.desktop {
                        ui.label(RichText::new("Desktop app update available").strong().color(crate::theme::TEXT));
                        ui.label(format!("Installed: v{} → Available: {}",env!("CARGO_PKG_VERSION"),release.tag));
                        if let Some(platform) = updates::platform() {
                            match release.desktop_name(platform) {
                                Ok(name) => {
                                    if ui.add_enabled(enabled,egui::Button::new(format!("Download {} desktop app…",release.tag))).clicked() {
                                        if let Some(path) = rfd::FileDialog::new().set_file_name(name).add_filter("Desktop ZIP", &["zip"]).save_file() {
                                            action = Some(Action::Desktop {release:release.clone(),platform:platform.into(),path});
                                        }
                                    }
                                }
                                Err(_) => { ui.label("This release has no desktop package for this platform. See the release page."); }
                            }
                        } else { ui.label("See the release page for available desktop packages."); }
                        ui.label("After downloading, close this app and extract the ZIP to replace the portable app. Library files and preferences are kept separately.");
                    }
                    if pending.desktop && pending.boot { ui.separator(); }
                    if pending.boot {
                        ui.label(RichText::new("Console boot.iso update").strong().color(crate::theme::TEXT));
                        if ui.add_enabled(enabled,egui::Button::new(format!("Download {} boot.iso…",release.tag))).clicked() {
                            if let Some(path) = rfd::FileDialog::new().set_file_name("boot.iso").add_filter("Boot image", &["iso"]).save_file() {
                                action = Some(Action::BootDownload {release:release.clone(),digest:report.boot_digest.clone(),path});
                            }
                        }
                        for target in report.targets.iter().filter(|t|t.digest.as_deref()!=Some(&report.boot_digest)) {
                            ui.group(|ui| {
                                ui.label(target.root.display().to_string());
                                ui.label(if target.digest.is_some() {"boot.iso differs from the latest released image."} else {"No boot.iso is installed here."});
                                if ui.add_enabled(enabled,egui::Button::new(format!("Install {} boot.iso here",release.tag))).clicked() {
                                    action = Some(Action::Boot {release:release.clone(),target:target.clone(),digest:report.boot_digest.clone()});
                                }
                            });
                        }
                        ui.label("Installing the menu creates missing support files and the saves folder, preserves existing settings, media, indexes and saves, and creates an undo journal. Disconnect the prepared drive from the PC, reinstall it with the console powered off, then cold boot.");
                    }
                    ui.separator();
                    ui.label("Downloads are verified against the release's published SHA-256 checksums.");
                });
            });
        self.visible = visible;
        action
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn completed_check(newer: bool, digest: Option<&str>) -> Updates {
        let current=env!("CARGO_PKG_VERSION");
        let major=current.split('.').next().unwrap().parse::<u64>().unwrap();
        let latest=if newer {format!("{}.0.0",major+1)} else {current.to_owned()};
        let release=Release::parse(&serde_json::to_vec(&serde_json::json!({
            "tag_name":format!("v{latest}"),"draft":false,"prerelease":false,"body":"","assets":[]
        })).unwrap()).unwrap();
        let report=Check {release,boot_digest:"published".into(),targets:vec![BootState {
            root:PathBuf::from("test-library"),digest:digest.map(str::to_owned)
        }],warnings:vec![]};
        let (sender,receiver)=mpsc::channel();sender.send(Ok(report)).unwrap();
        let mut state=Updates {receiver:Some(receiver),attempted:true,..Default::default()};
        state.tick(&egui::Context::default(),false,vec![]);
        state
    }
    fn rendered_text(state: &mut Updates) -> String {
        fn collect(shape: &egui::epaint::Shape, text: &mut String) {
            match shape {
                egui::epaint::Shape::Text(t) => {text.push_str(&t.galley.job.text);text.push('\n');}
                egui::epaint::Shape::Vec(v) => for s in v {collect(s,text)},
                _ => (),
            }
        }
        let ctx=egui::Context::default();
        let mut text=String::new();
        for _ in 0..3 {
            let output=ctx.run(egui::RawInput::default(),|ctx| {state.window(ctx,false,vec![]);});
            text.clear();
            for shape in &output.shapes {collect(&shape.shape,&mut text);}
        }
        text
    }
    #[test]
    fn no_window_when_everything_matches() {
        let state=completed_check(false,Some("published"));
        assert!(!state.visible);assert!(state.status.contains("up to date"));
    }
    #[test]
    fn boot_only_offer_contains_no_desktop_information() {
        for digest in [None,Some("local-build")] {
            let mut state=completed_check(false,digest);
            assert!(state.visible);
            let text=rendered_text(&mut state);
            assert!(text.contains("Console boot.iso update"),"{text}");
            assert!(!text.contains("Desktop") && !text.contains("desktop") && !text.contains("portable app"),"{text}");
        }
    }
    #[test]
    fn desktop_only_offer_contains_no_boot_information() {
        let mut state=completed_check(true,Some("published"));
        assert!(state.visible);
        let text=rendered_text(&mut state);
        assert!(text.contains("Desktop app update available"),"{text}");
        assert!(!text.contains("boot.iso") && !text.contains("console powered"),"{text}");
    }
    #[test]
    fn combined_offer_shows_both_then_removes_completed_boot_update() {
        let mut state=completed_check(true,Some("local-build"));
        let text=rendered_text(&mut state);
        assert!(text.contains("Desktop app update available") && text.contains("Console boot.iso update"),"{text}");
        state.installed_boot(std::path::Path::new("test-library"));
        assert!(state.visible);
        let text=rendered_text(&mut state);
        assert!(text.contains("Desktop app update available") && !text.contains("boot.iso"),"{text}");
        let mut boot_only=completed_check(false,Some("local-build"));
        boot_only.installed_boot(std::path::Path::new("test-library"));assert!(!boot_only.visible);
    }
}
