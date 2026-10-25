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
fn boot_update_preserves_existing_files_and_undo_restores_it() {
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
    assert!(root.path().join(".ohmy3do/saves").is_dir());
    assert!(root.path().join(".ohmy3do/ohmy3do.ini").is_file());
    assert!(root.path().join(".ohmy3do/save-cheats.3db").is_file());
    assert!(root
        .path()
        .join(".ohmy3do/saves/slot-catalog.3di")
        .is_file());
    library::undo(&journal).unwrap();
    assert!(!root.path().join(".ohmy3do").exists());
    assert!(!root.path().join("boot.iso").exists());
    assert_eq!(fs::read(root.path().join("keep")).unwrap(), b"keep");
}

fn component_release(tag: &str) -> Value {
    let mut value = release();
    value["tag_name"] = json!(tag);
    for asset in value["assets"].as_array_mut().unwrap() {
        let name = asset["name"].as_str().unwrap().to_owned();
        asset["browser_download_url"] = json!(format!(
            "https://github.com/Scyllinice/oh-my-3do/releases/download/{tag}/{name}"
        ));
    }
    value
}
#[test]
fn release_streams_choose_versions_independently_of_publication_order() {
    use updates::Component::{Desktop, Menu};
    let mut draft = component_release("menu-v90.0.0");
    draft["draft"] = json!(true);
    let mut prerelease = component_release("desktop-v90.0.0");
    prerelease["prerelease"] = json!(true);
    let releases = vec![
        component_release("desktop-v3.9.0"),
        component_release("menu-v1.10.0"),
        component_release("desktop-v3.10.0"),
        component_release("menu-v1.9.0"),
        component_release("v99.0.0"),
        draft,
        prerelease,
        component_release("other-v100.0.0"),
    ];
    assert_eq!(
        updates::select_release(&releases, Menu).unwrap().tag,
        "menu-v1.10.0"
    );
    assert_eq!(
        updates::select_release(&releases, Desktop).unwrap().tag,
        "desktop-v3.10.0"
    );
    let menu =
        Release::parse(&serde_json::to_vec(&component_release("menu-v1.10.0")).unwrap()).unwrap();
    assert!(menu.desktop_name("Windows-x64").is_err());
}
#[test]
fn each_stream_falls_back_to_combined_release_only_until_it_exists() {
    use updates::Component::{Desktop, Menu};
    let mut releases = vec![component_release("v1.0.0")];
    assert_eq!(
        updates::select_release(&releases, Menu).unwrap().tag,
        "v1.0.0"
    );
    assert_eq!(
        updates::select_release(&releases, Desktop).unwrap().tag,
        "v1.0.0"
    );
    releases.push(component_release("desktop-v1.10.0"));
    assert_eq!(
        updates::select_release(&releases, Menu).unwrap().tag,
        "v1.0.0"
    );
    assert_eq!(
        updates::select_release(&releases, Desktop).unwrap().tag,
        "desktop-v1.10.0"
    );
    releases.push(component_release("menu-v1.2.0"));
    assert_eq!(
        updates::select_release(&releases, Menu).unwrap().tag,
        "menu-v1.2.0"
    );
    assert!(updates::select_release(&[component_release("desktop-v1.10.0")], Menu).is_err());
}
#[test]
fn split_release_downloads_stay_pinned_to_their_own_tag() {
    for tag in ["menu-v1.10.0", "desktop-v1.10.0"] {
        let value = component_release(tag);
        let parsed = Release::parse(&serde_json::to_vec(&value).unwrap()).unwrap();
        assert_eq!(
            parsed.page(),
            format!("https://github.com/Scyllinice/oh-my-3do/releases/tag/{tag}")
        );
        if tag.starts_with("desktop-") {
            assert_eq!(
                parsed.desktop_name("Windows-x64").unwrap(),
                "OhMy3DO-Library-Windows-x64-v1.10.0.zip"
            );
        }
        let mut wrong = value;
        wrong["assets"][0]["browser_download_url"] =
            json!("https://github.com/Scyllinice/oh-my-3do/releases/download/v1.10.0/boot.iso");
        assert!(Release::parse(&serde_json::to_vec(&wrong).unwrap()).is_err());
    }
    for tag in ["menu-v1.0", "desktop-v01.0.0", "menu-v1.0.0-beta"] {
        assert!(Release::parse(&serde_json::to_vec(&component_release(tag)).unwrap()).is_err());
    }
}

