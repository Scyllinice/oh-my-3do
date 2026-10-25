//! The save cheat database: known layouts of particular games' save files,
//! and the values that can safely be changed in them (lives, level, and so
//! on), with ready-made presets.
//!
//! Every layout comes from a documented source and is checked before use:
//! a save must match the profile's file name, size, signature and checksum,
//! and every field must hold a value in its verified range, or it is not
//! touched. These are edits to save files only, never codes that patch
//! memory while a game runs.
//!
//! The database is kept as editable JSON (`save-cheats.json`), and a compact
//! binary copy (`save-cheats.3db`, see [`Database::binary`]) is written for
//! the console menu. The app ships with a built-in copy in
//! `data/save-cheats.json`.

use crate::save_archive::Entry;
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
    sync::{OnceLock, RwLock},
};

/// Where a drive or library keeps the editable database.
pub const JSON_PATH: &str = ".ohmy3do/save-cheats.json";

/// Where a drive keeps the console's binary copy.
pub const BINARY_PATH: &str = ".ohmy3do/save-cheats.3db";

/// The largest JSON database accepted.
pub const MAX_JSON: usize = 256 * 1024;

/// The largest binary database the console accepts.
pub const MAX_BINARY: usize = 65536;

/// Where the official database is published.
const DOWNLOAD_URL: &str =
    "https://raw.githubusercontent.com/Scyllinice/oh-my-3do/main/data/save-cheats.json";

/// Limits on a database's contents, matching the console's.
const MAX_PROFILES: usize = 128;
const MAX_FIELDS: usize = 160;
const MAX_PRESETS: usize = 32;
const MAX_SAVE_SIZE: usize = 32768;

/// Text lengths: the longest allowed, and the fixed space each takes in the
/// binary database (the longest plus at least one zero byte).
const ID_LENGTH: (usize, usize) = (63, 64);
const GAME_LENGTH: (usize, usize) = (31, 32);
const FILENAME_LENGTH: (usize, usize) = (31, 32);
const FIELD_LABEL_LENGTH: (usize, usize) = (25, 28);
const PRESET_LABEL_LENGTH: (usize, usize) = (34, 36);

/// Sizes in the binary database (see [`Database::binary`]).
const BINARY_HEADER_BYTES: usize = 24;
const BINARY_PROFILE_BYTES: usize = 176;
const BINARY_FIELD_BYTES: usize = 48;
const BINARY_PRESET_BYTES: usize = 48;
const BINARY_CHANGE_BYTES: usize = 8;

/// Fields and signatures are 4 bytes wide unless the profile says otherwise.
fn default_width() -> usize {
    4
}

/// A field uses all of its bits unless the profile gives a mask.
fn all_bits() -> u32 {
    u32::MAX
}

/// One more field a preset sets.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Change {
    /// The field's index in the profile.
    pub field: usize,
    pub value: u32,
}

/// A value in a save that can be changed.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Field {
    pub label: String,
    /// Where it is in the save, in bytes.
    pub offset: usize,
    /// The verified range of values.
    pub min: u32,
    pub max: u32,
    /// Its size in bytes: 1, 2 or 4 (big-endian).
    #[serde(default = "default_width")]
    pub width: usize,
    /// Which bits of those bytes hold it, for a value packed with others.
    /// The bits must be next to each other.
    #[serde(default = "all_bits")]
    pub mask: u32,
}

/// A named set of changes, such as "Max lives".
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Preset {
    /// Further fields to set, after the first.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub additional: Vec<Change>,
    pub label: String,
    /// The first field to set (its index), and its value.
    pub field: usize,
    pub value: u32,
}

/// A fixed value at a fixed place that identifies a game's save.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Signature {
    #[serde(default = "default_width")]
    pub width: usize,
    pub offset: usize,
    pub value: u32,
}

