use oh_my_3do_library::{console_config::{self as config, Settings}, library, usb, Job};
use std::{collections::BTreeMap, fs, path::Path, sync::atomic::Ordering};

fn put(root: &Path, data: &[u8]) {
    let path = root.join(config::PATH);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, data).unwrap();
}
fn snapshot(root: &Path) -> BTreeMap<String, Vec<u8>> {
    walkdir::WalkDir::new(root).into_iter().map(Result::unwrap)
        .filter(|e| e.file_type().is_file())
        .map(|e| (e.path().strip_prefix(root).unwrap().to_string_lossy().into_owned(), fs::read(e.path()).unwrap()))
        .collect()
}
fn changed() -> Settings {
    Settings { folder_workaround: 2, usb_covers: false, screen_margin: 12,
        large_titles: true, remember_position: false, recent_games: false,
        video_previews: true, preview_sound: false, hide_adult: true }
}

#[test]
fn defaults_and_every_setting_round_trip_in_the_console_format() {
    assert_eq!(Settings::parse(include_bytes!("../../ohmy3do.ini")).unwrap(), Settings::default());
    assert_eq!(Settings::parse(b"").unwrap(), Settings::default());
    assert!(Settings::default().video_previews);
    assert!(!Settings::parse(b"video_previews=0").unwrap().video_previews);
    let settings = changed();
    let text = settings.encode().unwrap();
    assert!(text.len() < 512 && text.ends_with(b"; End of Oh My 3DO settings.\r\n"));
    assert_eq!(Settings::parse(&text).unwrap(), settings);
    let legacy = Settings::parse(b"\xef\xbb\xbf[library]\r\n hide_adult = 1 ; comment\r\nscreen_margin=09 # margin\r\n").unwrap();
    assert!(legacy.hide_adult && legacy.usb_covers && legacy.preview_sound);
    assert_eq!(legacy.screen_margin, 9);
}

#[test]
fn console_written_padding_nul_and_old_tails_are_supported() {
    let expected = changed();
    let mut text = expected.encode().unwrap();
    text.extend_from_slice(b"theme=obsolete\n\xff\xfe\n");
    text.resize(512, b' ');
    assert_eq!(Settings::parse(&text).unwrap(), expected);
    let mut old = b"usb_covers=0\n\0".to_vec();
    old.resize(512, 0xff);
    assert!(!Settings::parse(&old).unwrap().usb_covers);
    // A pre-hide_adult generated INI remains valid with the option off.
    let text = Settings::default().encode().unwrap();
    let text = String::from_utf8(text).unwrap().replace("hide_adult=0\r\n", "");
    assert!(!Settings::parse(text.as_bytes()).unwrap().hide_adult);
}

#[test]
fn invalid_values_keys_sections_and_buffer_boundaries_are_rejected() {
    for text in ["hide_adult=2\n", "screen_margin=13\n", "folder_workaround=3\n",
        "usb_covers=-1\n", "usb_covers=001\n", "usb_covers=1x\n",
        "usb_covers=1\nusb_covers=0\n", "theme=gecko\n", "[unknown]\n",
        "screen_margin=1\u{b}\n", "; End of Oh My 3DO settings.\n"] {
        assert!(Settings::parse(text.as_bytes()).is_err(), "Accepted {text:?}");
    }
    assert!(Settings::parse(&vec![b' '; 512]).is_err());
    let mut bom = b"\xef\xbb\xbf# comment".to_vec();
    bom.resize(512, b' ');
    assert!(Settings::parse(&bom).is_err());
    assert!(Settings::parse(&vec![b' '; 513]).is_err());
    assert!(Settings { screen_margin: 13, ..Default::default() }.encode().is_err());
    assert!(Settings { folder_workaround: 3, ..Default::default() }.encode().is_err());
}

#[test]
fn edit_existing_usb_ini_then_undo_restores_every_original_byte() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("USB");
    let cache = tmp.path().join("PC-cache");
    let mut original = b"# Personal comment\r\nusb_covers=0\r\n\0".to_vec();
    original.resize(512, 0);
    put(&root, &original);
    fs::write(root.join("keep.txt"), b"USB files stay put").unwrap();
    let before = snapshot(&root);
    let loaded = config::load(&root).unwrap();
    assert!(!loaded.settings.usb_covers && loaded.exists());
    assert_eq!(before, snapshot(&root));
    let saved = config::save(&loaded, &changed(), &cache, &Job::default()).unwrap();
    assert_eq!(config::load(&root).unwrap().settings, changed());
    usb::config(&root.join(config::PATH)).unwrap();
    assert_eq!(saved.loaded.settings, changed());
    library::undo(&saved.journal).unwrap();
    assert_eq!(before, snapshot(&root));
}

