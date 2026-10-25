//! Permanently removing selected games from a library or drive.
//!
//! Removal happens in two steps:
//!
//! 1. [`review`] works out everything that would be deleted: each selected
//!    disc's files (image, CUE sheet, tracks), any other CUE sheet that
//!    points only at those files, covers and previews no remaining game
//!    uses, the discs' descriptions, and folder navigation helpers that
//!    would be left with no game under them. It also takes a fingerprint of
//!    the drive's game files and index files. The person sees this plan
//!    before agreeing.
//! 2. [`apply`] reviews again and refuses if anything changed. It then moves
//!    the files into a holding folder and rewrites the library database and
//!    menu indexes, all through a journal so an interruption can be undone.
//!    Only once everything is consistent are the held files deleted, along
//!    with folders left empty.
//!
//! Saves, other games, and anything the app does not recognize are never
//! removed.

use crate::{disc, fsutil as f, game_info, library, Job};
use anyhow::{bail, ensure, Context, Result};
use serde::Serialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    path::{Path, PathBuf},
};

/// The support files whose contents go into a plan's fingerprint (in
/// `.ohmy3do/`), so a change to any of them between review and removal is
/// noticed.
const FINGERPRINTED_FILES: [&str; 8] = [
    "library.json",
    "metadata.3di",
    "categories.json",
    "covers/index.3di",
    "previews/index.3di",
    "favorites.txt",
    "recent.txt",
    "last-position.txt",
];

/// The largest support file the fingerprint will read.
const MAX_FINGERPRINTED_BYTES: u64 = 16 * 1024 * 1024;

/// The size of a folder navigation helper.
const NAVIGATION_HELPER_BYTES: u64 = 2048;

/// What `last-position.txt` is reset to: the menu reopens at the top of
/// the drive instead of at a removed game.
const RESET_POSITION: &[u8] = b"OH3DO_POSITION1\n/Storage\n\n";

/// A file that removal would delete.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct File {
    /// Its path relative to the drive root.
    pub path: String,
    pub bytes: u64,
    /// Its modification time, to notice a change since the review.
    pub modified: u128,
}

/// What removal would do, as shown to the person for review.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Plan {
    pub root: PathBuf,
    /// The selected discs, relative to the root.
    pub discs: Vec<String>,
    /// Every file that would be deleted.
    pub files: Vec<File>,
    /// The total size of `files`.
    pub bytes: u64,
    /// SHA-256 over the drive's game files (paths, sizes, times) and its
    /// index files, so any change since the review is caught.
    pub fingerprint: String,
}

/// What a finished removal did.
#[derive(Serialize)]
pub struct Removed {
    pub discs: usize,
    pub bytes: u64,
    /// Files removed from the library that could not be deleted from the
    /// holding folder.
    pub warnings: Vec<String>,
}

/// Reads the cover or preview index (`folder`) and whether it keeps media
/// beside each disc (true) or in the old root folder. The index records this
/// in a flag at bytes 20-24 of its header; a missing or empty index means
/// the old layout.
fn media_index(root: &Path, folder: &str) -> Result<(BTreeMap<String, String>, bool)> {
    let file = f::safe(root, &format!(".ohmy3do/{folder}/index.3di"))?;
    let beside = if file.exists() && fs::metadata(&file)?.len() > 0 {
        let data = fs::read(&file)?;
        ensure!(data.len() >= 24, "Invalid media index");
        u32::from_be_bytes(data[20..24].try_into()?) == 1
    } else {
        false
    };

    let index = if beside {
        library::read_drive_media_index(root, folder)?
    } else {
        library::read_media_index(root, folder)?
    };
    Ok((index, beside))
}

/// Where a media file named in an index lives: in the `.ohmy3do` folder
/// beside its disc (`game`), or in the old root `.ohmy3do/<folder>/`.
fn media_path(game: &str, folder: &str, name: &str, beside: bool) -> String {
    if !beside {
        return format!(".ohmy3do/{folder}/{name}");
    }
    match game.rsplit_once('/') {
        Some((disc_folder, _)) => format!("{disc_folder}/.ohmy3do/{name}"),
        None => format!(".ohmy3do/{name}"),
    }
}

/// The case-folded paths of every disc in the library database.
fn record_paths(database: &Value) -> BTreeSet<String> {
    database["games"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|record| record["path"].as_str().map(f::fold))
        .collect()
}

/// Whether a path ends in the given extension, ignoring letter case.
fn has_extension(path: &str, extensions: &[&str]) -> bool {
    Path::new(path).extension().is_some_and(|extension| {
        extensions
            .iter()
            .any(|wanted| extension.eq_ignore_ascii_case(wanted))
    })
}