/// How a save is checksummed: "sum32be" (the sum of every big-endian 32-bit
/// word before `offset`, starting from `seed`, stored at `offset`) or "none".
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Checksum {
    pub algorithm: String,
    pub offset: usize,
    pub seed: u32,
}

/// One game's save layout.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    /// "u32be" or "be": big-endian, as the 3DO stores numbers.
    pub encoding: String,
    pub id: String,
    pub game: String,
    /// The save's file name on the console.
    pub filename: String,
    /// The save's exact size in bytes.
    pub size: usize,
    /// Where the layout was documented (an https link).
    pub source: String,
    pub notes: String,
    pub signature: Signature,
    pub checksum: Checksum,
    pub fields: Vec<Field>,
    pub presets: Vec<Preset>,
}

/// The whole database.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Database {
    pub version: u32,
    pub profiles: Vec<Profile>,
}

/// The built-in profiles, read once.
pub fn profiles() -> &'static [Profile] {
    static BUILT_IN: OnceLock<Database> = OnceLock::new();
    let database = BUILT_IN.get_or_init(|| {
        serde_json::from_str(include_str!("../../data/save-cheats.json"))
            .expect("Invalid built-in save database; run tools/build-save-cheats.py --check")
    });
    &database.profiles
}

/// Reads a big-endian number of `width` bytes (1, 2 or 4) at `offset` in a
/// save. None if the width is not allowed or the save is too short.
fn number(entry: &Entry, offset: usize, width: usize) -> Option<u32> {
    if ![1, 2, 4].contains(&width) {
        return None;
    }
    let end = offset.checked_add(width)?;
    let bytes = entry.data.get(offset..end)?;
    let mut value = 0u32;
    for byte in bytes {
        value = (value << 8) | *byte as u32;
    }
    Some(value)
}

/// Reads a big-endian 32-bit word at `offset` in a save.
fn word(entry: &Entry, offset: usize) -> Option<u32> {
    number(entry, offset, 4)
}

impl Profile {
    /// The "sum32be" checksum the save should have: the seed plus every
    /// 32-bit word before the checksum, wrapping on overflow.
    fn sum(&self, entry: &Entry) -> Option<u32> {
        let mut sum = self.checksum.seed;
        for offset in (0..self.checksum.offset).step_by(4) {
            sum = sum.wrapping_add(word(entry, offset)?);
        }
        Some(sum)
    }

    /// Whether a save is exactly this layout: right file name and size,
    /// signature in place, checksum correct, and every field in range.
    pub fn matches(&self, entry: &Entry) -> bool {
        let right_file = entry.name == self.filename.as_bytes() && entry.data.len() == self.size;
        if !right_file {
            return false;
        }

        let signature = number(entry, self.signature.offset, self.signature.width);
        if signature != Some(self.signature.value) {
            return false;
        }

        let checksum_correct = self.checksum.algorithm == "none"
            || self
                .sum(entry)
                .is_some_and(|sum| word(entry, self.checksum.offset) == Some(sum));
        if !checksum_correct {
            return false;
        }

        self.fields.iter().enumerate().all(|(index, field)| {
            self.value(entry, index)
                .is_some_and(|value| (field.min..=field.max).contains(&value))
        })
    }

    /// The current value of field number `field` in a save.
    pub fn value(&self, entry: &Entry, field: usize) -> Option<u32> {
        let field = self.fields.get(field)?;
        let raw = number(entry, field.offset, field.width)?;
        Some((raw & field.mask) >> field.mask.trailing_zeros())
    }

    /// Writes `value` into field number `field`, leaving the other bits of
    /// its bytes as they were. The checksum is not updated.
    fn change(&self, entry: &mut Entry, field: usize, value: u32) {
        let field = &self.fields[field];
        let current = number(entry, field.offset, field.width).unwrap();
        let shift = field.mask.trailing_zeros();
        let next = (current & !field.mask) | ((value << shift) & field.mask);

        // The lowest `width` bytes of the big-endian number.
        let bytes = next.to_be_bytes();
        entry.data[field.offset..field.offset + field.width]
            .copy_from_slice(&bytes[4 - field.width..]);
    }

