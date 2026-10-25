use oh_my_3do_library::{
    categories::{self, Categories, Category},
    library,
    model::{Catalog, Game, Rom},
    usb, Job,
};
use serde_json::json;
use std::{fs, path::Path};
fn put(root: &Path, name: &str, data: &[u8]) {
    let p = root.join(name);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, data).unwrap();
}
#[test]
fn category_names_ids_defaults_and_console_bounds_are_validated() {
    let config = Categories::default();
    config.validate().unwrap();
    for name in [
        "../Games",
        "Adult/USA",
        ".ohmy3do",
        "CON",
        "COM1",
        "NUL.txt",
        "Games ",
        "Games.",
    ] {
        let mut bad = config.clone();
        bad.categories[0].name = name.into();
        assert!(bad.validate().is_err(), "{name}");
    }
    let mut duplicate = config.clone();
    duplicate.categories[1].name = "games".into();
    assert!(duplicate.validate().is_err());
    let mut removed = config.clone();
    removed.categories.remove(0);
    assert!(removed.validate().is_err());
    let mut custom = config;
    custom.categories.push(Category {
        id: "custom".into(),
        name: "Arcade favorites".into(),
        hidden: true,
    });
    custom.defaults.insert("Games".into(), "custom".into());
    assert_eq!(custom.destination("Games"), Some("Arcade favorites"));
    custom.encode().unwrap();
}
#[test]
fn renaming_a_category_updates_folders_indexes_favorites_media_and_undoes() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let r = root.path();
    let old = Categories::default();
    put(r, categories::PATH, &old.encode().unwrap());
    let path = "Games/USA/Example/disc.iso";
    put(r, path, b"disc");
    put(r,".ohmy3do/library.json",&serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[{"path":path,"category":"Games","title":"Example"}]})).unwrap());
    for name in ["favorites.txt", "recent.txt"] {
        put(
            r,
            &format!(".ohmy3do/{name}"),
            format!("OH3DO_FAVORITES1\n{path}\n").as_bytes(),
        );
    }
    put(
        r,
        ".ohmy3do/last-position.txt",
        format!("OH3DO_POSITION1\n/Storage/Games/USA\nExample\n").as_bytes(),
    );
    let mut index = std::collections::BTreeMap::new();
    index.insert(path.to_ascii_lowercase(), "cover.3dc".into());
    put(
        r,
        ".ohmy3do/covers/index.3di",
        &library::media_index(&index).unwrap(),
    );
    let mut new = old.clone();
    new.categories[0].name = "My collection".into();
    new.categories[0].hidden = true;
    let journal = categories::save(r, &old, &new, cache.path(), &Job::default()).unwrap();
    assert!(!r.join("Games").exists());
    assert!(r.join("My collection/USA/Example/disc.iso").is_file());
    let db = library::database(r).unwrap();
    assert_eq!(db["games"][0]["category"], "My collection");
    assert_eq!(db["games"][0]["path"], "My collection/USA/Example/disc.iso");
    assert!(fs::read_to_string(r.join(".ohmy3do/favorites.txt"))
        .unwrap()
        .contains("My collection/USA/Example/disc.iso"));
    assert!(library::read_media_index(r, "covers")
        .unwrap()
        .contains_key("my collection/usa/example/disc.iso"));
    assert_eq!(categories::load(r).unwrap(), new);
    assert!(r.join(".ohmy3do/library.3di").is_file());
    library::undo(&journal).unwrap();
    assert!(r.join(path).is_file());
    assert_eq!(categories::load(r).unwrap(), old);
}
#[test]
fn stale_config_nonempty_removal_and_rename_collisions_do_not_change_files() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let r = root.path();
    let old = Categories::default();
    put(r, categories::PATH, &old.encode().unwrap());
    put(r, "Games/keep.iso", b"keep");
    let mut new = old.clone();
    new.categories.remove(0);
    for id in new.defaults.values_mut() {
        if id == "category-0" {
            *id = "category-1".into();
        }
    }
    assert!(categories::save(r, &old, &new, cache.path(), &Job::default()).is_err());
    assert!(r.join("Games/keep.iso").exists());
    let mut changed = old.clone();
    changed.categories[0].hidden = true;
    put(r, categories::PATH, &changed.encode().unwrap());
    assert!(categories::save(r, &old, &old, cache.path(), &Job::default()).is_err());
    assert_eq!(categories::load(r).unwrap(), changed);
}
#[test]
fn custom_import_destinations_and_multidisc_copy_use_shared_categories() {
    let source = tempfile::tempdir().unwrap();
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let r = root.path();
    let mut config = Categories::default();
    config.categories[0].name = "Collection".into();
    put(r, categories::PATH, &config.encode().unwrap());
    put(source.path(), "Example.iso", b"DISC");
    let catalog = Catalog {
        games: vec![Game {
            name: "Example (USA)".into(),
            release: "Example (USA)".into(),
            category: "Games".into(),
            roms: vec![Rom {
                name: "Example.iso".into(),
                size: 4,
                ..Default::default()
            }],
            ..Default::default()
        }],
        ..Default::default()
    };
    let plan = library::scan(
        r,
        &[source.path().join("Example.iso")],
        catalog,
        true,
        &Job::default(),
    )
    .unwrap();
    assert_eq!(plan.entries[0].category, "Collection");
    assert!(plan.entries[0].target.starts_with("Collection/"));
    put(r, "Collection/USA/Test/Disc 1.iso", b"one");
    put(r, "Collection/USA/Test/Disc 2.iso", b"two");
    let mut tx = library::Journal::new(r, cache.path()).unwrap();
    library::build_indexes(r, &mut tx, &Job::default()).unwrap();
    tx.finish().unwrap();
    let (_, groups) = usb::choices(r, &Job::default()).unwrap();
    assert_eq!(groups.len(), 1);
    assert_eq!(groups[0].discs.len(), 2);
}
#[test]
fn copying_custom_categories_preserves_the_consoles_visibility_choices() {
    let root = tempfile::tempdir().unwrap();
    let target = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let r = root.path();
    let t = target.path();
    let mut config = Categories::default();
    config.categories[0].name = "Collection".into();
    put(r, categories::PATH, &config.encode().unwrap());
    let mut image = vec![0; 2048];
    image[..7].copy_from_slice(&[1, 90, 90, 90, 90, 90, 1]);
    image[76..80].copy_from_slice(&2048u32.to_be_bytes());
    image[80..84].copy_from_slice(&1u32.to_be_bytes());
    oh_my_3do_library::setup::install_verified(
        r,
        cache.path(),
        &image,
        &Default::default(),
        &Job::default(),
    )
    .unwrap();
    let mut target_config = Categories::default();
    target_config.categories[0].hidden = true;
    target_config.categories.push(Category {
        id: "drive-only".into(),
        name: "Drive extras".into(),
        hidden: true,
    });
    put(t, "Games/USA/Extra/extra.iso", b"extra");
    put(t, "Drive extras/USA/Only/only.iso", b"only");
    put(t, categories::PATH, &target_config.encode().unwrap());
    put(r, "Collection/USA/Test/Disc 1.iso", b"one");
    put(r, "Collection/USA/Test/Disc 2.iso", b"two");
    let mut tx = library::Journal::new(r, cache.path()).unwrap();
    library::build_indexes(r, &mut tx, &Job::default()).unwrap();
    tx.finish().unwrap();
    let plan = usb::plan_selected(
        r,
        t,
        cache.path(),
        false,
        &["Collection/USA/Test/Disc 1.iso".into()],
        &Job::default(),
    )
    .unwrap();
    usb::apply(&plan, cache.path(), &Job::default()).unwrap();
    assert!(t.join("Collection/USA/Test/Disc 2.iso").exists());
    let copied = categories::load(t).unwrap();
    assert_eq!(copied.categories[0].name, "Collection");
    assert!(copied.categories[0].hidden);
    assert!(copied
        .categories
        .iter()
        .any(|c| c.name == "Drive extras" && c.hidden));
    assert!(t.join("Collection/USA/Extra/extra.iso").is_file());
    assert!(!t.join("Games").exists());
}
#[cfg(not(windows))]
#[test]
fn console_json_roundtrip_preserves_desktop_import_destinations() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap();
    let tmp = tempfile::tempdir().unwrap();
    let c = tmp.path().join("roundtrip.c");
    let exe = tmp.path().join("roundtrip");
    fs::write(&c,r#"#include "menu_categories.h"
#include <stdio.h>
int main(int argc,char **argv){char data[8192];FILE *f;size_t n;if(argc!=2)return 1;f=fopen(argv[1],"rb");if(!f)return 2;n=fread(data,1,sizeof(data),f);fclose(f);if(menu_categories_parse(data,n)||menu_category_toggle(0))return 3;puts(menu_categories_text());return 0;}"#).unwrap();
    assert!(std::process::Command::new("cc")
        .current_dir(root)
        .args([
            "-std=c89",
            "-Isrc",
            "src/menu_categories.c",
            "src/menu_ui.c",
            "src/cover_catalog.c"
        ])
        .arg(&c)
        .arg("-o")
        .arg(&exe)
        .status()
        .unwrap()
        .success());
    let mut config = Categories::default();
    for i in 8..32 {
        config.categories.push(Category {
            id: format!("custom-{i}"),
            name: format!("Custom collection {i}"),
            hidden: i % 2 == 0,
        });
    }
    let path = tmp.path().join("categories.json");
    fs::write(&path, config.encode().unwrap()).unwrap();
    let output = std::process::Command::new(exe).arg(path).output().unwrap();
    assert!(output.status.success());
    let toggled: Categories = serde_json::from_slice(&output.stdout).unwrap();
    config.categories[0].hidden = true;
    assert_eq!(toggled, config);
}

#[test]
fn removing_an_empty_category_is_undoable() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let r = root.path();
    let old = Categories::default();
    put(r, categories::PATH, &old.encode().unwrap());
    fs::create_dir(r.join("Demos")).unwrap();
    let mut new = old.clone();
    new.categories.remove(1);
    new.defaults.insert("Demos".into(), "category-0".into());
    let journal = categories::save(r, &old, &new, cache.path(), &Job::default()).unwrap();
    assert!(!r.join("Demos").exists());
    library::undo(&journal).unwrap();
    assert!(r.join("Demos").is_dir());
    assert_eq!(categories::load(r).unwrap(), old);
}

#[test]
fn a_fresh_drive_inherits_the_librarys_initial_visibility() {
    let root = tempfile::tempdir().unwrap();
    let target = tempfile::tempdir().unwrap();
    let mut config = Categories::default();
    config.categories[0].hidden = true;
    put(root.path(), categories::PATH, &config.encode().unwrap());
    assert!(
        categories::for_transfer(root.path(), target.path())
            .unwrap()
            .categories[0]
            .hidden
    );
}
