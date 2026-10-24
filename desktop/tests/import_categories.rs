use oh_my_3do_library::{categories, disc, library, model::{Catalog, Preferences}, Job};
use std::{fs, path::Path};

fn put(root: &Path, path: &str, data: &[u8]) -> std::path::PathBuf {
    let path = root.join(path);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(&path, data).unwrap();
    path
}

#[test]
fn unmatched_title_keeps_chosen_category_and_imports_without_catalog_identity() {
    let temp = tempfile::tempdir().unwrap();
    let root = temp.path().join("library");
    fs::create_dir(&root).unwrap();
    let source = put(temp.path(), "incoming/Unlisted (USA).iso", b"fictional disc");
    let mut plan = library::scan(&root, &[source.clone()], Catalog::default(), false, &Job::default()).unwrap();
    assert!(!plan.entries[0].resolvable());
    library::choose_import_category(&mut plan.entries[0], &plan.catalog, "Educational");
    assert_eq!(plan.entries[0].category, "Educational");
    assert!(plan.entries[0].target.starts_with("Educational/"));
    put(&root, &plan.entries[0].target, b"existing title");
    library::collisions(&mut plan).unwrap();
    assert!(plan.entries[0].blocked);
    library::choose_import_category(&mut plan.entries[0], &plan.catalog, "Bonus Discs");
    library::collisions(&mut plan).unwrap();
    assert!(!plan.entries[0].blocked);
    assert_eq!(plan.entries[0].category, "Bonus Discs");
    assert_eq!(plan.entries[0].confidence, "unverified");
    assert!(plan.entries[0].game.is_none());
    assert!(plan.entries[0].resolvable());
    plan.entries[0].selected = true;
    library::import(&plan, &Preferences { offline: true, ..Default::default() }, &temp.path().join("cache"), &Job::default()).unwrap();
    assert_eq!(fs::read(root.join(&plan.entries[0].target)).unwrap(), b"fictional disc");
    assert_eq!(fs::read(source).unwrap(), b"fictional disc");
}

#[test]
fn audio_cue_retains_all_tracks_and_imports_to_a_custom_category() {
    let temp = tempfile::tempdir().unwrap();
    let root = temp.path().join("library");
    fs::create_dir(&root).unwrap();
    let mut config = categories::Categories::default();
    config.categories.iter_mut().find(|c| c.name == "Bonus Discs").unwrap().name = "Music".into();
    put(&root, categories::PATH, &config.encode().unwrap());
    let cue = put(temp.path(), "incoming/A&M Music Sampler (1994)(A&M)(US)[!][Road Rash][CD-Audio].cue",
        b"FILE \"Track 01.wav\" WAVE\n TRACK 01 AUDIO\n INDEX 01 00:00:00\nFILE \"Track 02.wav\" WAVE\n TRACK 02 AUDIO\n INDEX 01 00:00:00\n");
    let tracks = [put(temp.path(), "incoming/Track 01.wav", b"first audio fixture"),
                  put(temp.path(), "incoming/Track 02.wav", b"second audio fixture")];
    let original = fs::read(&cue).unwrap();
    let mut plan = library::scan(&root, &[cue.clone()], Catalog::default(), false, &Job::default()).unwrap();
    let entry = &plan.entries[0];
    assert!(!entry.blocked);
    assert!(!entry.selected);
    assert_eq!(entry.confidence, "audio-only");
    assert_eq!(entry.files.len(), 3);
    assert!(entry.files.iter().all(|f| !f.fingerprint.sha256.is_empty()));
    assert!(!entry.resolvable());
    library::choose_import_category(&mut plan.entries[0], &plan.catalog, "Music");
    library::collisions(&mut plan).unwrap();
    assert!(plan.entries[0].resolvable());
    plan.entries[0].selected = true;
    library::import(&plan, &Preferences { offline: true, ..Default::default() }, &temp.path().join("cache"), &Job::default()).unwrap();
    let target = root.join(&plan.entries[0].target);
    assert!(target.is_file());
    let refs = disc::cue_refs(&target).unwrap().1;
    assert_eq!(refs.len(), 2);
    for (target, source) in refs.iter().zip(tracks) {
        assert_eq!(fs::read(target).unwrap(), fs::read(source).unwrap());
    }
    assert_eq!(fs::read(cue).unwrap(), original);
    let db = library::database(&root).unwrap();
    let game = &db["games"][0];
    assert_eq!(game["category"], "Music");
    assert_eq!(game["identification"], "unverified");
    assert!(game["catalog_name"].is_null());
}