    /// Recalculates and stores the save's checksum, if it has one.
    fn repair(&self, entry: &mut Entry) {
        if self.checksum.algorithm != "none" {
            let sum = self.sum(entry).unwrap();
            let offset = self.checksum.offset;
            entry.data[offset..offset + 4].copy_from_slice(&sum.to_be_bytes());
        }
    }

    /// Sets field number `field` to `value` and fixes the checksum. Refuses a
    /// save that does not match this layout, or a value outside the field's
    /// verified range.
    pub fn set(&self, entry: &mut Entry, field: usize, value: u32) -> Result<()> {
        ensure!(
            self.matches(entry),
            "Unsupported save layout, value or checksum"
        );
        let field_info = self.fields.get(field).context("Unknown save field")?;
        ensure!(
            (field_info.min..=field_info.max).contains(&value),
            "Value is outside the verified range"
        );

        self.change(entry, field, value);
        self.repair(entry);
        Ok(())
    }

    /// Applies preset number `preset` and fixes the checksum. Every change is
    /// checked before any is made, so a bad preset changes nothing.
    pub fn apply_preset(&self, entry: &mut Entry, preset: usize) -> Result<()> {
        ensure!(
            self.matches(entry),
            "Unsupported save layout, value or checksum"
        );
        let preset = self.presets.get(preset).context("Unknown save preset")?;
        let changes = preset_changes(preset);

        for &(field, value) in &changes {
            let in_range = self
                .fields
                .get(field)
                .is_some_and(|field| (field.min..=field.max).contains(&value));
            ensure!(in_range, "Invalid preset change");
        }

        for (field, value) in changes {
            self.change(entry, field, value);
        }
        self.repair(entry);
        Ok(())
    }
}

/// Every (field index, value) a preset sets, the first one first.
fn preset_changes(preset: &Preset) -> Vec<(usize, u32)> {
    let mut changes = vec![(preset.field, preset.value)];
    for change in &preset.additional {
        changes.push((change.field, change.value));
    }
    changes
}

/// The database in use instead of the built-in one, if any (the person's
/// edited copy).
static ACTIVE: RwLock<Option<Database>> = RwLock::new(None);

/// Uses `database` for [`find`] from now on (None: the built-in one).
pub fn activate(database: Option<Database>) {
    *ACTIVE.write().unwrap() = database;
}

/// The profile a save matches, from the active database.
pub fn find(entry: &Entry) -> Option<Profile> {
    let active = ACTIVE.read().unwrap();
    let candidates = match active.as_ref() {
        Some(database) => database.profiles.as_slice(),
        None => profiles(),
    };
    candidates
        .iter()
        .find(|profile| profile.matches(entry))
        .cloned()
}

/// Whether text is 1 to `max` printable ASCII characters (all the console's
/// font can show).
fn is_short_ascii(value: &str, max: usize) -> bool {
    !value.is_empty() && value.len() <= max && value.bytes().all(|byte| (32..127).contains(&byte))
}

/// Reads a JSON file of at most [`MAX_JSON`] bytes (one more byte is read,
/// so an oversized file is noticed without loading it all).
fn read_limited(path: &Path) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    fs::File::open(path)?
        .take((MAX_JSON + 1) as u64)
        .read_to_end(&mut bytes)?;
    Ok(bytes)
}

impl Database {
    /// The database built into the app.
    pub fn bundled() -> Self {
        Self::parse(include_bytes!("../../data/save-cheats.json"))
            .expect("Invalid bundled cheat database")
    }

