//! Cleaning a drive: removing only the menu's files, copying save backups to
//! the PC first, and refusing drives that hold a library or changed.

use oh_my_3do_library::{drive_cleanup, setup, Job};
use std::{
    fs,
    path::{Path, PathBuf},
};

fn put(root: &Path, name: &str, bytes: &[u8]) -> PathBuf {
    let path = root.join(name);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(&path, bytes).unwrap();
    path
}

/// A 3DO image header followed by the settings name every menu contains.
fn menu_image() -> Vec<u8> {
    let mut image = vec![0; 4096];
    image[..7].copy_from_slice(&[1, 90, 90, 90, 90, 90, 1]);
    image[1000..1011].copy_from_slice(b"ohmy3do.ini");
    image
}

/// The navigation helper exactly as the app writes it.
fn helper() -> Vec<u8> {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("drive");
    put(&root, "Games/USA/Game/Game.iso", b"disc");
    let mut tx = oh_my_3do_library::library::Journal::new(&root, &t.path().join("cache")).unwrap();
    setup::ensure_navigation(&root, &mut tx, &Job::default()).unwrap();
    fs::read(root.join(format!("Games/{}", setup::NAVIGATION_HELPER))).unwrap()
}

/// A drive as the app leaves it, plus things cleanup must not touch.
fn prepared_drive(root: &Path) {
    put(root, "boot.iso", &menu_image());
    put(root, "boot.iso.sha256", b"checksum");
    put(root, ".ohmy3do/ohmy3do.ini", b"[menu]");
    put(root, ".ohmy3do/saves/Game/SAVE1", b"save one");
    put(root, ".ohmy3do/saves/slot-catalog.3di", b"catalog");
    put(root, "Games/USA/Game/Game.iso", b"disc");
    put(root, "Games/USA/Game/.ohmy3do/cover.3dc", b"cover");
    put(
        root,
        &format!("Games/{}", setup::NAVIGATION_HELPER),
        &helper(),
    );
    put(root, "Games/USA/Game/notes.txt", b"mine");
    put(
        root,
        "Games/USA/Other/Oh My 3DO - Folder Navigation Helper (Keep).iso",
        b"not ours",
    );
}

#[test]
fn cleanup_removes_only_menu_files_and_keeps_games() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("drive");
    prepared_drive(&root);
    put(&root, "menu-demo.iso", b"another menu");

    let plan = drive_cleanup::review(&root, &Job::default()).unwrap();
    let paths: Vec<_> = plan.files.iter().map(|file| file.path.as_str()).collect();
    assert_eq!(
        paths,
        [
            ".ohmy3do/ohmy3do.ini",
            ".ohmy3do/saves/Game/SAVE1",
            ".ohmy3do/saves/slot-catalog.3di",
            "Games/Oh My 3DO - Folder Navigation Helper (Keep).iso",
            "Games/USA/Game/.ohmy3do/cover.3dc",
            "boot.iso",
            "boot.iso.sha256",
        ]
    );
    assert_eq!(plan.saves.len(), 2);
    assert_eq!(plan.kept, ["menu-demo.iso"]);

    let cleaned = drive_cleanup::apply(&plan, None, &Job::default()).unwrap();
    assert_eq!(cleaned.files, 7);
    assert!(cleaned.warnings.is_empty());
    assert!(!root.join(".ohmy3do").exists());
    assert!(!root.join("Games/USA/Game/.ohmy3do").exists());
    assert_eq!(
        fs::read(root.join("Games/USA/Game/Game.iso")).unwrap(),
        b"disc"
    );
    assert_eq!(
        fs::read(root.join("Games/USA/Game/notes.txt")).unwrap(),
        b"mine"
    );
    assert_eq!(
        fs::read(root.join("menu-demo.iso")).unwrap(),
        b"another menu"
    );
    assert!(root
        .join("Games/USA/Other/Oh My 3DO - Folder Navigation Helper (Keep).iso")
        .exists());
}

#[test]
fn cleanup_copies_save_backups_to_the_pc_first() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("drive");
    let pc = t.path().join("pc");
    prepared_drive(&root);
    fs::create_dir(&pc).unwrap();
    fs::create_dir(pc.join("Oh My 3DO saves")).unwrap();

    let plan = drive_cleanup::review(&root, &Job::default()).unwrap();
    let cleaned = drive_cleanup::apply(&plan, Some(&pc), &Job::default()).unwrap();
    let copied = cleaned.saves_copied_to.unwrap();
    assert_eq!(copied, pc.canonicalize().unwrap().join("Oh My 3DO saves 2"));
    assert_eq!(fs::read(copied.join("Game/SAVE1")).unwrap(), b"save one");
    assert_eq!(
        fs::read(copied.join("slot-catalog.3di")).unwrap(),
        b"catalog"
    );
    assert!(!root.join(".ohmy3do").exists());
}

#[test]
fn cleanup_refuses_libraries_saves_on_the_drive_and_changed_drives() {
    let t = tempfile::tempdir().unwrap();
    let library = t.path().join("library");
    put(&library, ".ohmy3do/library.json", b"{}");
    assert!(drive_cleanup::review(&library, &Job::default()).is_err());

    let root = t.path().join("drive");
    prepared_drive(&root);
    let plan = drive_cleanup::review(&root, &Job::default()).unwrap();
    assert!(drive_cleanup::apply(&plan, Some(&root.join("Games")), &Job::default()).is_err());
    assert!(root.join(".ohmy3do/saves/Game/SAVE1").exists());

    put(&root, ".ohmy3do/favorites.txt", b"new since the review");
    assert!(drive_cleanup::apply(&plan, None, &Job::default()).is_err());
    assert!(root.join("boot.iso").exists());
}
