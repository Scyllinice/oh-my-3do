use oh_my_3do_library::{
    library,
    save_archive::Entry,
    save_cheats::{self, Database, BINARY_PATH, JSON_PATH},
};
use std::fs;
fn custom() -> Database {
    let mut db = Database::bundled();
    db.profiles[0].game = "USB DOOM".into();
    db.profiles[0].presets[0].value = 7;
    db
}
fn doom() -> Entry {
    let words = [0x4c57u32, 2, 1, 15, 15, 3, 1, 2, 0];
    let checksum = words.iter().fold(12345u32, |sum, v| sum.wrapping_add(*v));
    Entry {
        name: b"DoomPrefs".to_vec(),
        file_type: 0x33444f46,
        data: words
            .into_iter()
            .chain([checksum])
            .flat_map(u32::to_be_bytes)
            .collect(),
    }
}
#[test]
fn database_roundtrip_compiles_bounded_self_describing_console_file() {
    let db = custom();
    let loaded = Database::parse(&db.json().unwrap()).unwrap();
    assert_eq!(loaded.profiles[0].presets[0].value, 7);
    let binary = db.binary().unwrap();
    assert_eq!(&binary[..8], b"OM3DCHT1");
    assert_eq!(
        u32::from_be_bytes(binary[8..12].try_into().unwrap()) as usize,
        binary.len()
    );
    assert_eq!(
        u32::from_be_bytes(binary[16..20].try_into().unwrap()),
        crc32fast::hash(&binary[24..])
    );
    let mut save = doom();
    db.profiles[0].apply_preset(&mut save, 0).unwrap();
    assert_eq!(&save.data[24..28], &7u32.to_be_bytes());
    assert!(db.profiles[0].matches(&save));
    for mutate in 0..9 {
        let mut bad = db.clone();
        match mutate {
            0 => bad.version = 2,
            1 => bad.profiles[0].fields[0].offset = usize::MAX,
            2 => bad.profiles[0].fields[1].offset = 4,
            3 => bad.profiles[0].filename = "../DoomPrefs".into(),
            4 => bad.profiles[0].presets[0].field = 999,
            5 => bad.profiles[0].presets[0].value = 999,
            6 => bad.profiles[0].checksum.algorithm = "unknown".into(),
            7 => bad.profiles.push(db.profiles[0].clone()),
            _ => bad.profiles[0].fields[0].label = "é".into(),
        };
        assert!(bad.json().is_err());
        assert!(bad.binary().is_err());
    }
    let oversized = vec![b' '; save_cheats::MAX_JSON + 1];
    assert!(Database::parse(&oversized).is_err());
}
#[test]
fn install_replaces_only_database_files_and_supports_undo_and_conflicts() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    fs::write(root.path().join("boot.iso"), b"keep boot").unwrap();
    fs::create_dir_all(root.path().join(".ohmy3do/saves")).unwrap();
    fs::write(root.path().join(".ohmy3do/saves/slot01.3ds"), b"keep save").unwrap();
    let old = Database::bundled();
    save_cheats::install(root.path(), &old, cache.path()).unwrap();
    let journal = save_cheats::install(root.path(), &custom(), cache.path()).unwrap();
    assert_eq!(
        Database::read(&root.path().join(JSON_PATH))
            .unwrap()
            .profiles[0]
            .game,
        "USB DOOM"
    );
    assert_eq!(
        fs::read(root.path().join(BINARY_PATH)).unwrap(),
        custom().binary().unwrap()
    );
    assert_eq!(
        fs::read(root.path().join("boot.iso")).unwrap(),
        b"keep boot"
    );
    assert_eq!(
        fs::read(root.path().join(".ohmy3do/saves/slot01.3ds")).unwrap(),
        b"keep save"
    );
    library::undo(&journal).unwrap();
    assert_eq!(
        fs::read(root.path().join(JSON_PATH)).unwrap(),
        old.json().unwrap()
    );
    assert_eq!(
        fs::read(root.path().join(BINARY_PATH)).unwrap(),
        old.binary().unwrap()
    );
    let bytes = fs::read(root.path().join(JSON_PATH)).unwrap();
    fs::write(root.path().join(JSON_PATH), b"external change").unwrap();
    assert!(save_cheats::save_json(
        &root.path().join(JSON_PATH),
        &old,
        Some(&bytes),
        cache.path()
    )
    .is_err());
    assert_eq!(
        fs::read(root.path().join(JSON_PATH)).unwrap(),
        b"external change"
    );
}
#[test]
fn failed_pair_install_rolls_back_source_and_preparation_preserves_custom_data() {
    let root = tempfile::tempdir().unwrap();
    let cache = tempfile::tempdir().unwrap();
    fs::create_dir_all(root.path().join(BINARY_PATH)).unwrap();
    fs::write(root.path().join(JSON_PATH), b"original source").unwrap();
    assert!(save_cheats::install(root.path(), &custom(), cache.path()).is_err());
    assert_eq!(
        fs::read(root.path().join(JSON_PATH)).unwrap(),
        b"original source"
    );
    fs::remove_dir(root.path().join(BINARY_PATH)).unwrap();
    fs::remove_file(root.path().join(JSON_PATH)).unwrap();
    save_cheats::install(root.path(), &custom(), cache.path()).unwrap();
    let mut tx = library::Journal::new(root.path(), cache.path()).unwrap();
    save_cheats::ensure_database(root.path(), &mut tx).unwrap();
    tx.finish().unwrap();
    assert_eq!(
        fs::read(root.path().join(BINARY_PATH)).unwrap(),
        custom().binary().unwrap()
    );
    let backup = root.path().join(".ohmy3do/saves/slot01.3ds");
    assert_eq!(
        save_cheats::for_backup(&backup).unwrap().unwrap().profiles[0].game,
        "USB DOOM"
    );
}
#[test]
fn adding_upstream_profiles_preserves_custom_values() {
    let current = custom();
    let mut upstream = Database::bundled();
    let mut extra = upstream.profiles[0].clone();
    extra.id = "other-game".into();
    extra.filename = "OtherSave".into();
    upstream.profiles.push(extra);
    let merged = save_cheats::merge_missing(&current, &upstream).unwrap();
    assert_eq!(merged.profiles.len(), 2);
    assert_eq!(merged.profiles[0].presets[0].value, 7);
}
#[cfg(not(windows))]
#[test]
fn desktop_database_is_loaded_and_applied_by_the_actual_console_backend() {
    use std::process::Command;
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap();
    let temp = tempfile::tempdir().unwrap();
    let source = temp.path().join("interop.c");
    let exe = temp.path().join("interop");
    fs::write(&source,r#"#include "save_cheats.h"
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){unsigned char db[65536],data[40];FILE *f;size_t n;SaveEntry entry;const SaveCheatProfile *p;(void)argc;f=fopen(argv[1],"rb");if(!f)return 1;n=fread(db,1,sizeof(db),f);fclose(f);if(save_cheats_load(db,n)<0)return 2;f=fopen(argv[2],"rb");if(!f)return 3;if(fread(data,1,40,f)!=40)return 4;fclose(f);memset(&entry,0,sizeof(entry));strcpy(entry.name,"DoomPrefs");entry.data=data;entry.size=40;p=save_cheats_find(&entry);if(!p||strcmp(p->game,"USB DOOM")||save_cheats_preset(&entry,p,0)<0)return 5;f=fopen(argv[3],"wb");if(!f)return 6;n=fwrite(data,1,40,f);fclose(f);save_cheats_reset();return n==40?0:7;}
"#).unwrap();
    let mut cc = Command::new("cc");
    cc.arg("-std=c89")
        .arg("-I")
        .arg(root.join("src"))
        .arg(&source);
    for name in [
        "save_cheats.c",
        "save_archive.c",
        "usb_art.c",
        "menu_ui.c",
        "cover_catalog.c",
    ] {
        cc.arg(root.join("src").join(name));
    }
    assert!(cc.arg("-o").arg(&exe).status().unwrap().success());
    let db = custom();
    let mut save = doom();
    let dbpath = temp.path().join("save-cheats.3db");
    let input = temp.path().join("input");
    let output = temp.path().join("output");
    fs::write(&dbpath, db.binary().unwrap()).unwrap();
    fs::write(&input, &save.data).unwrap();
    assert!(Command::new(&exe)
        .arg(dbpath)
        .arg(input)
        .arg(&output)
        .status()
        .unwrap()
        .success());
    db.profiles[0].apply_preset(&mut save, 0).unwrap();
    assert_eq!(fs::read(output).unwrap(), save.data);
}

#[test]
fn preparation_rejects_a_directory_at_a_database_file_path(){
 let root=tempfile::tempdir().unwrap();let cache=tempfile::tempdir().unwrap();fs::create_dir_all(root.path().join(BINARY_PATH)).unwrap();
 let mut tx=library::Journal::new(root.path(),cache.path()).unwrap();assert!(save_cheats::ensure_database(root.path(),&mut tx).is_err());assert!(!root.path().join(JSON_PATH).exists());tx.undo().unwrap();
}
