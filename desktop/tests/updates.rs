use oh_my_3do_library::{
    fsutil, library,
    updates::{self, BootState, Release},
    Job,
};
use serde_json::{json, Value};
use std::{fs, sync::atomic::Ordering};
fn release() -> Value {
    let names = [
        "boot.iso",
        "boot.iso.sha256",
        "OhMy3DO-Library-Windows-x64-v1.10.0.zip",
        "OhMy3DO-Library-Windows-x64-v1.10.0.zip.sha256",
    ];
    json!({"tag_name":"v1.10.0","draft":false,"prerelease":false,"body":"Test release", "assets":names.iter().map(|name| json!({"name":name,"browser_download_url":format!("https://github.com/Scyllinice/oh-my-3do/releases/download/v1.10.0/{name}")})).collect::<Vec<_>>()})
}
#[test]
fn versions_are_numeric_and_only_stable_release_tags_are_accepted() {
    assert!(updates::is_newer("1.10.0", "1.9.99").unwrap());
    assert!(updates::is_newer("1.0.10", "1.0.9").unwrap());
    assert!(!updates::is_newer("1.0.0", "1.0.0").unwrap());
    assert!(!updates::is_newer("1.0.0", "2.0.0").unwrap());
    for value in [
        "1.0",
        "01.0.0",
        "1.2.-1",
        "1.0.0-beta",
        "1.0.0+build",
        "1.0.18446744073709551616",
    ] {
        assert!(updates::is_newer(value, "1.0.0").is_err());
    }
    let parsed = Release::parse(&serde_json::to_vec(&release()).unwrap()).unwrap();
    assert_eq!(parsed.version, "1.10.0");
    assert_eq!(
        parsed.desktop_name("Windows-x64").unwrap(),
        "OhMy3DO-Library-Windows-x64-v1.10.0.zip"
    );
    assert!(parsed.desktop_name("Linux-x64").is_err());
    assert!(parsed.desktop_name("../../other").is_err());
    for key in ["draft", "prerelease"] {
        let mut bad = release();
        bad[key] = json!(true);
        assert!(Release::parse(&serde_json::to_vec(&bad).unwrap()).is_err());
    }
}
#[test]
fn release_assets_cannot_redirect_downloads_to_other_repositories_or_tags() {
    for url in [
        "http://github.com/Scyllinice/oh-my-3do/releases/download/v1.10.0/boot.iso",
        "https://github.com/other/project/releases/download/v1.10.0/boot.iso",
        "https://github.com/Scyllinice/oh-my-3do/releases/download/v2.0.0/boot.iso",
        "https://github.com/Scyllinice/oh-my-3do/releases/download/v1.10.0/boot.iso?anything",
    ] {
        let mut bad = release();
        bad["assets"][0]["browser_download_url"] = json!(url);
        assert!(Release::parse(&serde_json::to_vec(&bad).unwrap()).is_err());
    }
    let mut bad = release();
    let duplicate = bad["assets"][0].clone();
    bad["assets"].as_array_mut().unwrap().push(duplicate);
    assert!(Release::parse(&serde_json::to_vec(&bad).unwrap()).is_err());
}
#[test]
fn checksum_files_must_name_exactly_the_requested_asset() {
    let digest = "A".repeat(64);
    assert_eq!(
        updates::checksum(&format!("{digest} *boot.iso\n"), "boot.iso").unwrap(),
        digest.to_ascii_lowercase()
    );
    for text in [
        digest.clone(),
        format!("{digest} other.iso"),
        format!("{digest} **boot.iso"),
        format!("{} boot.iso", "z".repeat(64)),
        format!("{digest} boot.iso\n{digest} other.iso"),
    ] {
        assert!(updates::checksum(&text, "boot.iso").is_err());
    }
}
fn image(value: u8) -> Vec<u8> {
    let mut bytes = vec![value; 2048];
    bytes[..7].copy_from_slice(&[1, 90, 90, 90, 90, 90, 1]);
    bytes
}
#[test]
fn boot_update_changes_only_the_image_and_undo_restores_it() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let boot = root.path().join("boot.iso");
    let original = image(0);
    fs::write(&boot, &original).unwrap();
    fs::create_dir_all(root.path().join(".ohmy3do/saves")).unwrap();
    let keep = [
        (".ohmy3do/ohmy3do.ini", b"personal settings".as_slice()),
        (".ohmy3do/saves/slot01.3ds", b"personal backup"),
        (".ohmy3do/library.3di", b"existing index"),
    ];
    for (name, data) in keep {
        fs::write(root.path().join(name), data).unwrap();
    }
    let expected = BootState {
        root: root.path().canonicalize().unwrap(),
        digest: Some(fsutil::sha256(&boot, &Job::default()).unwrap()),
    };
    let journal =
        updates::install_boot_bytes(&expected, &image(1), cache.path(), &Job::default()).unwrap();
    assert_eq!(fs::read(&boot).unwrap(), image(1));
    for (name, data) in keep {
        assert_eq!(fs::read(root.path().join(name)).unwrap(), data);
    }
    library::undo(&journal).unwrap();
    assert_eq!(fs::read(&boot).unwrap(), original);
}
#[test]
fn stale_boot_offers_invalid_images_and_cancellation_do_not_overwrite() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let boot = root.path().join("boot.iso");
    fs::write(&boot, image(0)).unwrap();
    let expected = BootState {
        root: root.path().canonicalize().unwrap(),
        digest: Some(fsutil::sha256(&boot, &Job::default()).unwrap()),
    };
    fs::write(&boot, image(1)).unwrap();
    assert!(
        updates::install_boot_bytes(&expected, &image(2), cache.path(), &Job::default()).is_err()
    );
    assert_eq!(fs::read(&boot).unwrap(), image(1));
    assert!(
        updates::install_boot_bytes(&expected, b"not an image", cache.path(), &Job::default())
            .is_err()
    );
    let job = Job::default();
    job.cancel.store(true, Ordering::Relaxed);
    assert!(updates::install_boot_bytes(&expected, &image(2), cache.path(), &job).is_err());
    assert_eq!(fs::read(&boot).unwrap(), image(1));
    let missing = BootState {
        root: expected.root,
        digest: None,
    };
    assert!(
        updates::install_boot_bytes(&missing, &image(2), cache.path(), &Job::default()).is_err()
    );
}
#[test]
fn new_boot_install_can_be_undone_without_removing_other_files() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    fs::write(root.path().join("keep"), b"keep").unwrap();
    let state = BootState {
        root: root.path().canonicalize().unwrap(),
        digest: None,
    };
    let journal =
        updates::install_boot_bytes(&state, &image(0), cache.path(), &Job::default()).unwrap();
    library::undo(&journal).unwrap();
    assert!(!root.path().join("boot.iso").exists());
    assert_eq!(fs::read(root.path().join("keep")).unwrap(), b"keep");
}
