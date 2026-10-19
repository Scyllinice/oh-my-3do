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
