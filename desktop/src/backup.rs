//! Read-only archive comparisons and slot labels kept outside save payloads.
use crate::save_archive::{self, Archive, Entry};
use anyhow::{ensure, Context, Result};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
};
pub const LABEL_FILE: &str = "slot-labels.txt";
pub const LABEL_BYTES: usize = 24;
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Labels(pub [String; 8]);
impl Labels {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        ensure!(bytes.len() <= 512, "Slot labels exceed 512 bytes");
        // Firmware text writes terminate with NUL and cannot truncate old tails.
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
    pub fn save(&self, path: &Path, expected: Option<&[u8]>) -> Result<()> {
        let mut bytes = self.encode()?;
        bytes.push(0);
        save_archive::save_bytes(path, &bytes, expected)?;
        Ok(())
    }
}
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
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum State {
    Added,
    Removed,
    Changed,
    Unchanged,
}
#[derive(Clone, Debug)]
pub struct Difference {
    pub before: Option<usize>,
    pub after: Option<usize>,
    pub state: State,
    pub changed_bytes: usize,
    pub type_changed: bool,
    pub name_changed: bool,
}
impl Difference {
    pub fn name<'a>(&self, before: &'a Archive, after: &'a Archive) -> &'a Entry {
        if let Some(i) = self.after {
            &after.entries[i]
        } else {
            &before.entries[self.before.unwrap()]
        }
    }
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

#[derive(Debug, serde::Serialize)]
pub struct ComparisonRow {
    pub name: String,
    pub game: String,
    pub state: String,
    pub changed_bytes: usize,
    pub type_changed: bool,
    pub name_changed: bool,
    pub before_size: Option<usize>,
    pub after_size: Option<usize>,
    pub after_index: Option<usize>,
    pub changes: Vec<(usize, Option<u8>, Option<u8>)>,
}
#[derive(Debug, Default, serde::Serialize)]
pub struct Comparison {
    pub added: usize,
    pub removed: usize,
    pub changed: usize,
    pub unchanged: usize,
    pub rows: Vec<ComparisonRow>,
}
/// Changes from the selected reference archive to the current editable draft.
pub fn comparison(before: &Archive, after: &Archive) -> Comparison {
    let mut result = Comparison::default();
    for diff in compare(before, after) {
        let state = match diff.state {
            State::Added => {result.added += 1; "Added"},
            State::Removed => {result.removed += 1; "Removed"},
            State::Changed => {result.changed += 1; "Changed"},
            State::Unchanged => {result.unchanged += 1; "Unchanged"},
        };
        let name = diff.name(before, after).label();
        result.rows.push(ComparisonRow {
            game: crate::saves::known_title(&name).unwrap_or(&name).to_owned(), name,
            state: state.into(), changed_bytes: diff.changed_bytes,
            type_changed: diff.type_changed, name_changed: diff.name_changed,
            before_size: diff.before.map(|i|before.entries[i].data.len()),
            after_size: diff.after.map(|i|after.entries[i].data.len()),
            after_index: diff.after,
            changes: diff.byte_changes(before, after).into_iter().take(512).collect(),
        });
    }
    result
}
