//! Reviewed permanent removal of selected discs and their exclusively owned media.
use crate::{disc, fsutil as f, game_info, library, Job};
use anyhow::{ensure, Context, Result};
use serde::Serialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    path::{Path, PathBuf},
};
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct File {
    pub path: String,
    pub bytes: u64,
    pub modified: u128,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Plan {
    pub root: PathBuf,
    pub discs: Vec<String>,
    pub files: Vec<File>,
    pub bytes: u64,
    pub fingerprint: String,
}
#[derive(Serialize)]
pub struct Removed {
    pub discs: usize,
    pub bytes: u64,
    pub warnings: Vec<String>,
}
fn media(root: &Path, folder: &str) -> Result<(BTreeMap<String, String>, bool)> {
    let file = f::safe(root, &format!(".ohmy3do/{folder}/index.3di"))?;
    let beside = if file.exists() && fs::metadata(&file)?.len() > 0 {
        let data = fs::read(&file)?;
        ensure!(data.len() >= 24, "Invalid media index");
        u32::from_be_bytes(data[20..24].try_into()?) == 1
    } else {
        false
    };
    Ok((
        if beside {
            library::read_drive_media_index(root, folder)?
        } else {
            library::read_media_index(root, folder)?
        },
        beside,
    ))
}
fn media_path(game: &str, folder: &str, name: &str, beside: bool) -> String {
    if beside {
        format!(
            "{}.ohmy3do/{name}",
            game.rsplit_once('/')
                .map(|(p, _)| format!("{p}/"))
                .unwrap_or_default()
        )
    } else {
        format!(".ohmy3do/{folder}/{name}")
    }
}
fn record_paths(db: &Value) -> BTreeSet<String> {
    db["games"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|r| r["path"].as_str().map(f::fold))
        .collect()
}
pub fn review(root: &Path, selected: &[String], job: &Job) -> Result<Plan> {
    let root = f::root(root)?;
    ensure!(!selected.is_empty(), "Select titles to remove");
    let discs: BTreeSet<_> = selected.iter().cloned().collect();
    ensure!(discs.len() == selected.len(), "Duplicate selection");
    let mut keys: BTreeSet<_> = discs.iter().map(|p| f::fold(p)).collect();
    for path in &discs {
        f::safe(&root, path)?;
    }
    let sets = disc::collect(&[root.clone()], None, job)?;
    let mut actual_names = BTreeMap::new();
    let mut inventory = BTreeMap::new();
    let mut files = BTreeSet::new();
    let mut found = BTreeSet::new();
    for (disc, refs) in sets {
        let path = library::relative(&root, &disc)?;
        actual_names.insert(f::fold(&path), path.clone());
        let selected = keys.contains(&f::fold(&path));
        if selected {
            found.insert(path.clone());
        }
        for file in refs {
            let name = library::relative(&root, &file)?;
            f::safe(&root, &name)?;
            inventory.insert(name.clone(), f::stamp(&file)?);
            if selected {
                files.insert(name);
            }
        }
    }
    ensure!(found==discs,"Library contents changed or a selected disc is unavailable. Refresh the library and select again.");
    let db = library::database(&root)?;
    // Include redundant CUE representations of selected ISO data so they cannot
    // become broken entries. Shared/ambiguous CUE sets are refused by collect.
    for entry in walkdir::WalkDir::new(&root)
        .into_iter()
        .filter_entry(|e| e.depth() == 0 || !e.file_name().to_string_lossy().starts_with('.'))
    {
        job.check()?;
        let entry = entry?;
        if entry.file_type().is_file()
            && entry
                .path()
                .extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("cue"))
        {
            let (_, refs) = disc::cue_refs(entry.path())?;
            if !refs.is_empty()
                && refs
                    .iter()
                    .any(|p| library::relative(&root, p).is_ok_and(|p| files.contains(&p)))
            {
                ensure!(
                    refs.iter()
                        .all(|p| library::relative(&root, p).is_ok_and(|p| files.contains(&p))),
                    "A CUE shares selected game data; remove the complete disc set together"
                );
                let alias = library::relative(&root, entry.path())?;
                keys.insert(f::fold(&alias));
                files.insert(alias);
            }
        }
    }
    for folder in ["covers", "previews"] {
        let (mut map, beside) = media(&root, folder)?;
        // Library records can have generated media without a console association.
        if !beside {
            for r in db["games"].as_array().into_iter().flatten() {
                if let (Some(path), Some(name)) = (
                    r["path"].as_str(),
                    r[if folder == "covers" {
                        "cover"
                    } else {
                        "preview"
                    }]
                    .as_str(),
                ) {
                    map.entry(f::fold(path)).or_insert(name.into());
                }
            }
        }
        let mut owned = BTreeSet::new();
        let mut kept = BTreeSet::new();
        for (game, name) in &map {
            let mut names = vec![name.clone()];
            if folder == "covers"
                && Path::new(name)
                    .extension()
                    .is_some_and(|e| e.eq_ignore_ascii_case("3dc") || e.eq_ignore_ascii_case("png"))
            {
                names.push(crate::media::large_name(name));
            }
            for name in names {
                let path = media_path(
                    actual_names.get(game).unwrap_or(game),
                    folder,
                    &name,
                    beside,
                );
                f::safe(&root, &path)?;
                if keys.contains(game) {
                    owned.insert(path);
                } else {
                    kept.insert(path);
                }
            }
        }
        for path in owned.difference(&kept) {
            if f::safe(&root, path)?.is_file() {
                files.insert(path.clone());
            }
        }
    }
    // A library keeps descriptions at its root; a drive keeps them beside the disc.
    let info_names = |p: &str| [game_info::filename(p), game_info::drive_filename(p)];
    let remaining: BTreeSet<_> = record_paths(&db)
        .iter()
        .chain(actual_names.keys())
        .filter(|p| !keys.contains(*p))
        .flat_map(|p| info_names(p))
        .map(|n| f::fold(&n))
        .collect();
    for path in &discs {
        for info in info_names(path) {
            if !remaining.contains(&f::fold(&info)) && f::safe(&root, &info)?.is_file() {
                files.insert(info);
            }
        }
    }
    // Remove only recognized navigation placeholders from now-game-free
    // ancestors; never remove a helper needed by an unselected sibling title.
    for disc in &discs {
        let mut parent = Path::new(disc).parent();
        while let Some(folder) = parent {
            if folder.as_os_str().is_empty() {
                break;
            }
            let prefix = format!("{}/", folder.to_string_lossy());
            if !actual_names
                .keys()
                .any(|p| !keys.contains(p) && p.starts_with(&f::fold(&prefix)))
            {
                let helper = format!("{prefix}{}", crate::setup::NAVIGATION_HELPER);
                let file = f::safe(&root, &helper)?;
                if file.is_file()
                    && fs::metadata(&file)?.len() == 2048
                    && fs::read(&file)? == crate::setup::navigation_helper_data()
                {
                    files.insert(helper);
                }
            }
            parent = folder.parent();
        }
    }
    let mut fingerprint = Sha256::new();
    fingerprint.update(serde_json::to_vec(&inventory)?);
    for name in [
        "library.json",
        "metadata.3di",
        "categories.json",
        "covers/index.3di",
        "previews/index.3di",
        "favorites.txt",
        "recent.txt",
        "last-position.txt",
    ] {
        let file = f::safe(&root, &format!(".ohmy3do/{name}"))?;
        fingerprint.update(name);
        if file.exists() {
            ensure!(
                fs::metadata(&file)?.len() <= 16 * 1024 * 1024,
                "Oversized support file"
            );
            fingerprint.update(fs::read(file)?);
        }
    }
    let files = files
        .into_iter()
        .map(|path| -> Result<File> {
            let (bytes, modified) = f::stamp(&f::safe(&root, &path)?)?;
            Ok(File {
                path,
                bytes,
                modified,
            })
        })
        .collect::<Result<Vec<_>>>()?;
    let bytes = files.iter().map(|f| f.bytes).sum();
    Ok(Plan {
        root,
        discs: discs.into_iter().collect(),
        files,
        bytes,
        fingerprint: format!("{:x}", fingerprint.finalize()),
    })
}
pub fn apply(plan: &Plan, cache: &Path, job: &Job) -> Result<Removed> {
    let _lock = library::lock_library(&plan.root, cache)?;
    ensure!(
        &review(&plan.root, &plan.discs, job)? == plan,
        "Files changed since review. Review removal again."
    );
    let keys: BTreeSet<_> = plan
        .discs
        .iter()
        .map(String::as_str)
        .chain(
            plan.files
                .iter()
                .filter(|f| {
                    Path::new(&f.path).extension().is_some_and(|e| {
                        e.eq_ignore_ascii_case("iso") || e.eq_ignore_ascii_case("cue")
                    })
                })
                .map(|f| f.path.as_str()),
        )
        .map(f::fold)
        .collect();
    let mut db = library::database(&plan.root)?;
    // A prepared drive may have only console metadata, or games copied by hand.
    // Seed missing records so rebuilding indexes preserves remaining titles.
    let known = record_paths(&db);
    let browser =
        crate::drive_browser::scan(&plan.root, &crate::model::Preferences::default(), job)?;
    for mut record in browser["records"].as_array().cloned().unwrap_or_default() {
        let path = record["path"].as_str().context("Missing disc path")?;
        if !known.contains(&f::fold(path)) {
            for field in ["cover", "preview"] {
                if let Some(p) = record[field].as_str() {
                    record[field] = json!(p.rsplit('/').next().unwrap_or(p));
                }
            }
            db["games"]
                .as_array_mut()
                .context("Invalid records")?
                .push(record);
        }
    }
    db["games"]
        .as_array_mut()
        .context("Invalid library records")?
        .retain(|r| {
            !r["path"]
                .as_str()
                .is_some_and(|p| keys.contains(&f::fold(p)))
        });
    if !db["sources"].is_array() {
        db["sources"] = json!([]);
    }
    let staging = format!(".ohmy3do/removing/{}", uuid::Uuid::new_v4());
    let mut tx = library::Journal::new(&plan.root, cache)?;
    let result = (|| -> Result<()> {
        for (i, file) in plan.files.iter().enumerate() {
            job.check()?;
            job.report(
                i as u64,
                plan.files.len() as u64,
                "Removing selected title files",
            );
            tx.rename(&file.path, &format!("{staging}/{i}"))?;
        }
        job.phase("Updating library metadata");
        library::write_database(&mut tx, db, vec![], &[])?;
        for folder in ["covers", "previews"] {
            let (mut map, beside) = media(&plan.root, folder)?;
            map.retain(|p, _| !keys.contains(p));
            let bytes = library::media_index(&map)?;
            tx.write(
                &format!(".ohmy3do/{folder}/index.3di"),
                &if beside {
                    library::beside_index(&bytes)?
                } else {
                    bytes
                },
            )?;
        }
        for name in ["favorites.txt", "recent.txt"] {
            let path = format!(".ohmy3do/{name}");
            let file = f::safe(&plan.root, &path)?;
            if file.is_file() {
                let raw = fs::read(file)?;
                let text =
                    String::from_utf8_lossy(raw.split(|b| *b == 0).next().unwrap_or_default());
                let kept: String = text
                    .lines()
                    .filter(|line| {
                        !keys.contains(&f::fold(line.trim().trim_start_matches('\u{feff}')))
                    })
                    .map(|s| format!("{s}\n"))
                    .collect();
                tx.write(&path, kept.as_bytes())?;
            }
        }
        let position = ".ohmy3do/last-position.txt";
        let file = f::safe(&plan.root, position)?;
        if file.is_file() {
            tx.write(position, b"OH3DO_POSITION1\n/Storage\n\n")?;
        }
        job.phase("Updating menu indexes");
        library::build_indexes_after_removal(&plan.root, &mut tx, job)?;
        job.check()?;
        // Persist the irreversible boundary before unlinking the staged files.
        // A metadata-only undo after this point would create broken game entries.
        tx.state = "removal-committed".into();
        if let Err(error) = tx.save() {
            tx.state = "applying".into();
            return Err(error);
        }
        Ok(())
    })();
    if let Err(error) = result {
        job.phase("Restoring files after interrupted removal");
        job.say("Restoring staged files and library indexes...");
        if let Err(rollback) = tx.undo() {
            anyhow::bail!(
                "{error}. Rollback failed: {rollback}. Keep recovery journal {}",
                tx.path.display()
            );
        }
        return Err(error);
    }
    job.phase("Finishing permanent removal");
    let mut warnings = vec![];
    for (i, _) in plan.files.iter().enumerate() {
        let file = f::safe(&plan.root, &format!("{staging}/{i}"))?;
        if let Err(e) = fs::remove_file(&file) {
            warnings.push(format!("Could not reclaim {}: {e}", file.display()));
        }
        job.report(
            i as u64 + 1,
            plan.files.len() as u64,
            "Finishing permanent removal",
        );
    }
    // Only empty directories are removed. User files, sources, saves and
    // siblings in the same folder are never swept up by recursive deletion.
    let mut folders = BTreeSet::new();
    for file in &plan.files {
        let mut parent = Path::new(&file.path).parent();
        while let Some(p) = parent {
            if p.as_os_str().is_empty() {
                break;
            }
            folders.insert(p.to_owned());
            parent = p.parent();
        }
    }
    for path in folders.into_iter().rev() {
        let _ = fs::remove_dir(plan.root.join(path));
    }
    let _ = fs::remove_dir(plan.root.join(&staging));
    Ok(Removed {
        discs: plan.discs.len(),
        bytes: plan.bytes,
        warnings,
    })
}
