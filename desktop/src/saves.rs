//! Naming saves. The console's NVRAM lists saves only by file name, so the
//! Save Manager shows the game each belongs to from save-names.txt, which
//! this module writes: it looks through each disc's programs for the NVRAM
//! file names they use ("/NVRAM/..."). The output and cache match
//! tools/index-media.py.

use crate::{fsutil as f, Job};
use anyhow::Result;
use regex::bytes::Regex;
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, BTreeSet, HashMap},
    fs,
    io::{Read, Seek, SeekFrom},
    path::Path,
};

/// A disc block.
const BLOCK: u64 = 2048;

/// The largest save-names.txt (the size of the console's buffer for it).
pub const SAVE_NAMES_BYTES: usize = 32768;

/// The big-endian 32-bit number at `at`.
fn word(data: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(data[at..at + 4].try_into().unwrap())
}

/// (byte offset, size) of every file on a 3DO (Opera) disc image.
pub fn opera_files(file: &mut fs::File) -> Result<Vec<(u64, u64)>> {
    opera_files_with_job(file, &Job::default())
}
/// `opera_files`, stopping if the job is canceled. A disc that is not an
/// Opera image has no files. Damaged directories are read as far as they
/// make sense, with limits so a bad image cannot loop forever.
fn opera_files_with_job(file: &mut fs::File, job: &Job) -> Result<Vec<(u64, u64)>> {
    job.check()?;
    let mut header = [0u8; BLOCK as usize];
    let mut out = vec![];
    if file.read_exact(&mut header).is_err() || &header[..7] != b"\x01ZZZZZ\x01" {
        return Ok(out);
    }
    // The volume header (record type 1, five 'Z's, version 1) gives the
    // root directory's first block and length in blocks. Directories are
    // visited as found, each once.
    let mut pending = vec![(word(&header, 100) as u64, word(&header, 88) as u64)];
    let mut seen = BTreeSet::new();
    while let Some((start, count)) = pending.pop() {
        job.check()?;
        if !seen.insert(start) || seen.len() > 4096 {
            continue;
        }
        for block in 0..count.min(64) {
            job.check()?;
            let mut data = [0u8; BLOCK as usize];
            file.seek(SeekFrom::Start((start + block) * BLOCK))?;
            if file.read_exact(&mut data).is_err() {
                break;
            }
            // A directory block: offset 12 is where its entries end, 16
            // where they start. Each entry is 72 bytes plus 4 per extra
            // copy ("avatar") of the file: flags, ID, type, block size,
            // byte size, block count, ..., the index of the last avatar
            // (64) and the first avatar's block (68).
            let end = (word(&data, 12) as usize).min(BLOCK as usize);
            let mut pos = word(&data, 16) as usize;
            while pos >= 20 && pos + 72 <= end {
                let flags = word(&data, pos);
                let (block_size, size, blocks) = (
                    word(&data, pos + 12),
                    word(&data, pos + 16),
                    word(&data, pos + 20),
                );
                let (last, avatar) = (word(&data, pos + 64), word(&data, pos + 68) as u64);
                if last > 15 {
                    break;
                }
                // Low byte 7: a directory, read later.
                if flags & 0xff == 7 {
                    pending.push((avatar, blocks as u64));
                } else {
                    let unit = if block_size == 0 {
                        BLOCK
                    } else {
                        block_size as u64
                    };
                    out.push((avatar * unit, size as u64));
                }
                pos += 72 + 4 * last as usize;
                // The top flag bits mark the directory's (or block's) last entry.
                if flags & 0xc000_0000 != 0 {
                    break;
                }
            }
        }
    }
    Ok(out)
}

/// The NVRAM files a disc's programs name: "=FILE" for an exact name,
/// "*PREFIX" for a pattern like "FILE%d" (several numbered saves).
pub fn disc_save_names(path: &Path) -> Result<Vec<String>> {
    disc_save_names_with_job(path, &Job::default())
}
/// `disc_save_names`, stopping if the job is canceled. Only files of 64
/// bytes to 3 MiB (programs and data, not video) are searched.
fn disc_save_names_with_job(path: &Path, job: &Job) -> Result<Vec<String>> {
    job.check()?;
    let saves = Regex::new(r"(?i)/nvram/([\x21-\x7e][\x20-\x7e]{0,30})").unwrap();
    let plain = |s: &str| {
        s.chars()
            .all(|c| c.is_ascii_alphanumeric() || "_.- []'&+!()".contains(c))
    };
    let mut file = fs::File::open(path)?;
    let mut found = BTreeSet::new();
    for (offset, size) in opera_files_with_job(&mut file, job)? {
        job.check()?;
        if !(64..=3 * 1024 * 1024).contains(&size) {
            continue;
        }
        let mut data = vec![0; size as usize];
        file.seek(SeekFrom::Start(offset))?;
        let mut readable = true;
        for chunk in data.chunks_mut(64 * 1024) {
            job.check()?;
            if file.read_exact(chunk).is_err() {
                readable = false;
                break;
            }
        }
        if !readable {
            continue;
        }
        job.check()?;
        for m in saves.captures_iter(&data) {
            let name = String::from_utf8_lossy(&m[1]).into_owned();
            if let Some((prefix, _)) = name.split_once('%') {
                if prefix.len() >= 3 && plain(prefix) {
                    found.insert(format!("*{prefix}"));
                }
            } else if name.len() >= 2 && plain(&name) {
                found.insert(format!("={name}"));
            }
        }
    }
    Ok(found.into_iter().collect())
}