/// What a scan of the drive's discs found.
struct DiscScan {
    /// Every disc's case-folded path, mapped to its path as written.
    names: BTreeMap<String, String>,
    /// Every game file on the drive, with its size and modification time.
    inventory: BTreeMap<String, (u64, u128)>,
    /// The selected discs that were found.
    found: BTreeSet<String>,
    /// The files that belong to the selected discs.
    selected_files: BTreeSet<String>,
}

/// Finds every disc on the drive, notes which are selected (`selected_keys`
/// holds their case-folded paths), and collects the selected discs' files.
fn scan_discs(root: &Path, selected_keys: &BTreeSet<String>, job: &Job) -> Result<DiscScan> {
    let mut scan = DiscScan {
        names: BTreeMap::new(),
        inventory: BTreeMap::new(),
        found: BTreeSet::new(),
        selected_files: BTreeSet::new(),
    };

    for (disc_path, disc_files) in disc::collect(&[root.to_owned()], None, job)? {
        let path = library::relative(root, &disc_path)?;
        let is_selected = selected_keys.contains(&f::fold(&path));
        scan.names.insert(f::fold(&path), path.clone());
        if is_selected {
            scan.found.insert(path);
        }

        for file in disc_files {
            let name = library::relative(root, &file)?;
            f::safe(root, &name)?;
            scan.inventory.insert(name.clone(), f::stamp(&file)?);
            if is_selected {
                scan.selected_files.insert(name);
            }
        }
    }

    Ok(scan)
}

/// Works out what removing the `selected` discs (paths relative to `root`)
/// would delete. Changes nothing.
pub fn review(root: &Path, selected: &[String], job: &Job) -> Result<Plan> {
    let root = f::root(root)?;
    ensure!(!selected.is_empty(), "Select titles to remove");
    let discs: BTreeSet<String> = selected.iter().cloned().collect();
    ensure!(discs.len() == selected.len(), "Duplicate selection");
    for path in &discs {
        f::safe(&root, path)?;
    }

    // `removed_keys` holds the case-folded path of every disc entry going
    // away. It starts as the selection and gains any redundant CUE sheets.
    let mut removed_keys: BTreeSet<String> = discs.iter().map(|path| f::fold(path)).collect();

    let scan = scan_discs(&root, &removed_keys, job)?;
    ensure!(
        scan.found == discs,
        "Library contents changed or a selected disc is unavailable. \
         Refresh the library and select again."
    );
    let mut files = scan.selected_files;
    let database = library::database(&root)?;

    add_redundant_cue_sheets(&root, &mut removed_keys, &mut files, job)?;
    for folder in ["covers", "previews"] {
        add_unshared_media(
            &root,
            folder,
            &database,
            &scan.names,
            &removed_keys,
            &mut files,
        )?;
    }
    add_descriptions(
        &root,
        &discs,
        &database,
        &scan.names,
        &removed_keys,
        &mut files,
    )?;
    add_unneeded_navigation_helpers(&root, &discs, &scan.names, &removed_keys, &mut files)?;

    let fingerprint = fingerprint(&root, &scan.inventory)?;

    // Record each file's size and time, so a later change is noticed.
    let mut plan_files = vec![];
    for path in files {
        let (bytes, modified) = f::stamp(&f::safe(&root, &path)?)?;
        plan_files.push(File {
            path,
            bytes,
            modified,
        });
    }
    let bytes = plan_files.iter().map(|file| file.bytes).sum();

    Ok(Plan {
        root,
        discs: discs.into_iter().collect(),
        files: plan_files,
        bytes,
        fingerprint,
    })
}

