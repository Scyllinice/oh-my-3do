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
fn metadata_keeps_discs_without_catalog_details_for_game_folders() {
    let names = vec![
        "Adult/Japan/Bodyconscious/Bodyconscious.iso".to_string(),
        "Adult/Japan/Super Model/Super Model.iso".to_string(),
    ];
    let db = json!({"games":[{"path":names[0],"metadata":{}}]});
    let bytes = library::metadata_bytes(&db, &names).unwrap();
    assert_eq!(library::metadata_paths(&bytes).unwrap(), names);
    assert_eq!(bytes, library::metadata_bytes(&json!({"games":[]}), &names).unwrap());
    let duplicate = json!({"games":[{"path":names[0],"metadata":{}},{"path":names[0],"metadata":{}}]});
    assert!(library::metadata_bytes(&duplicate, &names).is_err());
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
fn usb_configuration_rejects_removed_theme_key() {
    let temp = tempfile::tempdir().unwrap();
    let text = include_str!("../../ohmy3do.ini");
    let path = put(temp.path(), "settings.ini", text.as_bytes());
    usb::config(&path).unwrap();
    fs::write(&path, text.replace("large_titles=0", "large_titles=0\ntheme=3")).unwrap();
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
    assert_eq!((im.width(), im.height()), (96, 120));
    let mut bytes = fs::read(&target).unwrap();
    bytes[30] ^= 1;
    fs::write(&target, bytes).unwrap();
    assert!(media::validate(&target).is_err());
}
#[test]
fn prepared_cover_matches_png_console_pixels_and_rejects_damage() {
    let t = tempfile::tempdir().unwrap();
    let source = t.path().join("cover.png");
    let mut image = image::RgbImage::new(96, 120);
    for (x, y, p) in image.enumerate_pixels_mut() {
        *p = image::Rgb([x as u8 * 2, y as u8 * 2, (x + y) as u8]);
    }
    image.save(&source).unwrap();
    let png = t.path().join("prepared.png");
    let fast = t.path().join("prepared.3dc");
    media::cover(&source, &png).unwrap();
    media::fast_cover(&source, &fast).unwrap();
    media::validate(&fast).unwrap();
    let original = image::open(png).unwrap().to_rgb8();
    let decoded = media::cover_display(&fast).unwrap().to_rgb8();
    for (a, b) in original.pixels().zip(decoded.pixels()) {
        for n in 0..3 {
            assert_eq!((a[n] >> 3) as u16 * 255 / 31, b[n] as u16);
        }
    }
    let bytes = fs::read(&fast).unwrap();
    for pos in [8, 12, 16, 20, 24, bytes.len() - 1] {
        let mut bad = bytes.clone();
        bad[pos] ^= 1;
        fs::write(&fast, bad).unwrap();
        assert!(media::validate(&fast).is_err());
    }
}
fn preview_frames(count: usize) -> Vec<u8> {
    // A drifting gradient with a busy square: motion, smooth areas and detail.
    let mut frames = Vec::with_capacity(count * 160 * 120 * 3);
    for frame in 0..count {
        for y in 0..120usize {
            for x in 0..160usize {
                if (40..104).contains(&x) && (30..94).contains(&y) {
                    let v = (x * 37 + y * 101 + frame * 13) * 2654435761 % 256;
                    frames.extend_from_slice(&[v as u8, (v * 7) as u8, (v * 13) as u8]);
                } else {
                    frames.extend_from_slice(&[(x + frame * 3) as u8, y as u8, ((x + y) / 2 + frame * 3) as u8]);
                }
            }
        }
    }
    frames
}
fn preview_container(video: &[u8], frames: usize) -> Vec<u8> {
    let audio = ((frames * 11025).div_ceil(30) + 3) & !3;
    let table = (frames + 1) * 4;
    let mut payload = video[..table].to_vec();
    payload.resize(table + audio, 0);
    payload.extend(&video[table..]);
    payload.resize(payload.len().next_multiple_of(4), 0);
    let mut data = b"OM3DVID5".to_vec();
    data.extend_from_slice(&160u16.to_be_bytes());
    data.extend_from_slice(&120u16.to_be_bytes());
    data.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    data.extend_from_slice(&media::word_sum(&payload).to_be_bytes());
    data.extend_from_slice(&(frames as u16).to_be_bytes());
    data.extend_from_slice(&30u16.to_be_bytes());
    for n in [video.len(), audio, 11025, audio] {
        data.extend_from_slice(&(n as u32).to_be_bytes());
    }
    data.extend(payload);
    data.extend_from_slice(b"END3DV05");
    data
}
#[test]
fn preview_encoder_gives_identical_output_on_parallel_threads() {
    // Media rebuilds encode several clips at once; the encoder's working state
    // must be per thread or concurrent clips corrupt each other.
    let clips = (0..4).map(|i| preview_frames(4 + i)).collect::<Vec<_>>();
    let alone = clips
        .iter()
        .map(|f| media::encode_preview(f, 64 * 1024, &Job::default()).unwrap())
        .collect::<Vec<_>>();
    for _ in 0..2 {
        let together = std::thread::scope(|s| {
            clips
                .iter()
                .map(|f| s.spawn(move || media::encode_preview(f, 64 * 1024, &Job::default()).unwrap()))
                .collect::<Vec<_>>()
                .into_iter()
                .map(|h| h.join().unwrap())
                .collect::<Vec<_>>()
        });
        assert_eq!(together, alone);
    }
}
#[test]
fn preview_encoder_fills_its_budget_and_validates() {
    let frames = preview_frames(20);
    let budget = 40 * 1024; // below best quality, so rate control must work
    let video = media::encode_preview(&frames, budget, &Job::default()).unwrap();
    assert!(video.len() <= budget && video.len() > budget * 9 / 10);
    assert_eq!(u32::from_be_bytes(video[0..4].try_into().unwrap()), 21 * 4);
    assert_eq!(u32::from_be_bytes(video[80..84].try_into().unwrap()) as usize, video.len());
    let t = tempfile::tempdir().unwrap();
    let path = t.path().join("clip.3dv");
    let valid = preview_container(&video, 20);
    fs::write(&path, &valid).unwrap();
    media::validate(&path).unwrap();
    let reseal = |mut bad: Vec<u8>| {
        let check = media::word_sum(&bad[40..bad.len() - 8]);
        bad[16..20].copy_from_slice(&check.to_be_bytes());
        bad
    };
    let mut bads = vec![];
    let mut v3 = valid.clone();
    v3[7] = b'3';
    let end = v3.len() - 1;
    v3[end] = b'3';
    bads.push(v3); // Cinepak (v3) was never released
    for (pos, value) in [(20usize, 451u16), (22, 15), (8, 320), (10, 240)] {
        let mut bad = valid.clone();
        bad[pos..pos + 2].copy_from_slice(&value.to_be_bytes());
        bads.push(reseal(bad));
    }
    let mut bad = valid.clone();
    bad[44..48].copy_from_slice(&0u32.to_be_bytes()); // frame table
    bads.push(reseal(bad));
    let mut bad = valid.clone();
    bad[50] ^= 1; // checksum
    bads.push(bad);
    if video.len() % 4 != 0 {
        let mut bad = valid.clone();
        let end = bad.len() - 9;
        bad[end] = 1; // padding after the packets must be zero
        bads.push(reseal(bad));
    }
    // The same vector is checked in tests/test_preview_tools.py.
    assert_eq!(media::word_sum(&[0, 0, 0, 1, 0, 0, 0, 2, 0x80, 0, 0, 0]), 0x8007_8003);
    for bad in bads {
        fs::write(&path, bad).unwrap();
        assert!(media::validate(&path).is_err());
    }
    // A budget below even the smallest encoding is reported, not overrun.
    assert!(media::encode_preview(&frames, 21 * 4, &Job::default()).is_err());
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
    assert_eq!(p.files.len(), 6);
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
fn usb_copy_places_media_beside_each_disc() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    let dest = t.path().join("USB");
    fs::create_dir(&root).unwrap();
    fs::create_dir(&dest).unwrap();
    put(&root, "boot.iso", &boot());
    put(&root, "Games/USA/Road Rash/Road Rash.iso", b"fictional");
    put(&root, "Top.iso", b"top level");
    put(
        &root,
        ".ohmy3do/ohmy3do.ini",
        include_bytes!("../../ohmy3do.ini"),
    );
    let cover = root.join(".ohmy3do/covers/rr.png");
    fs::create_dir_all(cover.parent().unwrap()).unwrap();
    image::RgbImage::from_pixel(96, 120, image::Rgb([30, 80, 180]))
        .save(&cover)
        .unwrap();
    put(&root, ".ohmy3do/previews/clip.3dv", &synthetic_preview());
    let cache = t.path().join("cache");
    let mut tx = library::Journal::new(&root, &cache).unwrap();
    library::build_indexes(&root, &mut tx, &Job::default()).unwrap();
    tx.finish().unwrap();
    let rr = "games/usa/road rash/road rash.iso".to_string();
    let covers = BTreeMap::from([(rr.clone(), "rr.png".to_string())]);
    let previews = BTreeMap::from([
        (rr, "clip.3dv".to_string()),
        ("top.iso".to_string(), "clip.3dv".to_string()),
    ]);
    let cover_index = library::media_index(&covers).unwrap();
    put(&root, ".ohmy3do/covers/index.3di", &cover_index);
    put(
        &root,
        ".ohmy3do/previews/index.3di",
        &library::media_index(&previews).unwrap(),
    );
    let before = snapshot(&dest);
    let plan = usb::plan(&root, &dest, &cache, false, &Job::default()).unwrap();
    let journal = usb::apply(&plan, &cache, &Job::default()).unwrap();
    // Each disc's media sits in .ohmy3do beside it; nothing under the root media folders.
    assert_eq!(
        fs::read(dest.join("Games/USA/Road Rash/.ohmy3do/rr.png")).unwrap(),
        fs::read(&cover).unwrap()
    );
    assert_eq!(
        fs::read(dest.join("Games/USA/Road Rash/.ohmy3do/clip.3dv")).unwrap(),
        synthetic_preview()
    );
    assert_eq!(
        fs::read(dest.join(".ohmy3do/clip.3dv")).unwrap(),
        synthetic_preview()
    );
    assert!(!dest.join(".ohmy3do/covers/rr.png").exists());
    assert!(!dest.join(".ohmy3do/previews/clip.3dv").exists());
    // The drive's index is the staging index with the beside flag set.
    let copied = fs::read(dest.join(".ohmy3do/covers/index.3di")).unwrap();
    assert_eq!(copied[20..24], 1u32.to_be_bytes());
    assert_eq!(copied[..20], cover_index[..20]);
    assert_eq!(copied[24..], cover_index[24..]);
    // A second preparation finds nothing to copy.
    let again = usb::plan(&root, &dest, &cache, false, &Job::default()).unwrap();
    assert!(again
        .files
        .iter()
        .all(|f| ["unchanged", "preserve"].contains(&f.status.as_str())));
    // Library edits refuse a prepared drive rather than misreading its media.
    assert!(library::read_media_index(&dest, "covers").is_err());
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
/// A one-frame OM3DVID5 clip: black, as SOLID runs over all 1200 blocks.
fn synthetic_preview() -> Vec<u8> {
    let mut packet = vec![0xbf, 0x00, 0x00]; // 64 blocks, literal colour 0
    for _ in 0..17 {
        packet.extend_from_slice(&[0xbf, 0x80]); // 64 blocks, cached colour 0
    }
    packet.extend_from_slice(&[0xaf, 0x80]); // the last 48 blocks
    synthetic_clip(b"OM3DVID5", (160, 120, 30), packet, b"END3DV05")
}
/// The same black frame as menu 0.16's OM3DVID4: 1200 blocks at 160x120.
fn synthetic_preview_v4() -> Vec<u8> {
    let mut packet = vec![0x5f, 0x00, 0x00];
    for _ in 0..36 {
        packet.extend_from_slice(&[0x5f, 0x80]);
    }
    packet.extend_from_slice(&[0x4f, 0x80]);
    synthetic_clip(b"OM3DVID4", (160, 120, 15), packet, b"END3DV04")
}
fn synthetic_clip(magic: &[u8; 8], (width, height, fps): (u16, u16, u16), packet: Vec<u8>, end: &[u8; 8]) -> Vec<u8> {
    let video = 8 + packet.len();
    let mut payload = 8u32.to_be_bytes().to_vec();
    payload.extend_from_slice(&(video as u32).to_be_bytes());
    payload.extend(packet);
    payload.resize(video.next_multiple_of(4), 0);
    let mut data = magic.to_vec();
    data.extend_from_slice(&width.to_be_bytes());
    data.extend_from_slice(&height.to_be_bytes());
    for n in [payload.len() as u32, media::word_sum(&payload)] {
        data.extend_from_slice(&n.to_be_bytes());
    }
    data.extend_from_slice(&1u16.to_be_bytes());
    data.extend_from_slice(&fps.to_be_bytes());
    for n in [video as u32, 0, 0, 0] {
        data.extend_from_slice(&n.to_be_bytes());
    }
    data.extend(payload);
    data.extend_from_slice(end);
    data
}
#[test]
fn corrupt_preview_checksum_or_frame_table_is_rejected() {
    let mut data = synthetic_preview();
    let t = tempfile::tempdir().unwrap();
    let path = put(t.path(), "clip.3dv", &data);
    media::validate(&path).unwrap();
    data[48] ^= 1;
    fs::write(&path, &data).unwrap();
    assert!(media::validate(&path).is_err());
    data[48] ^= 1;
    data[47] ^= 1; // the frame table's end offset, resealed
    let sum = media::word_sum(&data[40..data.len() - 8]);
    data[16..20].copy_from_slice(&sum.to_be_bytes());
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
        Sha256::digest(format!("{hash}:preview:om3dvid5-160x120-30fps-cap15-1400k").as_bytes())
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

#[test]
fn preview_validation_checks_dimensions_and_rate() {
    let t = tempfile::tempdir().unwrap();
    let path = put(t.path(), "preview.3dv", &synthetic_preview());
    media::validate(&path).unwrap();
    // Menu 0.16 clips (version 4, 15 fps) need reconverting.
    fs::write(&path, synthetic_preview_v4()).unwrap();
    assert!(media::validate(&path).is_err());
    for (width, height) in [(0u16, 120u16), (64, 80), (161, 120), (160, 121), (320, 240), (65535, 65535)] {
        let mut data = synthetic_preview();
        data[8..10].copy_from_slice(&width.to_be_bytes());
        data[10..12].copy_from_slice(&height.to_be_bytes());
        fs::write(&path, data).unwrap();
        assert!(media::validate(&path).is_err());
    }
    let mut slow = synthetic_preview();
    slow[22..24].copy_from_slice(&15u16.to_be_bytes());
    fs::write(&path, slow).unwrap();
    assert!(media::validate(&path).is_err());
}
#[test]
fn rebuild_media_preserves_original_associations_unavailable_sources_and_undo() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    fs::create_dir(&root).unwrap();
    put(&root, "Game.iso", b"fictional");
    put(
        &root,
        ".ohmy3do/ohmy3do.ini",
        include_bytes!("../../ohmy3do.ini"),
    );
    let source = t.path().join("original.jpg");
    image::RgbImage::from_pixel(200, 300, image::Rgb([30, 80, 180]))
        .save(&source)
        .unwrap();
    let old = root.join(".ohmy3do/covers/old.png");
    fs::create_dir_all(old.parent().unwrap()).unwrap();
    image::RgbImage::from_pixel(96, 120, image::Rgb([30, 80, 180]))
        .save(&old)
        .unwrap();
    put(&root, ".ohmy3do/previews/old.3dv", &synthetic_preview());
    let record = json!({"path":"Game.iso","title":"Example (USA)","region":"USA","category":"Games","cover":"old.png","cover_source":source,"preview":"old.3dv","preview_source":t.path().join("unavailable.mp4"),"metadata":{"publisher":"Café"},"registration":{"sha1":"preserve"}});
    put(
        &root,
        ".ohmy3do/library.json",
        &serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[record]})).unwrap(),
    );
    let before = snapshot(&root);
    let journal = library::rebuild_media(
        &root,
        &Preferences::default(),
        &t.path().join("cache"),
        &Job::default(),
    )
    .unwrap();
    let db = library::database(&root).unwrap();
    let updated = &db["games"][0];
    assert_ne!(updated["cover"], record["cover"]);
    for key in [
        "cover_source",
        "preview",
        "preview_source",
        "metadata",
        "registration",
        "path",
        "region",
    ] {
        assert_eq!(updated[key], record[key]);
    }
    let cover = root
        .join(".ohmy3do/covers")
        .join(updated["cover"].as_str().unwrap());
    let image = media::cover_display(&cover).unwrap();
    assert_eq!((image.width(), image.height()), (96, 120));
    assert!(old.is_file());
    library::undo(&journal).unwrap();
    assert_eq!(snapshot(&root), before);
}

