//! The menu's index files for copying selected games to a drive that
//! already has games. The new indexes list the games actually on the drive
//! plus the ones being copied, keeping what the drive's indexes already
//! said about the games that stay. (The file formats are described in
//! src/library_index.h.)

use crate::{disc, fsutil as f, library, usb, Job};
use anyhow::{bail, Result};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    path::Path,
};

/// An index file: header (`magic`, entry count, payload size, the
/// payload's CRC-32, a reserved 0), the payload, then the footer `end`.
/// Fails if the index would not fit the menu: more than 2048 entries, or
/// more than `limit` bytes counting `overhead` extra bytes per entry.
fn packed(
    magic: &[u8],
    end: &[u8],
    count: usize,
    payload: Vec<u8>,
    overhead: usize,
    limit: usize,
) -> Result<Vec<u8>> {
    if count > 2048 || payload.len() + overhead * count > limit {
        bail!("Selected games plus existing USB games exceed menu index capacity");
    }
    let mut out = magic.to_vec();
    for n in [
        count as u32,
        payload.len() as u32,
        crc32fast::hash(&payload),
        0,
    ] {
        out.extend(n.to_be_bytes());
    }
    out.extend(payload);
    out.extend(end);
    Ok(out)
}
/// Appends a string as the indexes store it: a 2-byte big-endian length
/// (counting the terminating zero), the text and a zero byte.
fn string(out: &mut Vec<u8>, value: &str) -> Result<()> {
    if value.len() > 8192 || value.contains('\0') {
        bail!("Invalid runtime index string");
    }
    out.extend(((value.len() + 1) as u16).to_be_bytes());
    out.extend(value.as_bytes());
    out.push(0);
    Ok(())
}
/// The drive's (or library's) metadata.3di: each disc's genre, publisher
/// and release date, by its path in case-insensitive form. Empty if there is
/// none.
pub(crate) fn metadata(root: &Path) -> Result<BTreeMap<String, [String; 3]>> {
    let path = f::safe(root, ".ohmy3do/metadata.3di")?;
    if !path.exists() {
        return Ok(BTreeMap::new());
    }
    if fs::metadata(&path)?.len() > 192 * 1024 + 32 {
        bail!("Oversized metadata index");
    }
    let raw = fs::read(path)?;
    let paths = library::metadata_paths(&raw)?;
    let mut pos = 24;
    let mut out = BTreeMap::new();
    for path in paths {
        let mut fields = [String::new(), String::new(), String::new(), String::new()];
        for field in &mut fields {
            let n = u16::from_be_bytes(raw[pos..pos + 2].try_into()?) as usize;
            pos += 2;
            *field = std::str::from_utf8(&raw[pos..pos + n - 1])?.into();
            pos += n;
        }
        out.insert(
            f::fold(&path),
            [fields[1].clone(), fields[2].clone(), fields[3].clone()],
        );
    }
    Ok(out)
}
/// `path` with its folder renamed if it is inside one of `moves` (old
/// folder, new folder).
fn remap(path: &str, moves: &[(String, String)]) -> String {
    moves
        .iter()
        .find_map(|(old, new)| {
            f::fold(path)
                .starts_with(&format!("{}/", f::fold(old)))
                .then(|| format!("{new}{}", &path[old.len()..]))
        })
        .unwrap_or_else(|| path.into())
}
/// The index files (by path on the drive) for copying the `selected` games
/// from the library at `source` to the drive at `target`, whose folders
/// `moves` renames. Lists the discs actually on the drive (not what its
/// old index says), so games copied there some other way are kept. Called
/// again just before copying, so the indexes written match the drive even
/// if it changed after the person reviewed the transfer.
pub(crate) fn indexes(
    source: &Path,
    target: &Path,
    selected: &[String],
    moves: &[(String, String)],
    job: &Job,
) -> Result<BTreeMap<String, Vec<u8>>> {
    // Every disc: those already on the drive (with their metadata from the
    // drive), then the selected ones (with the library's).
    let selected: BTreeSet<_> = selected.iter().map(|p| f::fold(p)).collect();
    let source_paths = usb::find(&f::safe(source, ".ohmy3do/library.3di")?)?;
    let destination_paths = disc::collect(&[target.to_owned()], None, job)?
        .iter()
        .map(|(p, _)| library::relative(target, p))
        .collect::<Result<Vec<_>>>()?;
    let mut paths = BTreeMap::new();
    let mut meta = BTreeMap::new();
    let old_meta = metadata(target)?;
    let new_meta = metadata(source)?;
    for path in &destination_paths {
        let updated = remap(path, moves);
        let key = f::fold(&updated);
        if paths.insert(key.clone(), updated).is_some() {
            bail!("Case-colliding USB disc names");
        }
        meta.insert(
            key,
            old_meta.get(&f::fold(path)).cloned().unwrap_or_default(),
        );
    }
    for path in source_paths
        .iter()
        .filter(|p| selected.contains(&f::fold(p)))
    {
        let key = f::fold(path);
        paths.insert(key.clone(), path.clone());
        meta.insert(key.clone(), new_meta.get(&key).cloned().unwrap_or_default());
    }
    if !selected.iter().all(|p| paths.contains_key(p)) {
        bail!("Selected game disappeared; reload the game list");
    }

    // library.3di: the paths; metadata.3di: each path and its metadata.
    let mut payload = vec![];
    for path in paths.values() {
        string(&mut payload, path)?;
    }
    let mut out = BTreeMap::from([(
        ".ohmy3do/library.3di".into(),
        packed(
            b"OM3DLIB1",
            b"END3DL01",
            paths.len(),
            payload,
            9,
            128 * 1024,
        )?,
    )]);
    let mut payload = vec![];
    for (key, path) in &paths {
        string(&mut payload, path)?;
        for field in &meta[key] {
            string(&mut payload, field)?;
        }
    }
    out.insert(
        ".ohmy3do/metadata.3di".into(),
        packed(
            b"OM3DMET1",
            b"END3DM01",
            paths.len(),
            payload,
            16,
            192 * 1024,
        )?,
    );

    // The cover and preview indexes: the drive's entries for discs that
    // stay, and the library's for the selected discs.
    let retained: BTreeSet<_> = destination_paths
        .iter()
        .map(|p| f::fold(&remap(p, moves)))
        .collect();
    for folder in ["covers", "previews"] {
        let old = library::read_drive_media_index(target, folder)?;
        let new = library::read_media_index(source, folder)?;
        let mut map = BTreeMap::new();
        for (path, name) in old {
            let path = f::fold(&remap(&path, moves));
            if retained.contains(&path) {
                map.insert(path, name);
            }
        }
        for (path, name) in new {
            if selected.contains(&path) {
                map.insert(path, name);
            }
        }
        // Keep an existing association if staging has no replacement media.
        out.insert(
            format!(".ohmy3do/{folder}/index.3di"),
            library::beside_index(&library::media_index(&map)?)?,
        );
    }
    Ok(out)
}
