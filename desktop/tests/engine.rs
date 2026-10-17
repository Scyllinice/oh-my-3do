use oh_my_3do_library::{catalog, disc, fsutil as f, library, media, model::*, usb, Job};
use serde_json::json;
use sha1::{Digest, Sha1};
use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
};
fn put(root: &Path, name: &str, bytes: &[u8]) -> PathBuf {
    let p = root.join(name);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(&p, bytes).unwrap();
    p
}
fn game(name: &str, data: &[u8]) -> Game {
    Game {
        name: name.into(),
        release: name.into(),
        category: "Games".into(),
        metadata: json!({"developer":"Café","genre":"Adventure"}),
        roms: vec![Rom {
            name: format!("{name}.bin"),
            size: data.len() as u64,
            sha1: format!("{:x}", Sha1::digest(data)),
            serial: String::new(),
        }],
        ..Default::default()
    }
}
fn catalog(data: &[u8]) -> Catalog {
    Catalog {
        games: vec![game("Example (USA)", data)],
        sources: vec![],
        complete_catalog: true,
    }
}
fn snapshot(root: &Path) -> BTreeMap<String, Vec<u8>> {
    walkdir::WalkDir::new(root)
        .into_iter()
        .filter_map(|e| e.ok())
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
fn boot() -> Vec<u8> {
    let mut b = vec![0; 2048];
    b[..7].copy_from_slice(&[1, 90, 90, 90, 90, 90, 1]);
    b[76..80].copy_from_slice(&2048u32.to_be_bytes());
    b[80..84].copy_from_slice(&1u32.to_be_bytes());
    b
}
#[test]
fn mode1_reconstruction_matches_python_reference() {
    let data = (0..4096)
        .map(|n| ((n * 17 + 3) % 256) as u8)
        .collect::<Vec<_>>();
    let mut raw = vec![];
    for (n, s) in data.chunks(2048).enumerate() {
        raw.extend(disc::encode_sector(s, n as u32).unwrap());
    }
    assert_eq!(
        format!("{:x}", Sha1::digest(raw)),
        "ff2272170b4b3373cfa3674a0c76c51358be39a7"
    );
}

#[test]
fn metadata_export_matches_console_fixture_and_rejects_corruption() {
    use sha2::Sha256;
    let names = vec![
        "Games/USA/Beta/Beta.iso".into(),
        "Games/USA/Alpha/Disc 1.iso".into(),
    ];
    let db = json!({"games":[
        {"path":"Games/USA/Alpha/Disc 1.iso","metadata":{"genre":"Action","publisher":"Café Studio","release_date":"1995-02-03T00:00:00","year":1995}},
        {"path":"Games/USA/Beta/Beta.iso","metadata":{"publisher":"Test","year":1994}},
        {"path":"missing.iso","metadata":{"genre":"Ignored"}}
    ]});
    let bytes = library::metadata_bytes(&db, &names).unwrap();
    assert_eq!(
        format!("{:x}", Sha256::digest(&bytes)),
        "b0dfcec455daf4c1a279ee17c522d029f23b22508ad84fd6e6f19e5826572058"
    );
    let mut expected = names.clone();
    expected.sort();
    assert_eq!(library::metadata_paths(&bytes).unwrap(), expected);
    for i in 0..bytes.len() {
        assert!(library::metadata_paths(&bytes[..i]).is_err());
        let mut bad = bytes.clone();
        bad[i] ^= 1;
        assert!(library::metadata_paths(&bad).is_err());
    }
}

#[test]
fn usb_configuration_accepts_gecko_tv_and_rejects_unknown_themes() {
    let temp = tempfile::tempdir().unwrap();
    let text = include_str!("../../ohmy3do.ini");
    let path = put(
        temp.path(),
        "settings.ini",
        text.replace("theme=0", "theme=3").as_bytes(),
    );
    usb::config(&path).unwrap();
    fs::write(&path, text.replace("theme=0", "theme=4")).unwrap();
    assert!(usb::config(&path).is_err());
}
#[test]
fn scanner_identifies_converted_iso_without_changing_input() {
    let t = tempfile::tempdir().unwrap();
    let source = t.path().join("incoming");
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let cooked = vec![0x4a; 2048];
    let iso = put(&source, "Unknown.iso", &cooked);
    let raw = disc::encode_sector(&cooked, 0).unwrap();
    let mut cat = catalog(&raw);
    catalog::prepare(&mut cat);
    let p = library::scan(&root, &[iso.clone()], cat, false, &Job::default()).unwrap();
    assert_eq!(p.entries[0].confidence, "converted-checksum");
    assert!(p.entries[0].selected);
    assert_eq!(fs::read(iso).unwrap(), cooked);
    assert!(snapshot(&root).is_empty());
}
#[test]
fn checksum_import_preserves_original_and_metadata_and_undoes() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let iso = put(t.path(), "incoming/a.iso", b"fictional disc");
    let before = snapshot(&root);
    let mut cat = catalog(b"fictional disc");
    catalog::prepare(&mut cat);
    let plan = library::scan(&root, &[iso.clone()], cat, false, &Job::default()).unwrap();
    let journal = library::import(
        &plan,
        &Preferences::default(),
        &t.path().join("cache"),
        &Job::default(),
    )
    .unwrap();
    let output = root.join(&plan.entries[0].target);
    assert_eq!(fs::read(&output).unwrap(), b"fictional disc");
    let db = library::database(&root).unwrap();
    assert_eq!(db["games"][0]["metadata"]["developer"], "Café");
    assert_eq!(db["games"][0]["identification"], "checksum");
    assert_eq!(fs::read(&iso).unwrap(), b"fictional disc");
    library::undo(&journal).unwrap();
    assert_eq!(snapshot(&root), before);
    assert!(iso.is_file());
}
#[test]
fn quick_matches_need_explicit_acceptance() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let iso = put(t.path(), "incoming/Example (USA).iso", b"fictional");
    let mut cat = catalog(b"fictional");
    catalog::prepare(&mut cat);
    let p = library::scan(&root, &[iso], cat, true, &Job::default()).unwrap();
    assert_eq!(p.entries[0].confidence, "filename");
    assert!(!p.entries[0].selected);
    assert!(library::import(
        &p,
        &Preferences::default(),
        &t.path().join("cache"),
        &Job::default()
    )
    .is_err());
}
#[test]
fn archives_are_rejected_before_import() {
    let t = tempfile::tempdir().unwrap();
    let iso = put(t.path(), "wrong.iso", b"PK\x03\x04archive");
    assert!(disc::issue(&iso).is_err());
    assert!(disc::collect(&[iso], None, &Job::default()).is_err());
}
#[test]
fn cue_wrappers_are_ignored_but_real_tracks_are_preserved() {
    let t = tempfile::tempdir().unwrap();
    let iso = put(t.path(), "game.iso", b"fictional");
    let cue = put(
        t.path(),
        "game.cue",
        b"FILE \"game.iso\" BINARY\n TRACK 01 MODE1/2048\n INDEX 01 00:00:00\n",
    );
    assert_eq!(
        disc::redundant_cue(&cue).unwrap(),
        Some(iso.canonicalize().unwrap())
    );
    let units = disc::collect(&[t.path().to_owned()], None, &Job::default()).unwrap();
    assert_eq!(
        units,
        vec![(
            iso.canonicalize().unwrap(),
            vec![iso.canonicalize().unwrap()]
        )]
    );
}
#[test]
fn cue_import_renames_tracks_and_undo_checks_original_bytes() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let cue = put(
        t.path(),
        "incoming/source.cue",
        b"FILE \"track.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n",
    );
    let bin = put(t.path(), "incoming/track.bin", b"fictional");
    let mut cat = catalog(b"fictional");
    catalog::prepare(&mut cat);
    let plan = library::scan(&root, &[cue.clone()], cat, false, &Job::default()).unwrap();
    let journal = library::import(
        &plan,
        &Preferences::default(),
        &t.path().join("cache"),
        &Job::default(),
    )
    .unwrap();
    let text = fs::read_to_string(root.join(&plan.entries[0].target)).unwrap();
    assert!(text.contains("Example (USA).bin"));
    assert!(bin.exists());
    library::undo(&journal).unwrap();
    assert!(!root.join(&plan.entries[0].target).exists());
    assert!(cue.exists());
}
#[test]
fn mult_disc_sets_consolidate_into_one_folder() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let a = put(t.path(), "one/1.iso", b"one");
    let b = put(t.path(), "two/2.iso", b"two");
    let mut cat = Catalog {
        games: vec![
            game("Example (USA) (Disc 1)", b"one"),
            game("Example (USA) (Disc 2)", b"two"),
        ],
        ..Default::default()
    };
    catalog::prepare(&mut cat);
    let p = library::scan(&root, &[a, b], cat, false, &Job::default()).unwrap();
    let parents = p
        .entries
        .iter()
        .map(|e| Path::new(&e.target).parent().unwrap())
        .collect::<std::collections::HashSet<_>>();
    assert_eq!(parents.len(), 1);
    assert!(parents.contains(Path::new("Games/USA/Example (USA)")));
}
#[test]
fn raw_track_partial_match_stays_unchecked() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let cooked = vec![0x4a; 2048];
    let iso = put(t.path(), "incoming/x.iso", &cooked);
    let raw = disc::encode_sector(&cooked, 0).unwrap();
    let mut g = game("Example (USA)", &raw);
    g.roms[0].name = "Example (Track 1).bin".into();
    g.roms.push(Rom {
        name: "Example (Track 2).bin".into(),
        size: 1,
        sha1: "0000000000000000000000000000000000000000".into(),
        ..Default::default()
    });
    let mut cat = Catalog {
        games: vec![g],
        ..Default::default()
    };
    catalog::prepare(&mut cat);
    let p = library::scan(&root, &[iso], cat, false, &Job::default()).unwrap();
    assert_eq!(p.entries[0].confidence, "data-track");
    assert!(!p.entries[0].selected);
}
#[test]
fn variants_have_stable_distinct_names() {
    let mut cat = Catalog {
        games: vec![game("Example (USA)", b"one"), game("Example (USA)", b"two")],
        ..Default::default()
    };
    catalog::prepare(&mut cat);
    assert_ne!(cat.games[0].release, cat.games[1].release);
    assert!(cat.games[0].release.contains("[Variant "));
    let names = cat
        .games
        .iter()
        .map(|g| g.release.clone())
        .collect::<Vec<_>>();
    catalog::prepare(&mut cat);
    assert_eq!(
        names,
        cat.games
            .iter()
            .map(|g| g.release.clone())
            .collect::<Vec<_>>()
    );
}
#[test]
fn dat_xml_and_clrmamepro_retain_audio_and_drop_cue() {
    let text="clrmamepro ( name \"3DO\" ) game ( name \"Example (USA)\" category Games rom ( name \"a.bin\" size 7 sha1 abcd ) rom ( name \"a.cue\" size 2 ) )";
    let games = catalog::parse_dat(text).unwrap();
    assert_eq!(games[0].roms.len(), 1);
    assert_eq!(games[0].roms[0].size, 7);
    let games=catalog::parse_dat("<datafile><game name=\"Example\"><category>Games</category><rom name=\"a.bin\" size=\"7\" sha1=\"abcd\"/><rom name=\"audio.bin\" size=\"9\" sha1=\"ef01\"/></game></datafile>").unwrap();
    assert_eq!(games[0].roms.len(), 2);
}
#[test]
fn seven_categories_remain_distinct() {
    for c in CATEGORIES {
        let mut g = game("Example (USA)", b"x");
        g.category = c.into();
        assert_eq!(library::classify(&g).0, c)
    }
    let mut g = game("A demo (USA) (Demo)", b"x");
    g.category.clear();
    assert_eq!(library::classify(&g).0, "Demos");
}
#[test]
fn duplicate_targets_and_occupied_destinations_block_import() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let a = put(t.path(), "incoming/a.iso", b"same");
    let b = put(t.path(), "incoming/b.iso", b"same");
    let mut cat = catalog(b"same");
    catalog::prepare(&mut cat);
    let plan = library::scan(&root, &[a, b], cat, false, &Job::default()).unwrap();
    assert!(plan.entries.iter().all(|e| !e.selected && e.resolvable()));
}
#[test]
fn journal_repeated_writes_restore_original_and_refuse_later_edits() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let cache = t.path().join("cache");
    let p = put(&root, "index.3di", b"before");
    let mut j = library::Journal::new(&root, &cache).unwrap();
    j.write("index.3di", b"pass one").unwrap();
    j.write("index.3di", b"pass two").unwrap();
    j.write("other.txt", b"new").unwrap();
    let path = j.path.clone();
    j.finish().unwrap();
    fs::write(&p, b"edited later").unwrap();
    let before = snapshot(&root);
    assert!(library::undo(&path).is_err());
    assert_eq!(snapshot(&root), before);
    fs::write(&p, b"pass two").unwrap();
    library::undo(&path).unwrap();
    assert_eq!(fs::read(p).unwrap(), b"before");
    assert!(!root.join("other.txt").exists());
}
#[test]
fn copy_hash_mismatch_never_publishes_bad_disc() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let source = put(t.path(), "a.iso", b"wrong");
    let mut j = library::Journal::new(&root, &t.path().join("cache")).unwrap();
    assert!(j
        .copy(&source, "game.iso", Some(&"0".repeat(64)), &Job::default())
        .is_err());
    j.undo().unwrap();
    assert!(!root.join("game.iso").exists());
}
#[test]
fn cancelled_import_does_not_change_library() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let iso = put(t.path(), "incoming/a.iso", b"x");
    let mut cat = catalog(b"x");
    catalog::prepare(&mut cat);
    let plan = library::scan(&root, &[iso], cat, false, &Job::default()).unwrap();
    let job = Job::default();
    job.cancel.store(true, std::sync::atomic::Ordering::Relaxed);
    assert!(library::import(
        &plan,
        &Preferences::default(),
        &t.path().join("cache"),
        &job
    )
    .is_err());
    assert!(snapshot(&root).is_empty());
}
#[test]
fn path_guards_reject_traversal_case_collisions_and_fat_names() {
    let t = tempfile::tempdir().unwrap();
    put(t.path(), "Games/file.iso", b"x");
    for name in [
        "../outside",
        "Games/../outside",
        "Games/CON.iso",
        "Games/Bad?.iso",
        "games/other.iso",
        "/absolute",
    ] {
        assert!(f::safe(t.path(), name).is_err(), "{name}")
    }
}
#[test]
fn cover_conversion_outputs_native_png_with_valid_chunks() {
    let t = tempfile::tempdir().unwrap();
    let source = t.path().join("cover.jpg");
    image::RgbImage::from_pixel(200, 300, image::Rgb([220, 20, 60]))
        .save(&source)
        .unwrap();
    let target = t.path().join("cover.png");
    media::cover(&source, &target).unwrap();
    media::validate(&target).unwrap();
    let im = image::open(&target).unwrap();
    assert_eq!((im.width(), im.height()), (84, 105));
    let mut bytes = fs::read(&target).unwrap();
    bytes[30] ^= 1;
    fs::write(&target, bytes).unwrap();
    assert!(media::validate(&target).is_err());
}
#[test]
fn media_matching_does_not_cross_regions_or_demo_types() {
    let paths = vec![
        PathBuf::from("Example (Japan).mp4"),
        PathBuf::from("Example (USA) (Demo).mp4"),
    ];
    assert!(media::matches("Example (USA)", Path::new("Example (USA).iso"), &paths).is_empty());
    assert_eq!(
        disc::title_key("Legend, The (USA)"),
        disc::title_key("The Legend (Japan)")
    );
}
#[test]
fn preview_matching_prefers_region_before_unique_fallback() {
    let source = Path::new("Gex (USA, Europe).iso");
    let jp = PathBuf::from("Gex (Japan, Korea).mp4");
    let us = PathBuf::from("Gex (USA).mp4");
    let exact = PathBuf::from("Gex (USA, Europe).mp4");
    let title = "Gex (USA, Europe)";
    assert_eq!(
        media::preview_matches(title, source, &[jp.clone()]),
        vec![jp.clone()]
    );
    assert!(media::matches(title, source, &[jp.clone()]).is_empty());
    assert_eq!(
        media::preview_matches(title, source, &[jp.clone(), us.clone()]),
        vec![us.clone()]
    );
    assert_eq!(
        media::preview_matches(title, source, &[jp.clone(), us, exact.clone()]),
        vec![exact]
    );
    // Multiple foreign clips remain ambiguous; demo and wrong-disc clips never qualify.
    let candidates = vec![jp.clone(), PathBuf::from("Gex (Japan).avi")];
    assert_eq!(media::preview_matches(title, source, &candidates).len(), 2);
    for bad in [
        "Gex (Japan) (Demo).mp4",
        "Gex (Japan) (Disc 2).mp4",
        "Other (Japan).mp4",
    ] {
        assert!(media::preview_matches(title, source, &[PathBuf::from(bad)]).is_empty());
    }
    let disc = PathBuf::from("D (Japan) (Disc 1).mp4");
    assert_eq!(
        media::preview_matches("D (USA) (Disc 1)", Path::new("D.iso"), &[disc.clone()]),
        vec![disc]
    );
    // Ambiguous compatible files must not be replaced with a foreign fallback.
    let candidates = vec![
        PathBuf::from("Gex (USA).mp4"),
        PathBuf::from("Gex (USA).avi"),
        jp,
    ];
    let selected = media::preview_matches(title, source, &candidates);
    assert_eq!(selected, candidates[..2]);
}
#[test]
fn usb_preview_excludes_databases_reports_recovery_and_unindexed_art() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    let dest = t.path().join("USB");
    fs::create_dir(&root).unwrap();
    fs::create_dir(&dest).unwrap();
    put(&root, "boot.iso", &boot());
    put(&root, "Games/example.iso", b"fictional");
    put(
        &root,
        ".ohmy3do/ohmy3do.ini",
        include_bytes!("../../ohmy3do.ini"),
    );
    put(&root, ".ohmy3do/recovery/old.bin", b"private backup");
    put(
        &root,
        ".ohmy3do/library.json",
        br#"{"schema":1,"sources":[],"games":[]}"#,
    );
    let cache = t.path().join("cache");
    let mut tx = library::Journal::new(&root, &cache).unwrap();
    library::build_indexes(&root, &mut tx, &Job::default()).unwrap();
    tx.finish().unwrap();
    let p = usb::plan(&root, &dest, &cache, false, &Job::default()).unwrap();
    assert_eq!(p.games, 1);
    assert_eq!(p.files.len(), 5);
    assert!(p
        .files
        .iter()
        .any(|item| item.path == ".ohmy3do/metadata.3di"));
    assert!(snapshot(&dest).is_empty());
}
#[test]
fn usb_preserves_preferences_saves_and_extra_files_then_undoes() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    let dest = t.path().join("USB");
    fs::create_dir(&root).unwrap();
    fs::create_dir(&dest).unwrap();
    put(&root, "boot.iso", &boot());
    put(&root, "Games/example.iso", b"fictional");
    put(
        &root,
        ".ohmy3do/ohmy3do.ini",
        include_bytes!("../../ohmy3do.ini"),
    );
    put(&root, ".ohmy3do/saves/save.3ds", b"staging save");
    put(&dest, ".ohmy3do/ohmy3do.ini", b"folder_workaround=1\n");
    put(&dest, ".ohmy3do/saves/save.3ds", b"USB save");
    put(&dest, "keep.txt", b"keep");
    let before = snapshot(&dest);
    let cache = t.path().join("cache");
    let mut tx = library::Journal::new(&root, &cache).unwrap();
    library::build_indexes(&root, &mut tx, &Job::default()).unwrap();
    tx.finish().unwrap();
    let plan = usb::plan(&root, &dest, &cache, false, &Job::default()).unwrap();
    let journal = usb::apply(&plan, &cache, &Job::default()).unwrap();
    assert_eq!(
        fs::read(dest.join(".ohmy3do/saves/save.3ds")).unwrap(),
        b"USB save"
    );
    assert_eq!(fs::read(dest.join("keep.txt")).unwrap(), b"keep");
    library::undo(&journal).unwrap();
    assert_eq!(snapshot(&dest), before);
}