    /// Reads and checks a database from JSON.
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        ensure!(bytes.len() <= MAX_JSON, "Database JSON exceeds 256 KiB");
        let database: Self = serde_json::from_slice(bytes)?;
        database.validate()?;
        Ok(database)
    }

    /// Reads and checks a database from a JSON file.
    pub fn read(path: &Path) -> Result<Self> {
        Self::parse(&read_limited(path)?)
    }

    /// Checks everything the app and the console rely on: limits, unique
    /// ids and layouts, sensible offsets, fields that do not overlap each
    /// other or the signature, and presets that stay in range. Also checks
    /// the console's copy would fit in [`MAX_BINARY`].
    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.version == 1 && !self.profiles.is_empty() && self.profiles.len() <= MAX_PROFILES,
            "Unsupported database version or profile count"
        );

        let mut ids = HashSet::new();
        let mut layouts = HashSet::new();
        for profile in &self.profiles {
            ensure!(
                is_short_ascii(&profile.id, ID_LENGTH.0)
                    && ids.insert(&profile.id)
                    && is_short_ascii(&profile.game, GAME_LENGTH.0),
                "Invalid or duplicate profile ID/game"
            );
            validate_profile_file(profile)?;

            // Two profiles must not claim the same save.
            let layout = (
                &profile.filename,
                profile.size,
                profile.signature.offset,
                profile.signature.value,
            );
            ensure!(layouts.insert(layout), "Duplicate save layout");

            ensure!(
                profile.source.len() <= 512
                    && profile.source.starts_with("https://")
                    && !profile.notes.is_empty()
                    && profile.notes.len() <= 2048,
                "Provide format provenance and notes"
            );
            ensure!(
                !profile.fields.is_empty()
                    && profile.fields.len() <= MAX_FIELDS
                    && !profile.presets.is_empty()
                    && profile.presets.len() <= MAX_PRESETS,
                "Each profile needs 1–160 fields and 1–32 presets"
            );

            validate_fields(profile)?;
            validate_presets(profile)?;
        }

        ensure!(
            self.binary_size() <= MAX_BINARY,
            "Console database exceeds 64 KiB"
        );
        Ok(())
    }

    /// The size of the console's binary copy (see [`Database::binary`]).
    fn binary_size(&self) -> usize {
        let profiles: usize = self.profiles.iter().map(binary_profile_size).sum();
        BINARY_HEADER_BYTES + profiles
    }

    /// The database as pretty-printed JSON, checked first.
    pub fn json(&self) -> Result<Vec<u8>> {
        self.validate()?;
        let bytes = serde_json::to_vec_pretty(self)?;
        ensure!(bytes.len() <= MAX_JSON, "Database JSON exceeds 256 KiB");
        Ok(bytes)
    }

    /// Whether `bytes` is this database's console copy: the current format,
    /// or the older "OM3DCHT1" format that desktop 1.0 wrote, so an existing
    /// 1.0 drive is not reported as out of date.
    ///
    /// The old format had no field widths, masks or extra preset changes, so
    /// only a database that uses none of them can match it.
    pub fn matches_binary(&self, bytes: &[u8]) -> Result<bool> {
        let current = self.binary()?;
        if bytes == current {
            return Ok(true);
        }

        let representable_in_old_format = self.profiles.iter().all(|profile| {
            profile.signature.width == 4
                && profile.signature.offset.is_multiple_of(4)
                && profile.checksum.algorithm == "sum32be"
                && profile.fields.len() <= 32
                && profile.fields.iter().all(|field| {
                    field.width == 4 && field.mask == u32::MAX && field.offset.is_multiple_of(4)
                })
                && profile
                    .presets
                    .iter()
                    .all(|preset| preset.additional.is_empty())
        });
        if !bytes.starts_with(b"OM3DCHT1") || !representable_in_old_format {
            return Ok(false);
        }

        Ok(bytes == old_format_binary(&current, &self.profiles))
    }

    /// The console's binary copy ("OM3DCHT2"). Every number is a big-endian
    /// 32-bit word; text is zero-padded to a fixed size.
    ///
    /// ```text
    /// header (24 bytes):
    ///   "OM3DCHT2", total length, profile count, CRC-32 of everything after
    ///   the header, 0
    /// each profile (176 bytes):
    ///   record size (this profile with its fields and presets), save size,
    ///   signature offset, signature value, checksum offset, checksum seed,
    ///   field count, preset count, signature width, 1 if checksummed,
    ///   0, 0, id (64), game (32), file name (32)
    /// then each field (48 bytes):
    ///   label (28), offset, min, max, width, mask
    /// then each preset (48 bytes, plus 8 per extra change):
    ///   label (36), field, value, extra change count,
    ///   then (field, value) for each extra change
    /// ```
    pub fn binary(&self) -> Result<Vec<u8>> {
        self.validate()?;
        let mut bytes = Vec::with_capacity(self.binary_size());
        bytes.extend_from_slice(b"OM3DCHT2");
        bytes.extend_from_slice(&[0; 16]);

        for profile in &self.profiles {
            let numbers = [
                binary_profile_size(profile),
                profile.size,
                profile.signature.offset,
                profile.signature.value as usize,
                profile.checksum.offset,
                profile.checksum.seed as usize,
                profile.fields.len(),
                profile.presets.len(),
                profile.signature.width,
                usize::from(profile.checksum.algorithm != "none"),
                0,
                0,
            ];
            for number in numbers {
                push_word(&mut bytes, number as u32);
            }
            push_text(&mut bytes, &profile.id, ID_LENGTH.1);
            push_text(&mut bytes, &profile.game, GAME_LENGTH.1);
            push_text(&mut bytes, &profile.filename, FILENAME_LENGTH.1);

            for field in &profile.fields {
                push_text(&mut bytes, &field.label, FIELD_LABEL_LENGTH.1);
                push_word(&mut bytes, field.offset as u32);
                push_word(&mut bytes, field.min);
                push_word(&mut bytes, field.max);
                push_word(&mut bytes, field.width as u32);
                push_word(&mut bytes, field.mask);
            }

            for preset in &profile.presets {
                push_text(&mut bytes, &preset.label, PRESET_LABEL_LENGTH.1);
                push_word(&mut bytes, preset.field as u32);
                push_word(&mut bytes, preset.value);
                push_word(&mut bytes, preset.additional.len() as u32);
                for change in &preset.additional {
                    push_word(&mut bytes, change.field as u32);
                    push_word(&mut bytes, change.value);
                }
            }
        }

        finish_binary_header(&mut bytes, self.profiles.len());
        Ok(bytes)
    }
}

