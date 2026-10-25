//! Backup slots: their labels, and comparing two backups. Labels live in
//! text files beside the slots (one per page of 8 slots), which the console
//! reads and writes too; they never touch the saves themselves. Comparing
//! only reads.

use crate::save_archive::{self, Archive, Entry};
use anyhow::{ensure, Context, Result};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
};
/// The labels file for the first page of slots (slots 1 to 8); page N's is
/// "slot-labels-N.txt".
pub const LABEL_FILE: &str = "slot-labels.txt";

/// The longest label.
pub const LABEL_BYTES: usize = 24;

/// The labels of one page of 8 slots ("" for a slot without one).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Labels(pub [String; 8]);

impl Labels {
    /// Reads a labels file: a line per labeled slot, its place on the page
    /// (1 to 8), a tab and the label (up to 24 printable ASCII characters).
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        ensure!(bytes.len() <= 512, "Slot labels exceed 512 bytes");
        // The console ends what it writes with a zero byte: the ODE cannot
        // shorten a file, so old bytes may follow it.
        let bytes = bytes.split(|b| *b == 0).next().unwrap_or_default();
        let text = std::str::from_utf8(bytes)?;
        let mut out = Self::default();
        let mut seen = [false; 8];
        for line in text.lines() {
            if line.is_empty() {
                continue;
            }
            let (slot, label) = line.split_once('\t').context("Invalid slot label line")?;
            ensure!(
                slot.len() == 1 && (b'1'..=b'8').contains(&slot.as_bytes()[0]),
                "Invalid backup slot number"
            );
            let i = (slot.as_bytes()[0] - b'1') as usize;
            ensure!(
                !seen[i]
                    && label.len() <= LABEL_BYTES
                    && label.bytes().all(|b| (32..127).contains(&b)),
                "Duplicate slot or invalid label (24 printable ASCII characters maximum)"
            );
            seen[i] = true;
            out.0[i] = label.into();
        }
        Ok(out)
    }
    /// The labels as a labels file.
    pub fn encode(&self) -> Result<Vec<u8>> {
        for label in &self.0 {
            ensure!(
                label.len() <= LABEL_BYTES && label.bytes().all(|b| (32..127).contains(&b)),
                "Names require at most 24 printable ASCII characters"
            );
        }
        let text = self
            .0
            .iter()
            .enumerate()
            .filter(|(_, s)| !s.is_empty())
            .map(|(i, s)| format!("{}\t{}\n", i + 1, s))
            .collect::<String>();
        let bytes = text.into_bytes();
        Self::parse(&bytes)?;
        Ok(bytes)
    }
    /// Reads a labels file, returning the labels and the file's bytes (None
    /// if there is no file), for a later `save` to check against.
    pub fn read(path: &Path) -> Result<(Self, Option<Vec<u8>>)> {
        let bytes = match fs::File::open(path) {
            Ok(file) => {
                let mut b = Vec::new();
                file.take(513).read_to_end(&mut b)?;
                Some(b)
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
            Err(e) => return Err(e.into()),
        };
        Ok((Self::parse(bytes.as_deref().unwrap_or_default())?, bytes))
    }
    /// Writes the labels, ending them with a zero byte as the console does,
    /// if the file still holds `expected`.
    pub fn save(&self, path: &Path, expected: Option<&[u8]>) -> Result<()> {
        let mut bytes = self.encode()?;
        bytes.push(0);
        save_archive::save_bytes(path, &bytes, expected)?;
        Ok(())
    }
}
/// For a backup slot file ("slot01.3ds" up to "slot999999999.3ds"), its
/// labels file and its place (0 to 7) on that page; None for other files.
pub fn slot(path: &Path) -> Option<(PathBuf, usize)> {
    let name = path.file_name()?.to_str()?.to_ascii_lowercase();
    let number = name
        .strip_prefix("slot")?
        .strip_suffix(".3ds")?
        .parse::<usize>()
        .ok()?;
    if number == 0 || number > 999_999_999 || name != format!("slot{number:02}.3ds") {
        return None;
    }
    let page = (number - 1) / 8;
    let file = if page == 0 {
        LABEL_FILE.into()
    } else {
        format!("slot-labels-{page}.txt")
    };
    Some((path.parent()?.join(file), (number - 1) % 8))
}
/// How a save differs between two backups.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum State {
    Added,
    Removed,
    Changed,
    Unchanged,
}
/// A save in either of two backups, and how it differs between them.
#[derive(Clone, Debug)]
pub struct Difference {
    /// Its place in the first backup, if it is there.
    pub before: Option<usize>,
    /// Its place in the second backup, if it is there.
    pub after: Option<usize>,
    pub state: State,
    /// How many bytes differ (counting bytes only one of them has).
    pub changed_bytes: usize,
    pub type_changed: bool,
    /// The name differs in case.
    pub name_changed: bool,
}