#[test]
fn missing_ini_loads_without_writing_and_creation_is_undoable() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("library");
    fs::create_dir(&root).unwrap();
    let loaded = config::load(&root).unwrap();
    assert!(!loaded.exists());
    assert!(!root.join(".ohmy3do").exists());
    let saved = config::save(&loaded, &Settings::default(), &tmp.path().join("cache"), &Job::default()).unwrap();
    assert!(saved.loaded.exists());
    library::undo(&saved.journal).unwrap();
    assert!(!root.join(config::PATH).exists());
    assert!(!root.join(".ohmy3do").exists());
}

#[test]
fn externally_changed_removed_or_created_files_are_never_overwritten() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("USB");
    let cache = tmp.path().join("cache");
    put(&root, b"screen_margin=1\n");
    let loaded = config::load(&root).unwrap();
    put(&root, b"screen_margin=2\n");
    let before = snapshot(&root);
    assert!(config::save(&loaded, &changed(), &cache, &Job::default()).is_err());
    assert_eq!(before, snapshot(&root));
    fs::remove_file(root.join(config::PATH)).unwrap();
    assert!(config::save(&loaded, &changed(), &cache, &Job::default()).is_err());
    assert!(!root.join(config::PATH).exists());
    let missing = config::load(&root).unwrap();
    put(&root, b"hide_adult=1\n");
    let before = snapshot(&root);
    assert!(config::save(&missing, &changed(), &cache, &Job::default()).is_err());
    assert_eq!(before, snapshot(&root));
}

#[test]
fn invalid_files_cancelled_saves_and_later_edits_are_preserved() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("USB");
    let cache = tmp.path().join("cache");
    put(&root, b"theme=obsolete\n");
    let before = snapshot(&root);
    assert!(config::load(&root).is_err());
    assert_eq!(before, snapshot(&root));
    put(&root, b"hide_adult=0\n");
    let loaded = config::load(&root).unwrap();
    let cancelled = Job::default();
    cancelled.cancel.store(true, Ordering::Relaxed);
    assert!(config::save(&loaded, &changed(), &cache, &cancelled).is_err());
    assert_eq!(fs::read(root.join(config::PATH)).unwrap(), b"hide_adult=0\n");
    let saved = config::save(&loaded, &changed(), &cache, &Job::default()).unwrap();
    put(&root, b"# Later edit\nhide_adult=0\n");
    let before = snapshot(&root);
    assert!(library::undo(&saved.journal).is_err());
    assert_eq!(before, snapshot(&root));
}

#[cfg(unix)]
#[test]
fn exported_ini_is_accepted_by_the_actual_console_c_parser() {
    use std::process::Command;
    let tmp = tempfile::tempdir().unwrap();
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let code = tmp.path().join("config_check.c");
    let binary = tmp.path().join("config_check");
    let ini = tmp.path().join("ohmy3do.ini");
    fs::write(&ini, changed().encode().unwrap()).unwrap();
    fs::write(&code, r#"
#include "menu_config.h"
#include <assert.h>
#include <stdio.h>
int main(int argc, char **argv) {
    char text[512]; size_t size; FILE *file; MenuConfig c;
    assert(argc == 2); file = fopen(argv[1], "rb"); assert(file);
    size = fread(text, 1, sizeof(text), file); fclose(file);
    assert(size < sizeof(text) && !menu_config_parse(&c, text, size));
    assert(c.folder_workaround == 2 && !c.usb_covers && c.screen_margin == 12);
    assert(c.large_titles && !c.remember_position && !c.recent_games);
    assert(c.video_previews && !c.preview_sound && c.hide_adult);
    return 0;
}
"#).unwrap();
    assert!(Command::new(std::env::var("CC").unwrap_or_else(|_| "cc".into()))
        .arg("-std=c89").arg("-I").arg(repo.join("src"))
        .arg(repo.join("src/menu_config.c")).arg(&code).arg("-o").arg(&binary)
        .status().unwrap().success());
    assert!(Command::new(binary).arg(ini).status().unwrap().success());
}

#[cfg(unix)]
#[test]
fn settings_paths_reject_linked_files_and_directories() {
    use std::os::unix::fs::symlink;
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("USB");
    fs::create_dir_all(root.join(".ohmy3do")).unwrap();
    let external = tmp.path().join("outside.ini");
    fs::write(&external, b"hide_adult=1\n").unwrap();
    symlink(&external, root.join(config::PATH)).unwrap();
    assert!(config::load(&root).is_err());
    fs::remove_file(root.join(config::PATH)).unwrap();
    fs::remove_dir(root.join(".ohmy3do")).unwrap();
    symlink(tmp.path(), root.join(".ohmy3do")).unwrap();
    assert!(config::load(&root).is_err());
    assert_eq!(fs::read(&external).unwrap(), b"hide_adult=1\n");
}
