//! Removing games from a drive: keeping saves, other games and shared media,
//! refreshing the indexes, and restoring everything if a step fails.

use oh_my_3do_library::{fsutil as f, library, title_removal as removal, Job};
use serde_json::json;
use std::{
    collections::BTreeMap,
    fs,
    path::Path,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
};
fn write(root: &Path, name: &str, data: &[u8]) {
    let p = root.join(name);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, data).unwrap();
}
fn database(root: &Path, paths: &[&str]) {
    write(root,".ohmy3do/library.json",&serde_json::to_vec(&json!({"schema":1,"sources":[],"games":paths.iter().map(|p|json!({"path":p,"title":"Example","metadata":{}})).collect::<Vec<_>>()})).unwrap());
}
#[test]
fn removal_preserves_saves_sibling_games_and_shared_media_and_refreshes_indexes() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cache = tempfile::tempdir().unwrap();
    let a = "Games/USA/A/A.iso";
    let b = "Games/USA/B/B.iso";
    write(root, a, &[1; 32]);
    write(root, b, &[2; 32]);
    database(root, &[a, b]);
    write(root, "saves/slot01.3ds", b"precious saves");
    write(root, "boot.iso", b"menu");
    write(root, "Games/USA/A/notes.txt", b"user notes");
    write(root, ".ohmy3do/covers/shared.3dc", b"shared art");
    let map = BTreeMap::from([
        (f::fold(a), "shared.3dc".into()),
        (f::fold(b), "shared.3dc".into()),
    ]);
    write(
        root,
        ".ohmy3do/covers/index.3di",
        &library::media_index(&map).unwrap(),
    );
    write(
        root,
        ".ohmy3do/favorites.txt",
        format!("{a}\n{b}\n").as_bytes(),
    );
    let plan = removal::review(root, &[a.into()], &Job::default()).unwrap();
    assert_eq!(plan.files.len(), 1);
    let result = removal::apply(&plan, cache.path(), &Job::default()).unwrap();
    assert!(result.warnings.is_empty());
    assert!(!root.join(a).exists());
    assert!(root.join(b).exists());
    assert!(root.join(".ohmy3do/covers/shared.3dc").exists());
    assert_eq!(
        fs::read(root.join("saves/slot01.3ds")).unwrap(),
        b"precious saves"
    );
    assert_eq!(fs::read(root.join("boot.iso")).unwrap(), b"menu");
    assert!(root.join("Games/USA/A/notes.txt").exists());
    assert_eq!(
        library::database(root).unwrap()["games"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    assert_eq!(
        fs::read_to_string(root.join(".ohmy3do/favorites.txt")).unwrap(),
        format!("{b}\n")
    );
    assert_eq!(library::read_media_index(root, "covers").unwrap().len(), 1);
    let index = fs::read(root.join(".ohmy3do/library.3di")).unwrap();
    assert!(!index.windows(a.len()).any(|p| p == a.as_bytes()));
    assert!(index.windows(b.len()).any(|p| p == b.as_bytes()));
    let journal = fs::read_dir(cache.path().join("native-transactions"))
        .unwrap()
        .next()
        .unwrap()
        .unwrap()
        .path();
    assert!(library::undo(&journal).is_err());
}
#[test]
fn cue_tracks_beside_art_and_multidisc_are_removed_without_database() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cache = tempfile::tempdir().unwrap();
    let cue = "Games/USA/Two/Disc 1.cue";
    let iso = "Games/USA/Two/Disc 2.iso";
    write(
        root,
        cue,
        b"FILE \"track.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n",
    );
    write(root, "Games/USA/Two/track.bin", &[3; 32]);
    write(root, iso, &[4; 32]);
    let map = BTreeMap::from([(f::fold(cue), "one.3dc".into())]);
    write(
        root,
        ".ohmy3do/covers/index.3di",
        &library::beside_index(&library::media_index(&map).unwrap()).unwrap(),
    );
    write(root, "Games/USA/Two/.ohmy3do/one.3dc", b"art");
    let plan = removal::review(root, &[cue.into(), iso.into()], &Job::default()).unwrap();
    assert_eq!(plan.files.len(), 4);
    removal::apply(&plan, cache.path(), &Job::default()).unwrap();
    assert!(!root.join("Games/USA/Two").exists());
    assert_eq!(
        fs::read(root.join(".ohmy3do/covers/index.3di")).unwrap()[20..24],
        1u32.to_be_bytes()
    );
}
#[test]
fn changed_files_and_unsafe_paths_are_rejected_before_removal() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cache = tempfile::tempdir().unwrap();
    write(root, "Game.iso", &[1; 32]);
    let plan = removal::review(root, &["Game.iso".into()], &Job::default()).unwrap();
    write(root, "Game.iso", &[2; 33]);
    assert!(removal::apply(&plan, cache.path(), &Job::default()).is_err());
    assert!(root.join("Game.iso").exists());
    for path in ["../Game.iso", "boot.iso", ".ohmy3do/secret.iso"] {
        assert!(removal::review(root, &[path.into()], &Job::default()).is_err());
    }
}
#[test]
fn cancellation_after_staging_restores_files_and_metadata() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cache = tempfile::tempdir().unwrap();
    write(root, "A.iso", &[1; 32]);
    write(root, "B.iso", &[2; 32]);
    database(root, &["A.iso", "B.iso"]);
    let original = fs::read(root.join(".ohmy3do/library.json")).unwrap();
    let plan = removal::review(root, &["A.iso".into(), "B.iso".into()], &Job::default()).unwrap();
    let cancel = Arc::new(AtomicBool::new(false));
    let signal = cancel.clone();
    let job = Job {
        cancel,
        progress: Arc::new(move |p| {
            if p.phase == "Removing selected title files" && p.completed == 1 {
                signal.store(true, Ordering::Relaxed);
            }
        }),
        ..Default::default()
    };
    assert!(removal::apply(&plan, cache.path(), &job).is_err());
    assert!(root.join("A.iso").exists());
    assert!(root.join("B.iso").exists());
    assert_eq!(
        fs::read(root.join(".ohmy3do/library.json")).unwrap(),
        original
    );
}
#[test]
fn exclusive_media_is_deleted_and_runtime_metadata_for_remaining_games_is_kept() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cache = tempfile::tempdir().unwrap();
    let a = "Games/USA/A/A.iso";
    let b = "Games/USA/B/B.iso";
    write(root, a, &[1; 32]);
    write(root, b, &[2; 32]);
    let db = json!({"games":[{"path":a,"metadata":{"genre":"Action"}},{"path":b,"metadata":{"genre":"Shooter","publisher":"Example"}}]});
    write(
        root,
        ".ohmy3do/metadata.3di",
        &library::metadata_bytes(&db, &[a.into(), b.into()]).unwrap(),
    );
    let map = BTreeMap::from([(f::fold(a), "unique.3dc".into())]);
    write(
        root,
        ".ohmy3do/covers/index.3di",
        &library::media_index(&map).unwrap(),
    );
    write(root, ".ohmy3do/covers/unique.3dc", b"art");
    let plan = removal::review(root, &[a.into()], &Job::default()).unwrap();
    assert_eq!(plan.files.len(), 2);
    removal::apply(&plan, cache.path(), &Job::default()).unwrap();
    assert!(!root.join(".ohmy3do/covers/unique.3dc").exists());
    let db = library::database(root).unwrap();
    assert_eq!(db["games"][0]["metadata"]["genre"], "Shooter");
    assert_eq!(db["games"][0]["metadata"]["publisher"], "Example");
}
#[cfg(unix)]
#[test]
fn linked_discs_cannot_be_removed() {
    let tmp = tempfile::tempdir().unwrap();
    let outside = tempfile::NamedTempFile::new().unwrap();
    std::os::unix::fs::symlink(outside.path(), tmp.path().join("Link.iso")).unwrap();
    assert!(removal::review(tmp.path(), &["Link.iso".into()], &Job::default()).is_err());
    assert!(outside.path().exists());
}
#[test]
fn redundant_cue_representations_are_removed_with_their_iso() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cache = tempfile::tempdir().unwrap();
    write(root, "Game.iso", &[1; 32]);
    write(
        root,
        "Game.cue",
        b"FILE \"Game.iso\" BINARY\n TRACK 01 MODE1/2048\n INDEX 01 00:00:00\n",
    );
    let plan = removal::review(root, &["Game.iso".into()], &Job::default()).unwrap();
    assert_eq!(plan.files.len(), 2);
    removal::apply(&plan, cache.path(), &Job::default()).unwrap();
    assert!(!root.join("Game.cue").exists());
    assert!(!root.join("Game.iso").exists());
}
#[test]
fn index_write_failure_restores_every_disc_and_original_database() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cache = tempfile::tempdir().unwrap();
    write(root, "Game.iso", &[1; 32]);
    database(root, &["Game.iso"]);
    fs::create_dir(root.join(".ohmy3do/library.3di")).unwrap();
    let before = fs::read(root.join(".ohmy3do/library.json")).unwrap();
    let plan = removal::review(root, &["Game.iso".into()], &Job::default()).unwrap();
    assert!(removal::apply(&plan, cache.path(), &Job::default()).is_err());
    assert!(root.join("Game.iso").exists());
    assert_eq!(
        fs::read(root.join(".ohmy3do/library.json")).unwrap(),
        before
    );
}
#[test]
fn removal_never_scans_remaining_discs_and_keeps_save_name_mappings() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cache = tempfile::tempdir().unwrap();
    write(root, "A.iso", &[1; 32]);
    write(root, "B.iso", &[2; 32]);
    database(root, &["A.iso", "B.iso"]);
    write(
        root,
        ".ohmy3do/save-names.txt",
        b"=A_SAVE\tRemoved game still has saves\n",
    );
    write(
        root,
        ".ohmy3do/save-names-cache.json",
        b"{\"schema\":2,\"discs\":{}}",
    );
    let plan = removal::review(root, &["A.iso".into()], &Job::default()).unwrap();
    let job = Job {
        log: Arc::new(|message| {
            assert!(
                !message.contains("Reading save names"),
                "Removal must not scan other discs"
            )
        }),
        progress: Arc::new(|p| assert!(!p.phase.contains("save-name"))),
        ..Default::default()
    };
    removal::apply(&plan, cache.path(), &job).unwrap();
    assert_eq!(
        fs::read(root.join(".ohmy3do/save-names.txt")).unwrap(),
        b"=A_SAVE\tRemoved game still has saves\n"
    );
    assert_eq!(
        fs::read(root.join(".ohmy3do/save-names-cache.json")).unwrap(),
        b"{\"schema\":2,\"discs\":{}}"
    );
}
#[test]
fn fullscreen_cover_is_reviewed_removed_and_its_empty_game_folder_is_pruned() {
    for beside in [false, true] {
        let tmp = tempfile::tempdir().unwrap();
        let root = tmp.path();
        let cache = tempfile::tempdir().unwrap();
        let game = "Games/USA/Example/Example.iso";
        write(root, game, &[1; 32]);
        database(root, &[game]);
        let map = BTreeMap::from([(f::fold(game), "art.3dc".into())]);
        let index = library::media_index(&map).unwrap();
        write(
            root,
            ".ohmy3do/covers/index.3di",
            &if beside {
                library::beside_index(&index).unwrap()
            } else {
                index
            },
        );
        let folder = if beside {
            "Games/USA/Example/.ohmy3do"
        } else {
            ".ohmy3do/covers"
        };
        write(root, &format!("{folder}/art.3dc"), b"thumbnail");
        write(
            root,
            &format!("{folder}/art-large.3dc"),
            b"fullscreen cover",
        );
        let plan = removal::review(root, &[game.into()], &Job::default()).unwrap();
        assert_eq!(plan.files.len(), 3);
        assert!(plan.files.iter().any(|f| f.path.ends_with("art-large.3dc")));
        removal::apply(&plan, cache.path(), &Job::default()).unwrap();
        assert!(!root.join(format!("{folder}/art-large.3dc")).exists());
        assert!(!root.join("Games/USA/Example").exists());
    }
}
#[test]
fn shared_fullscreen_cover_is_preserved_when_another_title_uses_it() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cache = tempfile::tempdir().unwrap();
    let a = "Games/USA/Example/A.iso";
    let b = "Games/USA/Example/B.iso";
    write(root, a, &[1; 32]);
    write(root, b, &[2; 32]);
    database(root, &[a, b]);
    let map = BTreeMap::from([
        (f::fold(a), "shared.3dc".into()),
        (f::fold(b), "shared.3dc".into()),
    ]);
    write(
        root,
        ".ohmy3do/covers/index.3di",
        &library::beside_index(&library::media_index(&map).unwrap()).unwrap(),
    );
    write(root, "Games/USA/Example/.ohmy3do/shared.3dc", b"thumbnail");
    write(
        root,
        "Games/USA/Example/.ohmy3do/shared-large.3dc",
        b"fullscreen",
    );
    let plan = removal::review(root, &[a.into()], &Job::default()).unwrap();
    assert_eq!(plan.files.len(), 1);
    removal::apply(&plan, cache.path(), &Job::default()).unwrap();
    assert!(root
        .join("Games/USA/Example/.ohmy3do/shared-large.3dc")
        .exists());
    assert!(root.join(b).exists());
}
