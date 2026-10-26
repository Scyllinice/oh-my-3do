//! Setting up a drive: installing the menu and its support files, folder
//! navigation helpers, repairing a drive, and leaving it unchanged on
//! cancellation or failure.

// Tests pass one-item slices as &[path.clone()], which reads more plainly than
// std::slice::from_ref(&path); the copy is free at test scale.
#![allow(clippy::cloned_ref_to_slice_refs)]

use oh_my_3do_library::{
    console_config::{self, Settings},
    library, setup, Job,
};
use sha2::{Digest, Sha256};
use std::{fs, sync::atomic::Ordering};
fn image() -> Vec<u8> {
    let mut bytes = vec![0; 2048];
    bytes[..7].copy_from_slice(&[1, 90, 90, 90, 90, 90, 1]);
    bytes
}
#[test]
fn folder_only_chains_get_navigation_helpers_without_becoming_games() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let game = root.path().join("Games/USA/Doom/Doom.iso");
    fs::create_dir_all(game.parent().unwrap()).unwrap();
    fs::write(&game, image()).unwrap();
    let journal = setup::install_verified(
        root.path(),
        cache.path(),
        &image(),
        &Settings::default(),
        &Job::default(),
    )
    .unwrap();
    for directory in ["Games", "Games/USA"] {
        let data = fs::read(
            root.path()
                .join(directory)
                .join("Oh My 3DO - Folder Navigation Helper (Keep).iso"),
        )
        .unwrap();
        assert_eq!(data.len(), 2048);
        assert_eq!(&data[..7], &[1, 90, 90, 90, 90, 90, 1]);
    }
    assert!(!game
        .parent()
        .unwrap()
        .join("Oh My 3DO - Folder Navigation Helper (Keep).iso")
        .exists());
    let discs =
        oh_my_3do_library::disc::collect(&[root.path().to_owned()], None, &Job::default()).unwrap();
    assert_eq!(discs.len(), 1);
    // Collected paths are canonical (on Windows, the \\?\ long-path form).
    assert_eq!(discs[0].0, game.canonicalize().unwrap());
    let helper = root
        .path()
        .join("Games/Oh My 3DO - Folder Navigation Helper (Keep).iso");
    assert!(oh_my_3do_library::disc::collect(&[helper.clone()], None, &Job::default()).is_err());
    let index = fs::read(root.path().join(".ohmy3do/library.3di")).unwrap();
    assert!(!index
        .windows(b"Folder Navigation Helper".len())
        .any(|s| s == b"Folder Navigation Helper"));
    setup::install_verified(
        root.path(),
        cache.path(),
        &image(),
        &Settings::default(),
        &Job::default(),
    )
    .unwrap();
    library::undo(&journal).unwrap();
    assert!(!helper.exists());
    assert_eq!(fs::read(&game).unwrap(), image());
}