#[test]
fn installed_menu_versions_follow_image_hashes_and_survive_offline_checks() {
    let t = tempfile::tempdir().unwrap();
    let cache = t.path().join("cache");
    let root = t.path().join("drive");
    std::fs::create_dir_all(&root).unwrap();
    let job = oh_my_3do_library::Job::default();
    let absent = updates::installed_menu(&root, &cache, &job).unwrap();
    assert_eq!(absent["installed"], false);
    let boot = root.join("boot.iso");
    std::fs::write(&boot, b"first menu").unwrap();
    assert!(updates::installed_menu(&root, &cache, &job).unwrap()["version"].is_null());
    let digest = oh_my_3do_library::fsutil::sha256(&boot, &job).unwrap();
    updates::remember_menu(&cache, &digest, "1.0.1").unwrap();
    assert_eq!(
        updates::installed_menu(&root, &cache, &job).unwrap()["version"],
        "1.0.1"
    );
    std::fs::write(&boot, b"different menu").unwrap();
    assert!(updates::installed_menu(&root, &cache, &job).unwrap()["version"].is_null());
    std::fs::write(&boot, b"first menu").unwrap();
    assert_eq!(
        updates::installed_menu(&root, &cache, &job).unwrap()["version"],
        "1.0.1"
    );
    assert!(updates::remember_menu(&cache, "not-a-hash", "1.0.1").is_err());
    assert!(updates::remember_menu(&cache, &digest, "not-a-version").is_err());
}

