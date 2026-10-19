//! The console's OM3DSAV1 format. Keep validation in sync with src/save_archive.c.
use anyhow::{bail, ensure, Context, Result};
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
};
pub const MAX_BYTES: usize = 65536;
pub const MAX_FILES: usize = 128;
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    /// Raw filename bytes: console filenames are not required to be UTF-8.
    pub name: Vec<u8>,
    pub file_type: u32,
    pub data: Vec<u8>,
}
impl Entry {
    pub fn label(&self) -> String {
        self.name.iter().map(|b| char::from(*b)).collect()
    }
}
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct Archive {
    pub entries: Vec<Entry>,
}
fn word(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes(bytes[offset..offset + 4].try_into().unwrap())
}
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
    pub fn read(path: &Path) -> Result<Self> {
        Ok(Self::read_with_bytes(path)?.0)
    }
    pub fn read_with_bytes(path: &Path) -> Result<(Self, Vec<u8>)> {
        let data = read_bytes(path)?;
        ensure!(data.len() >= 24, "Truncated backup header");
        let length = word(&data, 8) as usize;
        ensure!((24..=data.len()).contains(&length), "Invalid backup length");
        Ok((Self::parse(&data[..length])?, data))
    }
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
    pub fn save(&self, path: &Path, expected: Option<&[u8]>) -> Result<Saved> {
        save_bytes(path, &self.encode()?, expected)
    }
    pub fn save_copy(&self, path: &Path) -> Result<()> {
        write_new(path, &self.encode()?)
    }
}
#[derive(Debug)]
pub struct Saved {
    pub bytes: Vec<u8>,
    pub backup: Option<PathBuf>,
}
/// Keep physical bytes, including the firmware's possible old tail, for conflict
/// checks and exact recovery copies.
pub fn read_bytes(path: &Path) -> Result<Vec<u8>> {
    let mut data = Vec::new();
    fs::File::open(path)?
        .take(MAX_BYTES as u64 + 1)
        .read_to_end(&mut data)?;
    ensure!(data.len() <= MAX_BYTES, "Save file exceeds 64 KiB");
    Ok(data)
}
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
/// Replace only the version the caller reviewed; preserve it beside the file
/// before publishing the complete replacement atomically.
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
/// Atomic publication without overwriting any existing file (including the input).
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
pub fn decode_hex(text: &str) -> Result<Vec<u8>> {
    let digits: String = text.chars().filter(|c| !c.is_ascii_whitespace()).collect();
    ensure!(
        digits.len() % 2 == 0 && digits.bytes().all(|b| b.is_ascii_hexdigit()),
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
