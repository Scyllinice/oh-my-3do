//! Save backup tools: comparing two backups, naming slots and their labels,
//! and building a backup from chosen saves.

use oh_my_3do_library::{
    backup::{self, Labels, State},
    save_archive::{Archive, Entry},
};
use std::fs;
fn entry(name: &str, data: &[u8], file_type: u32) -> Entry {
    Entry {
        name: name.as_bytes().to_vec(),
        data: data.to_vec(),
        file_type,
    }
}
#[test]
fn comparison_pairs_case_insensitively_and_counts_exact_bytes_and_metadata() {
    let a = Archive {
        entries: vec![
            entry("DoomPrefs", &[1, 2, 3], 1),
            entry("Removed", &[5], 0),
            entry("Empty", &[], 1),
        ],
    };
    let b = Archive {
        entries: vec![
            entry("doomPREFS", &[1, 8, 3, 4], 2),
            entry("Empty", &[], 1),
            entry("Added", &[], 0),
        ],
    };
    let d = backup::compare(&a, &b);
    assert_eq!(
        d.iter().map(|d| d.state.clone()).collect::<Vec<_>>(),
        vec![
            State::Changed,
            State::Removed,
            State::Unchanged,
            State::Added
        ]
    );
    assert_eq!(d[0].changed_bytes, 2);
    assert!(d[0].type_changed && d[0].name_changed);
    assert_eq!(
        d[0].byte_changes(&a, &b),
        vec![(1, Some(2), Some(8)), (3, None, Some(4))]
    );
    assert_eq!(d[1].byte_changes(&a, &b), vec![(0, Some(5), None)]);
    assert_eq!(d[3].changed_bytes, 0);
    assert!(backup::compare(&a, &a)
        .iter()
        .all(|d| d.state == State::Unchanged));
}
#[test]
fn labels_are_bounded_shared_text_with_firmware_tail_support() {
    let labels =
        Labels::parse(b"1\tBefore editing\r\n8\tDOOM level 7\n\0old physical tail").unwrap();
    assert_eq!(labels.0[0], "Before editing");
    assert_eq!(labels.0[7], "DOOM level 7");
    assert_eq!(Labels::parse(&labels.encode().unwrap()).unwrap(), labels);
    for bad in [
        b"0\tbad".as_slice(),
        b"9\tbad",
        b"1\tfirst\n1\tduplicate",
        b"1\tbad\tname",
        b"1\t\xff",
        b"1\t1234567890123456789012345",
    ] {
        assert!(Labels::parse(bad).is_err());
    }
    let mut invalid = labels.clone();
    invalid.0[0] = "injected\n2\tlabel".into();
    assert!(invalid.encode().is_err());
    assert_eq!(Labels::parse(b"1\t\n").unwrap(), Labels::default());
    assert!(backup::slot(std::path::Path::new("saves/SLOT08.3ds")).is_some());
    assert!(backup::slot(std::path::Path::new("saves/recovery.3ds")).is_none());
}
#[test]
fn naming_changes_only_sidecar_preserves_labels_and_refuses_conflicts() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("slot-labels.txt");
    let archive = root.path().join("slot01.3ds");
    fs::write(&archive, b"unchanged archive").unwrap();
    let (mut labels, original) = Labels::read(&path).unwrap();
    assert!(original.is_none());
    labels.0[0] = "Before editing".into();
    labels.0[7] = "DOOM level 7".into();
    labels.save(&path, None).unwrap();
    let (mut next, original) = Labels::read(&path).unwrap();
    next.0[0].clear();
    next.save(&path, original.as_deref()).unwrap();
    let (saved, stale) = Labels::read(&path).unwrap();
    assert!(saved.0[0].is_empty());
    assert_eq!(saved.0[7], "DOOM level 7");
    fs::write(&path, b"2\tExternal name\n").unwrap();
    assert!(next.save(&path, stale.as_deref()).is_err());
    assert_eq!(fs::read(&path).unwrap(), b"2\tExternal name\n");
    assert_eq!(fs::read(archive).unwrap(), b"unchanged archive");
}
#[test]
fn arbitrary_numbered_slots_use_independent_pages_of_names() {
    let (first, i) = backup::slot(std::path::Path::new("saves/slot08.3ds")).unwrap();
    assert_eq!(i, 7);
    assert!(first.ends_with("slot-labels.txt"));
    let (next, i) = backup::slot(std::path::Path::new("saves/slot09.3ds")).unwrap();
    assert_eq!(i, 0);
    assert!(next.ends_with("slot-labels-1.txt"));
    let (last, i) = backup::slot(std::path::Path::new("saves/slot999999999.3ds")).unwrap();
    assert_eq!(i, 6);
    assert!(last.ends_with("slot-labels-124999999.txt"));
    for name in ["slot0.3ds", "slot001.3ds", "slot1000000000.3ds"] {
        assert!(backup::slot(std::path::Path::new(name)).is_none());
    }
}
#[test]
fn manual_slot_building_merges_only_chosen_entries_and_is_atomic_at_capacity() {
    let mut draft = Archive::default();
    draft
        .merge_entries(&[entry("GameA", &[1, 2], 1), entry("GameB", &[9], 2)])
        .unwrap();
    draft
        .merge_entries(&[entry("gamea", &[3, 4], 8), entry("GameC", &[7], 3)])
        .unwrap();
    assert_eq!(draft.entries.len(), 3);
    assert_eq!(draft.entries[0].name, b"GameA");
    assert_eq!(draft.entries[0].data, [3, 4]);
    assert_eq!(draft.entries[1], entry("GameB", &[9], 2));
    let before = draft.clone();
    assert!(draft
        .merge_entries(&[entry("TooBig", &vec![0; 65536], 1)])
        .is_err());
    assert_eq!(draft, before);
    assert!(draft
        .merge_entries(&[entry("../invalid", &[1], 0)])
        .is_err());
    assert_eq!(draft, before);
    assert_eq!(Archive::parse(&draft.encode().unwrap()).unwrap(), draft);
}