#[test]
fn cue_utf8_bom_and_windows_1252_are_supported() {
    let t = tempfile::tempdir().unwrap();
    put(t.path(), "café.bin", b"audio");
    let cue = put(
        t.path(),
        "album.cue",
        b"FILE \"caf\xe9.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n",
    );
    let (_, refs) = disc::cue_refs(&cue).unwrap();
    assert_eq!(refs.len(), 1);
    fs::write(
        &cue,
        "\u{feff}FILE \"café.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n",
    )
    .unwrap();
    assert_eq!(disc::cue_refs(&cue).unwrap().1, refs);
}
#[test]
fn long_disc_names_preserve_the_variant_tag() {
    let name = format!(
        "{} (USA) [Variant ABCD1234] (Disc 2)",
        "Long game title ".repeat(20)
    );
    let stem = disc::disc_stem(&name);
    assert!(stem.len() <= 110);
    assert!(stem.ends_with("[Variant ABCD1234] (Disc 2)"));
}
#[test]
fn corrupt_or_stale_find_blocks_copy_without_changing_destination() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    let dest = t.path().join("USB");
    fs::create_dir(&root).unwrap();
    fs::create_dir(&dest).unwrap();
    put(&root, "boot.iso", &boot());
    put(&root, "Game.iso", b"first");
    put(
        &root,
        ".ohmy3do/ohmy3do.ini",
        include_bytes!("../../ohmy3do.ini"),
    );
    let cache = t.path().join("cache");
    let mut j = library::Journal::new(&root, &cache).unwrap();
    library::build_indexes(&root, &mut j, &Job::default()).unwrap();
    j.finish().unwrap();
    put(&root, "New.iso", b"second");
    assert!(usb::plan(&root, &dest, &cache, false, &Job::default()).is_err());
    assert!(snapshot(&dest).is_empty());
}
#[test]
fn cancellation_mid_import_restores_all_already_copied_files() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let a = put(t.path(), "incoming/a.iso", b"one");
    let b = put(t.path(), "incoming/b.iso", b"two");
    let mut cat = Catalog {
        games: vec![game("One (USA)", b"one"), game("Two (USA)", b"two")],
        ..Default::default()
    };
    catalog::prepare(&mut cat);
    let plan = library::scan(&root, &[a, b], cat, false, &Job::default()).unwrap();
    let cancel = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let stop = cancel.clone();
    let job = Job {
        cancel,
        log: std::sync::Arc::new(move |line| {
            if line.starts_with("Importing 2/") {
                stop.store(true, std::sync::atomic::Ordering::Relaxed);
            }
        }),
        ..Default::default()
    };
    assert!(library::import(
        &plan,
        &Preferences::default(),
        &t.path().join("cache"),
        &job
    )
    .is_err());
    assert!(snapshot(&root).is_empty());
}
fn synthetic_preview() -> Vec<u8> {
    use std::io::Write;
    let mut z = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::best());
    z.write_all(&vec![0; 10240]).unwrap();
    let block = z.finish().unwrap();
    let mut video = 8u32.to_be_bytes().to_vec();
    video.extend_from_slice(&((8 + block.len()) as u32).to_be_bytes());
    video.extend(block);
    let mut data = b"OM3DVID2".to_vec();
    data.extend_from_slice(&64u16.to_be_bytes());
    data.extend_from_slice(&80u16.to_be_bytes());
    for n in [video.len() as u32, crc32fast::hash(&video)] {
        data.extend_from_slice(&n.to_be_bytes());
    }
    data.extend_from_slice(&1u16.to_be_bytes());
    data.extend_from_slice(&15u16.to_be_bytes());
    for n in [video.len() as u32, 0, 0, 0] {
        data.extend_from_slice(&n.to_be_bytes());
    }
    data.extend(video);
    data.extend_from_slice(b"END3DV02");
    data
}
#[test]
fn corrupt_preview_checksum_or_frame_data_is_rejected() {
    let mut data = synthetic_preview();
    let t = tempfile::tempdir().unwrap();
    let path = put(t.path(), "clip.3dv", &data);
    media::validate(&path).unwrap();
    data[48] ^= 1;
    let crc = crc32fast::hash(&data[40..data.len() - 8]);
    data[16..20].copy_from_slice(&crc.to_be_bytes());
    fs::write(&path, data).unwrap();
    assert!(media::validate(&path).is_err());
}
#[test]
fn native_media_update_preserves_identity_and_registration_fields() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    put(&root, "Game.iso", b"fictional");
    put(
        &root,
        ".ohmy3do/ohmy3do.ini",
        include_bytes!("../../ohmy3do.ini"),
    );
    let record = json!({"path":"Game.iso","title":"Homebrew (USA)","region":"USA","category":"Games","category_review":false,"category_reason":"Explicit category override","identification":"unverified","status":"registered","metadata":{"developer":"Café"},"registration":{"Game.iso":{"size":9,"sha1":"proof"}},"custom_field":"preserve","cover":null,"preview":null});
    put(
        &root,
        ".ohmy3do/library.json",
        &serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[record]})).unwrap(),
    );
    let source = t.path().join("provided.png");
    image::RgbImage::from_pixel(200, 300, image::Rgb([50, 100, 200]))
        .save(&source)
        .unwrap();
    let journal = library::update_media(
        &root,
        &Preferences::default(),
        &t.path().join("cache"),
        Some(("Game.iso", Some(source), None)),
        &Job::default(),
    )
    .unwrap();
    let db = library::database(&root).unwrap();
    for key in [
        "identification",
        "status",
        "registration",
        "custom_field",
        "metadata",
    ] {
        assert_eq!(db["games"][0][key], record[key]);
    }
    assert!(db["games"][0]["cover"].is_string());
    library::undo(&journal).unwrap();
    assert_eq!(library::database(&root).unwrap()["games"][0], record);
}