/// Checks a profile's save file: a plain file name, a size the console can
/// handle, a supported checksum, and a signature that fits before the
/// checksum.
fn validate_profile_file(profile: &Profile) -> Result<()> {
    ensure!(
        is_short_ascii(&profile.filename, FILENAME_LENGTH.0)
            && profile.filename != "."
            && profile.filename != ".."
            && !profile.filename.contains(['/', '\\', ':']),
        "Invalid save filename"
    );
    ensure!(
        (8..=MAX_SAVE_SIZE).contains(&profile.size)
            && profile.size.is_multiple_of(4)
            && ["u32be", "be"].contains(&profile.encoding.as_str()),
        "Unsupported size or encoding"
    );

    // "sum32be" stores its checksum in the last word; "none" has no checksum
    // (its offset is the end of the save, so every field must fit before it).
    let checksum = &profile.checksum;
    let checksum_supported = (checksum.algorithm == "sum32be"
        && checksum.offset == profile.size - 4)
        || (checksum.algorithm == "none" && checksum.offset == profile.size && checksum.seed == 0);

    let signature = &profile.signature;
    let signature_fits = [1, 2, 4].contains(&signature.width)
        && signature
            .offset
            .checked_add(signature.width)
            .is_some_and(|end| end <= checksum.offset)
        && (signature.width == 4 || signature.value < (1u32 << (signature.width * 8)));

    ensure!(
        checksum_supported && signature_fits,
        "Unsupported checksum or signature"
    );
    Ok(())
}

