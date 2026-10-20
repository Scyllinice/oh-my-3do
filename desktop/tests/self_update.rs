use oh_my_3do_library::{fsutil as f, self_update, Job};
use std::{fs, io::Write, path::Path};
fn package(root: &Path, name: &str, data: &[u8]) -> std::path::PathBuf {
    let path = root.join("release.zip");
    let mut z = zip::ZipWriter::new(fs::File::create(&path).unwrap());
    z.start_file(name, zip::write::SimpleFileOptions::default())
        .unwrap();
    z.write_all(data).unwrap();
    z.finish().unwrap();
    path
}
fn member() -> &'static str {
    if cfg!(windows) {
        "OhMy3DO-Library/OhMy3DOLibrary.exe"
    } else {
        "OhMy3DO-Library/OhMy3DOLibrary"
    }
}
fn image() -> Vec<u8> {
    let mut bytes = vec![0; 256];
    let magic: &[u8] = if cfg!(windows) {
        b"MZ"
    } else if cfg!(target_os = "macos") {
        &[0xcf, 0xfa, 0xed, 0xfe]
    } else {
        b"\x7fELF"
    };
    bytes[..magic.len()].copy_from_slice(magic);
    bytes
}
#[test]
fn staging_extracts_only_the_verified_member_and_preserves_app_and_preferences() {
    let t = tempfile::tempdir().unwrap();
    let old = t.path().join("current");
    fs::write(&old, b"old app").unwrap();
    fs::write(t.path().join("native-settings.json"), b"keep my settings").unwrap();
    let zip = package(t.path(), member(), &image());
    let (plan, path) =
        self_update::stage(&zip, &old, &t.path().join("cache"), &Job::default()).unwrap();
    assert_eq!(fs::read(&old).unwrap(), b"old app");
    assert_eq!(fs::read(&plan.staged).unwrap(), image());
    assert_eq!(
        plan.new_sha256,
        f::sha256(&plan.staged, &Job::default()).unwrap()
    );
    assert!(path.is_file());
    assert!(!plan.previous.exists());
    assert_eq!(
        fs::read(t.path().join("native-settings.json")).unwrap(),
        b"keep my settings"
    );
}
#[test]
fn wrong_archive_members_and_platform_headers_are_rejected_without_changing_app() {
    let t = tempfile::tempdir().unwrap();
    let old = t.path().join("current");
    fs::write(&old, b"old app").unwrap();
    for (name, data) in [("../OhMy3DOLibrary", image()), (member(), vec![0; 256])] {
        let zip = package(t.path(), name, &data);
        assert!(self_update::stage(&zip, &old, &t.path().join("cache"), &Job::default()).is_err());
        assert_eq!(fs::read(&old).unwrap(), b"old app");
    }
}
#[test]
fn changed_staging_or_installed_executable_prevents_replacement() {
    for change_old in [false, true] {
        let t = tempfile::tempdir().unwrap();
        let old = t.path().join("current");
        fs::write(&old, b"old app").unwrap();
        let zip = package(t.path(), member(), &image());
        let (mut plan, _) =
            self_update::stage(&zip, &old, &t.path().join("cache"), &Job::default()).unwrap();
        plan.parent = 99_999_999;
        if change_old {
            fs::write(&old, b"changed installed app").unwrap();
        } else {
            fs::write(&plan.staged, b"changed download").unwrap();
        }
        let expected = fs::read(&old).unwrap();
        assert!(self_update::apply(&plan).is_err());
        assert_eq!(fs::read(&old).unwrap(), expected);
        assert!(!plan.previous.exists());
    }
}
#[test]
fn cancelled_staging_never_replaces_the_executable() {
    let t = tempfile::tempdir().unwrap();
    let old = t.path().join("current");
    fs::write(&old, b"old app").unwrap();
    let zip = package(t.path(), member(), &image());
    let job = Job::default();
    job.cancel.store(true, std::sync::atomic::Ordering::Relaxed);
    assert!(self_update::stage(&zip, &old, &t.path().join("cache"), &job).is_err());
    assert_eq!(fs::read(old).unwrap(), b"old app");
}
#[test]
fn program_support_files_are_allowlisted_and_never_include_library_or_settings() {
    let t = tempfile::tempdir().unwrap();
    let old = t.path().join("current");
    fs::write(&old, b"old app").unwrap();
    let path = t.path().join("release.zip");
    let mut z = zip::ZipWriter::new(fs::File::create(&path).unwrap());
    for (name, data) in [
        (member(), image()),
        ("OhMy3DO-Library/README.md", b"new docs".to_vec()),
        (
            "OhMy3DO-Library/licenses/component/LICENSE",
            b"license".to_vec(),
        ),
        (
            "OhMy3DO-Library/native-settings.json",
            b"do not install".to_vec(),
        ),
        (
            "OhMy3DO-Library/Games/overwrite.iso",
            b"do not install".to_vec(),
        ),
    ] {
        z.start_file(name, zip::write::SimpleFileOptions::default())
            .unwrap();
        z.write_all(&data).unwrap();
    }
    z.finish().unwrap();
    let (plan, _) =
        self_update::stage(&path, &old, &t.path().join("cache"), &Job::default()).unwrap();
    assert_eq!(plan.assets.len(), 2);
    assert!(plan.assets.iter().any(|a| a.relative == "README.md"));
    assert!(!t.path().join("README.md").exists());
    assert!(!t.path().join("native-settings.json").exists());
}
#[test]
fn recovery_never_launches_an_externally_changed_executable() {
    let t = tempfile::tempdir().unwrap();
    let old = t.path().join("current");
    fs::write(&old, b"old app").unwrap();
    let zip = package(t.path(), member(), &image());
    let (mut plan, _) =
        self_update::stage(&zip, &old, &t.path().join("cache"), &Job::default()).unwrap();
    plan.parent = 99_999_999;
    fs::write(&old, b"changed executable").unwrap();
    assert!(self_update::restart_previous(&plan).is_err());
}