/// A list title without trailing (Region) / (Disc 1) groups, as the menu shows it.
pub fn short_title(title: &str) -> String {
    let group = regex::Regex::new(r" \([^()]*\)$").unwrap();
    let mut title = regex::Regex::new(r"(?i)\.(iso|cue)$")
        .unwrap()
        .replace(title, "")
        .into_owned();
    loop {
        let shorter = group.replace(&title, "").into_owned();
        if shorter == title || shorter.is_empty() {
            break;
        }
        title = shorter;
    }
    use unicode_normalization::UnicodeNormalization;
    let ascii: String = title
        .nfkd()
        .filter_map(|c| {
            c.is_ascii()
                .then_some(if c.is_ascii_control() { ' ' } else { c })
        })
        .collect();
    ascii
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(96)
        .collect()
}

/// What `save_names_text` made.
pub struct SaveNamesOutput {
    /// save-names.txt.
    pub text: String,
    /// How many save names it found.
    pub count: usize,
    /// The new per-disc cache, for `.ohmy3do/save-names-cache.json`.
    pub cache: Vec<u8>,
}

/// Makes save-names.txt for the drive at `root`: a line per save name ("=" or
/// "*" form, a tab, and the games using it, joined with " / "). `discs` are
/// the drive's disc paths and `titles` their titles by case-insensitive
/// path. Discs already read (same name, size and date) come from the cache
/// in .ohmy3do/save-names-cache.json instead of being read again. Names that
/// do not fit the console's buffer are left out.
pub fn save_names_text(
    root: &Path,
    discs: &[String],
    titles: &HashMap<String, String>,
    job: &Job,
) -> Result<SaveNamesOutput> {
    let cache_path = f::safe(root, ".ohmy3do/save-names-cache.json")?;
    let cache: Value = fs::read(&cache_path)
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .filter(|v: &Value| v["schema"] == 2)
        .unwrap_or(Value::Null);
    let mut stamps = serde_json::Map::new();
    let mut rows: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let total = discs
        .iter()
        .filter(|d| d.to_ascii_lowercase().ends_with(".iso"))
        .count() as u64;
    let mut completed = 0;
    job.report(0, total, "Reading save-name information");
    for disc in discs {
        if !disc.to_ascii_lowercase().ends_with(".iso") {
            continue;
        }
        job.check()?;
        let path = f::safe(root, disc)?;
        let (size, modified) = f::stamp(&path)?;
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        let stamp = format!("{name}|{size}|{modified}");
        let names: Vec<String> = match cache["discs"][&stamp].as_array() {
            Some(list) => list
                .iter()
                .filter_map(|v| v.as_str().map(str::to_owned))
                .collect(),
            None => {
                job.say(format!(
                    "Reading save names from {disc} ({}/{total})",
                    completed + 1
                ));
                let result = disc_save_names_with_job(&path, job);
                job.check()?;
                result.unwrap_or_default()
            }
        };
        let title = short_title(
            titles
                .get(&f::fold(disc))
                .map(String::as_str)
                .unwrap_or(&name),
        );
        for n in &names {
            let row = rows.entry(n.clone()).or_default();
            if !title.is_empty() && !row.contains(&title) {
                row.push(title.clone());
            }
        }
        stamps.insert(stamp, json!(names));
        completed += 1;
        job.report(completed, total, "Reading save-name information");
    }

    // Sorted by name, ignoring case and the "=" or "*".
    let mut keys: Vec<&String> = rows.keys().collect();
    keys.sort_by_key(|n| (n[1..].to_ascii_lowercase(), n.to_string()));
    let mut text = String::from("# Save names read from the discs by index-media.py\n");
    let mut dropped = 0;
    for key in &keys {
        let line = format!(
            "{key}\t{}\n",
            rows[*key].join(" / ").chars().take(63).collect::<String>()
        );
        if text.len() + line.len() >= SAVE_NAMES_BYTES {
            dropped += 1;
            continue;
        }
        text.push_str(&line);
    }
    if dropped > 0 {
        job.say(format!(
            "Save names: {dropped} names left out to stay within {SAVE_NAMES_BYTES} bytes"
        ));
    }
    Ok(SaveNamesOutput {
        text,
        count: rows.len(),
        cache: serde_json::to_vec(&json!({"schema": 2, "discs": stamps}))?,
    })
}

/// The game a save file belongs to, from the list of known saves shipped
/// with the app (data/save-names.txt, also built into the menu). An exact
/// name wins over a prefix; of prefixes, the longest wins.
pub fn known_title(file: &str) -> Option<&'static str> {
    let mut best = None;
    let mut longest = 0;
    for line in include_str!("../../data/save-names.txt").lines() {
        let Some((key, title)) = line.split_once('\t') else {
            continue;
        };
        if key.starts_with('=') && key[1..].eq_ignore_ascii_case(file) {
            return Some(title);
        }
        if key.starts_with('*')
            && key.len() > longest
            && file
                .get(..key.len() - 1)
                .is_some_and(|s| s.eq_ignore_ascii_case(&key[1..]))
        {
            best = Some(title);
            longest = key.len()
        }
    }
    best
}
