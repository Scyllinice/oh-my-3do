use crate::{
    disc as d, fsutil as f,
    library::{self, Journal},
    media,
    categories,
    Job,
};
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet, HashMap, HashSet},
    fs,
    path::{Path, PathBuf},
};
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CopyItem {
    /// Destination path; also the source path unless `source_path` is set.
    pub path: String,
    /// Media and descriptions copied from the staging root's .ohmy3do
    /// folders to the .ohmy3do folder beside their disc on the drive.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source_path: Option<String>,
    pub status: String,
    pub size: u64,
    pub source_stamp: u128,
    pub destination_stamp: Option<u128>,
    #[serde(default)]
    pub verified: bool,
}
#[derive(Clone, Serialize, Deserialize)]
pub struct CopyPlan {
    pub source: PathBuf,
    pub destination: PathBuf,
    /// Game folders already on the drive under another category: (from, to).
    #[serde(default)]
    pub moves: Vec<(String, String)>,
    pub files: Vec<CopyItem>,
    pub games: usize,
    pub bytes: u64,
    #[serde(default)]
    pub selection: Option<Vec<String>>,
    #[serde(default)]
    pub generated: BTreeMap<String, Vec<u8>>,
}
fn word(data: &[u8], pos: usize) -> Result<usize> {
    Ok(u32::from_be_bytes(
        data.get(pos..pos + 4)
            .context("Truncated index")?
            .try_into()?,
    ) as usize)
}
pub fn config(path: &Path) -> Result<()> {
    if fs::metadata(path)?.len() > 512 {
        bail!("Config exceeds 512 bytes")
    }
    crate::console_config::Settings::parse(&fs::read(path)?)?;
    Ok(())
}

