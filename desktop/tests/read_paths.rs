//! Reading a drive's files: path checks that stay valid across refreshes and
//! catch new collisions and links.

use oh_my_3do_library::{
    drive_browser::PresenceScan,
    fsutil::{safe, ReadPaths},
};
use std::{fs, path::Path};
fn put(root: &Path, name: &str) {
    let p = root.join(name);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, b"data").unwrap();
}
#[test]
fn read_inventory_preserves_path_validation_and_fresh_write_checks() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    put(root, "Games/USA/Example/Game.iso");
    let mut paths = ReadPaths::new(root).unwrap();
    for name in [
        "Games/USA/Example/Game.iso",
        "Games/USA/Example/Missing.iso",
        "games/USA/Example/Game.iso",
        "../outside",
        "Games/../escape",
        "/absolute",
        "Games/a:b",
        "Games/CON",
        "Games/trailing.",
        "Games/bad\\name",
    ] {
        let expected = safe(root, name);
        let actual = paths.path(name);
        assert_eq!(expected.is_ok(), actual.is_ok(), "{name}");
        if let Ok(path) = expected {
            assert_eq!(actual.unwrap(), path);
        }
    }
}
#[cfg(unix)]
#[test]
fn each_refresh_detects_new_collisions_and_cached_parents_cannot_become_links() {
    let tmp = tempfile::tempdir().unwrap();
    let outside = tempfile::tempdir().unwrap();
    let root = tmp.path();
    put(root, "Game.iso");
    let mut paths = ReadPaths::new(root).unwrap();
    paths.path("Game.iso").unwrap();
    put(root, "game.iso");
    assert!(safe(root, "Game.iso").is_err());
    assert!(ReadPaths::new(root).unwrap().path("Game.iso").is_err());
    fs::create_dir(root.join("Folder")).unwrap();
    put(outside.path(), "External.iso");
    paths.path("Folder/External.iso").unwrap();
    fs::remove_dir(root.join("Folder")).unwrap();
    std::os::unix::fs::symlink(outside.path(), root.join("Folder")).unwrap();
    assert!(paths.path("Folder/External.iso").is_err());
}
#[test]
fn batch_presence_keeps_live_file_sizes_and_all_cue_tracks() {
    let source = tempfile::tempdir().unwrap();
    let target = tempfile::tempdir().unwrap();
    for root in [source.path(), target.path()] {
        put(root, "Game.iso");
        put(root, "track.bin");
        fs::write(
            root.join("Game.cue"),
            b"FILE \"track.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n",
        )
        .unwrap();
    }
    let mut scan = PresenceScan::new(source.path(), target.path()).unwrap();
    assert!(scan.check("Game.iso").unwrap());
    assert!(scan.check("Game.cue").unwrap());
    fs::write(target.path().join("track.bin"), b"changed").unwrap();
    assert!(!scan.check("Game.cue").unwrap());
    fs::remove_file(target.path().join("Game.iso")).unwrap();
    assert!(!scan.check("Game.iso").unwrap());
}
