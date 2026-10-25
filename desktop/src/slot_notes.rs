//! Notes are separate from NVRAM data and archive checksums.
use crate::{backup, save_archive};
use anyhow::{ensure, Context, Result};
use std::{
    fs,
    path::{Path, PathBuf},
};
pub const MAX: usize = 4096;
pub fn path(archive: &Path) -> Result<PathBuf> {
    ensure!(
        backup::slot(archive).is_some(),
        "Choose a numbered backup slot"
    );
    Ok(archive.with_extension("notes.txt"))
}
pub fn read(archive: &Path) -> Result<Option<String>> {
    let file = path(archive)?;
    if !file.exists() {
        return Ok(None);
    }
    ensure!(
        fs::metadata(&file)?.len() <= MAX as u64,
        "Oversized slot notes"
    );
    let text = fs::read_to_string(file)?;
    validate(&text)?;
    Ok(Some(text))
}
fn validate(note: &str) -> Result<()> {
    ensure!(
        note.len() <= MAX
            && !note
                .chars()
                .any(|c| c.is_control() && !matches!(c, '\n' | '\r' | '\t')),
        "Notes must be UTF-8 text up to 4096 bytes"
    );
    Ok(())
}
pub fn save(archive: &Path, note: &str, expected: Option<&str>) -> Result<()> {
    ensure!(archive.is_file(), "Save the archive before adding notes");
    validate(note)?;
    let file = path(archive)?;
    let current = read(archive)?;
    ensure!(
        current.as_deref() == expected,
        "Slot notes changed; reopen the notes editor"
    );
    save_archive::save_bytes(
        &file,
        note.as_bytes(),
        current.as_deref().map(str::as_bytes),
    )
    .context("Save slot notes")?;
    Ok(())
}