pub fn find(path: &Path) -> Result<Vec<String>> {
    if fs::metadata(path)?.len() > 128 * 1024 + 32 {
        bail!("Oversized Find index")
    }
    let data = fs::read(path)?;
    if data.len() < 32 || &data[..8] != b"OM3DLIB1" || &data[data.len() - 8..] != b"END3DL01" {
        bail!("Invalid Find index header/end")
    }
    let count = word(&data, 8)?;
    let size = word(&data, 12)?;
    if count > 2048
        || size + 9 * count > 128 * 1024
        || size + 32 != data.len()
        || word(&data, 20)? != 0
        || crc32fast::hash(&data[24..data.len() - 8]) as usize != word(&data, 16)?
    {
        bail!("Invalid Find index checksum/bounds")
    }
    let mut pos = 24;
    let mut names = vec![];
    for _ in 0..count {
        let b = data.get(pos..pos + 2).context("Truncated Find path")?;
        let n = u16::from_be_bytes(b.try_into()?) as usize;
        pos += 2;
        let s = data.get(pos..pos + n).context("Truncated Find path")?;
        if n < 2 || s[n - 1] != 0 || s[..n - 1].contains(&0) {
            bail!("Invalid Find path")
        };
        names.push(std::str::from_utf8(&s[..n - 1])?.to_owned());
        pos += n;
    }
    if pos != data.len() - 8
        || names
            .iter()
            .map(|s| f::fold(s))
            .collect::<HashSet<_>>()
            .len()
            != names.len()
    {
        bail!("Duplicate/trailing Find paths")
    };
    Ok(names)
}
/// Runtime files as (USB path, staging path) pairs. Media moves beside its disc.
pub fn inventory(root: &Path, job: &Job) -> Result<(Vec<(String, String)>, usize)> {
    inventory_selected(root, None, job)
}
fn inventory_selected(root: &Path, selected: Option<&BTreeSet<String>>, job: &Job) -> Result<(Vec<(String, String)>, usize)> {
    let boot = f::safe(root, "boot.iso")?;
    d::issue(&boot)?;
    if fs::metadata(&boot)?.len() > 16 * 1024 * 1024 {
        bail!("Oversized menu boot image")
    }
    let raw = fs::read(&boot)?;
    if raw.len() < 84
        || raw[..7] != [1, 90, 90, 90, 90, 90, 1]
        || word(&raw, 76)? != 2048
        || word(&raw, 80)? * 2048 != raw.len()
    {
        bail!("Invalid boot volume header/size")
    }
    config(&f::safe(root, ".ohmy3do/ohmy3do.ini")?)?;
    let indexed = find(&f::safe(root, ".ohmy3do/library.3di")?)?;
    let sets = d::collect(&[root.to_owned()], None, job)?;
    let games = sets
        .iter()
        .map(|(p, _)| library::relative(root, p))
        .collect::<Result<BTreeSet<_>>>()?;
    if indexed.iter().cloned().collect::<BTreeSet<_>>() != games {
        bail!("Find index is stale. Rebuild indexes on the Library tab first.")
    }
    if let Some(selected) = selected {
        if !selected.iter().all(|p| games.contains(p)) { bail!("Selected games changed; reload the game list"); }
    }
    let chosen = |path: &str| selected.is_none_or(|s| s.contains(path));
    let mut files = BTreeSet::from([
        "boot.iso".into(),
        ".ohmy3do/ohmy3do.ini".into(),
        ".ohmy3do/library.3di".into(),
    ]);
    for (disc, set) in &sets {
        if !chosen(&library::relative(root, disc)?) { continue; }
        for p in set {
            files.insert(library::relative(root, p)?);
        }
    }
    let folded = games.iter().map(|s| f::fold(s)).collect::<HashSet<_>>();
    let homes = games
        .iter()
        .map(|g| (f::fold(g), g.clone()))
        .collect::<HashMap<_, _>>();
    let mut beside = BTreeMap::new();
    let mut checked = HashSet::new();
    let metadata = f::safe(root, ".ohmy3do/metadata.3di")?;
    if metadata.is_file() {
        if fs::metadata(&metadata)?.len() > 192 * 1024 + 32 {
            bail!("Oversized metadata index");
        }
        for path in library::metadata_paths(&fs::read(metadata)?)? {
            if !folded.contains(&f::fold(&path)) {
                bail!("Metadata index refers to a missing game; rebuild indexes");
            }
        }
        files.insert(".ohmy3do/metadata.3di".into());
    }
    for path in &games {if chosen(path) {let name=crate::game_info::filename(path);if f::safe(root,&name)?.is_file(){beside.insert(crate::game_info::drive_filename(path),name);}}}
    let cheats=f::safe(root,crate::save_cheats::JSON_PATH)?;
    if cheats.is_file(){
        let db=crate::save_cheats::Database::read(&cheats)?;
        let binary=f::safe(root,crate::save_cheats::BINARY_PATH)?;
        files.insert(crate::save_cheats::JSON_PATH.into());
        if binary.is_file(){
            if fs::metadata(&binary)?.len()>crate::save_cheats::MAX_BINARY as u64{bail!("Oversized console cheat database");}
            if !db.matches_binary(&fs::read(&binary)?)?{bail!("Cheat database source and console file differ; reinstall them with the database editor");}
            files.insert(crate::save_cheats::JSON_PATH.into());files.insert(crate::save_cheats::BINARY_PATH.into());
        }
    }
    let names = f::safe(root, ".ohmy3do/save-names.txt")?;
    if names.is_file() {
        if fs::metadata(&names)?.len() as usize >= crate::saves::SAVE_NAMES_BYTES {
            bail!("Oversized save names; rebuild indexes");
        }
        files.insert(".ohmy3do/save-names.txt".into());
    }
    for folder in ["covers", "previews"] {
        let map = library::read_media_index(root, folder)?;
        let index = format!(".ohmy3do/{folder}/index.3di");
        if f::safe(root, &index)?.is_file() {
            files.insert(index);
        }
        for (game, name) in map {
            let Some(disc) = homes.get(&game) else {
                bail!("Media index refers to a missing game")
            };
            if !chosen(disc) { continue; }
            let extension = Path::new(&name)
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .to_ascii_lowercase();
            if !(if folder == "covers" {
                ["png", "3dc"].contains(&extension.as_str())
            } else {
                extension == "3dv"
            }) {
                bail!("Indexed media type does not match its folder")
            }
            let path = format!(".ohmy3do/{folder}/{name}");
            if checked.insert(path.clone()) {
                job.check()?;
                media::validate(&f::safe(root, &path)?)?;
            }
            let home = disc.rsplit_once('/').map_or("", |(home, _)| home);
            let beside_path = |name: &str| {
                if home.is_empty() {
                    format!(".ohmy3do/{name}")
                } else {
                    format!("{home}/.ohmy3do/{name}")
                }
            };
            if folder == "covers" {
                let large = media::large_name(&name);
                let source = format!(".ohmy3do/covers/{large}");
                if f::safe(root, &source)?.is_file() {
                    if checked.insert(source.clone()) {
                        media::validate(&f::safe(root, &source)?)?;
                    }
                    beside.insert(beside_path(&large), source);
                }
            }
            beside.insert(beside_path(&name), path);
        }
    }
    for name in ["favorites.txt", "recent.txt", "last-position.txt"] {
        let path = format!(".ohmy3do/{name}");
        if f::safe(root, &path)?.is_file() {
            files.insert(path);
        }
    }
    let saves = f::safe(root, ".ohmy3do/saves")?;
    if saves.exists() {
        for e in walkdir::WalkDir::new(saves).follow_links(false) {
            let e = e?;
            if f::is_link(e.path()) {
                bail!("Links in saves are unsupported")
            };
            if e.file_type().is_file() {
                files.insert(library::relative(root, e.path())?);
            }
        }
    }
    for name in &files {
        let path = f::safe(root, name)?;
        if !path.is_file() || fs::metadata(path)?.len() >= 4 * 1024u64.pow(3) {
            bail!("Missing or oversized FAT32 file: {name}")
        }
    }
    let mut pairs = files
        .into_iter()
        .map(|n| (n.clone(), n))
        .collect::<Vec<_>>();
    for (dest, source) in beside {
        if pairs.iter().any(|(d, _)| f::fold(d) == f::fold(&dest)) {
            bail!("Media or description path collides with another file: {dest}")
        }
        pairs.push((dest, source));
    }
    pairs.sort();
    Ok((pairs, selected.map_or(games.len(), BTreeSet::len)))
}
fn media_index(name: &str) -> bool {
    [".ohmy3do/covers/index.3di", ".ohmy3do/previews/index.3di"].contains(&name)
}
fn personal(name: &str) -> bool {
    name.starts_with(".ohmy3do/saves/")
        || [
            ".ohmy3do/ohmy3do.ini",
            categories::PATH,
            ".ohmy3do/save-cheats.json",
            ".ohmy3do/save-cheats.3db",
            ".ohmy3do/favorites.txt",
            ".ohmy3do/recent.txt",
            ".ohmy3do/last-position.txt",
        ]
        .contains(&name)
}
/// Category/Region/Game folders that changed category in staging but are
/// still at their old place on the drive. Staging's own folders are never moved.
fn folder_moves(target: &Path, files: &[(String, String)], source_config: &categories::Categories) -> Result<Vec<(String, String)>> {
    let target_config=categories::load(target)?;
    let mut moves=vec![];
    for category in &source_config.categories {
        if let Some(old)=target_config.categories.iter().find(|c|c.id==category.id && c.name!=category.name) {
            if f::safe(target,&old.name)?.exists() {
                if !f::safe(target,&old.name)?.is_dir() {bail!("Category path is occupied by a file: {}",old.name);}
                if f::safe(target,&category.name)?.exists() {bail!("Both category folders exist on the drive: {} and {}. Resolve them before copying.",old.name,category.name);}
                moves.push((old.name.clone(),category.name.clone()));
            }
        }
    }
    let mut wanted = BTreeMap::new();
    for (name, _) in files {
        let parts: Vec<&str> = name.split('/').collect();
        if parts.len() >= 4 && source_config.contains(parts[0]) {
            wanted.insert(f::fold(&parts[..3].join("/")), parts[..3].join("/"));
        }
    }
    let mut found = HashMap::new();
    for category in target_config.names() {
        if moves.iter().any(|(old,_)|old==&category) {continue;}
        let Ok(regions) = fs::read_dir(target.join(&category)) else { continue };
        for region in regions.flatten().filter(|e| e.path().is_dir() && !f::is_link(&e.path())) {
            for game in fs::read_dir(region.path())?.flatten().filter(|e| e.path().is_dir() && !f::is_link(&e.path())) {
                let folder = format!(
                    "{category}/{}/{}",
                    region.file_name().to_string_lossy(),
                    game.file_name().to_string_lossy()
                );
                if !wanted.contains_key(&f::fold(&folder)) {
                    let key = f::fold(folder.split_once('/').unwrap().1);
                    found.entry(key).or_insert_with(Vec::new).push(folder);
                }
            }
        }
    }
    for folder in wanted.values() {
        if moves.iter().any(|(_,new)|folder.starts_with(&format!("{new}/"))) {continue;}
        if target.join(folder).exists() {
            continue;
        }
        // One unambiguous match only; anything else is copied as before.
        if let Some([old]) = found.get(&f::fold(folder.split_once('/').unwrap().1)).map(Vec::as_slice) {
            moves.push((old.clone(), folder.clone()));
        }
    }
    Ok(moves)
}
/// Disc paths on the drive under moved folders, old → new (folded keys).
fn moved_paths(target: &Path, moves: &[(String, String)]) -> HashMap<String, String> {
    let mut map = HashMap::new();
    for (old, new) in moves {
        for entry in walkdir::WalkDir::new(target.join(old)).follow_links(false).into_iter().flatten() {
            if let Ok(rest) = entry.path().strip_prefix(target.join(old)) {
                let rest = rest.to_string_lossy().replace('\\', "/");
                if entry.file_type().is_file() {
                    map.insert(f::fold(&format!("{old}/{rest}")), format!("{new}/{rest}"));
                }
            }
        }
    }
    map
}
fn report_path(source: &Path, target: &Path, cache: &Path) -> PathBuf {
    let key = format!(
        "{:x}",
        Sha256::digest(format!("{}\n{}", source.display(), target.display()).as_bytes())
    );
    cache.join("usb-copy").join(format!("{key}.json"))
}
fn read_report(path: &Path) -> Option<CopyPlan> {
    fs::read(path).ok().and_then(|b| serde_json::from_slice(&b).ok())
}
/// Saves the copy record, keeping earlier verified entries a smaller copy
/// did not touch, so one game's copy doesn't discard the whole drive's record.
fn save_report(path: &Path, plan: &CopyPlan) -> Result<()> {
    let mut report = plan.clone();
    if let Some(old) = read_report(path) {
        let known: HashSet<String> = report.files.iter().map(|file| file.path.clone()).collect();
        report.files.extend(old.files.into_iter().filter(|file| {
            !known.contains(&file.path) && (file.verified || file.status == "verified")
        }));
    }
    f::atomic(path, &serde_json::to_vec_pretty(&report)?)
}
fn plan_cheat_exists(root:&Path,name:&str)->bool {root.join(name).exists()}
pub fn plan(
    source: &Path,
    target: &Path,
    cache: &Path,
    verify: bool,
    job: &Job,
) -> Result<CopyPlan> {
    plan_impl(source, target, cache, verify, None, job)
}
/// Selection contains complete game groups from `choices`, never individual tracks.
pub fn plan_selected(source: &Path, target: &Path, cache: &Path, verify: bool, selected: &[String], job: &Job) -> Result<CopyPlan> {
    let indexed = find(&f::safe(&f::root(source)?, ".ohmy3do/library.3di")?)?;
    if !selected.iter().all(|p| indexed.contains(p)) { bail!("Selected games changed; reload the game list"); }
    let config = categories::load(source)?;
    let groups = selected.iter().map(|p| f::fold(&game_label(p,&config))).collect::<BTreeSet<_>>();
    let expanded = indexed.into_iter().filter(|p| groups.contains(&f::fold(&game_label(p,&config)))).collect();
    plan_impl(source, target, cache, verify, Some(expanded), job)
}
fn plan_impl(source: &Path, target: &Path, cache: &Path, verify: bool, selected: Option<BTreeSet<String>>, job: &Job) -> Result<CopyPlan> {
    let source = f::root(source)?;
    let target = f::root(target)?;
    if source.starts_with(&target) || target.starts_with(&source) {
        bail!("Choose separate, non-overlapping staging and USB roots")
    }
    let _source_lock = library::lock_library(&source, cache)?;
    let _destination_lock = library::lock_library(&target, cache)?;
    job.say("Validating staging and media");
    let (mut files, games) = inventory_selected(&source, selected.as_ref(), job)?;
    let report = report_path(&source, &target, cache);
    let previous: HashMap<String, CopyItem> = read_report(&report)
        .map(|p| p.files.into_iter().map(|file| (file.path.clone(), file)).collect())
        .unwrap_or_default();
    categories::for_transfer(&source,&target)?;
    let moves = folder_moves(&target, &files, &categories::load(&source)?)?;
    let selection = selected.as_ref().map(|s| s.iter().cloned().collect::<Vec<_>>());
    let generated = if let Some(selected) = &selection {
        crate::usb_selection::indexes(&source, &target, selected, &moves, job)?
    } else { BTreeMap::new() };
    files.retain(|(name,_)| !generated.contains_key(name));
    let remaps = moved_paths(&target, &moves);
    let mut items = vec![];
    let mut bytes = 0;
    const REVIEW_UNITS: u64 = 1_000_000;
    let review_total = (files.len()+generated.len()).max(1) as u64 * REVIEW_UNITS;
    job.report(0,review_total,"Reviewing files");
    for (review_index,(name, source_name)) in files.into_iter().enumerate() {
        let file_job = job.progress_range(review_index as u64*REVIEW_UNITS,REVIEW_UNITS,review_total,"Reviewing files");
        job.check()?;
        let src = f::safe(&source, &source_name)?;
        // A file in a folder that will move is compared where it is now.
        let current = moves
            .iter()
            .find(|(_, new)| name.starts_with(&format!("{new}/")))
            .map(|(old, new)| format!("{old}{}", &name[new.len()..]))
            .unwrap_or_else(|| name.clone());
        let dest = f::safe(&target, &current)?;
        let (mut size, source_stamp) = f::stamp(&src)?;
        let index = media_index(&name);
        let generated = if index {
            Some(library::beside_index(&fs::read(&src)?)?)
        } else {
            None
        };
        if let Some(content) = &generated {
            size = content.len() as u64;
        }
        let destination_stamp = if dest.is_file() {
            Some(f::stamp(&dest)?.1)
        } else {
            None
        };
        if dest.exists() && !dest.is_file() {
            bail!("Destination is occupied by a directory: {name}")
        }
        if name == ".ohmy3do/ohmy3do.ini" && dest.is_file() {
            config(&dest)?
        }
        let mut trusted = false;
        let cheat_file=[crate::save_cheats::JSON_PATH,crate::save_cheats::BINARY_PATH].contains(&name.as_str());
        let preserve_cheats=cheat_file && [crate::save_cheats::JSON_PATH,crate::save_cheats::BINARY_PATH].iter().any(|path|plan_cheat_exists(&target,path));
        let remapped = personal(&name) && !cheat_file
            && dest.is_file()
            && !moves.is_empty()
            && if name == ".ohmy3do/last-position.txt" {
                let old: Vec<String> = moves.iter().map(|(o, _)| o.clone()).collect();
                library::remap_position(&fs::read(&dest)?, &remaps, &old).is_some()
            } else {
                library::remap_lines(&fs::read(&dest)?, &remaps).is_some()
            };
        let status = if remapped {
            "remap"
        } else if preserve_cheats || (personal(&name) && dest.exists()) {
            "preserve"
        } else if !dest.exists() {
            "copy"
        } else if !dest.is_file() {
            bail!("Destination is occupied: {name}")
        } else if let Some(content) = &generated {
            if fs::read(&dest)? == *content {
                trusted = true;
                "unchanged"
            } else {
                "update"
            }
        } else {
            let ds = f::stamp(&dest)?;
            let disc = !name.starts_with(".ohmy3do/")
                && !name.contains("/.ohmy3do/")
                && name != "boot.iso";
            let receipt = previous
                .get(&name)
                .filter(|r| r.verified || r.status == "verified")
                .is_some_and(|r| {
                    r.size == size
                        && r.source_stamp == source_stamp
                        && r.destination_stamp == Some(ds.1)
                        && ds.0 == size
                });
            // A receipt from the last verified copy spares rereading unchanged
            // media too; previews alone can be gigabytes on a slow card.
            if !verify && size == ds.0 && (receipt || (disc && f::same_time(source_stamp, ds.1))) {
                trusted = receipt;
                "unchanged"
            } else if size == ds.0 && f::sha256(&src, &file_job.progress_range(0,size.max(1),size.max(1)*2,"Checking source"))? == f::sha256(&dest, &file_job.progress_range(size.max(1),size.max(1),size.max(1)*2,"Checking destination"))? {
                trusted = true;
                "unchanged"
            } else if disc {
                bail!("Different existing disc on destination: {name}")
            } else {
                "update"
            }
        };
        if ["copy", "update"].contains(&status) {
            bytes += size
        }
        items.push(CopyItem {
            source_path: (source_name != name).then_some(source_name),
            path: name,
            status: status.into(),
            size,
            source_stamp,
            destination_stamp,
            verified: trusted,
        });
        job.report((review_index as u64+1)*REVIEW_UNITS,review_total,"Reviewing files");
    }
    for (generated_index,(name, content)) in generated.iter().enumerate() {
        let dest = f::safe(&target, name)?;
        if dest.exists() && !dest.is_file() { bail!("Destination is occupied: {name}"); }
        let destination_stamp = if dest.is_file() { Some(f::stamp(&dest)?.1) } else { None };
        let status = if destination_stamp.is_none() { "copy" } else if fs::read(&dest)? == *content { "unchanged" } else { "update" };
        if status != "unchanged" { bytes += content.len() as u64; }
        items.push(CopyItem { path: name.clone(), source_path: None, status: status.into(), size: content.len() as u64, source_stamp: 0, destination_stamp, verified: status == "unchanged" });
        job.report(review_total-(generated.len() as u64-generated_index as u64-1)*REVIEW_UNITS,review_total,"Reviewing files");
    }
    // Descriptions now sit beside their discs; the menu no longer reads these.
    let old_info = f::safe(&target, ".ohmy3do/info")?;
    if old_info.is_dir() {
        let mut names = fs::read_dir(&old_info)?.map(|entry| Ok(entry?.file_name())).collect::<Result<Vec<_>>>()?;
        names.sort();
        for file_name in names {
            let Some(file_name) = file_name.to_str() else { continue };
            if !crate::game_info::old_drive_file(file_name, &old_info.join(file_name))? { continue; }
            let name = format!(".ohmy3do/info/{file_name}");
            let (size, stamp) = f::stamp(&f::safe(&target, &name)?)?;
            items.push(CopyItem { path: name, source_path: None, status: "remove".into(), size, source_stamp: 0, destination_stamp: Some(stamp), verified: false });
        }
    }
    // Menu-written lists on the drive follow moved folders even when staging has none.
    let old: Vec<String> = moves.iter().map(|(o, _)| o.clone()).collect();
    for name in [".ohmy3do/favorites.txt", ".ohmy3do/recent.txt", ".ohmy3do/last-position.txt"] {
        let dest = f::safe(&target, name)?;
        if moves.is_empty() || items.iter().any(|i| i.path == name) || !dest.is_file() {
            continue;
        }
        let raw = fs::read(&dest)?;
        let changed = if name.ends_with("last-position.txt") {
            library::remap_position(&raw, &remaps, &old).is_some()
        } else {
            library::remap_lines(&raw, &remaps).is_some()
        };
        if changed {
            let (size, stamp) = f::stamp(&dest)?;
            items.push(CopyItem {
                path: name.into(),
                source_path: None,
                status: "remap".into(),
                size,
                source_stamp: stamp,
                destination_stamp: Some(stamp),
                verified: false,
            });
        }
    }
    let plan = CopyPlan {
        source,
        destination: target,
        moves,
        files: items,
        games,
        bytes,
        selection,
        generated,
    };
    save_report(&report, &plan)?;
    job.say(format!(
        "USB preview: {} files; {} folders to move; {} disc entries; {:.2} GiB to copy",
        plan.files.len(),
        plan.moves.len(),
        games,
        bytes as f64 / 1024f64.powi(3)
    ));
    Ok(plan)
}
pub fn apply(plan: &CopyPlan, cache: &Path, job: &Job) -> Result<PathBuf> {
    if f::root(&plan.source)? != plan.source || f::root(&plan.destination)? != plan.destination {
        bail!("Copy roots changed since preview")
    }
    let _source_lock = library::lock_library(&plan.source, cache)?;
    let _destination_lock = library::lock_library(&plan.destination, cache)?;
    if !storage_estimate(plan)?.fits {
        bail!("Insufficient free space for this copy and its working-space reserve; review the copy again")
    }
    if let Some(selected) = &plan.selection {
        if crate::usb_selection::indexes(&plan.source, &plan.destination, selected, &plan.moves, job)? != plan.generated {
            bail!("Library or USB indexes changed since preview; review the copy again");
        }
    }
    let category_config=categories::for_transfer(&plan.source,&plan.destination)?;
    let total_bytes = plan.files.iter().filter(|file| ["copy","update"].contains(&file.status.as_str())).map(|file|file.size).sum::<u64>().max(1);
    let mut completed_bytes = 0u64;job.report(0,total_bytes,"Copying and verifying files");
    let mut tx = Journal::new(&plan.destination, cache)?;
    let mut receipt = plan.clone();
    let result = (|| -> Result<()> {
        let remaps = moved_paths(&plan.destination, &plan.moves);
        let moved: Vec<String> = plan.moves.iter().map(|(old, _)| old.clone()).collect();
        for (old, new) in &plan.moves {
            job.check()?;
            if f::safe(&plan.destination, new)?.exists() {
                bail!("Destination changed since preview: {new}")
            }
            job.say(format!("Moving {old} to {new}"));
            tx.rename(old, new)?;
        }
        for file in plan.files.iter().filter(|file| file.status == "remap") {
            let dest = f::safe(&plan.destination, &file.path)?;
            let raw = fs::read(&dest)?;
            let updated = if file.path == ".ohmy3do/last-position.txt" {
                library::remap_position(&raw, &remaps, &moved)
            } else {
                library::remap_lines(&raw, &remaps)
            };
            if let Some(text) = updated {
                tx.write(&file.path, &text)?;
            }
        }
        let mut order = (0..plan.files.len()).collect::<Vec<_>>();
        order.sort_by_key(|i| {
            let name = &plan.files[*i].path;
            (
                if name == "boot.iso" {
                    2
                } else if name.ends_with(".3di") {
                    1
                } else {
                    0
                },
                name.clone(),
            )
        });
        for i in order {
            let file = &plan.files[i];
            if !["copy", "update"].contains(&file.status.as_str()) {
                continue;
            };
            job.check()?;
            let file_job = job.progress_range(completed_bytes,file.size,total_bytes,"Copying and verifying files");
            if let Some(content) = plan.generated.get(&file.path) {
                let dest = f::safe(&plan.destination, &file.path)?;
                let stamp = if dest.exists() { Some(f::stamp(&dest)?.1) } else { None };
                if stamp != file.destination_stamp { bail!("Destination changed since preview: {}", file.path); }
                job.check()?;
                tx.write(&file.path, content)?;
                if f::read_from_disk(&dest)? != *content { bail!("Index copy readback failed"); }
                receipt.files[i].status = "verified".into();
                receipt.files[i].verified = true;
                receipt.files[i].destination_stamp = Some(f::stamp(&dest)?.1);
                completed_bytes += file.size;job.report(completed_bytes,total_bytes,"Copying and verifying files");
                continue;
            }
            let src = f::safe(
                &plan.source,
                file.source_path.as_deref().unwrap_or(&file.path),
            )?;
            let dest = f::safe(&plan.destination, &file.path)?;
            if f::stamp(&src)? != (file.size, file.source_stamp) {
                bail!("Staging changed since preview: {}", file.path)
            }
            job.say(format!("Copying {}", file.path));
            if media_index(&file.path) {
                if file.status == "update"
                    && f::stamp(&dest)?.1
                        != file
                            .destination_stamp
                            .context("Missing destination stamp")?
                {
                    bail!("Destination changed since preview")
                };
                let content = library::beside_index(&fs::read(&src)?)?;
                tx.write(&file.path, &content)?;
                if f::read_from_disk(&dest)? != content {
                    bail!("Copy readback failed")
                }
            } else if file.status == "copy" {
                tx.copy(&src, &file.path, None, &file_job)?;
            } else {
                if f::stamp(&dest)?.1
                    != file
                        .destination_stamp
                        .context("Missing destination stamp")?
                {
                    bail!("Destination changed since preview")
                };
                let bytes = fs::read(&src)?;
                tx.write(&file.path, &bytes)?;
                if f::sha256_from_disk(&dest, &file_job.progress_range(0,file.size.max(1),file.size.max(1)*2,"Verifying copied file"))? != f::sha256(&src, &file_job.progress_range(file.size.max(1),file.size.max(1),file.size.max(1)*2,"Verifying copied file"))? {
                    bail!("Copy readback failed")
                }
            }
            receipt.files[i].status = "verified".into();
            receipt.files[i].verified = true;
            receipt.files[i].destination_stamp = Some(f::stamp(&dest)?.1);
            completed_bytes += file.size;job.report(completed_bytes,total_bytes,"Copying and verifying files");
        }
        for (i, file) in plan.files.iter().enumerate().filter(|(_, file)| file.status == "remove") {
            job.check()?;
            let dest = f::safe(&plan.destination, &file.path)?;
            if !dest.exists() { continue; }
            if f::stamp(&dest)? != (file.size, file.destination_stamp.context("Missing destination stamp")?) {
                bail!("Destination changed since preview: {}", file.path)
            }
            job.say(format!("Removing old {}", file.path));
            tx.remove(&file.path)?;
            receipt.files[i].status = "removed".into();
        }
        let old_info = f::safe(&plan.destination, ".ohmy3do/info")?;
        if old_info.is_dir() && fs::read_dir(&old_info)?.next().is_none() {
            fs::remove_dir(&old_info)?;
        }
        job.phase("Finalizing drive support");
        for file in &mut receipt.files {
            if file.status == "unchanged" && file.verified {
                file.status = "verified".into();
            }
        }
        crate::setup::ensure_support(&plan.destination, &mut tx, &Default::default(), job)?;
        tx.write(categories::PATH,&category_config.encode()?)?;
        let save_slots=crate::save_catalog::scan(&plan.destination.join(".ohmy3do/saves"),job)?;
        crate::save_catalog::write(&save_slots,&mut tx,".ohmy3do/saves/",job)?;
        tx.finish()?;
        save_report(&report_path(&plan.source, &plan.destination, cache), &receipt)?;
        Ok(())
    })();
    if let Err(error) = result {
        job.phase("Restoring the drive after the interrupted copy");
        if let Err(undo) = tx.undo() {
            bail!(
                "{error:#}; rollback needs attention: {undo:#}. Keep {}",
                tx.path.display()
            )
        }
        return Err(error);
    }
    job.say("USB copy complete; existing settings and saves preserved");
    Ok(tx.path)
}
pub fn summary(plan: &CopyPlan) -> String {
    let mut counts = std::collections::BTreeMap::new();
    for f in &plan.files {
        *counts.entry(&f.status).or_insert(0) += 1
    }
    format!(
        "{} disc entries · {} files · {} game folders to move · {:.2} GiB to copy\n{}",
        plan.games,
        plan.files.len(),
        plan.moves.len(),
        plan.bytes as f64 / 1024f64.powi(3),
        counts
            .into_iter()
            .map(|(s, n)| format!("{s}: {n}"))
            .collect::<Vec<_>>()
            .join(" · ")
    )
}