/// Checks a profile's fields: each has a label, a valid width, lies before
/// the checksum, and has a range that fits its bits; and no two fields (or a
/// field and the signature) share a bit.
fn validate_fields(profile: &Profile) -> Result<()> {
    // Which bits of each byte are already taken, starting with the signature.
    let mut taken_bits = vec![0u8; profile.size];
    let signature = &profile.signature;
    taken_bits[signature.offset..signature.offset + signature.width].fill(0xff);

    for field in &profile.fields {
        ensure!(
            is_short_ascii(&field.label, FIELD_LABEL_LENGTH.0)
                && [1, 2, 4].contains(&field.width)
                && field
                    .offset
                    .checked_add(field.width)
                    .is_some_and(|end| end <= profile.checksum.offset)
                && field.mask != 0
                && field.min <= field.max,
            "Invalid field label, width, offset or range"
        );

        // The mask's bits must be one unbroken run (shifted down, it is
        // 2^n - 1), the maximum must fit in them, and the mask must fit in
        // the field's width.
        let shifted_mask = field.mask >> field.mask.trailing_zeros();
        let mask_is_one_run = shifted_mask & shifted_mask.wrapping_add(1) == 0;
        let mask_fits_width = field.width == 4 || field.mask < (1u32 << (field.width * 8));
        ensure!(
            mask_is_one_run && field.max <= shifted_mask && mask_fits_width,
            "Invalid field bit mask or range"
        );

        let mask_bytes = field.mask.to_be_bytes();
        for (index, &bits) in mask_bytes[4 - field.width..].iter().enumerate() {
            let byte = &mut taken_bits[field.offset + index];
            ensure!(*byte & bits == 0, "Overlapping save fields");
            *byte |= bits;
        }
    }
    Ok(())
}

/// Checks a profile's presets: each has a label, and sets existing fields,
/// each once, to values in range.
fn validate_presets(profile: &Profile) -> Result<()> {
    for preset in &profile.presets {
        ensure!(
            is_short_ascii(&preset.label, PRESET_LABEL_LENGTH.0)
                && preset.additional.len() < MAX_FIELDS,
            "Invalid preset label or size"
        );

        let mut fields_set = HashSet::new();
        for (index, value) in preset_changes(preset) {
            let field = profile
                .fields
                .get(index)
                .context("Preset refers to an unknown field")?;
            ensure!(
                fields_set.insert(index) && (field.min..=field.max).contains(&value),
                "Preset repeats a field or is outside its range"
            );
        }
    }
    Ok(())
}

/// The size of one profile's record in the binary database, with its fields
/// and presets.
fn binary_profile_size(profile: &Profile) -> usize {
    let presets: usize = profile
        .presets
        .iter()
        .map(|preset| BINARY_PRESET_BYTES + preset.additional.len() * BINARY_CHANGE_BYTES)
        .sum();
    BINARY_PROFILE_BYTES + profile.fields.len() * BINARY_FIELD_BYTES + presets
}

/// Appends a big-endian 32-bit word.
fn push_word(bytes: &mut Vec<u8>, value: u32) {
    bytes.extend_from_slice(&value.to_be_bytes());
}

/// Appends text, zero-padded to `size` bytes.
fn push_text(bytes: &mut Vec<u8>, text: &str, size: usize) {
    bytes.extend_from_slice(text.as_bytes());
    bytes.resize(bytes.len() + size - text.len(), 0);
}

/// Fills in a binary database's header: total length, profile count, and
/// the CRC-32 of everything after the header.
fn finish_binary_header(bytes: &mut [u8], profile_count: usize) {
    let length = bytes.len() as u32;
    let crc = crc32fast::hash(&bytes[BINARY_HEADER_BYTES..]);
    bytes[8..12].copy_from_slice(&length.to_be_bytes());
    bytes[12..16].copy_from_slice(&(profile_count as u32).to_be_bytes());
    bytes[16..20].copy_from_slice(&crc.to_be_bytes());
}