#[test]
fn converted_single_and_partial_track_matches_remain_ambiguous() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let cooked = vec![0x4a; 2048];
    let iso = put(t.path(), "incoming/Unknown.iso", &cooked);
    let raw = disc::encode_sector(&cooked, 0).unwrap();
    let one = game("Example (USA)", &raw);
    let mut multi = game("Example (Europe)", &raw);
    multi.roms[0].name = "Example (Track 1).bin".into();
    multi.roms.push(Rom {
        name: "Example (Track 2).bin".into(),
        size: 1,
        sha1: "0000000000000000000000000000000000000000".into(),
        ..Default::default()
    });
    let mut cat = Catalog {
        games: vec![one, multi],
        ..Default::default()
    };
    catalog::prepare(&mut cat);
    let p = library::scan(&root, &[iso], cat, false, &Job::default()).unwrap();
    assert!(!p.entries[0].selected);
    assert_eq!(p.entries[0].candidates.len(), 2);
    assert!(p.entries[0].target.is_empty());
}
#[test]
fn duplicate_sources_can_be_resolved_by_checking_only_one() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let a = put(t.path(), "incoming/a.iso", b"same");
    let b = put(t.path(), "incoming/b.iso", b"same");
    let mut cat = catalog(b"same");
    catalog::prepare(&mut cat);
    let mut p = library::scan(&root, &[a.clone(), b.clone()], cat, false, &Job::default()).unwrap();
    assert!(!p.entries[0].selected && !p.entries[1].selected);
    p.entries[0].selected = true;
    library::import(
        &p,
        &Preferences::default(),
        &t.path().join("cache"),
        &Job::default(),
    )
    .unwrap();
    assert!(a.exists() && b.exists());
    assert_eq!(
        library::database(&root).unwrap()["games"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
}
#[test]
fn native_library_lock_prevents_overlapping_writers() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    let cache = t.path().join("cache");
    let lock = library::lock_library(&root, &cache).unwrap();
    assert!(library::lock_library(&root, &cache).is_err());
    drop(lock);
    assert!(library::lock_library(&root, &cache).is_ok());
}