fn local_menu(root: &std::path::Path, bytes: &[u8]) {
    fs::write(root.join("boot.iso"), bytes).unwrap();
    let digest = fsutil::sha256(&root.join("boot.iso"), &Job::default()).unwrap();
    fs::write(root.join("menu-update.json"), serde_json::to_vec(&json!({"schema":1,"component":"menu","version":"1.0.2","artifact":"boot.iso","sha256":digest})).unwrap()).unwrap();
}
#[test]
fn local_menu_only_checks_and_installs_offline_with_rollback() {
    let source = tempfile::tempdir().unwrap();
    let drive = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    local_menu(source.path(), &image(2));
    fs::write(drive.path().join("boot.iso"), image(1)).unwrap();
    fs::write(drive.path().join("personal-save"), b"keep").unwrap();
    let check =
        updates::check_local(source.path(), &[drive.path().into()], &Job::default()).unwrap();
    assert!(check.desktop.is_none());
    let menu = check.menu.unwrap();
    assert!(menu.is_local());
    let journal = updates::install_boot(
        &menu,
        &check.targets[0],
        check.boot_digest.as_deref().unwrap(),
        cache.path(),
        &Job::default(),
    )
    .unwrap();
    assert_eq!(fs::read(drive.path().join("boot.iso")).unwrap(), image(2));
    assert_eq!(
        fs::read(drive.path().join("personal-save")).unwrap(),
        b"keep"
    );
    library::undo(&journal).unwrap();
    assert_eq!(fs::read(drive.path().join("boot.iso")).unwrap(), image(1));
}
#[test]
fn local_offer_pins_bytes_and_rejects_rebuilds_and_cancellation_before_writing() {
    let source = tempfile::tempdir().unwrap();
    let drive = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    local_menu(source.path(), &image(2));
    fs::write(drive.path().join("boot.iso"), image(1)).unwrap();
    let check =
        updates::check_local(source.path(), &[drive.path().into()], &Job::default()).unwrap();
    local_menu(source.path(), &image(3));
    let result = updates::install_boot(
        check.menu.as_ref().unwrap(),
        &check.targets[0],
        check.boot_digest.as_deref().unwrap(),
        cache.path(),
        &Job::default(),
    );
    assert!(format!("{:#}", result.unwrap_err()).contains("checksum mismatch"));
    assert_eq!(fs::read(drive.path().join("boot.iso")).unwrap(), image(1));
    let job = Job::default();
    job.cancel.store(true, Ordering::Relaxed);
    assert!(updates::check_local(source.path(), &[], &job).is_err());
}
#[test]
fn local_desktop_detects_same_version_rebuilds_by_executable_hash() {
    let source = tempfile::tempdir().unwrap();
    let binary = source.path().join("running-app");
    fs::write(&binary, b"same build").unwrap();
    let digest = fsutil::sha256(&binary, &Job::default()).unwrap();
    let platform = updates::platform().unwrap();
    let name = format!("OhMy3DO-Library-{platform}-v1.0.2.zip");
    fs::write(source.path().join(&name), b"package").unwrap();
    fs::write(source.path().join(format!("desktop-update-{platform}.json")),serde_json::to_vec(&json!({"schema":1,"component":"desktop","platform":platform,"version":"1.0.2","artifact":name,"sha256":"a".repeat(64),"binary_sha256":digest})).unwrap()).unwrap();
    let check = updates::check_local(source.path(), &[], &Job::default()).unwrap();
    assert!(check.menu.is_none());
    let desktop = check.desktop.unwrap();
    assert!(!desktop
        .desktop_available(&binary, "1.0.2", &Job::default())
        .unwrap());
    fs::write(&binary, b"different build, same version").unwrap();
    assert!(desktop
        .desktop_available(&binary, "1.0.2", &Job::default())
        .unwrap());
    assert!(desktop
        .desktop_available(&binary, "9.0.0", &Job::default())
        .unwrap());
    assert_eq!(desktop.desktop_name(platform).unwrap(), name);
}
#[test]
fn local_manifests_fail_explicitly_without_network_fallback() {
    let source = tempfile::tempdir().unwrap();
    assert!(updates::check_local(source.path(), &[], &Job::default()).is_err());
    local_menu(source.path(), &image(2));
    let manifest = source.path().join("menu-update.json");
    let valid: Value = serde_json::from_slice(&fs::read(&manifest).unwrap()).unwrap();
    for (key, value) in [
        ("artifact", json!("../boot.iso")),
        ("sha256", json!("invalid")),
        ("version", json!("1.0.2-beta")),
        ("schema", json!(2)),
        ("component", json!("desktop")),
    ] {
        let mut bad = valid.clone();
        bad[key] = value;
        fs::write(&manifest, serde_json::to_vec(&bad).unwrap()).unwrap();
        assert!(
            updates::check_local(source.path(), &[], &Job::default()).is_err(),
            "{key}"
        );
    }
    assert!(updates::check_local(&source.path().join("missing"), &[], &Job::default()).is_err());
}

#[test]
fn developer_drive_setup_uses_local_menu_and_creates_support_without_a_library_copy() {
    let source = tempfile::tempdir().unwrap();
    let drive = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    local_menu(source.path(), &image(2));
    let journal = oh_my_3do_library::setup::install_developer(
        source.path(),
        drive.path(),
        cache.path(),
        &Default::default(),
        &Job::default(),
    )
    .unwrap();
    assert_eq!(fs::read(drive.path().join("boot.iso")).unwrap(), image(2));
    assert!(drive
        .path()
        .join(".ohmy3do/saves/slot-catalog.3di")
        .is_file());
    assert!(drive.path().join(".ohmy3do/ohmy3do.ini").is_file());
    assert_eq!(
        updates::installed_menu(drive.path(), cache.path(), &Job::default()).unwrap()["version"],
        "1.0.2"
    );
    library::undo(&journal).unwrap();
    assert!(!drive.path().join("boot.iso").exists());
}
