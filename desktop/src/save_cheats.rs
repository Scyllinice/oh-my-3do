//! Shared, source-verified save-file offsets and presets. Never runtime RAM codes.
use crate::save_archive::Entry;
use anyhow::Context;
use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
    sync::{OnceLock, RwLock},
};
pub const JSON_PATH: &str = ".ohmy3do/save-cheats.json";
pub const BINARY_PATH: &str = ".ohmy3do/save-cheats.3db";
pub const MAX_JSON: usize = 256 * 1024;
pub const MAX_BINARY: usize = 65536;
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Field {
    pub label: String,
    pub offset: usize,
    pub min: u32,
    pub max: u32,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Preset {
    pub label: String,
    pub field: usize,
    pub value: u32,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Signature {
    pub offset: usize,
    pub value: u32,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Checksum {
    pub algorithm: String,
    pub offset: usize,
    pub seed: u32,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub encoding: String,
    pub id: String,
    pub game: String,
    pub filename: String,
    pub size: usize,
    pub source: String,
    pub notes: String,
    pub signature: Signature,
    pub checksum: Checksum,
    pub fields: Vec<Field>,
    pub presets: Vec<Preset>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Database {
    pub version: u32,
    pub profiles: Vec<Profile>,
}
pub fn profiles() -> &'static [Profile] {
    static DB: OnceLock<Database> = OnceLock::new();
    &DB.get_or_init(|| {
        serde_json::from_str(include_str!("../../data/save-cheats.json"))
            .expect("Invalid built-in save database; run tools/build-save-cheats.py --check")
    })
    .profiles
}
fn word(entry: &Entry, offset: usize) -> Option<u32> {
    Some(u32::from_be_bytes(
        entry
            .data
            .get(offset..offset.checked_add(4)?)?
            .try_into()
            .ok()?,
    ))
}
impl Profile {
    fn sum(&self, entry: &Entry) -> Option<u32> {
        (0..self.checksum.offset)
            .step_by(4)
            .try_fold(self.checksum.seed, |sum, offset| {
                Some(sum.wrapping_add(word(entry, offset)?))
            })
    }
    pub fn matches(&self, entry: &Entry) -> bool {
        entry.name == self.filename.as_bytes()
            && entry.data.len() == self.size
            && word(entry, self.signature.offset) == Some(self.signature.value)
            && self
                .sum(entry)
                .is_some_and(|sum| word(entry, self.checksum.offset) == Some(sum))
            && self
                .fields
                .iter()
                .all(|f| word(entry, f.offset).is_some_and(|v| (f.min..=f.max).contains(&v)))
    }
    pub fn value(&self, entry: &Entry, field: usize) -> Option<u32> {
        word(entry, self.fields.get(field)?.offset)
    }
    pub fn set(&self, entry: &mut Entry, field: usize, value: u32) -> Result<()> {
        ensure!(
            self.matches(entry),
            "Unsupported save layout, value or checksum"
        );
        let f = self
            .fields
            .get(field)
            .ok_or_else(|| anyhow::anyhow!("Unknown save field"))?;
        ensure!(
            (f.min..=f.max).contains(&value),
            "Value is outside the verified range"
        );
        entry.data[f.offset..f.offset + 4].copy_from_slice(&value.to_be_bytes());
        let sum = self.sum(entry).unwrap();
        entry.data[self.checksum.offset..self.checksum.offset + 4]
            .copy_from_slice(&sum.to_be_bytes());
        Ok(())
    }
    pub fn apply_preset(&self, entry: &mut Entry, preset: usize) -> Result<()> {
        let p = self
            .presets
            .get(preset)
            .ok_or_else(|| anyhow::anyhow!("Unknown save preset"))?;
        self.set(entry, p.field, p.value)
    }
}
static ACTIVE: RwLock<Option<Database>> = RwLock::new(None);
pub fn activate(db: Option<Database>) {
    *ACTIVE.write().unwrap() = db;
}
pub fn find(entry: &Entry) -> Option<Profile> {
    let active = ACTIVE.read().unwrap();
    let candidates = active
        .as_ref()
        .map(|db| db.profiles.as_slice())
        .unwrap_or_else(|| profiles());
    candidates.iter().find(|p| p.matches(entry)).cloned()
}

fn ascii(value: &str, max: usize) -> bool {
    !value.is_empty() && value.len() <= max && value.bytes().all(|b| (32..127).contains(&b))
}
impl Database {
    pub fn bundled() -> Self {
        Self::parse(include_bytes!("../../data/save-cheats.json"))
            .expect("Invalid bundled cheat database")
    }
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        ensure!(bytes.len() <= MAX_JSON, "Database JSON exceeds 256 KiB");
        let db: Self = serde_json::from_slice(bytes)?;
        db.validate()?;
        Ok(db)
    }
    pub fn read(path: &Path) -> Result<Self> {
        let mut bytes = Vec::new();
        fs::File::open(path)?
            .take((MAX_JSON + 1) as u64)
            .read_to_end(&mut bytes)?;
        Self::parse(&bytes)
    }
    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.version == 1 && !self.profiles.is_empty() && self.profiles.len() <= 128,
            "Unsupported database version or profile count"
        );
        let mut ids = HashSet::new();
        let mut layouts = HashSet::new();
        for p in &self.profiles {
            ensure!(
                ascii(&p.id, 63) && ids.insert(&p.id) && ascii(&p.game, 31),
                "Invalid or duplicate profile ID/game"
            );
            ensure!(
                ascii(&p.filename, 31)
                    && p.filename != "."
                    && p.filename != ".."
                    && !p.filename.contains(['/', '\\', ':']),
                "Invalid save filename"
            );
            ensure!(
                (8..=32768).contains(&p.size) && p.size % 4 == 0 && p.encoding == "u32be",
                "Unsupported size or encoding"
            );
            ensure!(
                p.checksum.algorithm == "sum32be"
                    && p.checksum.offset == p.size - 4
                    && p.signature.offset % 4 == 0
                    && p.signature.offset < p.checksum.offset,
                "Unsupported checksum or signature offset"
            );
            ensure!(
                layouts.insert((&p.filename, p.size, p.signature.offset, p.signature.value)),
                "Duplicate save layout"
            );
            ensure!(
                p.source.len() <= 512
                    && p.source.starts_with("https://")
                    && !p.notes.is_empty()
                    && p.notes.len() <= 2048,
                "Provide format provenance and notes"
            );
            ensure!(
                !p.fields.is_empty()
                    && p.fields.len() <= 32
                    && !p.presets.is_empty()
                    && p.presets.len() <= 32,
                "Each profile needs 1–32 fields and presets"
            );
            let mut offsets = HashSet::new();
            offsets.insert(p.signature.offset);
            for f in &p.fields {
                ensure!(
                    ascii(&f.label, 25)
                        && f.offset % 4 == 0
                        && f.offset < p.checksum.offset
                        && offsets.insert(f.offset)
                        && f.min <= f.max,
                    "Invalid field label, offset, overlap or range"
                );
            }
            for preset in &p.presets {
                ensure!(ascii(&preset.label, 34), "Invalid preset label");
                let field = p
                    .fields
                    .get(preset.field)
                    .context("Preset refers to an unknown field")?;
                ensure!(
                    (field.min..=field.max).contains(&preset.value),
                    "Preset is outside its field range"
                );
            }
        }
        ensure!(
            self.binary_size() <= MAX_BINARY,
            "Console database exceeds 64 KiB"
        );
        Ok(())
    }
    fn binary_size(&self) -> usize {
        24 + self
            .profiles
            .iter()
            .map(|p| 160 + p.fields.len() * 40 + p.presets.len() * 48)
            .sum::<usize>()
    }
    pub fn json(&self) -> Result<Vec<u8>> {
        self.validate()?;
        let bytes = serde_json::to_vec_pretty(self)?;
        ensure!(bytes.len() <= MAX_JSON, "Database JSON exceeds 256 KiB");
        Ok(bytes)
    }
    pub fn binary(&self) -> Result<Vec<u8>> {
        self.validate()?;
        let mut bytes = Vec::with_capacity(self.binary_size());
        bytes.extend_from_slice(b"OM3DCHT1");
        bytes.extend_from_slice(&[0; 16]);
        fn num(out: &mut Vec<u8>, n: u32) {
            out.extend_from_slice(&n.to_be_bytes());
        }
        fn text(out: &mut Vec<u8>, s: &str, n: usize) {
            out.extend_from_slice(s.as_bytes());
            out.resize(out.len() + n - s.len(), 0);
        }
        for p in &self.profiles {
            for n in [
                160 + p.fields.len() * 40 + p.presets.len() * 48,
                p.size,
                p.signature.offset,
                p.signature.value as usize,
                p.checksum.offset,
                p.checksum.seed as usize,
                p.fields.len(),
                p.presets.len(),
            ] {
                num(&mut bytes, n as u32);
            }
            text(&mut bytes, &p.id, 64);
            text(&mut bytes, &p.game, 32);
            text(&mut bytes, &p.filename, 32);
            for f in &p.fields {
                text(&mut bytes, &f.label, 28);
                num(&mut bytes, f.offset as u32);
                num(&mut bytes, f.min);
                num(&mut bytes, f.max);
            }
            for p in &p.presets {
                text(&mut bytes, &p.label, 36);
                num(&mut bytes, p.field as u32);
                num(&mut bytes, p.value);
                num(&mut bytes, 0);
            }
        }
        let length = bytes.len() as u32;
        let crc = crc32fast::hash(&bytes[24..]);
        bytes[8..12].copy_from_slice(&length.to_be_bytes());
        bytes[12..16].copy_from_slice(&(self.profiles.len() as u32).to_be_bytes());
        bytes[16..20].copy_from_slice(&crc.to_be_bytes());
        Ok(bytes)
    }
}
/// Publish the editable source and console file as one journalled operation.
pub fn install(root: &Path, db: &Database, cache: &Path) -> Result<PathBuf> {
    let json = db.json()?;
    let binary = db.binary()?;
    let _lock = crate::library::lock_library(root, cache)?;
    let mut tx = crate::library::Journal::new(root, cache)?;
    let result = (|| -> Result<()> {
        tx.write(JSON_PATH, &json)?;
        tx.write(BINARY_PATH, &binary)?;
        ensure!(
            fs::read(crate::fsutil::safe(root, JSON_PATH)?)? == json
                && fs::read(crate::fsutil::safe(root, BINARY_PATH)?)? == binary,
            "Cheat database readback failed"
        );
        tx.finish()
    })();
    if let Err(error) = result {
        tx.undo()
            .context(format!("{error:#}; database rollback failed"))?;
        return Err(error);
    }
    Ok(tx.path)
}
pub fn ensure_database(root: &Path, tx: &mut crate::library::Journal) -> Result<()> {
    let json = crate::fsutil::safe(root, JSON_PATH)?;
    let binary = crate::fsutil::safe(root, BINARY_PATH)?;
    ensure!(!json.exists() || json.is_file(), "Database JSON path is occupied by a directory");
    ensure!(!binary.exists() || binary.is_file(), "Console database path is occupied by a directory");
    if !binary.exists() {
        let db = if json.exists() {
            Database::read(&json)?
        } else {
            Database::bundled()
        };
        if !json.exists() {
            tx.write(JSON_PATH, &db.json()?)?;
        }
        tx.write(BINARY_PATH, &db.binary()?)?;
    }
    Ok(())
}
/// Preserve existing/custom profile IDs; add new upstream profiles only.
pub fn merge_missing(current: &Database, upstream: &Database) -> Result<Database> {
    current.validate()?;
    upstream.validate()?;
    let mut merged = current.clone();
    for p in &upstream.profiles {
        if !merged.profiles.iter().any(|old| {
            old.id == p.id
                || (old.filename == p.filename
                    && old.size == p.size
                    && old.signature.offset == p.signature.offset
                    && old.signature.value == p.signature.value)
        }) {
            merged.profiles.push(p.clone());
        }
    }
    merged.validate()?;
    Ok(merged)
}
pub fn download() -> Result<Database> {
    let client = reqwest::blocking::Client::builder()
        .timeout(std::time::Duration::from_secs(20))
        .user_agent("OhMy3DO-Library")
        .build()?;
    let response = client
        .get("https://raw.githubusercontent.com/Scyllinice/oh-my-3do/main/data/save-cheats.json")
        .send()?;
    ensure!(
        response.status() != reqwest::StatusCode::NOT_FOUND,
        "Official database has not been published yet; local definitions were not changed"
    );
    let response = response.error_for_status()?;
    let mut bytes = Vec::new();
    response
        .take((MAX_JSON + 1) as u64)
        .read_to_end(&mut bytes)?;
    Database::parse(&bytes)
}
pub fn save_json(
    path: &Path,
    db: &Database,
    expected: Option<&[u8]>,
    cache: &Path,
) -> Result<PathBuf> {
    let bytes = db.json()?;
    let current = source_bytes(path)?;
    ensure!(
        current.as_deref() == expected,
        "Database changed outside the editor; reopen it before saving"
    );
    let parent = path.parent().context("Database directory missing")?;
    let name = path
        .file_name()
        .and_then(|s| s.to_str())
        .context("Unsupported database filename")?;
    let mut tx = crate::library::Journal::new(parent, cache)?;
    let result = (|| -> Result<()> {
        tx.write(name, &bytes)?;
        ensure!(fs::read(path)? == bytes, "Database readback failed");
        tx.finish()
    })();
    if let Err(error) = result {
        tx.undo().context(format!("{error:#}; rollback failed"))?;
        return Err(error);
    }
    Ok(tx.path)
}

pub fn for_backup(path: &Path) -> Result<Option<Database>> {
    for parent in path.ancestors().skip(1) {
        let candidate = if parent.file_name().is_some_and(|s| s == ".ohmy3do") {
            parent.join("save-cheats.json")
        } else {
            parent.join(JSON_PATH)
        };
        if candidate.exists() {
            return Database::read(&candidate).map(Some);
        }
    }
    Ok(None)
}

pub fn source_bytes(path: &Path) -> Result<Option<Vec<u8>>> {
    match fs::symlink_metadata(path) {
        Ok(meta) => {
            ensure!(
                meta.is_file() && !crate::fsutil::is_link(path),
                "Choose a regular database file"
            );
            let mut bytes = Vec::new();
            fs::File::open(path)?
                .take((MAX_JSON + 1) as u64)
                .read_to_end(&mut bytes)?;
            ensure!(bytes.len() <= MAX_JSON, "Database JSON exceeds 256 KiB");
            Ok(Some(bytes))
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.into()),
    }
}
