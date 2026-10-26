//! Save archives (.3ds): the format round trip, damaged records, capacity,
//! the DOOM save editor, hex input, compatibility with the console's own
//! code, and safe overwrites with recovery copies.

use oh_my_3do_library::{
    save_archive::{Archive, Entry, MAX_BYTES},
    save_editor::DoomPrefs,
};
use std::fs;
fn doom() -> Entry {
    // Big-endian 32-bit Word, as built with -bi in the original doom.make.
    let mut words = vec![0x4c57u32, 2, 1, 15, 15, 3, 1, 2, 0];
    words.push(0x4c57 + 2 + 1 + 15 + 15 + 3 + 1 + 2 + 12345);
    Entry {
        name: b"DoomPrefs".to_vec(),
        file_type: 0x44415441,
        data: words.iter().flat_map(|w| w.to_be_bytes()).collect(),
    }
}
fn sample() -> Archive {
    Archive {
        entries: vec![
            doom(),
            Entry {
                name: vec![b'R', 0xe9],
                file_type: 0x12345678,
                data: vec![0, 128, 255, 42],
            },
        ],
    }
}
fn repair_crc(bytes: &mut [u8]) {
    let crc = crc32fast::hash(&bytes[24..]);
    bytes[16..20].copy_from_slice(&crc.to_be_bytes());
}
#[test]
fn archive_roundtrip_preserves_names_types_and_every_byte() {
    let expected = sample();
    let bytes = expected.encode().unwrap();
    assert_eq!(Archive::parse(&bytes).unwrap(), expected);
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("slot01.3ds");
    fs::write(&path, [bytes.clone(), vec![255; 20]].concat()).unwrap();
    assert_eq!(Archive::read(&path).unwrap(), expected);
    assert!(Archive::parse(&fs::read(&path).unwrap()).is_err());
    for end in 0..bytes.len() {
        assert!(Archive::parse(&bytes[..end]).is_err());
    }
    for offset in 0..bytes.len() {
        let mut bad = bytes.clone();
        bad[offset] ^= 1;
        assert!(
            Archive::parse(&bad).is_err(),
            "accepted changed byte {offset}"
        );
    }
}
#[test]
fn malformed_records_remain_invalid_even_with_repaired_outer_checksum() {
    let bytes = sample().encode().unwrap();
    for offset in [24, 24 + 10, 24 + 31, 24 + 36, 24 + 40, 24 + 44] {
        let mut bad = bytes.clone();
        bad[offset] = if offset == 24 { 0 } else { 255 };
        repair_crc(&mut bad);
        assert!(
            Archive::parse(&bad).is_err(),
            "accepted malformed record at {offset}"
        );
    }
    let mut duplicate = sample();
    duplicate.entries.push(duplicate.entries[0].clone());
    duplicate.entries[2].name = b"DOOMPREFS".to_vec();
    assert!(duplicate.encode().is_err());
    for name in [
        b"".as_slice(),
        b"..",
        b"/bad",
        b"bad\\name",
        b"bad:name",
        b"bad\0name",
        b"bad\x7fname",
        &[b'x'; 32],
    ] {
        let mut archive = sample();
        archive.entries[0].name = name.to_vec();
        assert!(archive.encode().is_err());
    }
}
#[test]
fn capacity_boundaries_and_new_copy_preserve_originals() {
    let mut archive = Archive {
        entries: vec![Entry {
            name: b"x".to_vec(),
            file_type: 0,
            data: vec![7; MAX_BYTES - 72],
        }],
    };
    assert_eq!(archive.encode().unwrap().len(), MAX_BYTES);
    archive.entries[0].data.push(0);
    assert!(archive.encode().is_err());
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("original.3ds");
    let expected = sample();
    expected.save_copy(&path).unwrap();
    assert!(Archive::default().save_copy(&path).is_err());
    assert_eq!(Archive::read(&path).unwrap(), expected);
    let max = Archive {
        entries: (0..128)
            .map(|i| Entry {
                name: format!("save{i}").into_bytes(),
                file_type: 0,
                data: vec![],
            })
            .collect(),
    };
    assert!(max.encode().is_ok());
    let mut too_many = max;
    too_many.entries.push(doom());
    assert!(too_many.encode().is_err());
}
#[test]
fn doom_editor_uses_original_layout_ranges_and_internal_checksum() {
    let mut entry = doom();
    let mut prefs = DoomPrefs::parse(&entry).unwrap();
    prefs.values[5] = 23;
    prefs.values[0] = 4;
    prefs.apply(&mut entry).unwrap();
    assert_eq!(DoomPrefs::parse(&entry).unwrap(), prefs);
    assert_eq!(&entry.data[24..28], &23u32.to_be_bytes());
    let checksum = u32::from_be_bytes(entry.data[36..40].try_into().unwrap());
    assert_eq!(checksum, 0x4c57 + 4 + 1 + 15 + 15 + 3 + 23 + 2 + 12345);
    let mut invalid = prefs.clone();
    invalid.values[5] = 26;
    let before = entry.clone();
    assert!(invalid.apply(&mut entry).is_err());
    assert_eq!(entry, before);
    entry.data[0] ^= 1;
    assert!(DoomPrefs::parse(&entry).is_err());
    let mut wrong = doom();
    wrong.name = b"OtherSave".to_vec();
    assert!(DoomPrefs::parse(&wrong).is_err());
    let mut little = doom();
    for bytes in little.data.chunks_exact_mut(4) {
        bytes.reverse();
    }
    assert!(DoomPrefs::parse(&little).is_err());
    let encoded = Archive {
        entries: vec![before],
    }
    .encode()
    .unwrap();
    assert!(DoomPrefs::parse(&Archive::parse(&encoded).unwrap().entries[0]).is_ok());
}
#[test]
fn hex_input_rejects_partial_non_ascii_and_oversized_values() {
    use oh_my_3do_library::save_archive::decode_hex;
    assert_eq!(decode_hex("00 ff\n12\tAB").unwrap(), [0, 255, 18, 171]);
    for input in ["F", "éé", "gg", "0x12"] {
        assert!(decode_hex(input).is_err());
    }
    assert!(decode_hex(&"AA".repeat(MAX_BYTES + 1)).is_err());
}

