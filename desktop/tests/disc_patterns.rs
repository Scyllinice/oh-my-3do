//! Finding discs: cached name patterns, CUE sheet inventories, and
//! collecting discs once across wrappers and multi-track sets.

use oh_my_3do_library::disc;
use std::fs;

#[test]
fn cached_patterns_keep_distinct_patterns_and_concurrent_matches() {
    let workers: Vec<_> = (0..8)
        .map(|_| {
            std::thread::spawn(|| {
                for _ in 0..100 {
                    for (pattern, text, expected) in [
                        (r"(?i)^disc \d+$", "Disc 12", true),
                        (r"^track$", "Disc 12", false),
                        (r"^track$", "track", true),
                    ] {
                        let regex = disc::rx(pattern);
                        assert_eq!(regex.as_str(), pattern);
                        assert_eq!(regex.is_match(text), expected);
                    }
                }
            })
        })
        .collect();
    for worker in workers {
        worker.join().unwrap();
    }
}

#[test]
fn cue_inventory_preserves_order_duplicates_and_fresh_lookups() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    for name in ["Track 01.bin", "Track 02.bin", "café.bin"] {
        fs::write(root.join(name), b"track").unwrap();
    }
    let cue = root.join("Game.cue");
    fs::write(&cue, "FILE \"TRACK 02.BIN\" BINARY\nFILE \"Track 01.bin\" BINARY\nFILE \"track 02.bin\" BINARY\nFILE \"café.bin\" BINARY\n").unwrap();
    let expected: Vec<_> = ["Track 02.bin", "Track 01.bin", "café.bin"]
        .into_iter()
        .map(|name| root.join(name).canonicalize().unwrap())
        .collect();
    assert_eq!(disc::cue_refs(&cue).unwrap().1, expected);
    fs::remove_file(root.join("Track 01.bin")).unwrap();
    assert!(disc::cue_refs(&cue).is_err());
    fs::write(root.join("Track 01.bin"), b"restored").unwrap();
    assert_eq!(disc::cue_refs(&cue).unwrap().1, expected);
    fs::write(&cue, "FILE \"../outside.bin\" BINARY\n").unwrap();
    assert!(disc::cue_refs(&cue).is_err());
    // The drive's firmware reads every track as raw data, whatever its type
    // word or file name, so dumps with .img tracks or WAVE/MOTOROLA audio boot.
    for name in ["Data.img", "Audio.wav", "Audio.raw"] {
        fs::write(root.join(name), b"track").unwrap();
    }
    fs::write(&cue, "FILE \"Data.img\" BINARY\n  TRACK 01 MODE1/2352\nFILE \"Audio.wav\" WAVE\n  TRACK 02 AUDIO\nFILE Audio.raw MOTOROLA\n  TRACK 03 AUDIO\n").unwrap();
    let expected: Vec<_> = ["Data.img", "Audio.wav", "Audio.raw"]
        .into_iter()
        .map(|name| root.join(name).canonicalize().unwrap())
        .collect();
    assert_eq!(disc::cue_refs(&cue).unwrap().1, expected);
    // Still rejected: a CUE naming another CUE, and a FILE line without a type.
    fs::write(&cue, "FILE \"Game.cue\" BINARY\n").unwrap();
    assert!(disc::cue_refs(&cue).is_err());
    fs::write(&cue, "FILE \"Data.img\"\n").unwrap();
    assert!(disc::cue_refs(&cue).is_err());
}

#[cfg(unix)]
#[test]
fn cue_inventory_rejects_collisions_and_links() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    let cue = root.join("Game.cue");
    fs::write(&cue, "FILE \"track.bin\" BINARY\n").unwrap();
    fs::write(root.join("Track.bin"), b"one").unwrap();
    fs::write(root.join("track.bin"), b"two").unwrap();
    assert!(disc::cue_refs(&cue).is_err());
    fs::remove_file(root.join("track.bin")).unwrap();
    assert!(disc::cue_refs(&cue).is_ok());
    fs::remove_file(root.join("Track.bin")).unwrap();
    let outside = tempfile::NamedTempFile::new().unwrap();
    std::os::unix::fs::symlink(outside.path(), root.join("track.bin")).unwrap();
    assert!(disc::cue_refs(&cue).is_err());
}

#[test]
fn collecting_once_preserves_wrappers_offsets_and_multitrack_discs() {
    use oh_my_3do_library::Job;
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path();
    for name in ["single.iso", "offset.iso", "data.bin", "audio.bin"] {
        fs::write(root.join(name), b"disc data").unwrap();
    }
    let wrapper = root.join("wrapper.cue");
    fs::write(
        &wrapper,
        "FILE \"single.iso\" BINARY\n TRACK 01 MODE1/2048\n INDEX 01 00:00:00\n",
    )
    .unwrap();
    fs::write(
        root.join("offset.cue"),
        "FILE \"offset.iso\" BINARY\n TRACK 01 MODE1/2048\n INDEX 01 00:00:01\n",
    )
    .unwrap();
    fs::write(root.join("multi.cue"),"FILE \"data.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\nFILE \"audio.bin\" BINARY\n TRACK 02 AUDIO\n INDEX 01 00:00:00\n").unwrap();
    let single = root.join("single.iso").canonicalize().unwrap();
    assert_eq!(
        disc::collect(&[wrapper], None, &Job::default()).unwrap(),
        vec![(single.clone(), vec![single.clone()])]
    );
    let sets = disc::collect(&[root.to_owned(), single.clone()], None, &Job::default()).unwrap();
    assert_eq!(sets.len(), 3);
    assert_eq!(sets.iter().map(|(_, files)| files.len()).sum::<usize>(), 6);
    assert!(sets
        .iter()
        .any(|(path, files)| path == &single && files == &vec![single.clone()]));
    assert!(sets.iter().any(|(path, files)| path.ends_with("offset.cue")
        && files.len() == 2
        && files[1].ends_with("offset.iso")));
    assert!(sets.iter().any(|(path, files)| path.ends_with("multi.cue")
        && files.len() == 3
        && files[1].ends_with("data.bin")
        && files[2].ends_with("audio.bin")));
    fs::write(
        root.join("conflict.cue"),
        "FILE \"data.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n",
    )
    .unwrap();
    assert!(disc::collect(&[root.to_owned()], None, &Job::default())
        .unwrap_err()
        .to_string()
        .contains("share data files"));
    fs::remove_file(root.join("conflict.cue")).unwrap();
    fs::remove_file(root.join("audio.bin")).unwrap();
    assert!(disc::collect(&[root.to_owned()], None, &Job::default()).is_err());
}