#[test]
fn navigation_helper_never_overwrites_an_existing_file() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    fs::create_dir_all(root.path().join("Games/Title")).unwrap();
    fs::write(root.path().join("Games/Title/Game.iso"), image()).unwrap();
    let helper = root
        .path()
        .join("Games/Oh My 3DO - Folder Navigation Helper (Keep).iso");
    fs::write(&helper, b"personal file").unwrap();
    assert!(setup::install_verified(
        root.path(),
        cache.path(),
        &image(),
        &Settings::default(),
        &Job::default()
    )
    .is_err());
    assert_eq!(fs::read(helper).unwrap(), b"personal file");
    assert!(!root.path().join("boot.iso").exists());
}
#[test]
fn release_digest_and_image_header_must_both_match() {
    let image = image();
    let checksum = format!("{:x}  boot.iso\n", Sha256::digest(&image));
    setup::verify_image(&image, &checksum).unwrap();
    let mut corrupt = image.clone();
    corrupt[100] = 1;
    assert!(setup::verify_image(&corrupt, &checksum).is_err());
    assert!(setup::verify_image(&image, &checksum.replace("boot.iso", "other.iso")).is_err());
    assert!(
        setup::verify_image(b"bad", &format!("{:x}  boot.iso", Sha256::digest(b"bad"))).is_err()
    );
}
#[test]
fn installation_creates_required_files_preserves_settings_and_undoes() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let settings = Settings {
        hide_adult: true,
        ..Default::default()
    };
    fs::write(root.path().join("keep.txt"), b"keep").unwrap();
    let journal = setup::install_verified(
        root.path(),
        cache.path(),
        &image(),
        &settings,
        &Job::default(),
    )
    .unwrap();
    assert!(root.path().join(".ohmy3do/saves").is_dir());
    assert!(root.path().join(".ohmy3do/library.3di").is_file());
    assert_eq!(
        console_config::load(root.path()).unwrap().settings,
        settings
    );
    let original = fs::read(root.path().join(console_config::PATH)).unwrap();
    setup::install_verified(
        root.path(),
        cache.path(),
        &image(),
        &Settings::default(),
        &Job::default(),
    )
    .unwrap();
    assert_eq!(
        fs::read(root.path().join(console_config::PATH)).unwrap(),
        original
    );
    library::undo(&journal).unwrap();
    assert_eq!(fs::read(root.path().join("keep.txt")).unwrap(), b"keep");
    assert!(!root.path().join("boot.iso").exists());
}
#[test]
fn cancellation_and_failure_leave_library_unchanged() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let job = Job::default();
    job.cancel.store(true, Ordering::Relaxed);
    assert!(setup::install_verified(
        root.path(),
        cache.path(),
        &image(),
        &Settings::default(),
        &job
    )
    .is_err());
    assert_eq!(fs::read_dir(root.path()).unwrap().count(), 0);
    fs::create_dir(root.path().join("boot.iso")).unwrap();
    assert!(setup::install_verified(
        root.path(),
        cache.path(),
        &image(),
        &Settings::default(),
        &Job::default()
    )
    .is_err());
    assert!(root.path().join("boot.iso").is_dir());
    assert!(!root.path().join(".ohmy3do").exists());
}

#[test]
fn local_iso_prepares_an_empty_drive_without_a_library_and_repairs_boot_only_drives() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let source = tempfile::tempdir().unwrap();
    let iso = source.path().join("downloaded.iso");
    fs::write(&iso, image()).unwrap();
    let journal = setup::install_local(
        root.path(),
        cache.path(),
        &iso,
        &Settings::default(),
        &Job::default(),
    )
    .unwrap();
    for dir in [".ohmy3do/saves", ".ohmy3do/covers", ".ohmy3do/previews"] {
        assert!(root.path().join(dir).is_dir(), "{dir}");
    }
    for file in [
        "boot.iso",
        console_config::PATH,
        ".ohmy3do/save-cheats.json",
        ".ohmy3do/save-cheats.3db",
        ".ohmy3do/library.3di",
        ".ohmy3do/metadata.3di",
        ".ohmy3do/save-names.txt",
        ".ohmy3do/saves/slot-catalog.3di",
    ] {
        assert!(root.path().join(file).is_file(), "{file}");
    }
    let index = fs::read(root.path().join(".ohmy3do/library.3di")).unwrap();
    assert_eq!(&index[8..12], &0u32.to_be_bytes());
    assert!(!root.path().join("Games").exists());
    assert!(!root.path().join(".ohmy3do/library.json").exists());
    library::undo(&journal).unwrap();
    assert_eq!(fs::read_dir(root.path()).unwrap().count(), 0);
    // A drive with only boot.iso is the failure users hit with the previous installer.
    let boot = root.path().join("boot.iso");
    fs::write(&boot, image()).unwrap();
    fs::create_dir_all(root.path().join(".ohmy3do/saves")).unwrap();
    fs::write(root.path().join(".ohmy3do/library.3di"), b"").unwrap();
    fs::write(root.path().join(".ohmy3do/ohmy3do.ini"), b"").unwrap();
    fs::write(root.path().join(".ohmy3do/save-cheats.3db"), b"").unwrap();
    fs::write(root.path().join(".ohmy3do/saves/slot-catalog.3di"), b"").unwrap();
    setup::install_local(
        root.path(),
        cache.path(),
        &boot,
        &Settings::default(),
        &Job::default(),
    )
    .unwrap();
    assert!(root.path().join(".ohmy3do/saves").is_dir());
    assert_eq!(fs::read(&boot).unwrap(), image());
    assert!(
        fs::metadata(root.path().join(".ohmy3do/library.3di"))
            .unwrap()
            .len()
            > 0
    );
    assert_eq!(
        fs::metadata(root.path().join(".ohmy3do/saves/slot-catalog.3di"))
            .unwrap()
            .len(),
        28
    );
    assert_eq!(
        console_config::load(root.path()).unwrap().settings,
        Settings::default()
    );
    assert!(fs::read(root.path().join(".ohmy3do/save-cheats.3db"))
        .unwrap()
        .starts_with(b"OM3DCHT2"));
}