#[cfg(target_os = "linux")]
#[test]
fn desktop_and_actual_console_archive_code_accept_each_others_output() {
    use std::{path::Path, process::Command};
    let dir = tempfile::tempdir().unwrap();
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let source = dir.path().join("interop.c");
    let exe = dir.path().join("interop");
    fs::write(
        &source,
        r#"
#include "save_archive.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    unsigned char bytes[SAVE_MAX_BYTES];
    SaveArchive parsed={0}, rebuilt={0};
    FILE *f; size_t n;
    if (argc!=3 || !(f=fopen(argv[1],"rb"))) return 1;
    n=fread(bytes,1,sizeof(bytes),f);fclose(f);
    if (save_archive_parse(&parsed,bytes,n) || parsed.count!=2 ||
        parsed.entries[0].type!=0x44415441UL || parsed.entries[0].size!=40 ||
        save_word(parsed.entries[0].data)!=0x4c57UL ||
        save_archive_build(&rebuilt,parsed.entries,parsed.count)) return 2;
    f=fopen(argv[2],"wb");if (!f) return 3;
    if (fwrite(rebuilt.bytes,1,rebuilt.length,f)!=rebuilt.length) return 4;
    fclose(f);save_archive_free(&parsed);save_archive_free(&rebuilt);return 0;
}
"#,
    )
    .unwrap();
    assert!(Command::new("cc")
        .arg("-ffunction-sections")
        .arg("-fdata-sections")
        .arg("-Wl,--gc-sections")
        .arg("-Isrc")
        .arg(&source)
        .args([
            "src/save_archive.c",
            "src/usb_art.c",
            "src/menu_ui.c",
            "src/menu_categories.c",
            "src/cover_catalog.c"
        ])
        .arg("-o")
        .arg(&exe)
        .current_dir(repo)
        .status()
        .unwrap()
        .success());
    let input = dir.path().join("input.3ds");
    let output = dir.path().join("output.3ds");
    sample().save_copy(&input).unwrap();
    assert!(Command::new(exe)
        .arg(&input)
        .arg(&output)
        .status()
        .unwrap()
        .success());
    assert_eq!(fs::read(&input).unwrap(), fs::read(&output).unwrap());
    assert_eq!(Archive::read(&output).unwrap(), sample());
}

