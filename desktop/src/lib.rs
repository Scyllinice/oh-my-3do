pub mod backup;
pub mod save_catalog;
pub mod catalog;
pub mod console_config;
pub mod disc;
pub mod fsutil;
pub mod library;
pub mod media;
pub mod model;
pub mod saves;
pub mod save_archive;
pub mod save_editor;
pub mod save_cheats;
pub mod setup;
pub mod usb;
mod usb_selection;
pub mod updates;
pub mod self_update;
use anyhow::{bail, Result};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
#[derive(Clone)]
pub struct Job {
    pub cancel: Arc<AtomicBool>,
    pub log: Arc<dyn Fn(String) + Send + Sync>,
    pub media_sources:
        Arc<std::sync::Mutex<std::collections::HashMap<String, Vec<std::path::PathBuf>>>>,
}
impl Default for Job {
    fn default() -> Self {
        Self {
            cancel: Arc::new(AtomicBool::new(false)),
            log: Arc::new(|_| {}),
            media_sources: Default::default(),
        }
    }
}
impl Job {
    pub fn check(&self) -> Result<()> {
        if self.cancel.load(Ordering::Relaxed) {
            bail!("Operation cancelled")
        }
        Ok(())
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