/// A minimal 3DO disc: header, one root directory block, one block per file.
fn opera_image(programs: &[&[u8]]) -> Vec<u8> {
    let mut blocks = vec![vec![0u8; 2048]; 2 + programs.len()];
    blocks[0][..7].copy_from_slice(b"\x01ZZZZZ\x01");
    blocks[0][88..92].copy_from_slice(&1u32.to_be_bytes());
    blocks[0][100..104].copy_from_slice(&1u32.to_be_bytes());
    let mut pos = 20;
    for (i, data) in programs.iter().enumerate() {
        let flags = 2 | if i + 1 == programs.len() { 0xc000_0000u32 } else { 0 };
        let entry = &mut blocks[1][pos..pos + 72];
        entry[..4].copy_from_slice(&flags.to_be_bytes());
        entry[12..16].copy_from_slice(&2048u32.to_be_bytes());
        entry[16..20].copy_from_slice(&(data.len() as u32).to_be_bytes());
        entry[20..24].copy_from_slice(&1u32.to_be_bytes());
        entry[68..72].copy_from_slice(&(2 + i as u32).to_be_bytes());
        blocks[2 + i][..data.len()].copy_from_slice(data);
        pos += 72;
    }
    blocks[1][..4].copy_from_slice(&(-1i32).to_be_bytes());
    blocks[1][12..16].copy_from_slice(&(pos as u32).to_be_bytes());
    blocks[1][16..20].copy_from_slice(&20u32.to_be_bytes());
    blocks.concat()
}

