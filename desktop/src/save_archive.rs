//! Save backups: the OM3DSAV1 files the console's Save Manager writes, one
//! per backup slot, holding copies of saves from the console's NVRAM. The
//! checks here must match the console's (src/save_archive.c).
//!
//! The layout (numbers are 32-bit big-endian):
//!
//! ```text
//! offset 0   8 bytes  "OM3DSAV1"
//!        8   4        the backup's length in bytes
//!       12   4        the number of saves (at most 128)
//!       16   4        CRC-32 of everything after the header
//!       20   4        reserved, 0
//!       24   ...      for each save, a 48-byte record and then its data:
//!                       0  32  name, zero-padded
//!                      32   4  file type
//!                      36   4  size of the data
//!                      40   4  CRC-32 of the data
//!                      44   4  reserved, 0
//! ```
//!
//! A whole backup is at most 64 KiB.

use anyhow::{bail, ensure, Context, Result};
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
};
/// The largest backup file.
pub const MAX_BYTES: usize = 65536;

/// The most saves in one backup.
pub const MAX_FILES: usize = 128;

/// One save in a backup.
#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct Entry {
    /// Raw filename bytes: console filenames are not required to be UTF-8.
    pub name: Vec<u8>,
    pub file_type: u32,
    pub data: Vec<u8>,
}
impl Entry {
    /// The save's name as text (each byte as a character).
    pub fn label(&self) -> String {
        self.name.iter().map(|b| char::from(*b)).collect()
    }
}
/// A backup's saves.
#[derive(Clone, Debug, PartialEq, Eq, Default, serde::Serialize, serde::Deserialize)]
pub struct Archive {
    pub entries: Vec<Entry>,
}

