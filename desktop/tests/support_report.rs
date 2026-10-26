//! The support report: what it contains (and doesn't, like save data), and
//! never replacing other files.

use oh_my_3do_library::{console_config::Settings, setup, support_report, Job};
use std::{collections::BTreeMap, fs, path::Path};
fn snapshot(root: &Path) -> BTreeMap<String, Vec<u8>> {
    walkdir::WalkDir::new(root)
        .into_iter()
        .map(Result::unwrap)
        .filter(|e| e.file_type().is_file())
        .map(|e| {
            (
                e.path()
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                fs::read(e.path()).unwrap(),
            )
        })
        .collect()
}
fn prepare(root: &Path, cache: &Path) {
    let mut b = vec![0; 2048];
    b[..7].copy_from_slice(&[1, 90, 90, 90, 90, 90, 1]);
    b[76..80].copy_from_slice(&2048u32.to_be_bytes());
    b[80..84].copy_from_slice(&1u32.to_be_bytes());
    setup::install_verified(root, cache, &b, &Settings::default(), &Job::default()).unwrap();
}
#[test]
fn report_contains_versions_readiness_console_text_and_activity_without_reading_save_payloads() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let out = tempfile::tempdir().unwrap();
    prepare(root.path(), cache.path());
    fs::write(
        root.path().join(".ohmy3do/diagnostics.txt"),
        b"Last command: e1\nODE error: 0\n",
    )
    .unwrap();
    fs::write(
        root.path().join(".ohmy3do/saves/slot01.3ds"),
        b"SECRET SAVE CONTENT",
    )
    .unwrap();
    let before = snapshot(root.path());
    let target = out.path().join("support.txt");
    support_report::export(
        root.path(),
        &target,
        &["Last task failed".into()],
        &Job::default(),
    )
    .unwrap();
    let text = fs::read_to_string(&target).unwrap();
    for part in [
        concat!("App version: ", env!("CARGO_PKG_VERSION")),
        "Platform:",
        "READINESS",
        "Last command: e1",
        "Last task failed",
        "Save support",
    ] {
        assert!(text.contains(part), "{part}");
    }
    assert!(!text.contains("SECRET SAVE CONTENT"));
    assert_eq!(snapshot(root.path()), before);
    support_report::export(root.path(), &target, &[], &Job::default()).unwrap();
}
#[test]
fn refuses_to_replace_user_documents_or_console_diagnostics() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    prepare(root.path(), cache.path());
    let doc = root.path().join("personal.txt");
    fs::write(&doc, b"keep").unwrap();
    assert!(support_report::export(root.path(), &doc, &[], &Job::default()).is_err());
    assert_eq!(fs::read(&doc).unwrap(), b"keep");
    assert!(support_report::export(
        root.path(),
        &root.path().join(".ohmy3do/diagnostics.txt"),
        &[],
        &Job::default()
    )
    .is_err());
    assert!(support_report::export(
        root.path(),
        &root.path().join("boot.iso"),
        &[],
        &Job::default()
    )
    .is_err());
}
#[test]
fn missing_support_is_reported_and_oversized_diagnostics_are_bounded() {
    let root = tempfile::tempdir().unwrap();
    fs::create_dir(root.path().join(".ohmy3do")).unwrap();
    fs::write(
        root.path().join(".ohmy3do/diagnostics.txt"),
        vec![b'x'; 8192],
    )
    .unwrap();
    let report = support_report::collect(root.path(), &[], &Job::default()).unwrap();
    assert!(report.contains("[error] Boot ISO"));
    assert!(report.contains("truncated at 4 KiB"));
    assert!(report.len() < 16 * 1024);
}
#[test]
fn cancellation_does_not_create_a_report() {
    let root = tempfile::tempdir().unwrap();
    let out = tempfile::tempdir().unwrap();
    let job = Job::default();
    job.cancel.store(true, std::sync::atomic::Ordering::Relaxed);
    let target = out.path().join("report.txt");
    assert!(support_report::export(root.path(), &target, &[], &job).is_err());
    assert!(!target.exists());
}
