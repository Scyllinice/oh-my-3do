//! Removes everything Oh My 3DO added to a USB / SD drive, for people who stop
//! using the menu. Games and every other file stay where they are.
//!
//! Like title removal, this is reviewed first and cannot be undone. The save
//! backups the menu keeps in `.ohmy3do/saves` can be copied to the PC before
//! they are removed.

use crate::{fsutil as f, library, setup, Job};
use anyhow::{bail, ensure, Context, Result};
use serde::Serialize;
use std::{
    fs,
    path::{Path, PathBuf},
};

/// Menu images at the drive root, each with its optional checksum file.
const MENU_IMAGES: [&str; 2] = ["boot.iso", "menu-demo.iso"];

/// The largest menu image worth reading to recognize it.
const MENU_IMAGE_LIMIT: u64 = 16 * 1024 * 1024;

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Entry {
    /// Path relative to the drive root, with `/` separators.
    pub path: String,
    pub bytes: u64,
    /// Modification time, so changes after the review are noticed.
    #[serde(skip)]
    pub modified: u128,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Plan {
    pub root: PathBuf,
    /// Every file that will be removed, save backups included.
    pub files: Vec<Entry>,
    /// The save backups among `files`, which can be copied to the PC first.
    pub saves: Vec<Entry>,
    /// `.ohmy3do` folders and the folders inside them, removed once they
    /// are empty. Deepest first.
    pub folders: Vec<String>,
    /// Files with an Oh My 3DO name that belong to something else, so stay.
    pub kept: Vec<String>,
    pub bytes: u64,
}

#[derive(Debug, Serialize)]
pub struct Cleaned {
    pub files: usize,
    /// Where the save backups were copied, when they were.
    pub saves_copied_to: Option<PathBuf>,
    pub warnings: Vec<String>,
}

/// Lists what cleanup would remove from the drive at `root`, changing nothing.
pub fn review(root: &Path, job: &Job) -> Result<Plan> {
    let root = f::root(root)?;
    ensure!(
        !root.join(".ohmy3do/library.json").exists(),
        "This is a PC library, not a console drive. Cleanup only removes menu files from USB / SD drives."
    );

    let mut plan = Plan {
        root: root.clone(),
        files: vec![],
        saves: vec![],
        folders: vec![],
        kept: vec![],
        bytes: 0,
    };

    job.phase("Finding menu files on the drive");
    for name in MENU_IMAGES {
        let image = root.join(name);
        if !image.is_file() {
            continue;
        }
        if is_menu_image(&image)? {
            add_file(&mut plan, &root, &image)?;
            let checksum = root.join(format!("{name}.sha256"));
            if checksum.is_file() {
                add_file(&mut plan, &root, &checksum)?;
            }
        } else {
            plan.kept.push(name.to_owned());
        }
    }

    let walker = walkdir::WalkDir::new(&root)
        .follow_links(false)
        .contents_first(true)
        .into_iter()
        .filter_entry(|entry| entry.depth() == 0 || !is_system_folder(entry.file_name()));
    for entry in walker {
        job.check()?;
        let entry = entry?;
        if entry.depth() == 0 {
            continue;
        }
        let path = entry.path();
        if f::is_link(path) {
            // Links are never followed or removed, but one inside a support
            // folder would keep that folder from being cleaned out.
            if inside_support_folder(&root, path) {
                bail!("Links are unsupported inside Oh My 3DO folders: {}", path.display());
            }
            continue;
        }
        if entry.file_type().is_dir() {
            if is_support_folder(entry.file_name()) || inside_support_folder(&root, path) {
                plan.folders.push(library::relative(&root, path)?);
            }
        } else if inside_support_folder(&root, path) || is_navigation_helper(path)? {
            add_file(&mut plan, &root, path)?;
        }
    }

    plan.files.sort_by(|a, b| a.path.cmp(&b.path));
    plan.saves = plan
        .files
        .iter()
        .filter(|file| file.path.to_ascii_lowercase().starts_with(".ohmy3do/saves/"))
        .cloned()
        .collect();
    plan.bytes = plan.files.iter().map(|file| file.bytes).sum();
    Ok(plan)
}

/// Removes the reviewed files. With `saves_to`, the save backups are first
/// copied into a new folder there and checked; without it they are deleted.
pub fn apply(plan: &Plan, saves_to: Option<&Path>, job: &Job) -> Result<Cleaned> {
    ensure!(
        review(&plan.root, job)? == *plan,
        "The drive changed since the review. Review cleanup again."
    );
    let mut cleaned = Cleaned { files: 0, saves_copied_to: None, warnings: vec![] };

    if let (Some(destination), false) = (saves_to, plan.saves.is_empty()) {
        cleaned.saves_copied_to = Some(copy_saves(plan, destination, job)?);
    }

    let total = plan.files.len() as u64;
    for (done, file) in plan.files.iter().enumerate() {
        job.check()?;
        job.report(done as u64, total, "Removing menu files");
        let path = f::safe(&plan.root, &file.path)?;
        fs::remove_file(&path).with_context(|| {
            format!(
                "Removed {done} of {total} files, then could not remove {}",
                path.display()
            )
        })?;
        cleaned.files += 1;
    }
    job.report(total, total, "Removing menu files");

    for folder in &plan.folders {
        let path = f::safe(&plan.root, folder)?;
        if path.is_dir() {
            if let Err(error) = fs::remove_dir(&path) {
                cleaned.warnings.push(format!("Kept {}: {error}", path.display()));
            }
        }
    }
    Ok(cleaned)
}

/// Copies every save backup into a new "Oh My 3DO saves" folder under `destination`,
/// keeping their folder layout, and checks each copy against the drive.
fn copy_saves(plan: &Plan, destination: &Path, job: &Job) -> Result<PathBuf> {
    let destination = f::root(destination)?;
    ensure!(
        !destination.starts_with(&plan.root),
        "Choose a folder on the PC, not on the drive being cleaned"
    );
    let folder = (1..1000)
        .map(|n| match n {
            1 => destination.join("Oh My 3DO saves"),
            n => destination.join(format!("Oh My 3DO saves {n}")),
        })
        .find(|folder| !folder.exists())
        .context("Too many earlier save folders there; choose another folder")?;
    fs::create_dir(&folder)?;

    job.phase("Copying save backups to the PC");
    let total: u64 = plan.saves.iter().map(|file| file.bytes.max(1) * 2).sum();
    let mut done = 0;
    for save in &plan.saves {
        let source = f::safe(&plan.root, &save.path)?;
        let inside_saves = &save.path[".ohmy3do/saves/".len()..];
        let target = folder.join(inside_saves);
        let step = save.bytes.max(1) * 2;
        // copy_new reads each copy back from the disk and compares it.
        f::copy_new(&source, &target, &job.progress_range(done, step, total, "Copying save backups"), None)?;
        done += step;
    }
    Ok(folder)
}

fn add_file(plan: &mut Plan, root: &Path, path: &Path) -> Result<()> {
    let (bytes, modified) = f::stamp(path)?;
    plan.files.push(Entry { path: library::relative(root, path)?, bytes, modified });
    Ok(())
}

/// A valid 3DO image that names the menu's settings file, which every Oh My
/// 3DO menu does. Another menu's boot.iso is left alone.
fn is_menu_image(path: &Path) -> Result<bool> {
    if fs::metadata(path)?.len() > MENU_IMAGE_LIMIT {
        return Ok(false);
    }
    let data = fs::read(path)?;
    let header_ok = data.len() >= 84 && data[..7] == [1, 90, 90, 90, 90, 90, 1];
    Ok(header_ok && data.windows(11).any(|window| window == b"ohmy3do.ini"))
}

/// The 2 KiB placeholder the app puts in category folders, recognized by name
/// and exact contents.
fn is_navigation_helper(path: &Path) -> Result<bool> {
    let named = path
        .file_name()
        .is_some_and(|name| name.to_string_lossy().eq_ignore_ascii_case(setup::NAVIGATION_HELPER));
    Ok(named && fs::metadata(path)?.len() == 2048 && fs::read(path)? == setup::navigation_helper_data())
}

fn is_support_folder(name: &std::ffi::OsStr) -> bool {
    name.to_string_lossy().eq_ignore_ascii_case(".ohmy3do")
}

/// Whether `path` lies inside any `.ohmy3do` folder on the drive.
fn inside_support_folder(root: &Path, path: &Path) -> bool {
    path.strip_prefix(root)
        .is_ok_and(|relative| relative.parent().is_some_and(|parent| parent.iter().any(is_support_folder)))
}

/// Folders the operating system keeps on removable drives.
fn is_system_folder(name: &std::ffi::OsStr) -> bool {
    let name = name.to_string_lossy().to_ascii_lowercase();
    ["system volume information", "$recycle.bin", ".trashes", ".spotlight-v100", ".fseventsd"].contains(&name.as_str())
}
