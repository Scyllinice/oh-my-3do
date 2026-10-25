use oh_my_3do_library::{
    recovery,
    save_archive::{Archive, Entry},
    Job,
};
use std::{
    fs,
    path::{Path, PathBuf},
};
fn archive() -> Vec<u8> {
    Archive {
        entries: vec![Entry {
            name: b"DoomPrefs".to_vec(),
            file_type: 0,
            data: vec![1, 2],
        }],
    }
    .encode()
    .unwrap()
}
fn copy(root: &Path, original: &str) -> PathBuf {
    let p = root.join(format!("{original}.{}.bak", uuid::Uuid::new_v4()));
    fs::write(&p, archive()).unwrap();
    p
}
#[test]
fn cleanup_only_lists_app_created_save_copies_and_never_live_slots_or_other_backups() {
    let root = tempfile::tempdir().unwrap();
    let a = copy(root.path(), "slot01.3ds");
    let b = copy(root.path(), "slot01.3ds");
    for name in [
        "slot01.3ds",
        "recovery.3ds",
        "recovery2.3ds",
        "notes.bak",
        "slot01.3ds.not-a-uuid.bak",
        "config.ini.12345678-1234-4234-8234-123456789abc.bak",
    ] {
        fs::write(root.path().join(name), archive()).unwrap();
    }
    let scan = recovery::scan(root.path(), &Job::default()).unwrap();
    assert_eq!(scan.copies.len(), 2);
    assert_eq!(scan.copies.iter().filter(|c| c.newest).count(), 1);
    assert_eq!(scan.copies.iter().filter(|c| c.keep).count(), 1);
    let selected = vec![a.file_name().unwrap().to_str().unwrap().into()];
    let removed = recovery::remove(&scan, &selected, &Job::default()).unwrap();
    assert_eq!(removed.names, selected);
    assert!(!a.exists());
    assert!(b.exists());
    assert!(root.path().join("slot01.3ds").exists());
    assert!(root.path().join("recovery.3ds").exists());
    assert!(recovery::remove(&scan, &["recovery.3ds".into()], &Job::default()).is_err());
}
#[test]
fn stale_copy_fails_the_whole_preflight_without_deleting_another_selection() {
    let root = tempfile::tempdir().unwrap();
    let a = copy(root.path(), "save.3ds");
    let b = copy(root.path(), "save.3ds");
    let scan = recovery::scan(root.path(), &Job::default()).unwrap();
    fs::write(&b, b"changed").unwrap();
    let names = vec![
        a.file_name().unwrap().to_str().unwrap().into(),
        b.file_name().unwrap().to_str().unwrap().into(),
    ];
    assert!(recovery::remove(&scan, &names, &Job::default()).is_err());
    assert!(a.exists() && b.exists());
}
#[test]
fn selection_never_includes_new_files_and_cancelled_cleanup_keeps_copies() {
    let root = tempfile::tempdir().unwrap();
    let a = copy(root.path(), "save.3ds");
    let scan = recovery::scan(root.path(), &Job::default()).unwrap();
    let b = copy(root.path(), "save.3ds");
    assert!(recovery::remove(
        &scan,
        &[b.file_name().unwrap().to_str().unwrap().into()],
        &Job::default()
    )
    .is_err());
    let job = Job::default();
    job.cancel.store(true, std::sync::atomic::Ordering::Relaxed);
    assert!(recovery::remove(
        &scan,
        &[a.file_name().unwrap().to_str().unwrap().into()],
        &job
    )
    .is_err());
    assert!(a.exists() && b.exists());
}
#[test]
fn older_selection_keeps_a_valid_copy_when_the_newest_is_invalid() {
    let root = tempfile::tempdir().unwrap();
    let valid = copy(root.path(), "save.3ds");
    std::thread::sleep(std::time::Duration::from_millis(5));
    let invalid = copy(root.path(), "save.3ds");
    fs::write(&invalid, b"invalid original").unwrap();
    let scan = recovery::scan(root.path(), &Job::default()).unwrap();
    assert_eq!(scan.copies.iter().filter(|c| c.keep).count(), 2);
    assert!(
        scan.copies
            .iter()
            .find(|c| c.name == valid.file_name().unwrap().to_str().unwrap())
            .unwrap()
            .keep
    );
}
#[cfg(unix)]
#[test]
fn links_are_not_cleanup_candidates() {
    let root = tempfile::tempdir().unwrap();
    let outside = tempfile::NamedTempFile::new().unwrap();
    let name = format!("slot01.3ds.{}.bak", uuid::Uuid::new_v4());
    std::os::unix::fs::symlink(outside.path(), root.path().join(name)).unwrap();
    assert!(recovery::scan(root.path(), &Job::default())
        .unwrap()
        .copies
        .is_empty());
}