/// Whole game folders, keeping all discs and CUE data together. Sizes exclude media/support files.
#[derive(Clone)]
pub struct GameChoice {
    pub label: String,
    pub discs: Vec<String>,
    pub bytes: u64,
}
fn game_label(path: &str, config: &categories::Categories) -> String {
    let parts = path.split('/').collect::<Vec<_>>();
    if parts.len() >= 4 && config.contains(parts[0]) { parts[..3].join("/") } else { path.into() }
}
pub fn choices(root: &Path, job: &Job) -> Result<(PathBuf, Vec<GameChoice>)> {
    let root = f::root(root)?;
    let indexed = find(&f::safe(&root, ".ohmy3do/library.3di")?)?.into_iter().collect::<BTreeSet<_>>();
    let sets = d::collect(&[root.clone()], None, job)?;
    let actual = sets.iter().map(|(p,_)| library::relative(&root,p)).collect::<Result<BTreeSet<_>>>()?;
    if indexed != actual { bail!("Find index is stale. Rebuild indexes on the Library tab first."); }
    let config = categories::load(&root)?;
    let mut groups = BTreeMap::<String, GameChoice>::new();
    for (disc, files) in sets {
        job.check()?;
        let path = library::relative(&root,&disc)?;
        let label = game_label(&path,&config);
        let group = groups.entry(f::fold(&label)).or_insert_with(|| GameChoice { label, discs: vec![], bytes: 0 });
        group.discs.push(path);
        for file in files { group.bytes += fs::metadata(file)?.len(); }
    }
    Ok((root, groups.into_values().collect()))
}