/// The big-endian 32-bit number at `offset`.
fn word(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes(bytes[offset..offset + 4].try_into().unwrap())
}
/// Whether a save name is one the console can store: 1 to 31 bytes, no
/// control characters or path separators, not "." or "..".
fn valid_name(name: &[u8]) -> bool {
    !name.is_empty()
        && name.len() < 32
        && name != b"."
        && name != b".."
        && name
            .iter()
            .all(|b| *b >= 32 && !matches!(*b, 127 | b'/' | b'\\' | b':'))
}
impl Archive {
    /// Adds saves to this backup, replacing any of the same name (ignoring
    /// case, and keeping the existing name's case). If the result would not
    /// be a valid backup, nothing changes.
    pub fn merge_entries(&mut self, incoming: &[Entry]) -> Result<()> {
        let mut next = self.clone();
        for e in incoming {
            if let Some(old) = next
                .entries
                .iter_mut()
                .find(|old| old.name.eq_ignore_ascii_case(&e.name))
            {
                let name = old.name.clone();
                *old = e.clone();
                old.name = name;
            } else {
                next.entries.push(e.clone());
            }
        }
        let bytes = next.encode()?;
        Self::parse(&bytes)?;
        *self = next;
        Ok(())
    }
    /// Reads a backup file.
    pub fn read(path: &Path) -> Result<Self> {
        Ok(Self::read_with_bytes(path)?.0)
    }
    /// Reads a backup file, returning also the file's bytes. The file may be
    /// longer than the backup (the console cannot shorten a file it
    /// rewrites); the backup is the length its header gives.
    pub fn read_with_bytes(path: &Path) -> Result<(Self, Vec<u8>)> {
        let data = read_bytes(path)?;
        ensure!(data.len() >= 24, "Truncated backup header");
        let length = word(&data, 8) as usize;
        ensure!((24..=data.len()).contains(&length), "Invalid backup length");
        Ok((Self::parse(&data[..length])?, data))
    }
    /// Reads a backup from exactly its bytes, checking everything: header,
    /// checksums, names (unique, ignoring case) and that nothing is left over.
    pub fn parse(data: &[u8]) -> Result<Self> {
        ensure!(
            (24..=MAX_BYTES).contains(&data.len()) && &data[..8] == b"OM3DSAV1",
            "Not an Oh My 3DO backup"
        );
        ensure!(
            word(data, 8) as usize == data.len() && word(data, 20) == 0,
            "Invalid backup header"
        );
        let count = word(data, 12) as usize;
        ensure!(
            count <= MAX_FILES && crc32fast::hash(&data[24..]) == word(data, 16),
            "Backup count or checksum is invalid"
        );
        let mut archive = Self::default();
        let mut pos = 24;
        for _ in 0..count {
            ensure!(data.len() - pos >= 48, "Truncated save record");
            let record = &data[pos..pos + 48];
            let end = record[..32]
                .iter()
                .position(|b| *b == 0)
                .context("Unterminated save name")?;
            let name = &record[..end];
            ensure!(
                valid_name(name)
                    && record[end..32].iter().all(|b| *b == 0)
                    && word(record, 44) == 0,
                "Invalid save record"
            );
            ensure!(
                !archive
                    .entries
                    .iter()
                    .any(|e| e.name.eq_ignore_ascii_case(name)),
                "Duplicate save name"
            );
            let size = word(record, 36) as usize;
            pos += 48;
            ensure!(size <= data.len() - pos, "Truncated save data");
            let payload = &data[pos..pos + size];
            ensure!(
                crc32fast::hash(payload) == word(record, 40),
                "Save checksum is invalid"
            );
            archive.entries.push(Entry {
                name: name.to_vec(),
                file_type: word(record, 32),
                data: payload.to_vec(),
            });
            pos += size;
        }
        ensure!(pos == data.len(), "Unexpected bytes inside backup");
        Ok(archive)
    }
    /// The backup's bytes, checked by reading them back.
    pub fn encode(&self) -> Result<Vec<u8>> {
        ensure!(self.entries.len() <= MAX_FILES, "Too many saves");
        let mut bytes = vec![0u8; 24];
        bytes[..8].copy_from_slice(b"OM3DSAV1");
        bytes[12..16].copy_from_slice(&(self.entries.len() as u32).to_be_bytes());
        for entry in &self.entries {
            ensure!(valid_name(&entry.name), "Invalid save name");
            ensure!(
                entry.data.len() <= MAX_BYTES.saturating_sub(bytes.len() + 48),
                "Backup exceeds 64 KiB"
            );
            let mut record = [0u8; 48];
            record[..entry.name.len()].copy_from_slice(&entry.name);
            record[32..36].copy_from_slice(&entry.file_type.to_be_bytes());
            record[36..40].copy_from_slice(&(entry.data.len() as u32).to_be_bytes());
            record[40..44].copy_from_slice(&crc32fast::hash(&entry.data).to_be_bytes());
            bytes.extend_from_slice(&record);
            bytes.extend_from_slice(&entry.data);
        }
        let length = bytes.len() as u32;
        let crc = crc32fast::hash(&bytes[24..]);
        bytes[8..12].copy_from_slice(&length.to_be_bytes());
        bytes[16..20].copy_from_slice(&crc.to_be_bytes());
        Self::parse(&bytes)?;
        Ok(bytes)
    }
    /// Writes the backup to `path` with `save_bytes`.
    pub fn save(&self, path: &Path, expected: Option<&[u8]>) -> Result<Saved> {
        save_bytes(path, &self.encode()?, expected)
    }
    /// Writes the backup to a new file (never replacing one).
    pub fn save_copy(&self, path: &Path) -> Result<()> {
        write_new(path, &self.encode()?)
    }
}
/// What `save_bytes` wrote.
#[derive(Debug)]
pub struct Saved {
    pub bytes: Vec<u8>,
    /// The recovery copy of the file it replaced, if it replaced one.
    pub backup: Option<PathBuf>,
}