/// Adds any CUE sheet that points only at files being removed (another way
/// of describing a selected ISO's data), so it does not become a broken
/// entry. A CUE sheet that also uses files of a game being kept is refused:
/// the whole set must be removed together. (`disc::collect` already refuses
/// shared or ambiguous CUE sets for the discs themselves.)
fn add_redundant_cue_sheets(
    root: &Path,
    removed_keys: &mut BTreeSet<String>,
    files: &mut BTreeSet<String>,
    job: &Job,
) -> Result<()> {
    // Skip hidden folders such as `.ohmy3do`, but not the root itself.
    let entries = walkdir::WalkDir::new(root)
        .into_iter()
        .filter_entry(|entry| {
            entry.depth() == 0 || !entry.file_name().to_string_lossy().starts_with('.')
        });

    for entry in entries {
        job.check()?;
        let entry = entry?;
        let is_cue = entry.file_type().is_file()
            && entry
                .path()
                .extension()
                .is_some_and(|extension| extension.eq_ignore_ascii_case("cue"));
        if !is_cue {
            continue;
        }

        let (_, referenced) = disc::cue_refs(entry.path())?;
        let is_removed = |file: &PathBuf| {
            library::relative(root, file).is_ok_and(|relative| files.contains(&relative))
        };
        let uses_removed_files = referenced.iter().any(is_removed);
        if referenced.is_empty() || !uses_removed_files {
            continue;
        }

        ensure!(
            referenced.iter().all(is_removed),
            "A CUE shares selected game data; remove the complete disc set together"
        );
        let cue_path = library::relative(root, entry.path())?;
        removed_keys.insert(f::fold(&cue_path));
        files.insert(cue_path);
    }

    Ok(())
}

/// Adds the covers or previews (`folder`) that only removed discs use.
/// Discs of a set can share one image, so a file is kept while any
/// remaining disc still points at it. Large cover versions are included
/// with their covers.
fn add_unshared_media(
    root: &Path,
    folder: &str,
    database: &Value,
    disc_names: &BTreeMap<String, String>,
    removed_keys: &BTreeSet<String>,
    files: &mut BTreeSet<String>,
) -> Result<()> {
    let (mut index, beside) = media_index(root, folder)?;

    // In the old layout, library records can name generated media that the
    // console index does not list.
    if !beside {
        let field = if folder == "covers" {
            "cover"
        } else {
            "preview"
        };
        for record in database["games"].as_array().into_iter().flatten() {
            if let (Some(path), Some(name)) = (record["path"].as_str(), record[field].as_str()) {
                index.entry(f::fold(path)).or_insert(name.into());
            }
        }
    }

    // Sort every media file into "used by a removed disc" and "used by a
    // kept disc".
    let mut used_by_removed = BTreeSet::new();
    let mut used_by_kept = BTreeSet::new();
    for (game, name) in &index {
        let mut names = vec![name.clone()];
        if folder == "covers" && has_extension(name, &["3dc", "png"]) {
            names.push(crate::media::large_name(name));
        }

        let disc_path = disc_names.get(game).unwrap_or(game);
        for name in names {
            let path = media_path(disc_path, folder, &name, beside);
            f::safe(root, &path)?;
            if removed_keys.contains(game) {
                used_by_removed.insert(path);
            } else {
                used_by_kept.insert(path);
            }
        }
    }

    for path in used_by_removed.difference(&used_by_kept) {
        if f::safe(root, path)?.is_file() {
            files.insert(path.clone());
        }
    }
    Ok(())
}

/// Adds each removed disc's description file. A library keeps descriptions
/// at its root and a drive keeps them beside the disc, so both places are
/// checked. Description file names come from a checksum of the disc's
/// path, so one is kept if a remaining disc would use the same name.
fn add_descriptions(
    root: &Path,
    discs: &BTreeSet<String>,
    database: &Value,
    disc_names: &BTreeMap<String, String>,
    removed_keys: &BTreeSet<String>,
    files: &mut BTreeSet<String>,
) -> Result<()> {
    let description_files =
        |path: &str| [game_info::filename(path), game_info::drive_filename(path)];

    let remaining_discs = record_paths(database)
        .into_iter()
        .chain(disc_names.keys().cloned())
        .filter(|path| !removed_keys.contains(path));
    let still_used: BTreeSet<String> = remaining_discs
        .flat_map(|path| description_files(&path))
        .map(|name| f::fold(&name))
        .collect();

    for path in discs {
        for description in description_files(path) {
            if !still_used.contains(&f::fold(&description))
                && f::safe(root, &description)?.is_file()
            {
                files.insert(description);
            }
        }
    }
    Ok(())
}

/// Adds the folder navigation helpers in each removed disc's parent folders
/// that will have no game under them afterwards. Only files that are
/// exactly a helper are removed; a helper an unselected game still needs is
/// always kept.
fn add_unneeded_navigation_helpers(
    root: &Path,
    discs: &BTreeSet<String>,
    disc_names: &BTreeMap<String, String>,
    removed_keys: &BTreeSet<String>,
    files: &mut BTreeSet<String>,
) -> Result<()> {
    for disc_path in discs {
        let mut parent = Path::new(disc_path).parent();
        while let Some(folder) = parent {
            if folder.as_os_str().is_empty() {
                break;
            }

            let prefix = format!("{}/", folder.to_string_lossy());
            let folded_prefix = f::fold(&prefix);
            let kept_game_inside = disc_names
                .keys()
                .any(|path| !removed_keys.contains(path) && path.starts_with(&folded_prefix));

            if !kept_game_inside {
                let helper = format!("{prefix}{}", crate::setup::NAVIGATION_HELPER);
                let file = f::safe(root, &helper)?;
                let is_real_helper = file.is_file()
                    && fs::metadata(&file)?.len() == NAVIGATION_HELPER_BYTES
                    && fs::read(&file)? == crate::setup::navigation_helper_data();
                if is_real_helper {
                    files.insert(helper);
                }
            }

            parent = folder.parent();
        }
    }
    Ok(())
}