#[test]
fn comparison_report_labels_games_preserves_direction_and_exposes_current_editor_index() {
    let reference = Archive {
        entries: vec![
            entry("DoomPrefs", &[1, 2, 3], 0),
            entry("Removed", &[9], 0),
            entry("Same", &[], 0),
        ],
    };
    let current = Archive {
        entries: vec![
            entry("Same", &[], 0),
            entry("DoomPrefs", &[1, 4, 3, 5], 2),
            entry("Added", &[8], 0),
        ],
    };
    let report = backup::comparison(&reference, &current);
    assert_eq!(
        (
            report.added,
            report.removed,
            report.changed,
            report.unchanged
        ),
        (1, 1, 1, 1)
    );
    let doom = &report.rows[0];
    assert_eq!(doom.game, "DOOM");
    assert_eq!(doom.after_index, Some(1));
    assert_eq!(doom.before_size, Some(3));
    assert_eq!(doom.after_size, Some(4));
    assert!(doom.type_changed);
    assert_eq!(
        doom.changes,
        vec![(1, Some(2), Some(4)), (3, None, Some(5))]
    );
    assert_eq!(report.rows[1].after_index, None);
    assert_eq!(report.rows[3].changes, vec![(0, None, Some(8))]);
}
#[test]
fn comparison_limits_byte_details_but_counts_all_changes_and_detects_metadata_only_edits() {
    let reference = Archive {
        entries: vec![entry("Huge", &vec![0; 1024], 0), entry("Case", &[], 0)],
    };
    let current = Archive {
        entries: vec![entry("Huge", &vec![1; 1024], 0), entry("CASE", &[], 1)],
    };
    let report = backup::comparison(&reference, &current);
    assert_eq!(report.changed, 2);
    assert_eq!(report.rows[0].changed_bytes, 1024);
    assert_eq!(report.rows[0].changes.len(), 512);
    assert_eq!(report.rows[1].changed_bytes, 0);
    assert!(report.rows[1].type_changed && report.rows[1].name_changed);
}