#[test]
fn save_names_come_from_disc_programs_and_are_cached() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let mut program = vec![b'x'; 64];
    program.extend_from_slice(b"/NVRAM/EA_RRX\0/NVRAM/SLOT%02d\0/NVRAM/%s\0/NVRAM/X\0/NVRAM/bad|name\0");
    let game = put(root, "Games/USA/Road Rash (USA)/Road Rash (USA).iso", &opera_image(&[&[b'n'; 80], &program]));
    let mut other = vec![b'z'; 64];
    other.extend_from_slice(b"/NVRAM/EA_RRX\0");
    put(root, "Other.iso", &opera_image(&[&other]));
    assert_eq!(oh_my_3do_library::saves::disc_save_names(&game).unwrap(), ["*SLOT", "=EA_RRX"]);
    let discs = vec!["Games/USA/Road Rash (USA)/Road Rash (USA).iso".to_string(), "Other.iso".to_string()];
    let titles = [(f::fold(&discs[0]), "Road Rash (USA)".to_string())].into_iter().collect();
    let out = oh_my_3do_library::saves::save_names_text(root, &discs, &titles, &Job::default()).unwrap();
    assert_eq!(out.count, 2);
    assert_eq!(out.text, "# Save names read from the discs by index-media.py\n=EA_RRX\tRoad Rash / Other\n*SLOT\tRoad Rash\n");
    put(root, ".ohmy3do/save-names-cache.json", &out.cache);
    // A cached disc is not read again, even when its bytes no longer parse.
    let stamp = fs::metadata(&game).unwrap().modified().unwrap();
    fs::write(&game, vec![0u8; fs::metadata(&game).unwrap().len() as usize]).unwrap();
    filetime::set_file_mtime(&game, filetime::FileTime::from_system_time(stamp)).unwrap();
    let again = oh_my_3do_library::saves::save_names_text(root, &discs, &titles, &Job::default()).unwrap();
    assert_eq!(again.text, out.text);
    assert_eq!(oh_my_3do_library::saves::short_title("Daedalus Encounter, The (Japan) (Disc 2).iso"), "Daedalus Encounter, The");
}

