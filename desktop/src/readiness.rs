//! The drive readiness check: a read-only inspection of a prepared USB/SD
//! drive that reports whether the console menu will work from it.
//!
//! Each check (boot image, settings, categories, game files, search indexes,
//! folder helpers, saves, cheats, covers and previews) adds one line to a
//! [`Report`] with a status ("ok", "warning" or "error"), what was found, and
//! how to fix a problem. The check never repairs or rewrites anything on the
//! drive; the fixes are left to the person.

use crate::{
    categories, console_config, disc, fsutil as f, library, media, save_catalog, save_cheats,
    setup, usb, Job,
};
use anyhow::{ensure, Context, Result};
use serde::Serialize;
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    io::Read,
    path::{Path, PathBuf},
};

/// The largest boot image the check will read.
const MAX_BOOT_ISO_BYTES: usize = 16 * 1024 * 1024;

/// The largest categories file the check will read.
const MAX_CATEGORIES_BYTES: usize = 16 * 1024;

/// The largest metadata index: 192 KiB of entries plus its 32-byte header
/// and footer.
const MAX_METADATA_INDEX_BYTES: usize = 192 * 1024 + 32;

/// The largest cover or preview index: 128 KiB of entries plus its 24-byte
/// header.
const MAX_MEDIA_INDEX_BYTES: usize = 128 * 1024 + 24;

/// The size of a folder navigation helper; the check reads no more.
const MAX_NAVIGATION_HELPER_BYTES: usize = 2048;

/// The largest save catalog entry the check will read.
const MAX_SAVE_CATALOG_NODE_BYTES: usize = 8192;

/// The size of the save catalog root file.
const SAVE_CATALOG_ROOT_BYTES: usize = 28;

/// The largest desktop cheat database the check will read.
const MAX_CHEAT_JSON_BYTES: usize = 1024 * 1024;

/// FAT32 cannot hold a file of 4 GiB or more.
const FAT32_FILE_LIMIT: u64 = 4 * 1024 * 1024 * 1024;

/// The start of a 3DO (Opera) volume header: record type 1, five 0x5A sync
/// bytes, and volume version 1.
const OPERA_VOLUME_START: [u8; 7] = [1, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 1];

/// Where the volume header keeps its block size and block count.
const OPERA_BLOCK_SIZE_OFFSET: usize = 76;
const OPERA_BLOCK_COUNT_OFFSET: usize = 80;

/// The block size of a 3DO disc image.
const OPERA_BLOCK_SIZE: u64 = 2048;

/// One line of the report.
#[derive(Debug, Serialize)]
pub struct Check {
    /// What was checked, such as "Boot ISO".
    pub name: String,
    /// "ok", "warning" or "error".
    pub status: String,
    /// What the check found.
    pub detail: String,
    /// How to fix a problem; empty when the status is "ok".
    pub fix: String,
}

/// The result of checking a drive.
#[derive(Debug, Serialize)]
pub struct Report {
    /// The drive's root folder.
    pub root: PathBuf,
    /// True when no check found an error (warnings are allowed).
    pub ready: bool,
    pub errors: usize,
    pub warnings: usize,
    pub checks: Vec<Check>,
}

impl Report {
    /// Adds a line to the report and counts it if it is an error or warning.
    /// The fix is only kept for lines that are not "ok".
    fn add(&mut self, name: &str, status: &str, detail: String, fix: &str) {
        if status == "error" {
            self.errors += 1;
        }
        if status == "warning" {
            self.warnings += 1;
        }

        let fix = if status == "ok" {
            String::new()
        } else {
            fix.to_owned()
        };

        self.checks.push(Check {
            name: name.into(),
            status: status.into(),
            detail,
            fix,
        });
    }

    /// Adds a line from a check's result: "ok" with the check's message if it
    /// passed, or "error" with the reason if it failed.
    fn add_result(&mut self, name: &str, result: Result<String>, fix: &str) {
        match result {
            Ok(detail) => self.add(name, "ok", detail, fix),
            Err(error) => self.add(name, "error", format!("{error:#}"), fix),
        }
    }
}

/// Reads a whole file from the drive, refusing one that is missing or larger
/// than `limit` bytes. Reads at most one byte past the limit, so a huge file
/// is never loaded.
fn read(root: &Path, name: &str, limit: usize) -> Result<Vec<u8>> {
    let path = f::safe(root, name)?;
    let file = fs::File::open(&path).with_context(|| format!("Missing or unreadable: {name}"))?;

    let mut data = Vec::new();
    file.take(limit as u64 + 1).read_to_end(&mut data)?;
    ensure!(data.len() <= limit, "Oversized file: {name}");

    Ok(data)
}