/// Rebuilds the older "OM3DCHT1" console copy from the current one
/// (`current`). It is the same except:
///
/// - each profile record has only the first 8 numbers (32 bytes, with the
///   record size counting old-format fields), then the 128 bytes of text;
/// - each field is 40 bytes: label, offset, min, max (no width or mask);
/// - presets have no extra changes.
fn old_format_binary(current: &[u8], profiles: &[Profile]) -> Vec<u8> {
    const OLD_PROFILE_BYTES: usize = 160;
    const OLD_FIELD_BYTES: usize = 40;

    let mut old = Vec::from(&b"OM3DCHT1"[..]);
    old.extend_from_slice(&[0; 16]);

    let mut position = BINARY_HEADER_BYTES;
    for profile in profiles {
        let record_size = OLD_PROFILE_BYTES
            + profile.fields.len() * OLD_FIELD_BYTES
            + profile.presets.len() * BINARY_PRESET_BYTES;
        let mut numbers = current[position..position + 32].to_vec();
        numbers[..4].copy_from_slice(&(record_size as u32).to_be_bytes());
        old.extend(numbers);

        // Skip the 4 newer numbers (16 bytes) and copy the text.
        old.extend_from_slice(&current[position + 48..position + BINARY_PROFILE_BYTES]);
        position += BINARY_PROFILE_BYTES;

        for _ in &profile.fields {
            old.extend_from_slice(&current[position..position + OLD_FIELD_BYTES]);
            position += BINARY_FIELD_BYTES;
        }
        for _ in &profile.presets {
            old.extend_from_slice(&current[position..position + BINARY_PRESET_BYTES]);
            position += BINARY_PRESET_BYTES;
        }
    }

    finish_binary_header(&mut old, profiles.len());
    old
}

/// Writes the database to a drive or library: the JSON and the console's
/// binary copy together, through a journal, so either both change or
/// neither does. Returns the journal's path.
pub fn install(root: &Path, database: &Database, cache: &Path) -> Result<PathBuf> {
    let json = database.json()?;
    let binary = database.binary()?;
    let _lock = crate::library::lock_library(root, cache)?;
    let mut journal = crate::library::Journal::new(root, cache)?;

    let result = (|| -> Result<()> {
        journal.write(JSON_PATH, &json)?;
        journal.write(BINARY_PATH, &binary)?;
        let json_on_disk = fs::read(crate::fsutil::safe(root, JSON_PATH)?)?;
        let binary_on_disk = fs::read(crate::fsutil::safe(root, BINARY_PATH)?)?;
        ensure!(
            json_on_disk == json && binary_on_disk == binary,
            "Cheat database readback failed"
        );
        journal.finish()
    })();

    if let Err(error) = result {
        journal
            .undo()
            .context(format!("{error:#}; database rollback failed"))?;
        return Err(error);
    }
    Ok(journal.path)
}

/// Makes sure a drive has the console's binary copy, as part of a larger
/// journaled job. If it is missing or empty, it is made from the drive's
/// JSON (or the built-in database, writing that JSON too if it is missing).
/// An existing binary copy is left alone.
pub fn ensure_database(root: &Path, journal: &mut crate::library::Journal) -> Result<()> {
    let json = crate::fsutil::safe(root, JSON_PATH)?;
    let binary = crate::fsutil::safe(root, BINARY_PATH)?;
    ensure!(
        !json.exists() || json.is_file(),
        "Database JSON path is occupied by a directory"
    );
    ensure!(
        !binary.exists() || binary.is_file(),
        "Console database path is occupied by a directory"
    );

    let binary_missing = !binary.exists() || binary.metadata()?.len() == 0;
    if !binary_missing {
        return Ok(());
    }

    let json_missing = !json.exists() || json.metadata()?.len() == 0;
    let database = if json_missing {
        Database::bundled()
    } else {
        Database::read(&json)?
    };
    if json_missing {
        journal.write(JSON_PATH, &database.json()?)?;
    }
    journal.write(BINARY_PATH, &database.binary()?)?;
    Ok(())
}