#[test]
fn recategorize_moves_game_folders_with_every_reference_then_undoes() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    let cache = t.path().join("cache");
    let old = "Games/Japan/Virtual Cameraman Part 1 (Japan)";
    let disc = format!("{old}/Virtual Cameraman Part 1 (Japan).iso");
    put(&root, &disc, b"fictional disc");
    put(&root, &format!("{old}/.ohmy3do/notes.txt"), b"beside media");
    put(&root, "Games/USA/Road Rash (USA)/Road Rash (USA).iso", b"other disc");
    put(
        &root,
        ".ohmy3do/library.json",
        &serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[
            {"path": disc, "title": "Virtual Cameraman Part 1 (Japan)", "category": "Games", "metadata": {}},
            {"path": "Games/USA/Road Rash (USA)/Road Rash (USA).iso", "title": "Road Rash (USA)", "category": "Games", "metadata": {}}
        ]}))
        .unwrap(),
    );
    let covers = [(disc.clone(), "vc1.png".to_string())].into_iter().collect();
    put(&root, ".ohmy3do/covers/index.3di", &library::media_index(&covers).unwrap());
    put(&root, ".ohmy3do/favorites.txt", format!("# mine\n{disc}\nGames/USA/Road Rash (USA)/Road Rash (USA).iso\n\0stale").as_bytes());
    put(&root, ".ohmy3do/recent.txt", format!("{disc}\n").as_bytes());
    put(&root, ".ohmy3do/last-position.txt", format!("OH3DO_POSITION1\n/Storage/{old}\nVirtual Cameraman Part 1 (Japan).iso\n").as_bytes());
    let before = snapshot(&root);

    let journal = library::recategorize(&root, &[disc.clone()], "adult", &cache, &Job::default()).unwrap();
    let moved = format!("Adult/Japan/Virtual Cameraman Part 1 (Japan)/Virtual Cameraman Part 1 (Japan).iso");
    assert!(root.join(&moved).is_file() && !root.join(old).exists());
    assert!(root.join("Adult/Japan/Virtual Cameraman Part 1 (Japan)/.ohmy3do/notes.txt").is_file());
    let db = library::database(&root).unwrap();
    let record = db["games"].as_array().unwrap().iter().find(|g| g["path"] == moved.as_str()).unwrap();
    assert_eq!(record["category"], "Adult");
    assert_eq!(record["category_reason"], "Explicit category override");
    assert_eq!(library::read_media_index(&root, "covers").unwrap().get(&f::fold(&moved)).map(String::as_str), Some("vc1.png"));
    assert_eq!(
        fs::read_to_string(root.join(".ohmy3do/favorites.txt")).unwrap(),
        format!("# mine\n{moved}\nGames/USA/Road Rash (USA)/Road Rash (USA).iso\n")
    );
    assert_eq!(fs::read_to_string(root.join(".ohmy3do/recent.txt")).unwrap(), format!("{moved}\n"));
    assert_eq!(
        fs::read_to_string(root.join(".ohmy3do/last-position.txt")).unwrap(),
        "OH3DO_POSITION1\n/Storage/Adult/Japan/Virtual Cameraman Part 1 (Japan)\nVirtual Cameraman Part 1 (Japan).iso\n"
    );
    let find = fs::read(root.join(".ohmy3do/library.3di")).unwrap();
    assert!(find.windows(moved.len()).any(|w| w == moved.as_bytes()));
    assert!(library::recategorize(&root, &[moved.clone()], "Adult", &cache, &Job::default()).is_err());
    assert!(library::recategorize(&root, &["loose.iso".into()], "Adult", &cache, &Job::default()).is_err());

    library::undo(&journal).unwrap();
    assert_eq!(snapshot(&root), before);
}