/// Reads a big-endian 32-bit number at `offset` in `data`.
fn read_u32(data: &[u8], offset: usize) -> u32 {
    let bytes: [u8; 4] = data[offset..offset + 4].try_into().unwrap();
    u32::from_be_bytes(bytes)
}

/// Inspects the drive at `root` and reports what is wrong with it, if
/// anything. Fails only if the drive cannot be opened or the job is
/// canceled; every other problem becomes a line in the report.
pub fn check(root: &Path, job: &Job) -> Result<Report> {
    let root = f::root(root)?;
    let mut report = Report {
        root: root.clone(),
        ready: false,
        errors: 0,
        warnings: 0,
        checks: vec![],
    };

    // The menu image and the drive-wide settings files.
    job.check()?;
    job.say("Checking menu and drive settings");
    report.add_result(
        "Boot ISO",
        check_boot_iso(&root),
        "Install a menu image from Drive setup.",
    );
    let settings = console_config::load(&root);
    report.add_result(
        "Console settings",
        check_console_settings(&settings),
        "Repair installed support or save console settings.",
    );
    report.add_result(
        "Categories",
        check_categories(&root),
        "Repair installed support or open Manage categories.",
    );

    // The games themselves and the indexes the menu uses to find them.
    // `games` maps each disc's case-folded path to its path as written; the
    // index and media checks compare against it.
    job.check()?;
    job.say("Checking game paths and search indexes");
    let discs = disc::collect(std::slice::from_ref(&root), None, job);
    let mut games = BTreeMap::new();
    report.add_result(
        "Game files",
        check_game_files(&root, &discs, &mut games, job),
        "Restore missing game/track files or copy the affected games again.",
    );
    if discs.is_ok() {
        report.add_result(
            "Search indexes",
            check_search_indexes(&root, &games),
            "Choose this drive as the library, then rebuild search indexes in Settings & tools.",
        );
    }
    report.add_result(
        "Folder navigation helpers",
        check_navigation_helpers(&root, job),
        "Repair missing helpers in Drive setup. \
         Remove Hidden/System attributes from helpers so the ODE can see them.",
    );

    // Save backups and cheats.
    job.check()?;
    job.say("Checking save support and backup catalog");
    report.add_result(
        "Save support",
        check_save_support(&root, job),
        "Refresh slots in Save Manager or repair installed support. Keep recovery backups.",
    );
    report.add_result(
        "Cheat database",
        check_cheat_database(&root),
        "Install the cheat database from Settings & tools.",
    );

    // Covers and previews. These are optional, so discs without them are
    // only a warning, and only if the console is set to show them.
    for folder in ["covers", "previews"] {
        job.check()?;
        job.say(format!("Checking {folder}"));
        match check_media(&root, folder, &games, job) {
            Err(error) => report.add(
                folder,
                "error",
                format!("{error:#}"),
                "Prepare missing media in the library, then copy the affected games again.",
            ),
            Ok((indexed, missing)) => {
                let shown_on_console = settings.as_ref().is_ok_and(|loaded| {
                    if folder == "covers" {
                        loaded.settings.usb_covers
                    } else {
                        loaded.settings.video_previews
                    }
                });
                let status = if missing > 0 && shown_on_console {
                    "warning"
                } else {
                    "ok"
                };
                report.add(
                    folder,
                    status,
                    format!(
                        "{indexed} indexed disc entries validated; \
                         {missing} without {folder}. Media is optional."
                    ),
                    "Prepare media in Artwork & previews, then copy those games.",
                );
            }
        }
    }

    job.check()?;
    report.ready = report.errors == 0;
    Ok(report)
}

/// Checks that `boot.iso` is a 3DO disc image whose size matches the block
/// count in its volume header (a truncated copy would not boot).
fn check_boot_iso(root: &Path) -> Result<String> {
    let data = read(root, "boot.iso", MAX_BOOT_ISO_BYTES)?;
    ensure!(
        data.len() >= OPERA_BLOCK_COUNT_OFFSET + 4 && data[..7] == OPERA_VOLUME_START,
        "Invalid 3DO volume header"
    );

    let block_size = read_u32(&data, OPERA_BLOCK_SIZE_OFFSET) as u64;
    let block_count = read_u32(&data, OPERA_BLOCK_COUNT_OFFSET) as u64;
    ensure!(
        block_size == OPERA_BLOCK_SIZE && block_count * OPERA_BLOCK_SIZE == data.len() as u64,
        "Boot ISO size does not match its volume header"
    );

    Ok("3DO volume header and image size are valid.".into())
}

