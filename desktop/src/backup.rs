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

#[cfg(test)]
mod tests {
    use super::*;

    /// A save with a name and some bytes, for the comparison tests.
    fn entry(name: &str, data: &[u8]) -> Entry {
        Entry {
            name: name.as_bytes().to_vec(),
            file_type: 1,
            data: data.to_vec(),
        }
    }
    fn archive(entries: Vec<Entry>) -> Archive {
        Archive { entries }
    }

    #[test]
    fn an_empty_labels_file_has_no_labels() {
        for empty in [b"".as_slice(), b"\n", b"1\tLabel\n\n"] {
            let labels = Labels::parse(empty).unwrap();
            if empty.starts_with(b"1") {
                assert_eq!(labels.0[0], "Label");
            } else {
                assert_eq!(labels, Labels::default());
            }
        }
    }

    #[test]
    fn a_label_line_gives_its_slot_a_label() {
        let labels = Labels::parse(b"1\tRoad Rash\n5\tPanel Pro\n").unwrap();
        assert_eq!(labels.0[0], "Road Rash");
        assert_eq!(labels.0[4], "Panel Pro");
        // Slots without a line stay blank.
        for i in [1, 2, 3, 5, 6, 7] {
            assert_eq!(labels.0[i], "");
        }
    }

    #[test]
    fn the_eighth_slot_is_the_last_one_allowed() {
        let labels = Labels::parse(b"8\tEighth\n").unwrap();
        assert_eq!(labels.0[7], "Eighth");
        // Slot 9 is not on a page of eight.
        assert!(Labels::parse(b"9\tNinth\n").is_err());
        assert!(Labels::parse(b"0\tZeroth\n").is_err());
    }

    #[test]
    fn a_slot_may_be_labeled_only_once() {
        let error = Labels::parse(b"3\tFirst\n3\tSecond\n")
            .unwrap_err()
            .to_string();
        assert!(error.contains("Duplicate slot"), "{error}");
    }

    #[test]
    fn labels_are_24_printable_ascii_characters_at_most() {
        // Exactly 24 is fine.
        let long = "x".repeat(LABEL_BYTES);
        let labels = Labels::parse(format!("1\t{long}\n").as_bytes()).unwrap();
        assert_eq!(labels.0[0], long);
        // 25 is not.
        assert!(Labels::parse(format!("1\t{}\n", "x".repeat(LABEL_BYTES + 1)).as_bytes()).is_err());
        // Nor are control characters, a tab inside the label, or non-ASCII.
        for bad in ["a\u{1}b", "\u{e9}", "tab\there"] {
            assert!(
                Labels::parse(format!("1\t{bad}\n").as_bytes()).is_err(),
                "{bad:?}"
            );
        }
    }

    #[test]
    fn a_line_without_a_tab_is_refused() {
        let error = Labels::parse(b"1 Road Rash\n").unwrap_err().to_string();
        assert!(error.contains("Invalid slot label line"), "{error}");
    }

    #[test]
    fn a_labels_file_padded_with_zero_bytes_by_the_console_loads() {
        // The ODE cannot shorten a file, so old bytes may follow the NUL.
        let mut bytes = b"2\tSecond\n".to_vec();
        bytes.push(0);
        bytes.extend_from_slice(b"7\tstale label from an older write\n");
        let labels = Labels::parse(&bytes).unwrap();
        assert_eq!(labels.0[1], "Second");
        assert_eq!(labels.0[6], "", "the stale padding is ignored");
    }

    #[test]
    fn a_labels_file_over_512_bytes_is_refused() {
        let oversized = vec![b';'; 513];
        assert!(Labels::parse(&oversized).is_err());
    }

    #[test]
    fn encoding_skips_blank_slots_and_reads_back_the_same() {
        let mut labels = Labels::default();
        labels.0[0] = "First".into();
        labels.0[7] = "Eighth".into();
        let bytes = labels.encode().unwrap();
        assert_eq!(bytes, b"1\tFirst\n8\tEighth\n");
        assert_eq!(Labels::parse(&bytes).unwrap(), labels);
    }

    #[test]
    fn encoding_refuses_a_label_it_cannot_write() {
        let mut labels = Labels::default();
        labels.0[0] = "way too long".repeat(4);
        assert!(labels.encode().is_err());
    }