#[test]
fn support_repair_preserves_custom_files_and_indexes_existing_backups() {
    use oh_my_3do_library::save_archive::{Archive, Entry};
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    fs::write(root.path().join("boot.iso"), image()).unwrap();
    fs::create_dir_all(root.path().join(".ohmy3do/saves")).unwrap();
    let backup = Archive {
        entries: vec![Entry {
            name: b"DoomPrefs".to_vec(),
            file_type: 42,
            data: vec![1, 2, 3],
        }],
    }
    .encode()
    .unwrap();
    fs::write(root.path().join(".ohmy3do/saves/slot09.3ds"), &backup).unwrap();
    let keep = [
        (".ohmy3do/ohmy3do.ini", b"personal settings".as_slice()),
        (".ohmy3do/library.3di", b"personal index"),
        (".ohmy3do/metadata.3di", b"personal metadata"),
        (".ohmy3do/save-names.txt", b"=DoomPrefs\tMy DOOM\n"),
        (".ohmy3do/favorites.txt", b"Games/DOOM.iso\n"),
    ];
    for (path, data) in keep {
        fs::write(root.path().join(path), data).unwrap();
    }
    fs::write(
        root.path().join(".ohmy3do/saves/slot-catalog.3di"),
        b"outdated catalog",
    )
    .unwrap();
    // Existing runtime indexes must not trigger a whole-library rescan merely
    // because this USB drive lacks the desktop's optional per-disc cache.
    fs::create_dir(root.path().join("Games")).unwrap();
    let cue = b"FILE \"missing.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n";
    fs::write(root.path().join("Games/broken.cue"), cue).unwrap();
    let boot = root.path().join("boot.iso");
    let journal = setup::install_local(
        root.path(),
        cache.path(),
        &boot,
        &Settings::default(),
        &Job::default(),
    )
    .unwrap();
    for (path, data) in keep {
        assert_eq!(fs::read(root.path().join(path)).unwrap(), data);
    }
    assert_eq!(
        fs::read(root.path().join(".ohmy3do/saves/slot09.3ds")).unwrap(),
        backup
    );
    assert!(root.path().join(".ohmy3do/saves/slot09.3si").is_file());
    assert_eq!(fs::read(root.path().join("Games/broken.cue")).unwrap(), cue);
    assert!(!root.path().join(".ohmy3do/save-names-cache.json").exists());
    library::undo(&journal).unwrap();
    assert_eq!(
        fs::read(root.path().join(".ohmy3do/saves/slot-catalog.3di")).unwrap(),
        b"outdated catalog"
    );
    assert_eq!(fs::read(&boot).unwrap(), image());
    for (path, data) in keep {
        assert_eq!(fs::read(root.path().join(path)).unwrap(), data);
    }
}

#[test]
fn setup_collision_rolls_back_support_and_invalid_local_images_leave_drive_untouched() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let source = tempfile::tempdir().unwrap();
    let iso = source.path().join("boot.iso");
    fs::write(&iso, b"not a boot ISO").unwrap();
    assert!(setup::install_local(
        root.path(),
        cache.path(),
        &iso,
        &Settings::default(),
        &Job::default()
    )
    .is_err());
    assert_eq!(fs::read_dir(root.path()).unwrap().count(), 0);
    fs::write(&iso, image()).unwrap();
    fs::create_dir(root.path().join(".ohmy3do")).unwrap();
    fs::write(root.path().join(".ohmy3do/saves"), b"occupied").unwrap();
    assert!(setup::install_local(
        root.path(),
        cache.path(),
        &iso,
        &Settings::default(),
        &Job::default()
    )
    .is_err());
    assert_eq!(
        fs::read(root.path().join(".ohmy3do/saves")).unwrap(),
        b"occupied"
    );
    assert!(!root.path().join("boot.iso").exists());
}