#[test]
fn usb_moves_recategorized_games_instead_of_copying_them_again() {
    let t = tempfile::tempdir().unwrap();
    let root = t.path().join("library");
    let dest = t.path().join("USB");
    let cache = t.path().join("cache");
    fs::create_dir(&root).unwrap();
    fs::create_dir(&dest).unwrap();
    put(&root, "boot.iso", &boot());
    put(&root, ".ohmy3do/ohmy3do.ini", include_bytes!("../../ohmy3do.ini"));
    let new = "Adult/Japan/Virtual Vivid (Japan)/Virtual Vivid (Japan).iso";
    let old = "Games/Japan/Virtual Vivid (Japan)/Virtual Vivid (Japan).iso";
    put(&root, new, b"same disc bytes");
    put(&root, "Games/USA/Road Rash (USA)/Road Rash (USA).iso", b"road rash");
    let mut tx = library::Journal::new(&root, &cache).unwrap();
    library::build_indexes(&root, &mut tx, &Job::default()).unwrap();
    tx.finish().unwrap();
    // The drive was prepared before the move.
    let copied = put(&dest, old, b"same disc bytes");
    let stamp = fs::metadata(root.join(new)).unwrap().modified().unwrap();
    filetime::set_file_mtime(&copied, filetime::FileTime::from_system_time(stamp)).unwrap();
    put(&dest, "Games/USA/Road Rash (USA)/Road Rash (USA).iso", b"road rash");
    put(&dest, ".ohmy3do/ohmy3do.ini", b"folder_workaround=1\n");
    put(&dest, ".ohmy3do/favorites.txt", format!("{old}\nGames/USA/Road Rash (USA)/Road Rash (USA).iso\n").as_bytes());
    put(&dest, ".ohmy3do/last-position.txt", b"OH3DO_POSITION1\n/Storage/Games/Japan/Virtual Vivid (Japan)\nVirtual Vivid (Japan).iso\n");
    let before = snapshot(&dest);

    let plan = usb::plan(&root, &dest, &cache, false, &Job::default()).unwrap();
    assert_eq!(plan.moves, [("Games/Japan/Virtual Vivid (Japan)".to_string(), "Adult/Japan/Virtual Vivid (Japan)".to_string())]);
    let status = |path: &str| plan.files.iter().find(|f| f.path == path).unwrap().status.clone();
    assert_eq!(status(new), "unchanged");
    assert_eq!(status(".ohmy3do/favorites.txt"), "remap");
    assert_eq!(status(".ohmy3do/last-position.txt"), "remap");
    assert_eq!(status(".ohmy3do/ohmy3do.ini"), "preserve");
    assert!(plan.bytes < 1024 * 1024, "the disc is not copied again");

    let journal = usb::apply(&plan, &cache, &Job::default()).unwrap();
    assert_eq!(fs::read(dest.join(new)).unwrap(), b"same disc bytes");
    assert!(!dest.join(old).exists());
    assert_eq!(
        fs::read_to_string(dest.join(".ohmy3do/favorites.txt")).unwrap(),
        format!("{new}\nGames/USA/Road Rash (USA)/Road Rash (USA).iso\n")
    );
    assert_eq!(
        fs::read_to_string(dest.join(".ohmy3do/last-position.txt")).unwrap(),
        "OH3DO_POSITION1\n/Storage/Adult/Japan/Virtual Vivid (Japan)\nVirtual Vivid (Japan).iso\n"
    );
    library::undo(&journal).unwrap();
    assert_eq!(snapshot(&dest), before);
}

