use oh_my_3do_library::{
    backup::Labels,
    library::Journal,
    save_archive::{Archive, Entry},
    save_catalog, saves, Job,
};
use std::fs;
fn backup(path: &std::path::Path, names: &[&str]) {
    let archive = Archive {
        entries: names
            .iter()
            .map(|n| Entry {
                name: n.as_bytes().to_vec(),
                file_type: 42,
                data: vec![1, 2, 3],
            })
            .collect(),
    };
    fs::write(path, archive.encode().unwrap()).unwrap();
}
#[test]
fn discovers_sparse_slots_games_labels_raw_names_and_recovery_without_altering_archives() {
    let t = tempfile::tempdir().unwrap();
    let dir = t.path().join(".ohmy3do/saves");
    fs::create_dir_all(&dir).unwrap();
    backup(&dir.join("slot01.3ds"), &["EA_RRX", "UnknownSave"]);
    backup(&dir.join("slot1000.3ds"), &["icebreaker[levelchart]"]);
    backup(&dir.join("slot999999999.3ds"), &["DoomPrefs"]);
    backup(&dir.join("recovery.3ds"), &["DoomPrefs"]);
    backup(&dir.join("slot20.3ds"), &[]);
    fs::write(dir.join("slot02.3ds"), b"").unwrap(); // Firmware probe artifact.
    fs::write(dir.join("slot03.3ds"), b"bad backup").unwrap();
    fs::write(dir.join("slot0004.3ds"), b"noncanonical").unwrap();
    let mut labels = Labels::default();
    labels.0[7] = "Ice goals".into();
    fs::write(dir.join("slot-labels-124.txt"), labels.encode().unwrap()).unwrap();
    fs::write(
        dir.parent().unwrap().join("saves-learned.txt"),
        b"=UnknownSave\tMy custom game\n",
    )
    .unwrap();
    let before = fs::read(dir.join("slot01.3ds")).unwrap();
    let slots = save_catalog::refresh(&dir, &t.path().join("cache"), &Job::default()).unwrap();
    assert_eq!(slots.len(), 6);
    assert_eq!(slots.iter().filter(|s| s.matches("road rash")).count(), 1);
    assert_eq!(
        slots
            .iter()
            .filter(|s| s.matches("ICE GOALS icebreaker"))
            .count(),
        1
    );
    assert_eq!(slots.iter().filter(|s| s.matches("unknownsave")).count(), 1);
    assert_eq!(slots.iter().filter(|s| s.matches("custom game")).count(), 1);
    assert_eq!(slots.iter().filter(|s| s.matches("doom")).count(), 2);
    assert_eq!(
        slots.iter().filter(|s| s.matches("slot999999999")).count(),
        1
    );
    assert!(slots.iter().any(|s| s.id == 3 && !s.valid));
    assert!(slots
        .iter()
        .any(|s| s.id == 20 && s.valid && s.files.is_empty()));
    assert_eq!(fs::read(dir.join("slot01.3ds")).unwrap(), before);
    let root = fs::read(dir.join(save_catalog::ROOT)).unwrap();
    assert_eq!(root, save_catalog::root_bytes(save_catalog::RECOVERY));
    let mut head = save_catalog::RECOVERY;
    for slot in slots.iter().rev() {
        assert_eq!(head, slot.id);
        let name = if head == save_catalog::RECOVERY {
            "recovery.3si".into()
        } else {
            format!("slot{head:02}.3si")
        };
        let mut b = fs::read(dir.join(name)).unwrap();
        assert_eq!(&b[..8], b"OM3DSLT1");
        let stored = u32::from_be_bytes(b[20..24].try_into().unwrap());
        b[20..24].fill(0);
        assert_eq!(crc32fast::hash(&b), stored);
        head = u32::from_be_bytes(b[16..20].try_into().unwrap());
    }
    assert_eq!(head, 0);
    // Refresh removes deleted backups from the chain, even if old sidecars remain.
    fs::remove_file(dir.join("slot01.3ds")).unwrap();
    let slots = save_catalog::refresh(&dir, &t.path().join("cache"), &Job::default()).unwrap();
    assert!(!slots.iter().any(|s| s.id == 1));
}
#[test]
fn changed_sources_and_corrupt_labels_refuse_publication_and_rollback_is_safe() {
    let t = tempfile::tempdir().unwrap();
    let dir = t.path().join("saves");
    fs::create_dir(&dir).unwrap();
    backup(&dir.join("slot01.3ds"), &["DoomPrefs"]);
    let slots = save_catalog::scan(&dir, &Job::default()).unwrap();
    fs::write(dir.join("slot01.3ds"), b"changed").unwrap();
    let mut tx = Journal::new(&dir, &t.path().join("cache")).unwrap();
    assert!(save_catalog::write(&slots, &mut tx, "", &Job::default()).is_err());
    tx.undo().unwrap();
    assert!(!dir.join(save_catalog::ROOT).exists());
    assert_eq!(fs::read(dir.join("slot01.3ds")).unwrap(), b"changed");
    fs::write(dir.join("slot-labels.txt"), b"invalid").unwrap();
    assert!(save_catalog::refresh(&dir, &t.path().join("cache"), &Job::default()).is_err());
    assert_eq!(fs::read(dir.join("slot-labels.txt")).unwrap(), b"invalid");
}
#[test]
fn builtin_names_cover_lowercase_and_bracket_names_and_keep_unknowns_unknown() {
    assert_eq!(saves::known_title("EA_RRX"), Some("Road Rash"));
    assert_eq!(
        saves::known_title("ICEBREAKER[LEVELCHART]"),
        Some("Icebreaker")
    );
    assert_eq!(saves::known_title("POed.03"), Some("PO'ed"));
    assert_eq!(saves::known_title("EA_RRX_OTHER"), None);
}
