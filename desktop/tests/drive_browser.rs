//! Browsing a drive: listing the discs actually on it (not the database's
//! idea of them), reporting bad indexes, and media beside each disc.

use oh_my_3do_library::{drive_browser, fsutil, game_info, library, model::Preferences, Job};
use std::{collections::BTreeMap, fs, path::Path};
fn write(root: &Path, name: &str, bytes: &[u8]) {
    let p = root.join(name);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, bytes).unwrap();
}
#[test]
fn scans_actual_discs_without_desktop_database_and_resolves_beside_media() {
    let drive = tempfile::tempdir().unwrap();
    let root = drive.path();
    let path = "Games/USA/Example/Example (USA).iso";
    write(root, path, &[42; 32]);
    write(root, "boot.iso", &[42; 32]);
    write(
        root,
        "Games/USA/Example/Oh My 3DO - Folder Navigation Helper (Keep).iso",
        &[42; 32],
    );
    let map = BTreeMap::from([(fsutil::fold(path), "example.3dc".into())]);
    write(
        root,
        ".ohmy3do/covers/index.3di",
        &library::beside_index(&library::media_index(&map).unwrap()).unwrap(),
    );
    write(root, "Games/USA/Example/.ohmy3do/example.3dc", &[42; 32]);
    write(
        root,
        &game_info::filename(path),
        &game_info::encode(path, "A console description.").unwrap(),
    );
    let before = walkdir::WalkDir::new(root).into_iter().count();
    let result = drive_browser::scan(root, &Preferences::default(), &Job::default()).unwrap();
    assert_eq!(result["records"].as_array().unwrap().len(), 1);
    let r = &result["records"][0];
    assert_eq!(r["cover"], "Games/USA/Example/.ohmy3do/example.3dc");
    assert_eq!(r["title"], "Example");
    assert_eq!(r["region"], "USA");
    assert_eq!(r["cover_missing"], false);
    assert_eq!(r["preview_missing"], true);
    assert_eq!(r["metadata"]["description"], "A console description.");
    assert_eq!(result["game_bytes"], 32);
    assert_eq!(before, walkdir::WalkDir::new(root).into_iter().count());
    assert!(!root.join(".ohmy3do/library.json").exists());
}
#[test]
fn stale_database_is_not_a_file_inventory_and_bad_indexes_are_warnings() {
    let drive = tempfile::tempdir().unwrap();
    let root = drive.path();
    write(
        root,
        ".ohmy3do/library.json",
        br#"{"schema":1,"games":[{"path":"Gone.iso","title":"Gone"}]}"#,
    );
    write(root, "Actual.iso", &[42; 32]);
    write(root, ".ohmy3do/previews/index.3di", b"bad");
    let result = drive_browser::scan(root, &Preferences::default(), &Job::default()).unwrap();
    assert_eq!(result["records"].as_array().unwrap().len(), 1);
    assert_eq!(result["records"][0]["path"], "Actual.iso");
    assert!(!result["warnings"].as_array().unwrap().is_empty());
}
#[test]
fn presence_requires_all_cue_tracks_and_matching_sizes() {
    let source = tempfile::tempdir().unwrap();
    let target = tempfile::tempdir().unwrap();
    let cue = b"FILE \"track.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n";
    write(source.path(), "Game.cue", cue);
    write(source.path(), "track.bin", &[42; 32]);
    write(target.path(), "Game.cue", cue);
    assert!(!drive_browser::presence(source.path(), target.path(), "Game.cue").unwrap());
    write(target.path(), "track.bin", &[42; 31]);
    assert!(!drive_browser::presence(source.path(), target.path(), "Game.cue").unwrap());
    write(target.path(), "track.bin", &[42; 32]);
    assert!(drive_browser::presence(source.path(), target.path(), "Game.cue").unwrap());
}