#[test]
fn usb_preserves_database_pairs_and_compiles_json_only_custom_sources() {
    use oh_my_3do_library::save_cheats::{Database,JSON_PATH,BINARY_PATH};
    for case in 0..5 {
        let t=tempfile::tempdir().unwrap();let root=t.path().join("library");let dest=t.path().join("USB");let cache=t.path().join("cache");fs::create_dir(&root).unwrap();fs::create_dir(&dest).unwrap();
        put(&root,"boot.iso",&boot());put(&root,"Games/example.iso",b"fictional");put(&root,".ohmy3do/ohmy3do.ini",include_bytes!("../../ohmy3do.ini"));
        let mut source=Database::bundled();source.profiles[0].presets[0].value=7;put(&root,JSON_PATH,&source.json().unwrap());if case!=4{put(&root,BINARY_PATH,&source.binary().unwrap());}
        let mut personal=Database::bundled();personal.profiles[0].presets[0].value=9;
        if case==0 || case==1 {put(&dest,JSON_PATH,&personal.json().unwrap());}
        if case==0 || case==2 {put(&dest,BINARY_PATH,&personal.binary().unwrap());}
        let before=snapshot(&dest);let mut tx=library::Journal::new(&root,&cache).unwrap();library::build_indexes(&root,&mut tx,&Job::default()).unwrap();tx.finish().unwrap();
        let plan=usb::plan(&root,&dest,&cache,false,&Job::default()).unwrap();let journal=usb::apply(&plan,&cache,&Job::default()).unwrap();
        let expected=if case<3{&personal}else{&source};assert_eq!(fs::read(dest.join(BINARY_PATH)).unwrap(),expected.binary().unwrap(),"case {case}");
        if case==2 {assert!(!dest.join(JSON_PATH).exists());}else{assert_eq!(fs::read(dest.join(JSON_PATH)).unwrap(),expected.json().unwrap());}
        library::undo(&journal).unwrap();assert_eq!(snapshot(&dest),before);
    }
}

#[test]
fn save_name_scanner_handles_lowercase_paths_and_brackets() {
    let tmp=tempfile::tempdir().unwrap();
    let mut program=vec![b'x';64];program.extend_from_slice(b"/nvram/icebreaker[levelchart]\0/NvRaM/Maps O'Death V1.0\0");
    let path=put(tmp.path(),"game.iso",&opera_image(&[&program]));
    assert_eq!(oh_my_3do_library::saves::disc_save_names(&path).unwrap(),["=Maps O'Death V1.0","=icebreaker[levelchart]"]);
}