/// SHA-256 (in hex) over every game file's path, size and time, and the
/// contents of the [`FINGERPRINTED_FILES`].
fn fingerprint(root: &Path, inventory: &BTreeMap<String, (u64, u128)>) -> Result<String> {
    let mut hash = Sha256::new();
    hash.update(serde_json::to_vec(inventory)?);

    for name in FINGERPRINTED_FILES {
        let file = f::safe(root, &format!(".ohmy3do/{name}"))?;
        hash.update(name);
        if file.exists() {
            ensure!(
                fs::metadata(&file)?.len() <= MAX_FINGERPRINTED_BYTES,
                "Oversized support file"
            );
            hash.update(fs::read(file)?);
        }
    }

    Ok(format!("{:x}", hash.finalize()))
}

/// Carries out a reviewed plan. Refuses if a fresh review differs in any way
/// (files changed, added or removed since). `cache` holds the recovery
/// journal.
pub fn apply(plan: &Plan, cache: &Path, job: &Job) -> Result<Removed> {
    let _lock = library::lock_library(&plan.root, cache)?;
    ensure!(
        &review(&plan.root, &plan.discs, job)? == plan,
        "Files changed since review. Review removal again."
    );

    // Every disc entry going away: the selected discs, plus any redundant
    // ISO or CUE files the review added.
    let removed_disc_files = plan
        .files
        .iter()
        .map(|file| file.path.as_str())
        .filter(|path| has_extension(path, &["iso", "cue"]));
    let removed_keys: BTreeSet<String> = plan
        .discs
        .iter()
        .map(String::as_str)
        .chain(removed_disc_files)
        .map(f::fold)
        .collect();

    let database = database_without_removed(plan, &removed_keys, job)?;

    // Move the files into a holding folder and update everything, through
    // the journal. On any failure, undo it all.
    let holding_folder = format!(".ohmy3do/removing/{}", uuid::Uuid::new_v4());
    let mut journal = library::Journal::new(&plan.root, cache)?;
    let result = remove_through_journal(
        plan,
        &mut journal,
        database,
        &removed_keys,
        &holding_folder,
        job,
    );
    if let Err(error) = result {
        job.phase("Restoring files after interrupted removal");
        job.say("Restoring staged files and library indexes...");
        if let Err(rollback) = journal.undo() {
            bail!(
                "{error}. Rollback failed: {rollback}. Keep recovery journal {}",
                journal.path.display()
            );
        }
        return Err(error);
    }

    // The library is consistent without the files, so delete them for good.
    job.phase("Finishing permanent removal");
    let mut warnings = vec![];
    let file_count = plan.files.len() as u64;
    for index in 0..plan.files.len() {
        let file = f::safe(&plan.root, &format!("{holding_folder}/{index}"))?;
        if let Err(error) = fs::remove_file(&file) {
            warnings.push(format!("Could not reclaim {}: {error}", file.display()));
        }
        job.report(index as u64 + 1, file_count, "Finishing permanent removal");
    }

    remove_empty_folders(plan);
    let _ = fs::remove_dir(plan.root.join(&holding_folder));

    Ok(Removed {
        discs: plan.discs.len(),
        bytes: plan.bytes,
        warnings,
    })
}