impl Difference {
    /// The save: as in the second backup, if it is there.
    pub fn entry<'a>(&self, before: &'a Archive, after: &'a Archive) -> &'a Entry {
        if let Some(i) = self.after {
            &after.entries[i]
        } else {
            &before.entries[self.before.unwrap()]
        }
    }
    /// Every byte that differs: its offset, and its value before and after
    /// (None past the end of a save).
    pub fn byte_changes(
        &self,
        before: &Archive,
        after: &Archive,
    ) -> Vec<(usize, Option<u8>, Option<u8>)> {
        let old = self
            .before
            .map(|i| before.entries[i].data.as_slice())
            .unwrap_or_default();
        let new = self
            .after
            .map(|i| after.entries[i].data.as_slice())
            .unwrap_or_default();
        (0..old.len().max(new.len()))
            .filter_map(|i| {
                let a = old.get(i).copied();
                let b = new.get(i).copied();
                (a != b).then_some((i, a, b))
            })
            .collect()
    }
}
/// Every save in either backup and how it differs, matching saves by name
/// (ignoring case): the first backup's saves in order, then those only in
/// the second.
pub fn compare(before: &Archive, after: &Archive) -> Vec<Difference> {
    let mut pairs = before
        .entries
        .iter()
        .enumerate()
        .map(|(i, e)| {
            (
                Some(i),
                after
                    .entries
                    .iter()
                    .position(|n| n.name.eq_ignore_ascii_case(&e.name)),
            )
        })
        .collect::<Vec<_>>();
    pairs.extend(
        after
            .entries
            .iter()
            .enumerate()
            .filter(|(_, e)| {
                !before
                    .entries
                    .iter()
                    .any(|old| old.name.eq_ignore_ascii_case(&e.name))
            })
            .map(|(i, _)| (None, Some(i))),
    );
    pairs
        .into_iter()
        .map(|(a, b)| {
            let old = a.map(|i| &before.entries[i]);
            let new = b.map(|i| &after.entries[i]);
            let od = old.map(|e| e.data.as_slice()).unwrap_or_default();
            let nd = new.map(|e| e.data.as_slice()).unwrap_or_default();
            let changed_bytes = (0..od.len().max(nd.len()))
                .filter(|i| od.get(*i) != nd.get(*i))
                .count();
            let type_changed = old
                .zip(new)
                .is_some_and(|(a, b)| a.file_type != b.file_type);
            let name_changed = old.zip(new).is_some_and(|(a, b)| a.name != b.name);
            let state = match (a, b) {
                (None, _) => State::Added,
                (_, None) => State::Removed,
                _ if changed_bytes > 0 || type_changed || name_changed => State::Changed,
                _ => State::Unchanged,
            };
            Difference {
                before: a,
                after: b,
                state,
                changed_bytes,
                type_changed,
                name_changed,
            }
        })
        .collect()
}

/// One save in a comparison, as the app shows it.
#[derive(Debug, serde::Serialize)]
pub struct ComparisonRow {
    /// The save's file name.
    pub name: String,
    /// The game it belongs to, if known; otherwise its name.
    pub game: String,
    pub state: String,
    pub changed_bytes: usize,
    pub type_changed: bool,
    pub name_changed: bool,
    pub before_size: Option<usize>,
    pub after_size: Option<usize>,
    pub after_index: Option<usize>,
    /// The first 512 differing bytes (see `Difference::byte_changes`).
    pub changes: Vec<(usize, Option<u8>, Option<u8>)>,
}

/// A comparison of two backups: how many saves were added, removed,
/// changed or unchanged, and a row per save.
#[derive(Debug, Default, serde::Serialize)]
pub struct Comparison {
    pub added: usize,
    pub removed: usize,
    pub changed: usize,
    pub unchanged: usize,
    pub rows: Vec<ComparisonRow>,
}
/// Compares a backup (`before`) with another, or with the draft being
/// edited (`after`), for the app's comparison view.
pub fn comparison(before: &Archive, after: &Archive) -> Comparison {
    let mut result = Comparison::default();
    for diff in compare(before, after) {
        let state = match diff.state {
            State::Added => {
                result.added += 1;
                "Added"
            }
            State::Removed => {
                result.removed += 1;
                "Removed"
            }
            State::Changed => {
                result.changed += 1;
                "Changed"
            }
            State::Unchanged => {
                result.unchanged += 1;
                "Unchanged"
            }
        };
        let name = diff.entry(before, after).label();
        result.rows.push(ComparisonRow {
            game: crate::saves::known_title(&name).unwrap_or(&name).to_owned(),
            name,
            state: state.into(),
            changed_bytes: diff.changed_bytes,
            type_changed: diff.type_changed,
            name_changed: diff.name_changed,
            before_size: diff.before.map(|i| before.entries[i].data.len()),
            after_size: diff.after.map(|i| after.entries[i].data.len()),
            after_index: diff.after,
            changes: diff
                .byte_changes(before, after)
                .into_iter()
                .take(512)
                .collect(),
        });
    }
    result
}