#[test]
fn native_missing_media_uses_foreign_preview_but_not_cover_and_preserves_existing() {
    use sha2::Sha256;
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    put(&root, "Gex.iso", b"fictional");
    put(
        &root,
        ".ohmy3do/ohmy3do.ini",
        include_bytes!("../../ohmy3do.ini"),
    );
    let record = json!({"path":"Gex.iso","title":"Gex (USA, Europe)","region":"USA","category":"Games","metadata":{"developer":"Café"},"identification":"checksum","cover":null,"preview":null});
    put(
        &root,
        ".ohmy3do/library.json",
        &serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[record]})).unwrap(),
    );
    let emu = t.path().join("EmuMovies");
    let source = put(
        &emu,
        "Video_MP4_HI_QUAL/Gex (Japan, Korea).mp4",
        b"higher quality source",
    );
    put(
        &emu,
        "Video_MP4/Gex (Japan, Korea).mp4",
        b"lower quality source",
    );
    put(
        &emu,
        "Box/Gex (Japan, Korea).jpg",
        b"must not import foreign cover",
    );
    let cache = t.path().join("cache");
    // Seed a validated conversion so the integration test needs no FFmpeg.
    let hash = f::sha256(&source, &Job::default()).unwrap();
    let key = format!(
        "{:x}",
        Sha256::digest(format!("{hash}:preview:native-v1-15fps-cap15").as_bytes())
    );
    put(
        &cache,
        &format!("converted-native/{key}.3dv"),
        &synthetic_preview(),
    );
    let prefs = Preferences {
        emumovies: emu.to_string_lossy().into_owned(),
        offline: true,
        ..Default::default()
    };
    let before = snapshot(&root);
    let journal = library::update_media(&root, &prefs, &cache, None, &Job::default()).unwrap();
    let db = library::database(&root).unwrap();
    assert!(db["games"][0]["preview"].is_string());
    assert_eq!(
        Path::new(db["games"][0]["preview_source"].as_str().unwrap()),
        source.as_path()
    );
    assert!(db["games"][0]["cover"].is_null());
    assert_eq!(db["games"][0]["metadata"], record["metadata"]);
    assert_eq!(db["games"][0]["identification"], record["identification"]);
    put(
        &emu,
        "Video_MP4_HD/Gex (USA, Europe).mp4",
        b"new source must not replace preview",
    );
    library::update_media(&root, &prefs, &cache, None, &Job::default()).unwrap();
    assert_eq!(library::database(&root).unwrap()["games"], db["games"]);
    library::undo(&journal).unwrap();
    assert_eq!(snapshot(&root), before);
}