    #[test]
    fn a_slot_file_maps_to_its_labels_file_and_place_on_the_page() {
        // Slots 1-8 are on the first page, whose file has no page number.
        let (file, place) = slot(Path::new("/saves/slot01.3ds")).unwrap();
        assert_eq!(file, PathBuf::from("/saves/slot-labels.txt"));
        assert_eq!(place, 0);
        let (_, place) = slot(Path::new("/saves/slot08.3ds")).unwrap();
        assert_eq!(place, 7);

        // Slots 9-16 are the second page.
        let (file, place) = slot(Path::new("/saves/slot09.3ds")).unwrap();
        assert_eq!(file, PathBuf::from("/saves/slot-labels-1.txt"));
        assert_eq!(place, 0);
        let (file, _) = slot(Path::new("/saves/slot16.3ds")).unwrap();
        assert_eq!(file, PathBuf::from("/saves/slot-labels-1.txt"));

        // The page number keeps growing with the slot number.
        let (file, place) = slot(Path::new("/saves/slot123456789.3ds")).unwrap();
        assert_eq!(place, 4);
        assert_eq!(
            file,
            PathBuf::from(format!("/saves/slot-labels-{}.txt", (123456789 - 1) / 8))
        );
    }

    #[test]
    fn a_slot_name_is_matched_case_insensitively_but_must_be_padded() {
        assert!(slot(Path::new("/s/SLOT01.3DS")).is_some());
        // Slot numbers are written at least two digits wide.
        assert!(slot(Path::new("/saves/slot1.3ds")).is_none());
        // And the padded form must be exactly what the number implies.
        assert!(
            slot(Path::new("/saves/slot00.3ds")).is_none(),
            "slot 0 is not used"
        );
        assert!(slot(Path::new("/saves/slot001.3ds")).is_none());
    }

    #[test]
    fn only_slot_backup_files_are_recognised() {
        for name in [
            "notes.txt",
            "slot-labels.txt",
            "slot01.bin",
            "slotab.3ds",
            "slot9999999999.3ds", // past the largest slot number
            "slot 1.3ds",
        ] {
            assert!(
                slot(Path::new("/saves/").join(name).as_path()).is_none(),
                "{name}"
            );
        }
    }

    #[test]
    fn saves_match_across_two_backups_by_name_ignoring_case() {
        let before = archive(vec![entry("Road Rash", b"AAA")]);
        let after = archive(vec![entry("road rash", b"AAA")]);
        let diffs = compare(&before, &after);
        assert_eq!(diffs.len(), 1);
        // Same bytes, different case: counted as changed because the name differs.
        assert_eq!(diffs[0].state, State::Changed);
        assert!(diffs[0].name_changed);
        assert_eq!(diffs[0].changed_bytes, 0);
    }

    #[test]
    fn a_save_present_in_both_backups_with_the_same_bytes_is_unchanged() {
        let before = archive(vec![entry("Instalker", b"same")]);
        let after = archive(vec![entry("Instalker", b"same")]);
        let diffs = compare(&before, &after);
        assert_eq!(diffs.len(), 1);
        assert_eq!(diffs[0].state, State::Unchanged);
        assert_eq!(diffs[0].before, Some(0));
        assert_eq!(diffs[0].after, Some(0));
    }

    #[test]
    fn added_and_removed_saves_are_distinguished() {
        let before = archive(vec![entry("Gone", b"x")]);
        let after = archive(vec![entry("New", b"y")]);
        let diffs = compare(&before, &after);
        assert_eq!(diffs.len(), 2);
        assert_eq!(diffs[0].state, State::Removed);
        assert_eq!(diffs[0].before, Some(0));
        assert_eq!(diffs[0].after, None);
        assert_eq!(diffs[1].state, State::Added);
        assert_eq!(diffs[1].before, None);
        assert_eq!(diffs[1].after, Some(0));
    }

    #[test]
    fn the_changed_bytes_count_counts_differences_and_both_lengths() {
        let before = archive(vec![entry("Save", b"abcd")]);
        let after = archive(vec![entry("Save", b"abXd")]);
        let diffs = compare(&before, &after);
        assert_eq!(diffs[0].changed_bytes, 1);
        assert_eq!(diffs[0].state, State::Changed);

        // A longer save counts its extra bytes as changed.
        let after_longer = archive(vec![entry("Save", b"abcdEF")]);
        let diffs = compare(&before, &after_longer);
        assert_eq!(diffs[0].changed_bytes, 2);
    }

    #[test]
    fn a_changed_save_type_is_a_change_even_with_identical_bytes() {
        let mut old = entry("Save", b"data");
        old.file_type = 1;
        let mut new = entry("Save", b"data");
        new.file_type = 2;
        let diffs = compare(&archive(vec![old]), &archive(vec![new]));
        assert_eq!(diffs[0].state, State::Changed);
        assert!(diffs[0].type_changed);
        assert_eq!(diffs[0].changed_bytes, 0);
    }

    #[test]
    fn the_order_is_the_first_backup_s_then_those_only_in_the_second() {
        let before = archive(vec![entry("One", b"1"), entry("Two", b"2")]);
        let after = archive(vec![entry("Three", b"3"), entry("One", b"1")]);
        let diffs = compare(&before, &after);
        let names: Vec<_> = diffs
            .iter()
            .map(|d| d.entry(&before, &after).label())
            .collect();
        assert_eq!(names, ["One", "Two", "Three"]);
    }