/// The library database with the removed discs' records taken out.
///
/// A prepared drive may have only console metadata, or games copied there
/// by hand, so the database may not list every disc. Records for the
/// missing ones are added from the Drive view first, so rebuilding the
/// indexes afterwards keeps the remaining games.
fn database_without_removed(
    plan: &Plan,
    removed_keys: &BTreeSet<String>,
    job: &Job,
) -> Result<Value> {
    let mut database = library::database(&plan.root)?;
    let known = record_paths(&database);

    let drive_view =
        crate::drive_browser::scan(&plan.root, &crate::model::Preferences::default(), job)?;
    let drive_records = drive_view["records"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    for mut record in drive_records {
        let path = record["path"].as_str().context("Missing disc path")?;
        if known.contains(&f::fold(path)) {
            continue;
        }

        // The Drive view gives media as paths; the database keeps file names.
        for field in ["cover", "preview"] {
            if let Some(media_path) = record[field].as_str() {
                let file_name = media_path.rsplit('/').next().unwrap_or(media_path);
                record[field] = json!(file_name);
            }
        }
        database["games"]
            .as_array_mut()
            .context("Invalid records")?
            .push(record);
    }

    database["games"]
        .as_array_mut()
        .context("Invalid library records")?
        .retain(|record| {
            let removed = record["path"]
                .as_str()
                .is_some_and(|path| removed_keys.contains(&f::fold(path)));
            !removed
        });
    if !database["sources"].is_array() {
        database["sources"] = json!([]);
    }

    Ok(database)
}

/// The undoable part of removal, all recorded in `journal`: moves the
/// plan's files into `holding_folder` (numbered in plan order), writes the
/// new database, takes the removed discs out of the media indexes,
/// favorites and recent lists, resets the menu's last position, and
/// rebuilds the menu indexes.
///
/// Finally marks the journal "removal-committed". From then on undoing only
/// the metadata would create broken game entries, so recovery finishes the
/// removal instead.
fn remove_through_journal(
    plan: &Plan,
    journal: &mut library::Journal,
    database: Value,
    removed_keys: &BTreeSet<String>,
    holding_folder: &str,
    job: &Job,
) -> Result<()> {
    let file_count = plan.files.len() as u64;
    for (index, file) in plan.files.iter().enumerate() {
        job.check()?;
        job.report(index as u64, file_count, "Removing selected title files");
        journal.rename(&file.path, &format!("{holding_folder}/{index}"))?;
    }

    job.phase("Updating library metadata");
    library::write_database(journal, database, vec![], &[])?;

    for folder in ["covers", "previews"] {
        let (mut index, beside) = media_index(&plan.root, folder)?;
        index.retain(|game, _| !removed_keys.contains(game));
        let mut bytes = library::media_index(&index)?;
        if beside {
            bytes = library::beside_index(&bytes)?;
        }
        journal.write(&format!(".ohmy3do/{folder}/index.3di"), &bytes)?;
    }

    for name in ["favorites.txt", "recent.txt"] {
        let path = format!(".ohmy3do/{name}");
        let file = f::safe(&plan.root, &path)?;
        if file.is_file() {
            let kept = list_without_removed(&fs::read(file)?, removed_keys);
            journal.write(&path, kept.as_bytes())?;
        }
    }

    let position = ".ohmy3do/last-position.txt";
    if f::safe(&plan.root, position)?.is_file() {
        journal.write(position, RESET_POSITION)?;
    }

    job.phase("Updating menu indexes");
    library::build_indexes_after_removal(&plan.root, journal, job)?;
    job.check()?;

    // Persist the irreversible boundary before deleting the held files.
    journal.state = "removal-committed".into();
    if let Err(error) = journal.save() {
        journal.state = "applying".into();
        return Err(error);
    }
    Ok(())
}

/// A favorites or recent list (one disc path per line) without the removed
/// discs. The menu may leave zero padding at the end, which is dropped, as
/// is a byte-order mark at the start of a line.
fn list_without_removed(raw: &[u8], removed_keys: &BTreeSet<String>) -> String {
    let before_padding = raw.split(|byte| *byte == 0).next().unwrap_or_default();
    let text = String::from_utf8_lossy(before_padding);

    let mut kept = String::new();
    for line in text.lines() {
        let path = line.trim().trim_start_matches('\u{feff}');
        if !removed_keys.contains(&f::fold(path)) {
            kept.push_str(line);
            kept.push('\n');
        }
    }
    kept
}

/// Removes folders the deleted files leave empty, deepest first. Only empty
/// folders are removed: saves, other games and the person's own files in
/// the same folders are never swept up.
fn remove_empty_folders(plan: &Plan) {
    let mut folders = BTreeSet::new();
    for file in &plan.files {
        let mut parent = Path::new(&file.path).parent();
        while let Some(folder) = parent {
            if folder.as_os_str().is_empty() {
                break;
            }
            folders.insert(folder.to_owned());
            parent = folder.parent();
        }
    }

    // Reverse sorted order visits a folder's subfolders before the folder.
    // `remove_dir` fails harmlessly on a folder that is not empty.
    for folder in folders.into_iter().rev() {
        let _ = fs::remove_dir(plan.root.join(folder));
    }
}