/// Adds the profiles in `upstream` that `current` does not have. A profile
/// is already there if it has the same id or claims the same save, so the
/// person's own edits and additions are never replaced.
pub fn merge_missing(current: &Database, upstream: &Database) -> Result<Database> {
    current.validate()?;
    upstream.validate()?;

    let mut merged = current.clone();
    for profile in &upstream.profiles {
        let already_there = merged.profiles.iter().any(|existing| {
            existing.id == profile.id
                || (existing.filename == profile.filename
                    && existing.size == profile.size
                    && existing.signature.offset == profile.signature.offset
                    && existing.signature.value == profile.signature.value)
        });
        if !already_there {
            merged.profiles.push(profile.clone());
        }
    }

    merged.validate()?;
    Ok(merged)
}

/// Downloads the official database from the project's repository.
pub fn download() -> Result<Database> {
    let client = reqwest::blocking::Client::builder()
        .timeout(std::time::Duration::from_secs(20))
        .user_agent("OhMy3DO-Library")
        .build()?;
    let response = client.get(DOWNLOAD_URL).send()?;
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

/// Saves the database editor's work to `path`, through a journal. `expected`
/// is the file's contents when the editor opened it (None if it did not
/// exist); if the file has changed since, nothing is saved. Returns the
/// journal's path.
pub fn save_json(
    path: &Path,
    database: &Database,
    expected: Option<&[u8]>,
    cache: &Path,
) -> Result<PathBuf> {
    let bytes = database.json()?;
    let current = source_bytes(path)?;
    ensure!(
        current.as_deref() == expected,
        "Database changed outside the editor; reopen it before saving"
    );

    let folder = path.parent().context("Database directory missing")?;
    let name = path
        .file_name()
        .and_then(|name| name.to_str())
        .context("Unsupported database filename")?;
    let mut journal = crate::library::Journal::new(folder, cache)?;

    let result = (|| -> Result<()> {
        journal.write(name, &bytes)?;
        ensure!(fs::read(path)? == bytes, "Database readback failed");
        journal.finish()
    })();

    if let Err(error) = result {
        journal
            .undo()
            .context(format!("{error:#}; rollback failed"))?;
        return Err(error);
    }
    Ok(journal.path)
}

/// The cheat database that belongs with a save backup at `path`: the
/// nearest `save-cheats.json` in a folder above it (directly inside an
/// `.ohmy3do` folder, or at `.ohmy3do/save-cheats.json`). None if there is
/// none.
pub fn for_backup(path: &Path) -> Result<Option<Database>> {
    for folder in path.ancestors().skip(1) {
        let in_support_folder = folder.file_name().is_some_and(|name| name == ".ohmy3do");
        let candidate = if in_support_folder {
            folder.join("save-cheats.json")
        } else {
            folder.join(JSON_PATH)
        };
        if candidate.exists() {
            return Database::read(&candidate).map(Some);
        }
    }
    Ok(None)
}

/// The raw contents of a database file, for the editor to compare against
/// when saving. None if the file does not exist. Refuses anything but a
/// regular file within the size limit.
pub fn source_bytes(path: &Path) -> Result<Option<Vec<u8>>> {
    match fs::symlink_metadata(path) {
        Ok(metadata) => {
            ensure!(
                metadata.is_file() && !crate::fsutil::is_link(path),
                "Choose a regular database file"
            );
            let bytes = read_limited(path)?;
            ensure!(bytes.len() <= MAX_JSON, "Database JSON exceeds 256 KiB");
            Ok(Some(bytes))
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.into()),
    }
}