/// Checks that the console settings file (`ohmy3do.ini`) exists and loaded
/// without errors.
fn check_console_settings(settings: &Result<console_config::Loaded>) -> Result<String> {
    let loaded = settings
        .as_ref()
        .map_err(|error| anyhow::anyhow!("{error:#}"))?;
    ensure!(loaded.exists(), "ohmy3do.ini is missing");
    Ok("Console settings are valid.".into())
}

/// Checks that the categories file is a sensible size and parses.
fn check_categories(root: &Path) -> Result<String> {
    read(root, categories::PATH, MAX_CATEGORIES_BYTES)?;
    let config = categories::load(root)?;
    Ok(format!(
        "{} configured categories.",
        config.categories.len()
    ))
}

/// Checks every disc found on the drive: each of its files (the disc image
/// and any tracks it refers to) exists, fits on FAT32 and is usable, and no
/// two discs have paths that differ only in letter case (the console would
/// not tell them apart).
///
/// Fills `games` with each disc's case-folded path mapped to its path, as far
/// as the check got; the later checks use it even if this one failed.
fn check_game_files(
    root: &Path,
    discs: &Result<Vec<(PathBuf, Vec<PathBuf>)>>,
    games: &mut BTreeMap<String, String>,
    job: &Job,
) -> Result<String> {
    let discs = discs
        .as_ref()
        .map_err(|error| anyhow::anyhow!("{error:#}"))?;

    for (disc_path, files) in discs {
        for file in files {
            job.check()?;
            ensure!(
                fs::metadata(file)?.len() < FAT32_FILE_LIMIT,
                "File exceeds FAT32 limit: {}",
                file.display()
            );
            disc::issue(file)?;
        }

        let name = library::relative(root, disc_path)?;
        let is_new = games.insert(f::fold(&name), name).is_none();
        ensure!(is_new, "Case-colliding disc paths");
    }

    Ok(format!(
        "{} disc entries; referenced tracks exist. Game contents were not checksummed.",
        games.len()
    ))
}

/// Checks that the find index (`library.3di`) and the metadata index
/// (`metadata.3di`) list exactly the discs on the drive. If they are stale,
/// the menu's search and game details would be wrong.
fn check_search_indexes(root: &Path, games: &BTreeMap<String, String>) -> Result<String> {
    let expected: BTreeSet<String> = games.values().cloned().collect();

    let find_index = usb::find(&f::safe(root, ".ohmy3do/library.3di")?)?;
    let find_paths: BTreeSet<String> = find_index.into_iter().collect();
    ensure!(
        find_paths == expected,
        "Find index differs from the games on this drive"
    );

    let metadata_data = read(root, ".ohmy3do/metadata.3di", MAX_METADATA_INDEX_BYTES)?;
    let metadata_paths: BTreeSet<String> = library::metadata_paths(&metadata_data)?
        .into_iter()
        .collect();
    ensure!(
        metadata_paths == expected,
        "Metadata index differs from the games on this drive"
    );

    Ok("Find and metadata indexes match the drive.".into())
}

/// Checks that every folder that needs a navigation helper (the small file
/// the ODE needs to open a folder) has one with the right contents. On
/// Windows, also checks that no helper is marked Hidden or System, because
/// the ODE skips such files.
fn check_navigation_helpers(root: &Path, job: &Job) -> Result<String> {
    let paths = setup::navigation_helpers(root, job)?;

    for path in &paths {
        job.check()?;
        let data = read(root, path, MAX_NAVIGATION_HELPER_BYTES)?;
        ensure!(
            data == setup::navigation_helper_data(),
            "Invalid navigation helper: {path}"
        );

        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            const FILE_ATTRIBUTE_HIDDEN: u32 = 0x2;
            const FILE_ATTRIBUTE_SYSTEM: u32 = 0x4;
            let attributes = fs::metadata(f::safe(root, path)?)?.file_attributes();
            ensure!(
                attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) == 0,
                "Navigation helper has a hidden/system attribute: {path}"
            );
        }
    }

    Ok(format!("{} required helpers present.", paths.len()))
}