    #[test]
    fn byte_changes_report_each_differing_offset_with_both_values() {
        let before = archive(vec![entry("Save", b"ab\xff")]);
        let after = archive(vec![entry("Save", b"aX")]);
        let diffs = compare(&before, &after);
        let changes = diffs[0].byte_changes(&before, &after);
        assert_eq!(
            changes,
            vec![(1, Some(b'b'), Some(b'X')), (2, Some(0xff), None)]
        );
    }

    #[test]
    fn entry_prefers_the_second_backup_and_falls_back_to_the_first() {
        let before = archive(vec![entry("Save", b"old")]);
        let after = archive(vec![entry("Save", b"new")]);
        let diffs = compare(&before, &after);
        assert_eq!(diffs[0].entry(&before, &after).data, b"new");

        // Removed: only the first backup has it.
        let empty = archive(vec![]);
        let diffs = compare(&before, &empty);
        assert_eq!(diffs[0].entry(&before, &empty).data, b"old");
    }

    #[test]
    fn a_comparison_counts_each_state_and_lists_every_save() {
        let before = archive(vec![
            entry("Same", b"a"),
            entry("Edit", b"b"),
            entry("Gone", b"c"),
        ]);
        let after = archive(vec![
            entry("Edit", b"B"),
            entry("Same", b"a"),
            entry("Fresh", b"d"),
        ]);
        let result = comparison(&before, &after);
        assert_eq!(result.added, 1);
        assert_eq!(result.removed, 1);
        assert_eq!(result.changed, 1);
        assert_eq!(result.unchanged, 1);
        assert_eq!(result.rows.len(), 4);
        let states: Vec<_> = result.rows.iter().map(|r| r.state.as_str()).collect();
        assert_eq!(states, ["Unchanged", "Changed", "Removed", "Added"]);
    }

    #[test]
    fn a_comparison_row_carries_sizes_and_the_after_index_for_the_view() {
        let before = archive(vec![entry("Save", b"1234")]);
        let after = archive(vec![entry("Other", b"x"), entry("Save", b"12X45")]);
        let result = comparison(&before, &after);
        let row = result.rows.iter().find(|r| r.name == "Save").unwrap();
        assert_eq!(row.before_size, Some(4));
        assert_eq!(row.after_size, Some(5));
        assert_eq!(row.after_index, Some(1));
        assert_eq!(row.changed_bytes, 2);
    }

    #[test]
    fn a_comparison_shows_at_most_512_byte_changes_per_save() {
        let before = archive(vec![entry("Save", &vec![0u8; 600])]);
        let after = archive(vec![entry("Save", &vec![1u8; 600])]);
        let result = comparison(&before, &after);
        assert_eq!(result.rows[0].changes.len(), 512);
        // The count is the full total, not the truncated list.
        assert_eq!(result.rows[0].changed_bytes, 600);
    }

    #[test]
    fn reading_a_missing_labels_file_gives_blank_labels_and_no_bytes() {
        let temp = TempDir::new("labels-missing");
        let (labels, bytes) = Labels::read(&temp.path().join("slot-labels.txt")).unwrap();
        assert_eq!(labels, Labels::default());
        assert!(bytes.is_none(), "no file to compare against later");
    }

    #[test]
    fn reading_a_labels_file_returns_its_exact_bytes_for_a_later_check() {
        let temp = TempDir::new("labels-read");
        let raw = b"1\tFirst\n";
        fs::write(temp.path().join("slot-labels.txt"), raw).unwrap();
        let (labels, bytes) = Labels::read(&temp.path().join("slot-labels.txt")).unwrap();
        assert_eq!(labels.0[0], "First");
        assert_eq!(bytes.as_deref(), Some(raw.as_slice()));
    }

    #[test]
    fn a_written_labels_file_ends_with_a_zero_byte_as_the_console_does() {
        let temp = TempDir::new("labels-save");
        let path = temp.path().join("slot-labels.txt");
        let mut labels = Labels::default();
        labels.0[2] = "Third".into();
        labels.save(&path, None).unwrap();

        let written = fs::read(&path).unwrap();
        assert_eq!(written, b"3\tThird\n\0");
        // And it reads back as the same labels.
        let (again, _) = Labels::read(&path).unwrap();
        assert_eq!(again, labels);
    }

    /// A fresh folder removed when the test ends, so no test here can reach a
    /// real drive or its saves.
    struct TempDir(PathBuf);
    impl TempDir {
        fn new(label: &str) -> Self {
            use std::cell::Cell;
            thread_local! {
                static COUNTER: Cell<u32> = const { Cell::new(0) };
            }
            let n = COUNTER.with(|c| {
                let n = c.get();
                c.set(n + 1);
                n
            });
            let path = std::env::temp_dir()
                .join(format!("ohmy3do-backup-{}-{label}-{n}", std::process::id()));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
        fn path(&self) -> &Path {
            &self.0
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
}
