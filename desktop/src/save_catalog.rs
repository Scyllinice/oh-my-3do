//! Searchable occupied-slot catalog shared with the console. Archives remain authoritative.
use crate::{
    backup::{self, Labels},
    fsutil as f,
    library::{self, Journal},
    save_archive::Archive,
    saves, Job,
};
use anyhow::{ensure, Context, Result};
use std::{
    collections::BTreeMap,
    fs,
    io::Read,
    path::{Path, PathBuf},
};
pub const ROOT: &str = "slot-catalog.3di";
pub const RECOVERY: u32 = 1_000_000_000;
#[derive(Clone, Debug)]
pub struct Slot {
    pub id: u32,
    pub path: PathBuf,
    pub label: String,
    pub files: Vec<String>,
    pub valid: bool,
    pub aliases: Vec<String>,
    length: u32,
    crc: u32,
    stamp: (u64, u128),
}
impl Slot {
    pub fn name(&self) -> String {
        let base = if self.id == RECOVERY {
            "Recovery".into()
        } else if self.id == RECOVERY + 1 {
            "Recovery 2".into()
        } else {
            format!("Slot {}", self.id)
        };
        if self.label.is_empty() {
            base
        } else {
            format!("{base} — {}", self.label)
        }
    }
    pub fn matches(&self, query: &str) -> bool {
        let mut haystack = format!(
            "{} {} {}",
            self.name(),
            self.path.file_name().unwrap().to_string_lossy(),
            self.files.join(" ")
        );
        for file in &self.files {
            if let Some(title) = saves::known_title(file) {
                haystack.push(' ');
                haystack.push_str(title)
            }
        }
        haystack.push_str(&self.aliases.join(" "));
        let haystack = haystack.to_ascii_lowercase();
        query
            .split_whitespace()
            .all(|q| haystack.contains(&q.to_ascii_lowercase()))
    }
    pub fn games(&self) -> String {
        if !self.valid {
            return "Invalid backup — inspect before restoring".into();
        }
        if self.files.is_empty() {
            return "Empty archive".into();
        }
        self.files
            .iter()
            .map(|n| saves::known_title(n).unwrap_or(n))
            .collect::<Vec<_>>()
            .join(", ")
    }
    pub fn node_bytes(&self, next: u32) -> Vec<u8> {
        let mut b = vec![0; 68 + self.files.len() * 32];
        let size = b.len() as u32;
        b[..8].copy_from_slice(b"OM3DSLT1");
        put(&mut b, 8, size);
        put(&mut b, 12, self.id);
        put(&mut b, 16, next);
        put(&mut b, 24, if self.valid { 1 } else { 2 });
        put(&mut b, 28, self.length);
        put(&mut b, 32, self.crc);
        b[36..36 + self.label.len()].copy_from_slice(self.label.as_bytes());
        put(&mut b, 64, self.files.len() as u32);
        for (i, name) in self.files.iter().enumerate() {
            for (j, c) in name.chars().enumerate() {
                b[68 + i * 32 + j] = c as u8
            }
        }
        seal(&mut b);
        b
    }
}
fn put(b: &mut [u8], at: usize, value: u32) {
    b[at..at + 4].copy_from_slice(&value.to_be_bytes())
}
fn word(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(b[at..at + 4].try_into().unwrap())
}
fn seal(b: &mut [u8]) {
    put(b, 20, 0);
    let crc = crc32fast::hash(b);
    put(b, 20, crc)
}
pub fn root_bytes(head: u32) -> Vec<u8> {
    let mut b = vec![0; 28];
    b[..8].copy_from_slice(b"OM3DSLC1");
    put(&mut b, 8, 28);
    put(&mut b, 24, head);
    seal(&mut b);
    b
}
fn id(path: &Path) -> Option<u32> {
    let name = path.file_name()?.to_str()?.to_ascii_lowercase();
    if name == "recovery.3ds" {
        return Some(RECOVERY);
    }
    if name == "recovery2.3ds" {
        return Some(RECOVERY + 1);
    }
    backup::slot(path)?;
    name.strip_prefix("slot")?
        .strip_suffix(".3ds")?
        .parse()
        .ok()
}
fn node_name(id: u32) -> String {
    if id == RECOVERY {
        "recovery.3si".into()
    } else if id == RECOVERY + 1 {
        "recovery2.3si".into()
    } else {
        format!("slot{id:02}.3si")
    }
}
fn mapping(directory: &Path) -> Vec<(String, String, bool)> {
    let Some(support) = directory.parent() else {
        return vec![];
    };
    ["saves-learned.txt", "save-names.txt"]
        .iter()
        .flat_map(|name| {
            let text = fs::File::open(support.join(name))
                .ok()
                .and_then(|file| {
                    let mut b = Vec::new();
                    file.take(16385).read_to_end(&mut b).ok()?;
                    (b.len() <= 16384).then_some(b)
                })
                .unwrap_or_default();
            String::from_utf8_lossy(&text)
                .lines()
                .filter_map(|line| {
                    let (key, title) = line.split_once('\t')?;
                    if key.len() < 2 || !key.starts_with(['=', '*']) {
                        return None;
                    }
                    Some((key[1..].to_owned(), title.to_owned(), key.starts_with('*')))
                })
                .collect::<Vec<_>>()
        })
        .collect()
}
pub fn scan(directory: &Path, job: &Job) -> Result<Vec<Slot>> {
    let directory = f::root(directory)?;
    let mappings = mapping(&directory);
    let mut slots = BTreeMap::new();
    for entry in fs::read_dir(&directory)? {
        job.check()?;
        let entry = entry?;
        if !entry.file_type()?.is_file() {
            continue;
        }
        let path = entry.path();
        let Some(id) = id(&path) else { continue };
        let stamp = f::stamp(&path)?;
        if stamp.0 == 0 {
            continue;
        }
        ensure!(
            !slots.contains_key(&id),
            "Duplicate slot filenames for slot {id}"
        );
        let label = if let Some((path, i)) = backup::slot(&path) {
            Labels::read(&path)
                .with_context(|| format!("Read slot labels {}", path.display()))?
                .0
                 .0[i]
                .clone()
        } else {
            String::new()
        };
        let (valid, files, length, crc) = match Archive::read_with_bytes(&path) {
            Ok((archive, b)) => (
                true,
                archive
                    .entries
                    .iter()
                    .map(|e| e.label())
                    .collect::<Vec<_>>(),
                word(&b, 8),
                word(&b, 16),
            ),
            Err(_) => (false, vec![], 0, 0),
        };
        ensure!(
            f::stamp(&path)? == stamp,
            "Backup changed while scanning: {}",
            path.display()
        );
        let mut aliases = Vec::new();
        for file in &files {
            for (key, title, prefix) in &mappings {
                if file.eq_ignore_ascii_case(key)
                    || (*prefix
                        && file
                            .get(..key.len())
                            .is_some_and(|s| s.eq_ignore_ascii_case(key)))
                {
                    aliases.push(title.clone())
                }
            }
        }
        slots.insert(
            id,
            Slot {
                id,
                path,
                label,
                files,
                valid,
                aliases,
                length,
                crc,
                stamp,
            },
        );
    }
    Ok(slots.into_values().collect())
}
/// Write nodes first, root last, using the caller's rollback transaction.
pub fn write(slots: &[Slot], tx: &mut Journal, prefix: &str, job: &Job) -> Result<()> {
    let mut next = 0;
    for slot in slots {
        job.check()?;
        ensure!(
            f::stamp(&slot.path)? == slot.stamp,
            "Backup changed; refresh the catalog again"
        );
        let name = format!("{prefix}{}", node_name(slot.id));
        let b = slot.node_bytes(next);
        tx.write(&name, &b)?;
        ensure!(
            fs::read(tx.root.join(&name))? == b,
            "Catalog verification failed"
        );
        next = slot.id;
    }
    let b = root_bytes(next);
    let name = format!("{prefix}{ROOT}");
    tx.write(&name, &b)?;
    ensure!(
        fs::read(tx.root.join(name))? == b,
        "Catalog root verification failed"
    );
    Ok(())
}
pub fn refresh(directory: &Path, cache: &Path, job: &Job) -> Result<Vec<Slot>> {
    let directory = f::root(directory)?;
    let _lock = library::lock_library(&directory, cache)?;
    let slots = scan(&directory, job)?;
    let mut tx = Journal::new(&directory, cache)?;
    if let Err(error) = write(&slots, &mut tx, "", job).and_then(|_| tx.finish()) {
        tx.undo()
            .context("Catalog rollback failed; keep the transaction receipt")?;
        return Err(error);
    }
    Ok(slots)
}
