//! Notes about a backup slot, written by the person in the desktop app. They
//! live in a text file beside the slot ("slot01.notes.txt" for
//! "slot01.3ds"), so they never touch the saves or the backup's checksums.

use crate::{backup, save_archive};
use anyhow::{ensure, Context, Result};
use std::{
    fs,
    path::{Path, PathBuf},
};
/// The most bytes a note may have.
pub const MAX: usize = 4096;

/// The notes file for a backup slot.
pub fn path(archive: &Path) -> Result<PathBuf> {
    ensure!(
        backup::slot(archive).is_some(),
        "Choose a numbered backup slot"
    );
    Ok(archive.with_extension("notes.txt"))
}
/// The slot's notes, or None if it has none.
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
/// Checks a note is text (tabs and line breaks allowed) within [`MAX`].
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
/// Saves the slot's notes, if the file still holds `expected` (what the
/// editor opened with), so two edits never overwrite each other.
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
