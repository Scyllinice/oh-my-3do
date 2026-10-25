//! The Oh My 3DO desktop app's engine: everything it does with disc images,
//! the PC library and USB / SD drives, without any user interface. The
//! desktop shell (`desktop/shell`) calls into it and shows the results.
//!
//! The main pieces:
//!
//! - [`library`]: the PC library of disc images, matched against the
//!   catalog of known games ([`catalog`], [`model`], [`disc`]).
//! - [`media`] and [`game_info`]: covers, video previews and descriptions.
//! - [`usb`]: preparing a drive for the console menu and copying games to it;
//!   [`readiness`], [`drive_browser`] and [`drive_cleanup`] inspect and
//!   clean drives.
//! - [`saves`], [`save_archive`], [`save_catalog`], [`save_cheats`] and
//!   [`backup`]: the console's save backups.
//! - [`setup`], [`updates`] and [`self_update`]: installing the console menu
//!   and keeping the app and menu up to date.
//!
//! Long operations take a [`Job`], which carries cancellation, a log and
//! progress reports back to the shell.

pub mod backup;
pub mod catalog;
pub mod categories;
pub mod console_config;
pub mod disc;
pub mod drive_browser;
pub mod drive_cleanup;
pub mod drive_identity;
pub mod eject;
pub mod fsutil;
pub mod game_info;
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
pub mod slot_notes;
pub mod support_report;
pub mod title_removal;
pub mod updates;
pub mod usb;
mod usb_selection;

use anyhow::{bail, Result};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex,
};

/// How far along a job is, as the shell's progress bar shows it.
#[derive(Clone, Debug, serde::Serialize)]
pub struct Progress {
    pub completed: u64,
    /// 0 when the amount of work is unknown (the bar then just shows `phase`).
    pub total: u64,
    /// What is happening now, like "Copying games".
    pub phase: String,
}

/// A long-running operation's link back to the shell. Cheap to clone: the
/// clones share the same cancel flag, log and progress callbacks.
#[derive(Clone)]
pub struct Job {
    /// Set by the shell when the person cancels; see [`Job::check`].
    pub cancel: Arc<AtomicBool>,
    /// Receives lines for the activity log.
    pub log: Arc<dyn Fn(String) + Send + Sync>,
    /// Receives progress reports.
    pub progress: Arc<dyn Fn(Progress) + Send + Sync>,
    /// Media files found in each EmuMovies folder, so a job that prepares
    /// many games lists each folder only once (see `media`).
    pub media_sources: Arc<Mutex<HashMap<String, Vec<PathBuf>>>>,
}

impl Default for Job {
    /// A job that cannot be canceled and reports to nobody (for tests and
    /// tools).
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
    /// Returns an error if the job has been canceled. Long loops call this
    /// between steps, so canceling stops them at a safe point.
    pub fn check(&self) -> Result<()> {
        if self.cancel.load(Ordering::Relaxed) {
            bail!("Operation canceled")
        }
        Ok(())
    }

    /// Reports `completed` out of `total` (clamped to `total`).
    pub fn report(&self, completed: u64, total: u64, phase: impl Into<String>) {
        (self.progress)(Progress {
            completed: if total == 0 { 0 } else { completed.min(total) },
            total,
            phase: phase.into(),
        });
    }

    /// Reports a new phase whose amount of work is unknown.
    pub fn phase(&self, phase: impl Into<String>) {
        self.report(0, 0, phase);
    }

    /// A job for one step of a larger operation: the step's progress (in
    /// whatever units it counts) is mapped onto `base..base + span` of the
    /// parent's `total`, under the parent's `phase`.
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

    /// The same job, but with its progress reports dropped (for a step whose
    /// own progress would confuse the parent's).
    pub fn quiet_progress(&self) -> Self {
        let mut child = self.clone();
        child.progress = Arc::new(|_| {});
        child
    }

    /// Adds a line to the activity log.
    pub fn say(&self, s: impl Into<String>) {
        (self.log)(s.into())
    }
}

/// Where the app keeps downloads and other files it can fetch again.
pub fn cache_dir() -> PathBuf {
    dirs::cache_dir()
        .unwrap_or_else(std::env::temp_dir)
        .join("oh-my-3do")
}
