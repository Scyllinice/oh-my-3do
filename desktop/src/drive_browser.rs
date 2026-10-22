//! Read-only snapshots of actual console-drive contents. Never creates support files.
use crate::{categories, disc, drive_identity, fsutil as f, game_info, library, model::Preferences, usb_selection, Job};
use anyhow::Result;
use serde_json::{json, Value};
use std::{collections::BTreeMap, fs, path::Path};

fn media_index(root: &Path, folder: &str, warnings: &mut Vec<String>) -> (BTreeMap<String, String>, bool) {
    match library::read_drive_media_index(root, folder) {
        Ok(map) => (map, true),
        Err(_) => match library::read_media_index(root, folder) {
            Ok(map) => (map, false),
            Err(e) => { warnings.push(format!("Cannot read {folder} index: {e}")); (BTreeMap::new(), true) }
        }
    }
}
fn description(paths: &mut f::ReadPaths, path: &str) -> Option<String> {
    let file = paths.path(&game_info::filename(path)).ok()?;
    if fs::metadata(&file).ok()?.len() > 16384 { return None; }
    let text = fs::read_to_string(file).ok()?;
    let lines: Vec<_> = text.lines().collect();
    if lines.len() != 4 || lines[0] != "OM3DINFO1" || f::fold(lines[1]) != f::fold(path) { return None; }
    let body = format!("{}\n{}\n", lines[1], lines[2]);
    (u32::from_str_radix(lines[3], 16).ok()? == crc32fast::hash(body.as_bytes())).then(|| lines[2].to_owned())
}
pub fn scan(root: &Path, prefs: &Preferences, job: &Job) -> Result<Value> {
    let root = f::root(root)?;
    let identity = drive_identity::inspect(&root, prefs)?;
    let mut warnings = vec![];
    let names = categories::load(&root)?.names();
    let (covers, cover_beside) = media_index(&root, "covers", &mut warnings);
    let (previews, preview_beside) = media_index(&root, "previews", &mut warnings);
    let metadata = usb_selection::metadata(&root).unwrap_or_else(|e| { warnings.push(format!("Cannot read game metadata: {e}")); BTreeMap::new() });
    let db = library::database(&root).unwrap_or_else(|e| { warnings.push(format!("Cannot read desktop metadata: {e}")); json!({"games":[]}) });
    let known: BTreeMap<_, _> = db["games"].as_array().into_iter().flatten().filter_map(|r| r["path"].as_str().map(|p| (f::fold(p),r.clone()))).collect();
    let mut paths = f::ReadPaths::new(&root)?;
    let mut records = vec![];
    let mut total = 0u64;
    job.phase("Finding games on the drive");
    let sets=disc::collect(&[root.clone()], None, job)?;let total_discs=sets.len() as u64;
    job.report(0,total_discs,"Reading drive library");
    for (index,(disc, files)) in sets.into_iter().enumerate() {
        job.check()?;
        let path = library::relative(&root, &disc)?;
        let key = f::fold(&path);
        let size = files.iter().try_fold(0u64, |sum,p| -> std::io::Result<u64> { Ok(sum + fs::metadata(p)?.len()) })?;
        total += size;
        let stem = disc.file_stem().unwrap_or_default().to_string_lossy();
        let mut record = known.get(&key).cloned().unwrap_or_else(|| json!({"path":path,"title":stem,"region":disc::primary_region(&stem),"metadata":{}}));
        let parts: Vec<_> = path.split('/').collect();
        let organized = parts.len() >= 4 && names.iter().any(|name|name == parts[0]);
        if !known.contains_key(&key) && organized {
            record["title"] = json!(parts[2]);
            record["region"] = json!(parts[1]);
        }
        record["path"] = json!(path);
        record["bytes"] = json!(size);
        if !record["metadata"].is_object() { record["metadata"] = json!({}); }
        if disc::disc_number(&stem).is_some() {
            let parent = path.rsplit_once('/').map(|(parent,_)|parent).unwrap_or("");
            record["group_key"] = json!(format!("{parent}/{}", disc::without_disc(&stem)));
            if !known.contains_key(&key) && !organized { record["title"] = json!(disc::without_disc(&stem)); }
        }
        if let Some(fields) = metadata.get(&key) {
            for (field,value) in ["genre","publisher","year"].iter().zip(fields) { record["metadata"][*field] = json!(value); }
        }
        if let Some(text) = description(&mut paths, &path) { record["metadata"]["description"] = json!(text); }
        for (field,folder,index,beside) in [("cover","covers",&covers,cover_beside),("preview","previews",&previews,preview_beside)] {
            let indexed = index.contains_key(&key);
            let name = index.get(&key).cloned().or_else(|| record[field].as_str().map(str::to_owned));
            let relative = name.map(|name| if beside && indexed { format!("{}.ohmy3do/{name}", path.rsplit_once('/').map(|(parent,_)|format!("{parent}/")).unwrap_or_default()) } else { format!(".ohmy3do/{folder}/{name}") });
            let existing = relative.filter(|p| paths.path(p).is_ok_and(|p| p.is_file()));
            record[field] = json!(existing);
            record[format!("{field}_missing")] = json!(record[field].is_null());
        }
        records.push(record);job.report(index as u64+1,total_discs,"Reading drive library");
    }
    Ok(json!({"root":root,"records":records,"categories":names,"identity":identity,"warnings":warnings,"game_bytes":total,"available":fs2::available_space(&root).ok(),"capacity":fs2::total_space(&root).ok()}))
}
/// Presence comparison checks paths and file sizes, including every CUE track.
/// It deliberately does not claim to verify file contents.
pub fn presence(source: &Path, destination: &Path, path: &str) -> Result<bool> {
    PresenceScan::new(source, destination)?.check(path)
}
/// Reuse path inventories across every title in one PC-library refresh.
pub struct PresenceScan { source: f::ReadPaths, target: f::ReadPaths }
impl PresenceScan {
    pub fn new(source: &Path, destination: &Path) -> Result<Self> {
        Ok(Self { source: f::ReadPaths::new(source)?, target: f::ReadPaths::new(destination)? })
    }
    pub fn check(&mut self, path: &str) -> Result<bool> {
        let main = self.source.path(path)?;
        let files = if main.extension().is_some_and(|e|e.eq_ignore_ascii_case("cue")) {
            let (_, refs) = disc::cue_refs(&main)?;
            let mut files = vec![main]; files.extend(refs); files
        } else { vec![main] };
        for file in files {
            let target = self.target.path(&library::relative(self.source.root(),&file)?)?;
            if !target.is_file() || fs::metadata(&target)?.len() != fs::metadata(&file)?.len() { return Ok(false); }
        }
        Ok(true)
    }
}
