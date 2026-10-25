//! Cleaning up recovery copies. Before the app replaces a save backup file,
//! it keeps the old one as "NAME.3ds.<random id>.bak" (see
//! `save_archive::save_bytes`). These pile up; this module lists them and
//! removes the ones the person picks. It never touches the backups
//! themselves, and suggests keeping the newest copy (and the newest valid
//! one) of each backup.

use crate::{fsutil as f, save_archive::Archive, Job};
use anyhow::{ensure, Context, Result};
use serde::Serialize;
use std::{
    collections::HashSet,
    fs,
    path::{Path, PathBuf},
    time::UNIX_EPOCH,
};
/// One recovery copy.
#[derive(Clone, Debug, Serialize)]
pub struct Copy {
    pub name: String,
    /// The backup file it is a copy of.
    pub original: String,
    pub bytes: u64,
    /// When it was made, in seconds since 1970.
    pub modified: u64,
    /// Whether it is a readable backup.
    pub valid: bool,
    /// The newest copy of its backup.
    pub newest: bool,
    /// Suggested for keeping: the newest copy, or the newest valid one.
    pub keep: bool,
    /// Its size and time, and its SHA-256, when listed: removal refuses a
    /// copy that changed since.
    #[serde(skip)]
    stamp: (u64, u128),
    #[serde(skip)]
    hash: String,
}

/// The recovery copies in a folder, newest first.
#[derive(Clone, Debug, Serialize)]
pub struct Inventory {
    pub directory: PathBuf,
    pub bytes: u64,
    pub copies: Vec<Copy>,
}
/// For a recovery copy's name, the name of the backup it copies; None for
/// any other file.
fn original(name: &str) -> Option<&str> {
    let without_bak = name.strip_suffix(".bak")?;
    let (original, id) = without_bak.rsplit_once('.')?;
    if !original.to_ascii_lowercase().ends_with(".3ds") || id.len() != 36 {
        return None;
    }
    let parsed = uuid::Uuid::parse_str(id).ok()?;
    if parsed.get_version_num() != 4 || parsed.hyphenated().to_string() != id {
        return None;
    }
    Some(original)
}
/// Lists the recovery copies in `directory`.
pub fn scan(directory: &Path, job: &Job) -> Result<Inventory> {
    let directory = f::root(directory)?;
    let mut copies = vec![];
    let mut bytes = 0u64;
    for entry in fs::read_dir(&directory)? {
        job.check()?;
        let entry = entry?;
        if !entry.file_type()?.is_file() || f::is_link(&entry.path()) {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        let Some(source) = original(&name) else {
            continue;
        };
        let path = f::safe(&directory, &name)?;
        let stamp = f::stamp(&path)?;
        // App-generated archive backups cannot exceed the archive size limit.
        if stamp.0 > crate::save_archive::MAX_BYTES as u64 {
            continue;
        }
        let hash = f::sha256(&path, job)?;
        let valid = Archive::read(&path).is_ok();
        ensure!(
            f::stamp(&path)? == stamp,
            "Recovery copy changed during scan; refresh again"
        );
        bytes = bytes.saturating_add(stamp.0);
        let modified = fs::metadata(path)?
            .modified()?
            .duration_since(UNIX_EPOCH)?
            .as_secs();
        copies.push(Copy {
            name: name.clone(),
            original: source.into(),
            bytes: stamp.0,
            modified,
            valid,
            newest: false,
            keep: false,
            stamp,
            hash,
        });
    }

    // Newest first; mark the newest (and newest valid) copy of each backup.
    copies.sort_by(|a, b| b.stamp.1.cmp(&a.stamp.1).then(a.name.cmp(&b.name)));
    let mut seen = HashSet::new();
    let mut valid_seen = HashSet::new();
    for copy in &mut copies {
        let key = f::fold(&copy.original);
        copy.newest = seen.insert(key.clone());
        let newest_valid = copy.valid && valid_seen.insert(key);
        copy.keep = copy.newest || newest_valid;
    }
    Ok(Inventory {
        directory,
        bytes,
        copies,
    })
}
/// What a cleanup removed, and why it stopped early if it did.
#[derive(Debug, Serialize)]
pub struct Removed {
    pub names: Vec<String>,
    pub bytes: u64,
    pub errors: Vec<String>,
}
/// Removes the `selected` copies from a listing the person reviewed. Every
/// one is checked against the listing before anything is deleted; if a
/// deletion then fails (or the job is canceled), it stops and reports
/// exactly which files were removed.
pub fn remove(review: &Inventory, selected: &[String], job: &Job) -> Result<Removed> {
    ensure!(
        f::root(&review.directory)? == review.directory,
        "Recovery directory changed"
    );
    let mut seen = HashSet::new();
    let mut files = vec![];
    for name in selected {
        job.check()?;
        ensure!(seen.insert(name), "Duplicate recovery selection");
        let copy = review
            .copies
            .iter()
            .find(|c| c.name == *name)
            .context("Recovery selection changed; refresh again")?;
        ensure!(
            original(name).is_some(),
            "Not an app-created archive recovery copy"
        );
        let path = f::safe(&review.directory, name)?;
        ensure!(
            path.is_file() && f::stamp(&path)? == copy.stamp && f::sha256(&path, job)? == copy.hash,
            "Recovery copy changed; refresh before deleting: {name}"
        );
        files.push((path, copy));
    }
    job.check()?;
    let mut result = Removed {
        names: vec![],
        bytes: 0,
        errors: vec![],
    };
    for (path, copy) in files {
        if job.cancel.load(std::sync::atomic::Ordering::Relaxed) {
            result
                .errors
                .push("Cleanup canceled; remaining copies were kept.".into());
            break;
        }
        match fs::remove_file(path) {
            Ok(()) => {
                result.names.push(copy.name.clone());
                result.bytes += copy.bytes;
                job.say(format!("Removed recovery copy: {}", copy.name));
            }
            Err(e) => {
                result.errors.push(format!("{}: {e}", copy.name));
                break;
            }
        }
    }
    Ok(result)
}
