pub mod backup;
pub mod catalog;
pub mod categories;
pub mod console_config;
pub mod disc;
pub mod drive_cleanup;
pub mod eject;
pub mod fsutil;
pub mod library;
pub mod media;
pub mod model;
pub mod readiness;
pub mod recovery;
pub mod save_archive;
pub mod save_catalog;
pub mod save_cheats;
pub mod save_editor;
pub mod saves;
pub mod self_update;
pub mod setup;
pub mod support_report;
pub mod title_removal;
pub mod updates;
pub mod usb;
mod usb_selection;
use anyhow::{bail, Result};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
#[derive(Clone, Debug, serde::Serialize)]
pub struct Progress {
    pub completed: u64,
    pub total: u64,
    pub phase: String,
}
#[derive(Clone)]
pub struct Job {
    pub cancel: Arc<AtomicBool>,
    pub log: Arc<dyn Fn(String) + Send + Sync>,
    pub progress: Arc<dyn Fn(Progress) + Send + Sync>,
    pub media_sources:
        Arc<std::sync::Mutex<std::collections::HashMap<String, Vec<std::path::PathBuf>>>>,
}
impl Default for Job {
    fn default() -> Self {
        Self {
            cancel: Arc::new(AtomicBool::new(false)),
            log: Arc::new(|_| {}),
            progress: Arc::new(|_| {}),
            media_sources: Default::default(),
        }
    }
}
impl Job {
    pub fn check(&self) -> Result<()> {
        if self.cancel.load(Ordering::Relaxed) {
            bail!("Operation canceled")
        }
        Ok(())
    }
    pub fn report(&self, completed: u64, total: u64, phase: impl Into<String>) {
        (self.progress)(Progress {
            completed: if total == 0 { 0 } else { completed.min(total) },
            total,
            phase: phase.into(),
        });
    }
    pub fn phase(&self, phase: impl Into<String>) {
        self.report(0, 0, phase);
    }
    /// Map a nested operation (bytes, files or stages) to a parent's work range.
    pub fn progress_range(
        &self,
        base: u64,
        span: u64,
        total: u64,
        phase: impl Into<String>,
    ) -> Self {
        let mut child = self.clone();
        let progress = self.progress.clone();
        let phase = phase.into();
        child.progress = Arc::new(move |p| {
            let offset = if p.total == 0 {
                0
            } else {
                (span as u128 * p.completed.min(p.total) as u128 / p.total as u128) as u64
            };
            progress(Progress {
                completed: base.saturating_add(offset).min(total),
                total,
                phase: phase.clone(),
            });
        });
        child
    }
    pub fn quiet_progress(&self) -> Self {
        let mut child = self.clone();
        child.progress = Arc::new(|_| {});
        child
    }
    pub fn say(&self, s: impl Into<String>) {
        (self.log)(s.into())
    }
}
pub fn cache_dir() -> std::path::PathBuf {
    dirs::cache_dir()
        .unwrap_or_else(std::env::temp_dir)
        .join("oh-my-3do")
}

pub mod drive_identity;
pub mod game_info;
pub mod slot_notes;

pub mod drive_browser;
