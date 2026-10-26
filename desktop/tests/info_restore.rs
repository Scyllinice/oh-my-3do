//! Restoring game information: one field or a whole game, previewed first,
//! undoable, and never writing when the edit is stale or the data missing.

use oh_my_3do_library::{catalog, game_info, library, model::Preferences, Job};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    fs,
    io::Write,
    path::{Path, PathBuf},
};
struct Fixture {
    _temp: tempfile::TempDir,
    root: PathBuf,
    cache: PathBuf,
    discs: Vec<String>,
    prefs: Preferences,
}
fn put(root: &Path, name: &str, bytes: &[u8]) {
    let p = root.join(name);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, bytes).unwrap();
}
fn files(root: &Path) -> BTreeMap<PathBuf, Vec<u8>> {
    walkdir::WalkDir::new(root)
        .into_iter()
        .map(Result::unwrap)
        .filter(|e| e.file_type().is_file())
        .map(|e| {
            (
                e.path().strip_prefix(root).unwrap().to_owned(),
                fs::read(e.path()).unwrap(),
            )
        })
        .collect()
}
impl Fixture {
    fn new() -> Self {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("library");
        let cache = temp.path().join("cache");
        let discs = vec![
            "Games/USA/Example/Disc 1.iso".to_owned(),
            "Games/USA/Example/Disc 2.iso".to_owned(),
        ];
        for path in discs
            .iter()
            .map(String::as_str)
            .chain(["Games/USA/Other/disc.iso"])
        {
            put(&root, path, b"fixture");
        }
        let games = vec![
            json!({"path":discs[0],"title":"Example (Disc 1)","catalog_name":"Example","cover":"keep.png","metadata":{"description":"Manual A","genre":"Downloaded genre","publisher":"My publisher","year":2000,"players":2,"description_source":"Edited in Oh My 3DO"},"metadata_overrides":{"description":"Manual A","genre":"Downloaded genre","publisher":"My publisher","year":2000,"players":2}}),
            json!({"path":discs[1],"title":"Example (Disc 2)","catalog_name":"Second Example","metadata":{"description":"Manual B","genre":"Custom genre","publisher":"My publisher","year":2000,"players":2},"metadata_overrides":{"description":"Manual B","genre":"Custom genre","publisher":"My publisher","year":2000,"players":2}}),
            json!({"path":"Games/USA/Other/disc.iso","title":"Other","metadata":{}}),
        ];
        put(
            &root,
            ".ohmy3do/library.json",
            &serde_json::to_vec(&json!({"schema":1,"sources":[],"games":games})).unwrap(),
        );
        let mut tx = library::Journal::new(&root, &cache).unwrap();
        library::build_indexes(&root, &mut tx, &Job::default()).unwrap();
        tx.finish().unwrap();
        let fixture = Self {
            _temp: temp,
            root,
            cache,
            discs,
            prefs: Preferences {
                offline: true,
                ..Default::default()
            },
        };
        fixture.download("Downloaded genre", 1994);
        fixture
    }
    fn download(&self, genre: &str, year: u32) {
        let xml=format!("<LaunchBox><Game><Name>Example</Name><Platform>3DO</Platform><Overview>Provider A</Overview><Genres>{genre}</Genres><Publisher>Provider publisher</Publisher><ReleaseYear>{year}</ReleaseYear></Game><Game><Name>Second Example</Name><Platform>3DO</Platform><Overview>Provider B</Overview><Genres>{genre}</Genres><Publisher>Provider publisher</Publisher><ReleaseYear>{}</ReleaseYear></Game></LaunchBox>",year+1);
        let mut zip =
            zip::ZipWriter::new(fs::File::create(self.cache.join("launchbox.zip")).unwrap());
        zip.start_file("Metadata.xml", zip::write::SimpleFileOptions::default())
            .unwrap();
        zip.write_all(xml.as_bytes()).unwrap();
        zip.finish().unwrap();
    }
    fn view(&self) -> game_info::EditView {
        game_info::edit_view(&self.root, &self.discs).unwrap()
    }
    fn preview(&self, view: &game_info::EditView) -> game_info::RestorePlan {
        game_info::preview_restore(
            &self.root,
            &view.records,
            &self.prefs,
            &self.cache,
            &Job::default(),
        )
        .unwrap()
    }
    fn db(&self) -> Value {
        library::database(&self.root).unwrap()
    }
}
#[test]
fn single_field_restore_is_previewed_per_disc_preserves_other_edits_and_supports_undo() {
    let f = Fixture::new();
    let before = files(&f.root);
    let view = f.view();
    let plan = f.preview(&view);
    assert_eq!(files(&f.root), before);
    assert_eq!(plan.records[0].fields["description"], "Provider A");
    assert_eq!(plan.records[1].fields["description"], "Provider B");
    assert!(!plan.available.contains(&"players".into()));
    let journal = game_info::save_edits_restoring(
        &f.root,
        &view.records,
        &json!({"genre":"Still custom"}),
        &plan,
        &["description".into()],
        &f.cache,
        &Job::default(),
    )
    .unwrap();
    let db = f.db();
    for (i, description) in ["Provider A", "Provider B"].iter().enumerate() {
        let r = &db["games"][i];
        assert_eq!(r["metadata"]["description"], *description);
        assert!(r["metadata_overrides"].get("description").is_none());
        assert_eq!(r["metadata"]["description_source"], catalog::LAUNCHBOX);
        assert_eq!(r["metadata_overrides"]["genre"], "Still custom");
        assert_eq!(r["metadata"]["players"], 2);
        assert_eq!(
            fs::read(f.root.join(game_info::filename(&f.discs[i]))).unwrap(),
            game_info::encode(&f.discs[i], description).unwrap()
        );
    }
    assert_eq!(db["games"][0]["cover"], "keep.png");
    assert_eq!(db["games"][2]["metadata"], json!({}));
    library::undo(&journal).unwrap();
    assert_eq!(files(&f.root), before);
}
#[test]
fn whole_game_restore_unpins_equal_values_and_future_downloads_update_them() {
    let f = Fixture::new();
    let view = f.view();
    let plan = f.preview(&view);
    game_info::save_edits_restoring(
        &f.root,
        &view.records,
        &json!({}),
        &plan,
        &plan.available,
        &f.cache,
        &Job::default(),
    )
    .unwrap();
    let db = f.db();
    for (i, year) in [1994, 1995].iter().enumerate() {
        let r = &db["games"][i];
        assert_eq!(r["metadata"]["year"], *year);
        assert_eq!(r["metadata"]["genre"], "Downloaded genre");
        assert!(r["metadata_overrides"].get("genre").is_none());
        assert_eq!(r["metadata_overrides"]["players"], 2);
    }
    f.download("Updated genre", 2007);
    game_info::refresh(&f.root, &f.prefs, &f.cache, &Job::default()).unwrap();
    let db = f.db();
    assert_eq!(db["games"][0]["metadata"]["genre"], "Updated genre");
    assert_eq!(db["games"][0]["metadata"]["year"], 2007);
    assert_eq!(db["games"][0]["metadata"]["players"], 2);
}
#[test]
fn unavailable_fields_invalid_combinations_stale_edits_and_offline_failures_do_not_write() {
    let f = Fixture::new();
    let view = f.view();
    let plan = f.preview(&view);
    let before = files(&f.root);
    for fields in [
        vec!["players".into()],
        vec!["description".into(), "description".into()],
        vec![],
    ] {
        assert!(game_info::save_edits_restoring(
            &f.root,
            &view.records,
            &json!({}),
            &plan,
            &fields,
            &f.cache,
            &Job::default()
        )
        .is_err());
        assert_eq!(files(&f.root), before);
    }
    assert!(game_info::save_edits_restoring(
        &f.root,
        &view.records,
        &json!({"description":"conflict"}),
        &plan,
        &["description".into()],
        &f.cache,
        &Job::default()
    )
    .is_err());
    assert_eq!(files(&f.root), before);
    fs::remove_file(f.cache.join("launchbox.zip")).unwrap();
    assert!(game_info::preview_restore(
        &f.root,
        &view.records,
        &f.prefs,
        &f.cache,
        &Job::default()
    )
    .is_err());
    assert_eq!(files(&f.root), before);
    game_info::save_edits(
        &f.root,
        &view.records,
        &json!({"publisher":"Changed elsewhere"}),
        &f.cache,
        &Job::default(),
    )
    .unwrap();
    let edited = files(&f.root);
    assert!(game_info::save_edits_restoring(
        &f.root,
        &view.records,
        &json!({}),
        &plan,
        &["description".into()],
        &f.cache,
        &Job::default()
    )
    .is_err());
    assert_eq!(files(&f.root), edited);
}