fn usb_selection_fixture(root: &Path, cache: &Path) {
    fs::create_dir_all(root).unwrap();
    put(root,"boot.iso",&boot());
    put(root,".ohmy3do/ohmy3do.ini",include_bytes!("../../ohmy3do.ini"));
    let mut tx = library::Journal::new(root,cache).unwrap();
    library::build_indexes(root,&mut tx,&Job::default()).unwrap();
    tx.finish().unwrap();
}
#[test]
fn selected_usb_copy_keeps_multidisc_cue_tracks_media_and_undo() {
    let t = tempfile::tempdir().unwrap(); let root=t.path().join("library"); let dest=t.path().join("usb"); let cache=t.path().join("cache");
    fs::create_dir(&dest).unwrap();
    let first="Games/USA/Example/disc1.iso"; let second="Games/USA/Example/disc2.cue";
    put(&root,first,b"first disc");
    put(&root,second,b"FILE \"track.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n");
    put(&root,"Games/USA/Example/track.bin",&vec![0;2352]);
    put(&root,"Games/USA/Unselected/other.iso",b"do not copy");
    usb_selection_fixture(&root,&cache);
    put(&root,".ohmy3do/previews/yes.3dv",&synthetic_preview());
    // Unselected missing media must not prevent a small transfer.
    put(&root,".ohmy3do/previews/index.3di",&library::media_index(&BTreeMap::from([
        (f::fold(first),"yes.3dv".into()),(f::fold(second),"yes.3dv".into()),
        ("games/usa/unselected/other.iso".into(),"missing.3dv".into())])).unwrap());
    let (_,choices)=usb::choices(&root,&Job::default()).unwrap();
    assert_eq!(choices.len(),2); assert_eq!(choices[0].discs.len(),2);
    let before=snapshot(&dest);
    let plan=usb::plan_selected(&root,&dest,&cache,false,&[first.into()],&Job::default()).unwrap();
    assert_eq!(plan.games,2); assert!(!plan.files.iter().any(|p| p.path.contains("Unselected")));
    let journal=usb::apply(&plan,&cache,&Job::default()).unwrap();
    for name in [first,second,"Games/USA/Example/track.bin","Games/USA/Example/.ohmy3do/yes.3dv"] { assert!(dest.join(name).is_file(),"{name}"); }
    assert!(!dest.join("Games/USA/Unselected").exists());
    assert_eq!(usb::find(&dest.join(".ohmy3do/library.3di")).unwrap(),vec![first,second]);
    let meta=fs::read(dest.join(".ohmy3do/metadata.3di")).unwrap();
    assert_eq!(library::metadata_paths(&meta).unwrap(),vec![first,second]);
    let raw=fs::read(dest.join(".ohmy3do/previews/index.3di")).unwrap();
    assert_eq!(u32::from_be_bytes(raw[8..12].try_into().unwrap()),2);
    library::undo(&journal).unwrap(); assert_eq!(snapshot(&dest),before);
}
#[test]
fn selected_usb_copy_is_additive_and_preserves_existing_metadata_media_and_saves() {
    let t=tempfile::tempdir().unwrap(); let root=t.path().join("library");let dest=t.path().join("usb");let cache=t.path().join("cache");
    fs::create_dir(&dest).unwrap(); let first="Games/USA/First/first.iso";let second="Games/Japan/Second/second.iso";
    put(&root,first,b"one");put(&root,second,b"two");
    put(&root,".ohmy3do/library.json",&serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[{"path":first,"metadata":{"genre":"Adventure"}},{"path":second,"metadata":{"genre":"Puzzle"}}]})).unwrap());
    usb_selection_fixture(&root,&cache);
    put(&root,".ohmy3do/previews/one.3dv",&synthetic_preview());
    put(&root,".ohmy3do/previews/two.3dv",&synthetic_preview());
    put(&root,".ohmy3do/previews/index.3di",&library::media_index(&BTreeMap::from([(f::fold(first),"one.3dv".into()),(f::fold(second),"two.3dv".into())])).unwrap());
    let p=usb::plan_selected(&root,&dest,&cache,false,&[first.into()],&Job::default()).unwrap();usb::apply(&p,&cache,&Job::default()).unwrap();
    put(&dest,".ohmy3do/saves/personal.bin",b"keep");put(&dest,".ohmy3do/ohmy3do.ini",b"folder_workaround=1\n");
    // The retained title no longer even exists in the staging library.
    fs::remove_dir_all(root.join("Games/USA/First")).unwrap();
    put(&root,".ohmy3do/previews/index.3di",&library::media_index(&BTreeMap::from([(f::fold(second),"two.3dv".into())])).unwrap());
    usb_selection_fixture(&root,&cache);
    let before=snapshot(&dest);
    let p=usb::plan_selected(&root,&dest,&cache,false,&[second.into()],&Job::default()).unwrap();let journal=usb::apply(&p,&cache,&Job::default()).unwrap();
    assert_eq!(usb::find(&dest.join(".ohmy3do/library.3di")).unwrap(),vec![second,first]);
    let raw=fs::read(dest.join(".ohmy3do/previews/index.3di")).unwrap();assert_eq!(u32::from_be_bytes(raw[8..12].try_into().unwrap()),2);
    let meta=fs::read(dest.join(".ohmy3do/metadata.3di")).unwrap();assert!(meta.windows(9).any(|s|s==b"Adventure"));assert!(meta.windows(6).any(|s|s==b"Puzzle"));
    assert_eq!(fs::read(dest.join(".ohmy3do/saves/personal.bin")).unwrap(),b"keep");assert_eq!(fs::read(dest.join(".ohmy3do/ohmy3do.ini")).unwrap(),b"folder_workaround=1\n");
    library::undo(&journal).unwrap();assert_eq!(snapshot(&dest),before);
}
#[test]
fn selected_usb_copy_rejects_changed_indexes_and_supports_empty_selection() {
    let t=tempfile::tempdir().unwrap();let root=t.path().join("library");let dest=t.path().join("usb");let cache=t.path().join("cache");fs::create_dir(&dest).unwrap();
    put(&root,"Games/one.iso",b"one");usb_selection_fixture(&root,&cache);
    let p=usb::plan_selected(&root,&dest,&cache,false,&["Games/one.iso".into()],&Job::default()).unwrap();
    put(&dest,"Games/new.iso",b"new after preview");
    put(&dest,".ohmy3do/covers/index.3di",b"");put(&dest,".ohmy3do/previews/index.3di",b"");
    let before=snapshot(&dest);
    assert!(usb::apply(&p,&cache,&Job::default()).is_err());assert_eq!(snapshot(&dest),before);
    let p=usb::plan_selected(&root,&dest,&cache,false,&[],&Job::default()).unwrap();assert_eq!(p.games,0);
    usb::apply(&p,&cache,&Job::default()).unwrap();assert!(!dest.join("Games/one.iso").exists());
    assert_eq!(usb::find(&dest.join(".ohmy3do/library.3di")).unwrap(),vec!["Games/new.iso"]);
    assert!(usb::plan_selected(&root,&dest,&cache,false,&["missing.iso".into()],&Job::default()).is_err());
}
#[test]
fn selected_usb_moves_only_checked_titles_and_remaps_retained_media() {
    let t=tempfile::tempdir().unwrap();let root=t.path().join("library");let dest=t.path().join("usb");let cache=t.path().join("cache");fs::create_dir(&dest).unwrap();
    let old="Games/Japan/Moved/disc.iso";let new="Adult/Japan/Moved/disc.iso";let other_old="Games/Japan/Unchecked/disc.iso";let other_new="Adult/Japan/Unchecked/disc.iso";
    put(&root,new,b"same");put(&root,other_new,b"other");usb_selection_fixture(&root,&cache);
    put(&dest,old,b"same");put(&dest,other_old,b"other");put(&dest,"Games/Japan/Moved/.ohmy3do/keep.3dv",&synthetic_preview());
    put(&dest,".ohmy3do/previews/index.3di",&library::beside_index(&library::media_index(&BTreeMap::from([(f::fold(old),"keep.3dv".into())])).unwrap()).unwrap());
    put(&dest,".ohmy3do/favorites.txt",format!("{old}\n{other_old}\n").as_bytes());
    let before=snapshot(&dest);
    let p=usb::plan_selected(&root,&dest,&cache,false,&[new.into()],&Job::default()).unwrap();assert_eq!(p.moves.len(),1);
    let journal=usb::apply(&p,&cache,&Job::default()).unwrap();
    assert!(dest.join(new).is_file());assert!(dest.join(other_old).is_file());assert!(!dest.join(other_new).exists());
    let names=usb::find(&dest.join(".ohmy3do/library.3di")).unwrap();assert!(names.contains(&other_old.into()));assert!(names.contains(&new.into()));assert!(!names.contains(&old.into()));
    let raw=fs::read(dest.join(".ohmy3do/previews/index.3di")).unwrap();assert!(raw.windows(new.len()).any(|s|s==f::fold(new).as_bytes()));assert!(dest.join("Adult/Japan/Moved/.ohmy3do/keep.3dv").is_file());
    assert_eq!(fs::read_to_string(dest.join(".ohmy3do/favorites.txt")).unwrap(),format!("{new}\n{other_old}\n"));
    library::undo(&journal).unwrap();assert_eq!(snapshot(&dest),before);
}
#[test]
fn selected_usb_ignores_unchecked_disc_conflicts_but_blocks_checked_conflicts() {
    let t=tempfile::tempdir().unwrap();let root=t.path().join("library");let dest=t.path().join("usb");let cache=t.path().join("cache");fs::create_dir(&dest).unwrap();
    put(&root,"Games/one.iso",b"one");put(&root,"Games/two.iso",b"two");usb_selection_fixture(&root,&cache);put(&dest,"Games/two.iso",b"different disc");
    assert!(usb::plan_selected(&root,&dest,&cache,true,&["Games/two.iso".into()],&Job::default()).is_err());
    let p=usb::plan_selected(&root,&dest,&cache,true,&["Games/one.iso".into()],&Job::default()).unwrap();usb::apply(&p,&cache,&Job::default()).unwrap();assert_eq!(fs::read(dest.join("Games/two.iso")).unwrap(),b"different disc");
}