/// File-content totals, plus a conservative reserve for allocation, temporary
/// replacements and runtime support. Space is queried again before copying.
#[derive(Clone, Debug, Serialize)]
pub struct StorageEstimate {
    pub games: u64,
    pub covers: u64,
    pub previews: u64,
    pub support: u64,
    pub total: u64,
    pub to_copy: u64,
    pub reserve: u64,
    pub required: u64,
    pub available: u64,
    pub remaining: u64,
    pub shortfall: u64,
    pub fits: bool,
}
pub fn storage_estimate(plan: &CopyPlan) -> Result<StorageEstimate> {
    Ok(estimate_with_space(plan, fs2::available_space(&plan.destination)?))
}
/// Separate space arithmetic from filesystem queries for deterministic checks.
pub fn estimate_with_space(plan: &CopyPlan, available: u64) -> StorageEstimate {
    let mut estimate = StorageEstimate {games:0,covers:0,previews:0,support:0,total:0,to_copy:0,reserve:0,required:0,available,remaining:0,shortfall:0,fits:false};
    for file in plan.files.iter().filter(|file| file.status != "remove") {
        let name = file.path.to_ascii_lowercase();
        let support = name.starts_with(".ohmy3do/") || name.contains("/.ohmy3do/") || name == "boot.iso";
        let bucket = if support && (name.ends_with(".3dc") || name.ends_with(".png")) { &mut estimate.covers }
            else if support && name.ends_with(".3dv") { &mut estimate.previews }
            else if support { &mut estimate.support } else { &mut estimate.games };
        *bucket = bucket.saturating_add(file.size);
        estimate.total = estimate.total.saturating_add(file.size);
        if ["copy", "update"].contains(&file.status.as_str()) { estimate.to_copy = estimate.to_copy.saturating_add(file.size); }
    }
    estimate.reserve = (8 * 1024 * 1024).max(estimate.to_copy / 100).max((plan.files.len() as u64).saturating_mul(16 * 1024));
    estimate.required = estimate.to_copy.saturating_add(estimate.reserve);
    estimate.fits = estimate.required <= available;
    estimate.remaining = available.saturating_sub(estimate.required);
    estimate.shortfall = estimate.required.saturating_sub(available);
    estimate
}