#[test]
fn overwrite_validates_edits_and_keeps_exact_recovery_copies() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("slot01.3ds");
    let mut original = sample().encode().unwrap();
    original.extend_from_slice(b"old firmware tail");
    fs::write(&path, &original).unwrap();
    let mut edited = sample();
    let mut prefs = DoomPrefs::parse(&edited.entries[0]).unwrap();
    prefs.values[5] = 23;
    prefs.apply(&mut edited.entries[0]).unwrap();
    let first = edited.save(&path, Some(&original)).unwrap();
    assert_eq!(Archive::read(&path).unwrap(), edited);
    assert_eq!(fs::read(first.backup.as_ref().unwrap()).unwrap(), original);
    let prior = first.bytes;
    edited.entries[1].data[0] = 42;
    let second = edited.save(&path, Some(&prior)).unwrap();
    assert_ne!(first.backup, second.backup);
    assert_eq!(fs::read(second.backup.unwrap()).unwrap(), prior);
    assert_eq!(Archive::read(&path).unwrap(), edited);
    assert!(DoomPrefs::parse(&Archive::read(&path).unwrap().entries[0]).is_ok());
}
#[test]
fn stale_or_invalid_saves_leave_the_file_and_existing_backups_untouched() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("slot01.3ds");
    sample().save_copy(&path).unwrap();
    let original = fs::read(&path).unwrap();
    fs::write(&path, b"outside edit").unwrap();
    assert!(sample().save(&path, Some(&original)).is_err());
    assert_eq!(fs::read(&path).unwrap(), b"outside edit");
    assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 1);
    let mut invalid = sample();
    invalid.entries[0].name = b"../bad".to_vec();
    assert!(invalid.save(&path, Some(b"outside edit")).is_err());
    assert_eq!(fs::read(&path).unwrap(), b"outside edit");
    assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 1);
    assert!(sample().save(&path, None).is_err());
}

#[test]
fn shared_cheat_database_validates_layouts_and_preserves_rejected_edits() {
    use oh_my_3do_library::save_cheats;
    let mut entry = doom();
    let p = save_cheats::find(&entry).unwrap();
    assert_eq!(p.fields.len(), 8);
    assert_eq!(p.fields[5].offset, 24);
    assert!(p
        .source
        .contains("8a5cdae476e09f2bedd2cc4dcd105dae4b35d4af"));
    p.apply_preset(&mut entry, 0).unwrap();
    assert_eq!(DoomPrefs::parse(&entry).unwrap().values[5], 23);
    assert!(p.matches(&entry));
    for (i, field) in p.fields.iter().enumerate() {
        p.set(&mut entry, i, field.min).unwrap();
        p.set(&mut entry, i, field.max).unwrap();
        let before = entry.clone();
        assert!(p.set(&mut entry, i, field.max + 1).is_err());
        assert_eq!(entry, before);
    }
    let before = entry.clone();
    assert!(p.apply_preset(&mut entry, 100).is_err());
    assert_eq!(entry, before);
    entry.data[39] ^= 1;
    let corrupt = entry.clone();
    assert!(save_cheats::find(&entry).is_none());
    assert!(p.apply_preset(&mut entry, 0).is_err());
    assert_eq!(entry, corrupt);
}