/// A save file's bytes exactly as on disk (any old bytes after the end
/// included), for spotting changes and for exact recovery copies.
pub fn read_bytes(path: &Path) -> Result<Vec<u8>> {
    let mut data = Vec::new();
    fs::File::open(path)?
        .take(MAX_BYTES as u64 + 1)
        .read_to_end(&mut data)?;
    ensure!(data.len() <= MAX_BYTES, "Save file exceeds 64 KiB");
    Ok(data)
}
/// Like `read_bytes`, but None if there is no file, and an error if it is
/// not a regular file.
pub fn existing_bytes(path: &Path) -> Result<Option<Vec<u8>>> {
    match fs::symlink_metadata(path) {
        Ok(meta) => {
            ensure!(
                meta.is_file() && !crate::fsutil::is_link(path),
                "Choose a regular save file"
            );
            Ok(Some(read_bytes(path)?))
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.into()),
    }
}
/// Writes `bytes` to `path`, but only if the file still holds `expected`
/// (what the person reviewed; None if there should be no file yet). The old
/// file is first kept beside it as a recovery copy ("NAME.<id>.bak"); then
/// the new file replaces it all at once, and is read back to check.
pub fn save_bytes(path: &Path, bytes: &[u8], expected: Option<&[u8]>) -> Result<Saved> {
    ensure!(bytes.len() <= MAX_BYTES, "Save file exceeds 64 KiB");
    ensure!(
        existing_bytes(path)?.as_deref() == expected,
        "File changed outside the editor. Reload it or choose another filename"
    );
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let mut temporary = tempfile::NamedTempFile::new_in(parent)?;
    temporary.write_all(bytes)?;
    temporary.as_file().sync_all()?;
    let backup = if let Some(original) = expected {
        let mut name = path
            .file_name()
            .context("Save filename missing")?
            .to_os_string();
        name.push(format!(".{}.bak", uuid::Uuid::new_v4()));
        let backup = path.with_file_name(name);
        write_new(&backup, original)
            .context("Could not create recovery copy; save was not replaced")?;
        ensure!(
            fs::read(&backup)? == original,
            "Recovery copy verification failed; save was not replaced"
        );
        Some(backup)
    } else {
        None
    };
    ensure!(
        existing_bytes(path)?.as_deref() == expected,
        "File changed while saving. Reload it or choose another filename"
    );
    if expected.is_some() {
        temporary
            .persist(path)
            .map_err(|e| e.error)
            .with_context(|| {
                format!(
                    "Could not replace save; recovery copy: {}",
                    backup.as_ref().unwrap().display()
                )
            })?;
    } else {
        temporary
            .persist_noclobber(path)
            .map_err(|e| e.error)
            .context("Could not create save; existing files are preserved")?;
    }
    ensure!(
        fs::read(path)
            .with_context(|| format!("Cannot verify saved file; recovery copy: {backup:?}"))?
            == bytes,
        "Saved file readback failed; recovery copy: {backup:?}"
    );
    Ok(Saved {
        bytes: bytes.to_vec(),
        backup,
    })
}
/// Writes a new file all at once, failing if anything is already there.
pub fn write_new(path: &Path, bytes: &[u8]) -> Result<()> {
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let mut temporary = tempfile::NamedTempFile::new_in(parent)?;
    temporary.write_all(bytes)?;
    temporary.as_file().sync_all()?;
    temporary
        .persist_noclobber(path)
        .map_err(|e| e.error)
        .context("Choose a new filename; existing files are preserved")?;
    Ok(())
}
/// Bytes from hex digits typed in the editor (spaces and line breaks
/// ignored).
pub fn decode_hex(text: &str) -> Result<Vec<u8>> {
    let digits: String = text.chars().filter(|c| !c.is_ascii_whitespace()).collect();
    ensure!(
        digits.len().is_multiple_of(2) && digits.bytes().all(|b| b.is_ascii_hexdigit()),
        "Enter pairs of hexadecimal digits"
    );
    if digits.len() > MAX_BYTES * 2 {
        bail!("Save exceeds 64 KiB")
    }
    (0..digits.len())
        .step_by(2)
        .map(|i| Ok(u8::from_str_radix(&digits[i..i + 2], 16)?))
        .collect()
}