/// Checks the save backups in `.ohmy3do/saves`: every backup is valid, and
/// the slot catalog the console reads (one entry file per backup, linked in
/// order, plus a root file pointing at the last) matches them exactly.
fn check_save_support(root: &Path, job: &Job) -> Result<String> {
    let directory = f::safe(root, ".ohmy3do/saves")?;
    let slots = save_catalog::scan(&directory, job)?;

    let invalid: Vec<String> = slots
        .iter()
        .filter(|slot| !slot.valid)
        .map(|slot| slot.name())
        .collect();
    ensure!(
        invalid.is_empty(),
        "Invalid backups: {}",
        invalid.join(", ")
    );

    // Each catalog entry links back to the previous slot's id (0 for the
    // first), so walk the slots in order and rebuild each entry to compare.
    let mut previous_id = 0;
    for slot in &slots {
        job.check()?;
        let node = format!(".ohmy3do/saves/{}", save_catalog::node_name(slot.id));
        let data = read(root, &node, MAX_SAVE_CATALOG_NODE_BYTES)?;
        ensure!(
            data == slot.node_bytes(previous_id),
            "Stale or invalid save catalog entry: {node}"
        );
        previous_id = slot.id;
    }

    // The root file points at the last slot.
    let root_data = read(
        root,
        ".ohmy3do/saves/slot-catalog.3di",
        SAVE_CATALOG_ROOT_BYTES,
    )?;
    ensure!(
        root_data == save_catalog::root_bytes(previous_id),
        "Stale or invalid save catalog root"
    );

    Ok(format!(
        "{} occupied backups validated; slot catalog matches.",
        slots.len()
    ))
}

/// Checks that the desktop's cheat database (JSON) and the console's copy
/// (binary) both exist and hold the same cheats.
fn check_cheat_database(root: &Path) -> Result<String> {
    let json = read(root, save_cheats::JSON_PATH, MAX_CHEAT_JSON_BYTES)?;
    let database = save_cheats::Database::parse(&json)?;

    let binary = read(root, save_cheats::BINARY_PATH, save_cheats::MAX_BINARY)?;
    ensure!(
        database.matches_binary(&binary)?,
        "Desktop and console cheat databases differ"
    );

    Ok("Desktop and console cheat databases match.".into())
}

/// Checks the cover or preview index (`folder` is "covers" or "previews"):
/// every entry is for a disc on the drive, has the right file type, and its
/// file exists and is valid. Large cover versions are checked when present.
///
/// Returns how many discs have an entry and how many do not.
fn check_media(
    root: &Path,
    folder: &str,
    games: &BTreeMap<String, String>,
    job: &Job,
) -> Result<(usize, usize)> {
    let beside = media_is_beside_discs(root, folder)?;
    let index = if beside {
        library::read_drive_media_index(root, folder)?
    } else {
        library::read_media_index(root, folder)?
    };

    // Discs of one set can share a file, so remember what has been
    // validated already.
    let mut validated = BTreeSet::new();

    for (game, name) in &index {
        job.check()?;

        let extension = Path::new(name)
            .extension()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase();
        let right_type = if folder == "covers" {
            extension == "png" || extension == "3dc"
        } else {
            extension == "3dv"
        };
        ensure!(right_type, "Wrong media type in {folder} index: {name}");

        let disc_path = games
            .get(game)
            .with_context(|| format!("{folder} index refers to a missing game: {game}"))?;
        let relative = media_file_path(folder, beside, disc_path, name);
        let path = f::safe(root, &relative)?;
        if validated.insert(path.clone()) {
            media::validate(&path)
                .with_context(|| format!("Missing or invalid {folder}: {relative}"))?;
        }

        if folder == "covers" {
            let large = path.with_file_name(media::large_name(name));
            if large.exists() && validated.insert(large.clone()) {
                media::validate(&large)?;
            }
        }
    }

    let missing = games.len().saturating_sub(index.len());
    Ok((index.len(), missing))
}

/// Whether the drive keeps this folder's media beside each disc (in the
/// disc's own `.ohmy3do` folder) rather than in the old root folder. The
/// index records this in a flag at bytes 20-24 of its header; a missing or
/// empty index means the old layout.
fn media_is_beside_discs(root: &Path, folder: &str) -> Result<bool> {
    let index = format!(".ohmy3do/{folder}/index.3di");
    let path = f::safe(root, &index)?;
    if !path.is_file() || fs::metadata(&path)?.len() == 0 {
        return Ok(false);
    }

    let data = read(root, &index, MAX_MEDIA_INDEX_BYTES)?;
    ensure!(data.len() >= 24, "Truncated {folder} index");
    Ok(read_u32(&data, 20) == 1)
}

/// Where a media file named in the index lives on the drive: in the
/// `.ohmy3do` folder beside its disc, or in the old root
/// `.ohmy3do/<folder>/` folder.
fn media_file_path(folder: &str, beside: bool, disc_path: &str, name: &str) -> String {
    if !beside {
        return format!(".ohmy3do/{folder}/{name}");
    }

    let disc_folder = disc_path.rsplit_once('/').map_or("", |(parent, _)| parent);
    if disc_folder.is_empty() {
        format!(".ohmy3do/{name}")
    } else {
        format!("{disc_folder}/.ohmy3do/{name}")
    }
}
