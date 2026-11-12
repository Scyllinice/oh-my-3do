//! Disc images: recognizing them, reading their names, and checking them.
//!
//! - Names: turning a file name such as "Road Rash (USA) (Disc 2)" into a
//!   title key for matching, a disc number, regions, and a safe file name.
//! - Files: refusing archives renamed to `.iso`, reading CUE sheets, and
//!   finding which tracks each CUE uses.
//! - Collecting: finding every disc under a set of files and folders, with
//!   the files that make up each one ([`collect`]).
//! - Fingerprints: checksums of a disc image, including the checksum it
//!   would have as a raw 2352-byte-sector image, which is what the Redump
//!   database lists ([`fingerprint`], [`encode_sector`]).

use crate::{fsutil as f, model::*, Job};
use anyhow::{bail, ensure, Context, Result};
use regex::Regex;
use sha1::{Digest, Sha1};
use sha2::Sha256;
use std::{
    collections::{HashMap, HashSet},
    fs::{self, File},
    io::Read,
    path::{Path, PathBuf},
    sync::{OnceLock, RwLock},
};
use unicode_normalization::UnicodeNormalization;

// ---- Regular expressions ----

/// A compiled regular expression for the constant pattern `pattern`.
///
/// Each pattern is compiled once and kept. A clone shares the compiled
/// expression, but has its own working memory, so several jobs can match
/// at the same time.
pub fn rx(pattern: &'static str) -> Regex {
    static COMPILED: OnceLock<RwLock<HashMap<&'static str, Regex>>> = OnceLock::new();
    let compiled = COMPILED.get_or_init(|| RwLock::new(HashMap::new()));

    if let Some(regex) = compiled.read().expect("regex cache").get(pattern) {
        return regex.clone();
    }
    compiled
        .write()
        .expect("regex cache")
        .entry(pattern)
        .or_insert_with(|| Regex::new(pattern).expect("constant regex"))
        .clone()
}

// ---- Names ----

/// Anything in (parentheses) or [brackets], such as "(USA)" or "[!]".
const TAGS: &str = r"\([^)]*\)|\[[^]]*\]";

/// A disc number in a name, in one of two forms (group 1 or 2 holds the
/// number):
const DISC_NUMBER: &str = r"(?xi)
    # In parentheses: '(Disc 2)', '(CD 02 of 3)'.
    \( \s* (?:disc|disk|cd) \s* 0*(\d+) (?:\s+of\s+\d+)? \s* \)
    |
    # At the end, after a separator: ' - Disc 2', '_disk02', ' CD 2 of 3'.
    (?: \s*[-_.]\s* | \s+ ) (?:disc|disk|cd) [\s_.-]* 0*(\d+) (?:\s+of\s+\d+)? $
";

/// The same two forms as [`DISC_NUMBER`], with surrounding spaces, to cut
/// them out of a name.
const DISC_LABEL: &str = r"(?xi)
    \s* \( \s* (?:disc|disk|cd) \s* \d+ (?:\s+of\s+\d+)? \s* \)
    |
    (?: \s*[-_.]\s* | \s+ ) (?:disc|disk|cd) [\s_.-]* \d+ (?:\s+of\s+\d+)? $
";

/// A key for matching a game's title regardless of how it is written:
/// without tags such as "(USA)", without a leading or trailing "The", "A"
/// or "An", and with only lowercase letters and digits (accents removed).
/// "The Horde (USA)" and "Horde, The" both give "horde".
pub fn title_key(name: &str) -> String {
    let name = rx(TAGS).replace_all(name, "");
    let name = rx(r"(?i),\s*(the|a|an)$").replace(name.trim(), "");
    let name = rx(r"(?i)^(the|a|an)\s+").replace(&name, "");

    // Splitting accented letters (NFKD) and dropping the accent marks
    // turns "é" into "e".
    name.nfkd()
        .filter(|c| !unicode_normalization::char::is_combining_mark(*c))
        .flat_map(char::to_lowercase)
        .filter(|c| c.is_alphanumeric())
        .collect()
}

/// The disc number in a name such as "Game (Disc 2)" or "Game - CD2".
pub fn disc_number(name: &str) -> Option<u32> {
    let captures = rx(DISC_NUMBER).captures(name)?;
    let number = captures.get(1).or_else(|| captures.get(2))?;
    number.as_str().parse().ok()
}

/// A name with its disc number label removed: "Game (Disc 2)" gives "Game".
pub fn without_disc(name: &str) -> String {
    rx(DISC_LABEL).replace_all(name, "").trim().to_owned()
}

/// The standard name of a region written in a file name tag, such as "USA"
/// for "us" or "Europe" for "pal". None if the text is not a region.
pub fn region(text: &str) -> Option<&'static str> {
    match text.trim().to_ascii_lowercase().as_str() {
        "us" | "usa" | "na" => Some("USA"),
        "eu" | "europe" | "pal" | "uk" | "gb" => Some("Europe"),
        "jp" | "japan" | "j" => Some("Japan"),
        "korea" | "kr" => Some("Korea"),
        "germany" => Some("Germany"),
        "france" => Some("France"),
        "world" => Some("World"),
        "australia" => Some("Australia"),
        "canada" => Some("Canada"),
        _ => None,
    }
}

/// Every region named in a name's (parentheses), where several can be
/// listed with commas, slashes or dashes: "(USA, Europe)", "(Japan/Korea)".
pub fn regions(name: &str) -> HashSet<String> {
    let mut found = HashSet::new();
    for captures in rx(r"\(([^)]*)\)").captures_iter(name) {
        for part in captures[1].split([',', '/', '-']) {
            if let Some(region) = region(part) {
                found.insert(region.to_owned());
            }
        }
    }
    found
}

/// The first region named in a name: the first item of the first tag in
/// (parentheses) that starts with a region. "Unknown" if there is none.
pub fn primary_region(name: &str) -> String {
    rx(r"\(([^)]*)\)")
        .captures_iter(name)
        .find_map(|captures| region(captures[1].split(',').next().unwrap_or("")))
        .unwrap_or("Unknown")
        .to_owned()
}

/// A name that is safe as a file name on Windows, macOS, Linux and the
/// console's FAT32 drive, at most `max` bytes long.
///
/// Characters Windows forbids become "_", trailing spaces and dots are
/// removed, Windows' reserved device names (CON, COM1, ...) get a leading
/// "_", and an empty name becomes "Untitled". A name that is too long is cut
/// and given the start of its SHA-1, so two long names that start the same
/// stay different.
pub fn safe_name(name: &str, max: usize) -> String {
    let replaced: String = name
        .chars()
        .map(|c| {
            let forbidden = c < ' ' || "<>:\"/\\|?*".contains(c);
            if forbidden {
                '_'
            } else {
                c
            }
        })
        .collect();
    let mut safe = replaced.trim().trim_end_matches([' ', '.']).to_owned();

    if safe.is_empty() {
        safe = "Untitled".into();
    }
    if rx(r"(?i)^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)").is_match(&safe) {
        safe.insert(0, '_');
    }

    if safe.len() > max {
        // "-" and the first 8 hex digits of the SHA-1: 9 bytes.
        let hash = format!("-{:x}", Sha1::digest(safe.as_bytes()));
        let hash_suffix = &hash[..9];
        let mut end = max.saturating_sub(hash_suffix.len());
        while !safe.is_char_boundary(end) {
            end -= 1;
        }
        let start = safe[..end].trim_end_matches([' ', '.']);
        safe = format!("{start}{hash_suffix}");
    }
    safe
}

/// The longest file name stem used for a disc in an organized library.
const MAX_DISC_STEM: usize = 110;

/// The file name stem a disc gets in an organized library: its name, with
/// any trailing [tags] kept when there is room, and its disc number written
/// the standard way. "Game - CD2 [!]" gives "Game [!] (Disc 2)".
pub fn disc_stem(name: &str) -> String {
    let mut base = without_disc(name);
    let mut suffix = match disc_number(name) {
        Some(number) => format!(" (Disc {number})"),
        None => String::new(),
    };

    // Keep trailing [tags] such as "[!]" if they leave room for at least
    // 20 bytes of title.
    let trailing_tags = rx(r"\s*((?:\[[^\[\]]+\]\s*)+)$")
        .captures(&base)
        .and_then(|captures| captures.get(1))
        .map(|tags| (tags.start(), tags.as_str().trim().to_owned()));
    if let Some((start, tags)) = trailing_tags {
        if tags.len() + suffix.len() + 20 < MAX_DISC_STEM {
            suffix = format!(" {tags}{suffix}");
            base = base[..start].trim().to_owned();
        }
    }

    format!("{}{suffix}", safe_name(&base, MAX_DISC_STEM - suffix.len()))
}

// ---- Checking disc files ----

/// The first bytes of common archive formats: ZIP (three forms), 7-Zip,
/// RAR, gzip, bzip2 and xz.
const ARCHIVE_SIGNATURES: [&[u8]; 8] = [
    b"PK\x03\x04",
    b"PK\x05\x06",
    b"PK\x07\x08",
    b"7z\xbc\xaf\x27\x1c",
    b"Rar!\x1a\x07",
    b"\x1f\x8b",
    b"BZh",
    b"\xfd7zXZ\0",
];

/// Refuses a disc file that is empty, or that is really an archive renamed
/// to look like a disc.
pub fn issue(path: &Path) -> Result<()> {
    let mut start = [0; 16];
    let length = File::open(path)?.read(&mut start)?;
    if length == 0 {
        bail!("Empty disc: {}", path.display())
    }

    for signature in ARCHIVE_SIGNATURES {
        if start[..length].starts_with(signature) {
            bail!(
                "Archive disguised as disc data: {}. Extract it first.",
                path.display()
            )
        }
    }
    Ok(())
}

// ---- CUE sheets ----

/// The largest CUE sheet that will be read.
const MAX_CUE_BYTES: u64 = 256 * 1024;

/// A CUE sheet's text: UTF-8, or else Windows-1252 (what older tools
/// wrote), without a byte-order mark.
pub fn decode_cue(raw: &[u8]) -> Result<String> {
    let text = match std::str::from_utf8(raw) {
        Ok(text) => text.to_owned(),
        Err(_) => {
            let (text, had_errors) = encoding_rs::WINDOWS_1252.decode_without_bom_handling(raw);
            if had_errors {
                bail!("Unsupported CUE text encoding")
            }
            text.into_owned()
        }
    };
    Ok(text.trim_start_matches('\u{feff}').to_owned())
}

/// Reads a CUE sheet and finds the track files it names. Returns its text
/// and the tracks' full paths, in order, each listed once.
///
/// Every FILE line must name a file in the same folder, matched ignoring
/// letter case (as the console does). A missing name, or one that matches
/// two files differing only in case, is refused, as is a track that is
/// itself a CUE sheet.
pub fn cue_refs(path: &Path) -> Result<(String, Vec<PathBuf>)> {
    if fs::metadata(path)?.len() > MAX_CUE_BYTES {
        bail!("Oversized CUE")
    }
    let text = decode_cue(&fs::read(path)?)?;

    // Like the drive's firmware, accept any type word (BINARY, WAVE,
    // MOTOROLA) and any track file: it reads every track as raw sectors.
    // The file name is group 1 if quoted, or group 2 if not.
    let file_line = rx(r#"(?im)^\s*FILE\s+(?:"([^"\r\n]+)"|(\S+))\s+\S+\s*$"#);
    let folder = path.parent().context("CUE parent missing")?;

    let mut tracks = vec![];
    // The folder's files by lowercase name, read on the first FILE line.
    let mut folder_files: Option<HashMap<String, Vec<PathBuf>>> = None;

    for captures in file_line.captures_iter(&text) {
        let name = captures
            .get(1)
            .or_else(|| captures.get(2))
            .unwrap()
            .as_str();
        if name.contains(['/', '\\', ':']) || name == ".." {
            bail!("CUE FILE must name a sibling file")
        }

        if folder_files.is_none() {
            folder_files = Some(files_by_lowercase_name(folder)?);
        }
        let matches = folder_files
            .as_ref()
            .unwrap()
            .get(&name.to_ascii_lowercase())
            .map(Vec::as_slice)
            .unwrap_or(&[]);
        if matches.len() != 1 || !matches[0].is_file() || f::is_link(&matches[0]) {
            bail!("Missing or ambiguous CUE track: {name}")
        }

        let track = matches[0].canonicalize()?;
        let is_cue = track
            .extension()
            .unwrap_or_default()
            .eq_ignore_ascii_case("cue");
        if is_cue {
            bail!("A CUE track cannot be another CUE")
        }
        if !tracks.contains(&track) {
            tracks.push(track);
        }
    }

    // Every line starting with FILE must have matched the full pattern
    // above; one that did not (no type word, say) would be silently lost.
    let file_lines = rx(r"(?im)^\s*FILE\b").find_iter(&text).count();
    let understood_lines = file_line.captures_iter(&text).count();
    if tracks.is_empty() || file_lines != understood_lines {
        bail!("Unsupported CUE: each FILE line needs a file name and a type")
    }

    Ok((text, tracks))
}

/// Every entry in `folder`, grouped by lowercase name. Names that differ
/// only in case share a group, so an ambiguous name can be refused even on
/// a system where both files can exist.
fn files_by_lowercase_name(folder: &Path) -> Result<HashMap<String, Vec<PathBuf>>> {
    let mut names: HashMap<String, Vec<PathBuf>> = HashMap::new();
    for entry in fs::read_dir(folder)? {
        let entry = entry?;
        let lowercase = entry.file_name().to_string_lossy().to_ascii_lowercase();
        names.entry(lowercase).or_default().push(entry.path());
    }
    Ok(names)
}

/// If the CUE sheet at `path` only describes a plain ISO (one data track
/// and nothing else), the ISO's path. Such a CUE adds nothing, so the ISO
/// is used on its own. None for any other CUE, or one that cannot be read.
pub fn redundant_cue(path: &Path) -> Result<Option<PathBuf>> {
    let Ok((text, tracks)) = cue_refs(path) else {
        return Ok(None);
    };
    Ok(redundant_cue_from_refs(&text, &tracks))
}

/// [`redundant_cue`] for a CUE sheet already read with [`cue_refs`].
///
/// The CUE must name exactly one `.iso` and contain only a FILE ... BINARY
/// line, TRACK 1 MODE1/2048 and INDEX 1 00:00:00, in that order, apart from
/// comments (REM) and descriptive lines (CATALOG, TITLE, PERFORMER,
/// SONGWRITER).
fn redundant_cue_from_refs(text: &str, tracks: &[PathBuf]) -> Option<PathBuf> {
    let is_single_iso = tracks.len() == 1
        && tracks[0]
            .extension()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase()
            == "iso";
    if !is_single_iso {
        return None;
    }

    let mut directives = vec![];
    for line in text.lines() {
        let line = line.trim();
        let is_comment = rx(r"(?i)^REM(?:\s|$)").is_match(line);
        let is_description =
            rx(r#"(?i)^(?:CATALOG\s+\d{13}|(?:TITLE|PERFORMER|SONGWRITER)\s+"[^"\r\n]*")$"#)
                .is_match(line);
        if line.is_empty() || is_comment {
            continue;
        }

        if rx(r#"(?i)^FILE\s+"[^"\r\n]+"\s+BINARY$"#).is_match(line) {
            directives.push("file");
        } else if rx(r"(?i)^TRACK\s+0*1\s+MODE1/2048$").is_match(line) {
            directives.push("track");
        } else if rx(r"(?i)^INDEX\s+0*1\s+00:00:00$").is_match(line) {
            directives.push("index");
        } else if is_description {
            continue;
        } else {
            return None;
        }
    }

    (directives == ["file", "track", "index"]).then(|| tracks[0].clone())
}

// ---- Collecting discs ----

/// Folders never searched for games: hidden folders (such as `.ohmy3do`)
/// are skipped too.
const SKIPPED_FOLDERS: [&str; 2] = ["$boot", "system"];

/// Disc images at a drive's top level that are the menu, not games.
const MENU_IMAGES: [&str; 2] = ["boot.iso", "menu-demo.iso"];

/// A file's extension in lowercase ("" if it has none).
fn lowercase_extension(path: &Path) -> String {
    path.extension()
        .unwrap_or_default()
        .to_string_lossy()
        .to_ascii_lowercase()
}

/// Finds every disc in `inputs` (files, and folders searched all the way
/// down) and the files that make up each: a CUE sheet with its tracks, or
/// an ISO on its own. Returns them sorted by path.
///
/// When importing into a library, pass it as `library`: inputs inside it,
/// or folders containing it, are refused. A CUE that only wraps an ISO is
/// replaced by the ISO. Two discs sharing a data file (a CUE and the ISO
/// it points to, say) are refused; the person must pick one.
pub fn collect(
    inputs: &[PathBuf],
    library: Option<&Path>,
    job: &Job,
) -> Result<Vec<(PathBuf, Vec<PathBuf>)>> {
    let candidates = find_disc_files(inputs, library, job)?;
    let mut ordered: Vec<PathBuf> = candidates.into_iter().collect();
    ordered.sort();

    let mut sets = vec![];
    // Files already part of a set.
    let mut used = HashSet::new();

    // CUE sheets first, so the tracks they use are known.
    for path in &ordered {
        job.check()?;
        if lowercase_extension(path) != "cue" {
            continue;
        }

        let (text, tracks) = cue_refs(path)?;
        if let Some(iso) = redundant_cue_from_refs(&text, &tracks) {
            // Use the ISO instead. If it was not among the inputs itself,
            // add it here; otherwise it is added with the other ISOs below.
            if ordered.binary_search(&iso).is_err() {
                add_iso_once(&mut sets, &mut used, &iso)?;
            }
            used.insert(path.clone());
            continue;
        }

        let mut files = vec![path.clone()];
        for track in tracks {
            used.insert(track.clone());
            files.push(track);
        }
        sets.push((path.clone(), files));
        used.insert(path.clone());
    }

    // Then every ISO not already used as a CUE's track.
    for path in ordered {
        job.check()?;
        if !used.contains(&path) {
            issue(&path)?;
            sets.push((path.clone(), vec![path]));
        }
    }

    // No file may belong to two sets.
    let mut uses_per_file: HashMap<&PathBuf, usize> = HashMap::new();
    for (_, files) in &sets {
        for file in files {
            *uses_per_file.entry(file).or_insert(0) += 1;
        }
    }
    if uses_per_file.values().any(|uses| *uses > 1) {
        bail!("Selected CUE/ISO entries share data files; select one representation")
    }

    sets.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(sets)
}

/// Every ISO and CUE file in `inputs`, as full paths. Folders are searched
/// all the way down, skipping links, hidden and system folders, folder
/// navigation helpers, and the menu's own images. A file chosen directly
/// must be an ISO or CUE.
fn find_disc_files(
    inputs: &[PathBuf],
    library: Option<&Path>,
    job: &Job,
) -> Result<HashSet<PathBuf>> {
    let mut found = HashSet::new();

    for input in inputs {
        job.check()?;
        if f::is_link(input) {
            bail!("Input is a link or junction")
        }
        let path = input.canonicalize()?;

        if let Some(library) = library {
            let inside_library = path.starts_with(library);
            let contains_library = path.is_dir() && library.starts_with(&path);
            if inside_library || contains_library {
                bail!("Import sources must be outside the destination library")
            }
        }

        if path.is_dir() {
            let entries = walkdir::WalkDir::new(&path)
                .follow_links(false)
                .into_iter()
                .filter_entry(should_search);
            for entry in entries {
                job.check()?;
                let entry = entry?;
                if entry.file_type().is_file() && is_disc_file(&entry) {
                    found.insert(entry.path().canonicalize()?);
                }
            }
        } else {
            let is_helper = path
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .eq_ignore_ascii_case(crate::setup::NAVIGATION_HELPER);
            ensure!(!is_helper, "The navigation helper is not a game");

            let extension = lowercase_extension(&path);
            if extension != "iso" && extension != "cue" {
                bail!("Select ISO/CUE files or directories")
            }
            found.insert(path);
        }
    }

    Ok(found)
}

/// Whether a folder search should look at (or into) this entry: never a
/// link, and never a hidden or system folder below the starting folder.
fn should_search(entry: &walkdir::DirEntry) -> bool {
    if f::is_link(entry.path()) {
        return false;
    }
    if entry.depth() == 0 || !entry.file_type().is_dir() {
        return true;
    }

    let name = entry.file_name().to_string_lossy();
    let hidden = name.starts_with('.');
    let system = SKIPPED_FOLDERS.contains(&name.to_ascii_lowercase().as_str());
    !hidden && !system
}

/// Whether a file found by a folder search is a disc: an ISO or CUE that is
/// not a folder navigation helper, nor the menu's own image at the top of
/// the search.
fn is_disc_file(entry: &walkdir::DirEntry) -> bool {
    let name = entry.file_name().to_string_lossy();
    let extension = lowercase_extension(entry.path());

    let is_helper = name.eq_ignore_ascii_case(crate::setup::NAVIGATION_HELPER);
    let is_disc_type = extension == "iso" || extension == "cue";
    let is_menu = entry.depth() == 1 && MENU_IMAGES.contains(&name.to_ascii_lowercase().as_str());
    !is_helper && is_disc_type && !is_menu
}

/// Adds an ISO as a set of its own, unless it was already used.
fn add_iso_once(
    sets: &mut Vec<(PathBuf, Vec<PathBuf>)>,
    used: &mut HashSet<PathBuf>,
    iso: &Path,
) -> Result<()> {
    if used.insert(iso.to_owned()) {
        issue(iso)?;
        sets.push((iso.to_owned(), vec![iso.to_owned()]));
    }
    Ok(())
}

// ---- Raw sectors and fingerprints ----
//
// A 3DO ISO stores 2048 bytes of data per sector. The disc itself (and a
// raw BIN image, as Redump lists) stores 2352-byte "Mode 1" sectors:
//
//   bytes    0-11    sync pattern: 00, ten FFs, 00
//   bytes   12-15    header: the sector's address as minute, second and
//                    frame in BCD, then the mode (1)
//   bytes   16-2063  the 2048 data bytes
//   bytes 2064-2067  EDC: a CRC-32 of bytes 0-2063
//   bytes 2068-2075  zero
//   bytes 2076-2247  ECC "P" parity: 86 columns of 24 bytes
//   bytes 2248-2351  ECC "Q" parity: 52 diagonals of 43 bytes
//
// Rebuilding the raw sectors lets an ISO be matched to Redump's checksums.

/// Sectors on a full 99-minute disc (99 × 60 seconds × 75 sectors, less the
/// 2-second lead-in); no image can be larger.
const MAX_SECTORS: u32 = 449_850;

/// The 2-second lead-in before sector 0 (150 sectors), counted in the
/// address written in each sector header.
const LEAD_IN_SECTORS: u32 = 150;

const DATA_BYTES: usize = 2048;
const RAW_SECTOR_BYTES: usize = 2352;

/// Lookup tables for building raw sectors, made once.
struct SectorTables {
    /// CRC-32 table for the EDC (the reversed polynomial 0xD8018001).
    crc: [u32; 256],
    /// Each byte multiplied by 2 in the Reed-Solomon field GF(2^8)
    /// (polynomial 0x11D).
    times_two: [u8; 256],
    /// Each byte divided by 3 in the same field.
    divide_by_three: [u8; 256],
}

fn sector_tables() -> &'static SectorTables {
    static TABLES: OnceLock<SectorTables> = OnceLock::new();
    TABLES.get_or_init(|| {
        let mut tables = SectorTables {
            crc: [0; 256],
            times_two: [0; 256],
            divide_by_three: [0; 256],
        };
        for value in 0..256usize {
            let mut crc = value as u32;
            for _ in 0..8 {
                crc = (crc >> 1) ^ if crc & 1 != 0 { 0xd801_8001 } else { 0 };
            }
            tables.crc[value] = crc;

            let doubled = ((value << 1) ^ if value & 0x80 != 0 { 0x11d } else { 0 }) as u8;
            tables.times_two[value] = doubled;
            // value × 3 = value × 2 + value (addition is XOR in this field).
            let tripled = doubled ^ value as u8;
            tables.divide_by_three[tripled as usize] = value as u8;
        }
        tables
    })
}

/// The shape of one of the two ECC parity blocks, as the CD standard lays
/// them out over bytes 12-2075 of the sector.
struct ParityShape {
    /// How many parity pairs (columns or diagonals) to compute.
    columns: usize,
    /// How many bytes each one covers.
    rows: usize,
    /// How far apart neighboring column pairs start.
    column_step: usize,
    /// How far to move to the next byte of a column.
    row_step: usize,
    /// Where the parity bytes are written.
    output: usize,
}

const P_PARITY: ParityShape = ParityShape {
    columns: 86,
    rows: 24,
    column_step: 2,
    row_step: 86,
    output: 2076,
};

const Q_PARITY: ParityShape = ParityShape {
    columns: 52,
    rows: 43,
    column_step: 86,
    row_step: 88,
    output: 2248,
};

/// Builds the raw 2352-byte Mode 1 sector for 2048 data bytes (`cooked`) at
/// sector number `lba`, as the disc would hold it.
pub fn encode_sector(cooked: &[u8], lba: u32) -> Result<[u8; RAW_SECTOR_BYTES]> {
    if cooked.len() != DATA_BYTES || lba >= MAX_SECTORS {
        bail!("Invalid MODE1 sector")
    }
    let tables = sector_tables();
    let mut sector = [0u8; RAW_SECTOR_BYTES];

    // Sync pattern: 00, ten FFs, 00.
    sector[1..11].fill(0xff);

    // Header: the address (counting the lead-in) as BCD minute, second and
    // frame (75 frames a second), then mode 1.
    let address = lba + LEAD_IN_SECTORS;
    let bcd = |n: u32| ((n / 10) * 16 + n % 10) as u8;
    let minute = address / (60 * 75);
    let second = address / 75 % 60;
    let frame = address % 75;
    sector[12..16].copy_from_slice(&[bcd(minute), bcd(second), bcd(frame), 1]);

    sector[16..2064].copy_from_slice(cooked);

    // EDC: CRC-32 of everything so far, stored little-endian.
    let mut crc = 0u32;
    for byte in &sector[..2064] {
        crc = (crc >> 8) ^ tables.crc[((crc ^ *byte as u32) & 0xff) as usize];
    }
    sector[2064..2068].copy_from_slice(&crc.to_le_bytes());

    // ECC: P parity first, then Q parity (which also covers the P bytes).
    for shape in [P_PARITY, Q_PARITY] {
        write_parity(&mut sector, &shape, tables);
    }

    Ok(sector)
}

/// Computes one ECC parity block (Reed-Solomon over GF(2^8)) and writes it
/// into the sector: two parity bytes per column, `columns` bytes apart.
fn write_parity(sector: &mut [u8; RAW_SECTOR_BYTES], shape: &ParityShape, tables: &SectorTables) {
    let covered_bytes = shape.columns * shape.rows;

    for column in 0..shape.columns {
        let mut position = (column / 2) * shape.column_step + column % 2;
        let mut sum = 0u8;
        let mut weighted = 0u8;
        for _ in 0..shape.rows {
            let value = sector[12 + position];
            sum ^= value;
            weighted = tables.times_two[(weighted ^ value) as usize];
            position = (position + shape.row_step) % covered_bytes;
        }

        let first = tables.divide_by_three[(tables.times_two[weighted as usize] ^ sum) as usize];
        sector[shape.output + column] = first;
        sector[shape.output + shape.columns + column] = first ^ sum;
    }
}

/// How many bytes [`fingerprint`] reads at a time.
const READ_CHUNK_BYTES: usize = DATA_BYTES * 512;

/// Checksums a disc file: its size, time, SHA-1 and SHA-256. If
/// `reconstruct` is set and the file is a 2048-byte-sector image, also the
/// SHA-1 of the raw image it would be (see [`encode_sector`]). Fails if the
/// file changes while it is read.
pub fn fingerprint(path: &Path, reconstruct: bool, job: &Job) -> Result<Fingerprint> {
    issue(path)?;
    let (size, modified) = f::stamp(path)?;
    let rebuild_raw = reconstruct
        && size % DATA_BYTES as u64 == 0
        && size / DATA_BYTES as u64 <= MAX_SECTORS as u64;

    let mut sha1 = Sha1::new();
    let mut sha256 = Sha256::new();
    let mut raw_sha1 = Sha1::new();
    let mut file = File::open(path)?;
    let mut buffer = vec![0; READ_CHUNK_BYTES];
    let mut sectors = 0u32;

    loop {
        job.check()?;

        // Fill the buffer (a read can return less than asked).
        let mut filled = 0;
        while filled < buffer.len() {
            let read = file.read(&mut buffer[filled..])?;
            if read == 0 {
                break;
            }
            filled += read;
        }
        if filled == 0 {
            break;
        }

        let chunk = &buffer[..filled];
        sha1.update(chunk);
        sha256.update(chunk);

        if rebuild_raw {
            for data in chunk.chunks(DATA_BYTES) {
                raw_sha1.update(encode_sector(data, sectors)?);
                sectors += 1;
            }
            if sectors.is_multiple_of(8192) {
                let percent = sectors as u64 * DATA_BYTES as u64 * 100 / size;
                job.say(format!("Verifying converted sectors: {percent}%"));
            }
        }
    }

    if f::stamp(path)? != (size, modified) {
        bail!("Source changed while scanning")
    }

    Ok(Fingerprint {
        size,
        mtime_ns: modified,
        sha1: format!("{:x}", sha1.finalize()),
        sha256: format!("{:x}", sha256.finalize()),
        raw_sha1: rebuild_raw.then(|| format!("{:x}", raw_sha1.finalize())),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_title_key_drops_tags_article_and_punctuation() {
        assert_eq!(title_key("Road Rash (USA)"), "roadrash");
        // The article moves but the key is the same either way.
        assert_eq!(title_key("The Horde (USA)"), "horde");
        assert_eq!(title_key("Horde, The"), "horde");
        assert_eq!(
            title_key("A Day at the Fair"),
            "dayatthe fair".replace(' ', "")
        );
        // Disc numbers are tags too, so a multi-disc title keeps its base key.
        assert_eq!(title_key("Need for Speed (Disc 2)"), "needforspeed");
        // Brackets such as Redump's "[!]" are dropped as well.
        assert_eq!(title_key("Instalker [!]"), "instalker");
    }

    #[test]
    fn a_title_key_removes_accents_and_lowercases() {
        assert_eq!(title_key("Rayman Origins"), "raymanorigins");
        assert_eq!(title_key("Klonoa"), title_key("KLONOA"));
        // An accented letter matches its plain form.
        assert_eq!(title_key("Baldies\u{301}"), title_key("Baldies"));
        assert_eq!(title_key("Étoile"), "etoile");
    }

    #[test]
    fn a_title_key_keeps_digits_and_drops_other_punctuation() {
        assert_eq!(title_key("Flash: The Quest - 2!"), "flashthequest2");
        assert_eq!(title_key(""), "");
    }

    #[test]
    fn a_disc_number_is_found_in_the_two_written_forms() {
        // In parentheses.
        assert_eq!(disc_number("Game (Disc 2)"), Some(2));
        assert_eq!(disc_number("Game (Disk 3)"), Some(3));
        assert_eq!(disc_number("Game (CD 02 of 3)"), Some(2));
        // At the end, after a separator.
        assert_eq!(disc_number("Game - Disc 2"), Some(2));
        assert_eq!(disc_number("Game_disk02"), Some(2));
        assert_eq!(disc_number("Game CD 2 of 3"), Some(2));
    }

    #[test]
    fn a_disc_number_must_be_at_the_end_or_in_parentheses() {
        // A bare number in the middle is not a disc label.
        assert_eq!(disc_number("Disc 2 of Battle"), None);
        assert_eq!(disc_number("Game (Disc)"), None);
        assert_eq!(disc_number("Game"), None);
    }

    #[test]
    fn removing_a_disc_label_leaves_a_clean_title() {
        assert_eq!(without_disc("Game (Disc 2)"), "Game");
        assert_eq!(without_disc("Game - CD2"), "Game");
        assert_eq!(without_disc("Game (CD 1 of 3)"), "Game");
        // Nothing to remove.
        assert_eq!(without_disc("Game"), "Game");
    }

    #[test]
    fn a_region_tag_is_named_the_standard_way() {
        assert_eq!(region("us"), Some("USA"));
        assert_eq!(region("USA"), Some("USA"));
        assert_eq!(region("na"), Some("USA"));
        assert_eq!(region("PAL"), Some("Europe"));
        assert_eq!(region("uk"), Some("Europe"));
        assert_eq!(region("Japan"), Some("Japan"));
        assert_eq!(region(" kr "), Some("Korea"));
        // Not a region.
        assert_eq!(region("Unreleased"), None);
        assert_eq!(region(""), None);
    }

    #[test]
    fn every_region_in_a_name_is_found_even_when_listed_together() {
        let all = regions("Game (USA, Europe)");
        assert!(all.contains("USA") && all.contains("Europe"));
        assert_eq!(all.len(), 2);

        let slashed = regions("Game (Japan/Korea)");
        assert!(slashed.contains("Japan") && slashed.contains("Korea"));

        // Several tags: all are collected.
        let two_tags = regions("Game (USA) (Europe)");
        assert!(two_tags.contains("USA") && two_tags.contains("Europe"));

        // A tag with no region contributes nothing.
        assert!(regions("Game [!]").is_empty());
    }

    #[test]
    fn the_primary_region_is_the_first_item_of_the_first_region_tag() {
        assert_eq!(primary_region("Game (USA, Europe)"), "USA");
        assert_eq!(primary_region("Game (Europe, USA)"), "Europe");
        // A non-region tag is skipped in favour of a later region tag.
        assert_eq!(primary_region("Game [!] (Japan)"), "Japan");
        assert_eq!(primary_region("Game"), "Unknown");
    }

    #[test]
    fn a_safe_name_replaces_characters_windows_forbids() {
        assert_eq!(safe_name("A/B:C*D?E\"F|G<H>I", 100), "A_B_C_D_E_F_G_H_I");
        // Control characters become underscores too.
        assert_eq!(safe_name("tab\there", 100), "tab_here");
    }

    #[test]
    fn a_safe_name_trims_spaces_and_dots_and_names_an_empty_one() {
        assert_eq!(safe_name("  Padded  ", 100), "Padded");
        assert_eq!(safe_name("Trailing...", 100), "Trailing");
        assert_eq!(safe_name("", 100), "Untitled");
        assert_eq!(safe_name("   ", 100), "Untitled");
    }

    #[test]
    fn a_safe_name_escapes_windows_reserved_device_names() {
        for device in ["CON", "PRN", "AUX", "NUL", "COM1", "LPT9"] {
            let safe = safe_name(device, 100);
            assert_eq!(safe, format!("_{device}"), "{device}");
            // And a name merely starting with one is left alone.
            assert_eq!(
                safe_name(&format!("{device}sole"), 100),
                format!("{device}sole")
            );
        }
        // Case does not matter, and a device name with an extension is escaped.
        assert_eq!(safe_name("con.txt", 100), "_con.txt");
    }

    #[test]
    fn an_over_long_safe_name_is_cut_and_tagged_with_a_hash() {
        let long = "x".repeat(200);
        let safe = safe_name(&long, 60);
        assert_eq!(safe.len(), 60);
        assert!(safe.starts_with('x'));
        // The suffix is "-" plus 8 hex digits.
        let suffix = &safe[safe.len() - 9..];
        assert!(suffix.starts_with('-'));
        assert!(suffix[1..].bytes().all(|b| b.is_ascii_hexdigit()));

        // Two long names sharing a prefix stay different, but the same name is
        // shortened the same way twice.
        let other = format!("{}y", "x".repeat(199));
        assert_ne!(safe_name(&long, 60), safe_name(&other, 60));
        assert_eq!(safe_name(&long, 60), safe_name(&long, 60));
    }

    #[test]
    fn an_over_long_safe_name_is_cut_on_a_character_boundary() {
        // A multi-byte character must not be split by the cut.
        let name = format!("{}é", "x".repeat(58));
        let safe = safe_name(&name, 60);
        assert!(safe.len() <= 60);
        assert!(std::str::from_utf8(safe.as_bytes()).is_ok());
    }

    #[test]
    fn a_disc_stem_is_never_longer_than_the_limit() {
        for name in [
            "A".repeat(300),
            format!("{} (Disc 9)", "B".repeat(200)),
            format!("{} [{}]", "C".repeat(150), "z".repeat(40)),
        ] {
            let stem = disc_stem(&name);
            assert!(
                stem.len() <= MAX_DISC_STEM,
                "{} bytes for {name:?}",
                stem.len()
            );
        }
    }

    #[test]
    fn a_cue_sheets_text_is_read_as_utf8_or_windows_1252() {
        // Plain UTF-8.
        assert_eq!(
            decode_cue(b"FILE \"a.bin\" BINARY").unwrap(),
            "FILE \"a.bin\" BINARY"
        );
        // A byte-order mark is removed.
        assert_eq!(decode_cue("\u{feff}TRACK 1".as_bytes()).unwrap(), "TRACK 1");
        // Windows-1252: 0xE9 is é, which is not valid UTF-8 on its own.
        assert_eq!(decode_cue(&[b'T', b'\xe9']).unwrap(), "Té");
    }

    #[test]
    fn a_cue_sheet_is_never_rejected_for_its_text_encoding() {
        // decode_cue has an "Unsupported CUE text encoding" error, but it is
        // unreachable: Windows-1252 decodes every byte (the five positions the
        // standard leaves undefined become U+0081 and friends), so
        // had_errors is never set. This records that rather than asserting an
        // error that cannot happen.
        for bytes in [vec![0x81, 0x8d], vec![0x8f, 0x90, 0x9d]] {
            let text = decode_cue(&bytes).expect("decodes without error");
            assert!(
                text.chars().all(|c| ('\u{80}'..'\u{a0}').contains(&c)),
                "{text:?}"
            );
        }
    }

    #[test]
    fn a_disc_stem_writes_a_trailing_disc_number_the_standard_way() {
        // The disc label has to be at the end of the name for it to be found,
        // so a trailing [tag] after it is left as written.
        assert_eq!(disc_stem("Game - CD2"), "Game (Disc 2)");
        assert_eq!(disc_stem("Game (Disc 3)"), "Game (Disc 3)");
        // With the tag first, both the tag and the standard disc label survive.
        assert_eq!(disc_stem("Game (Disc 2) [!]"), "Game [!] (Disc 2)");
        // No disc number: just the cleaned name.
        assert_eq!(disc_stem("Road Rash (USA)"), "Road Rash (USA)");
    }

    #[test]
    fn a_disc_stem_keeps_trailing_tags_only_when_they_leave_room() {
        // A short title keeps its tags.
        let kept = disc_stem("Short [!][b]");
        assert!(kept.contains("[!]"), "{kept}");
        // A long title is cut, and the tag rides along after the hash suffix.
        let long_title = "L".repeat(120);
        let cut = disc_stem(&format!("{long_title} [!]"));
        assert!(cut.len() <= MAX_DISC_STEM, "{} bytes", cut.len());
        assert!(cut.contains("[!]"), "{cut}");
        // The title itself is shortened rather than the tag dropped.
        assert!(cut.contains('-'), "expected a hash suffix in {cut}");
    }

    #[test]
    fn an_empty_or_archive_like_file_is_not_accepted_as_a_disc() {
        let temp = TempDir::new("issue");

        let empty = temp.path().join("empty.iso");
        fs::write(&empty, b"").unwrap();
        assert!(issue(&empty).is_err(), "an empty disc");

        // Each archive signature the app recognises.
        for (label, bytes) in [
            ("zip", &b"PK\x03\x04extra"[..]),
            ("zip-empty", &b"PK\x05\x06"[..]),
            ("7z", &b"7z\xbc\xaf\x27\x1cextra"[..]),
            ("rar", &b"Rar!\x1a\x07extra"[..]),
            ("gzip", &b"\x1f\x8bextra"[..]),
            ("bz2", &b"BZh9extra"[..]),
            ("xz", &b"\xfd7zXZ\x00extra"[..]),
        ] {
            let path = temp.path().join(format!("{label}.iso"));
            fs::write(&path, bytes).unwrap();
            assert!(issue(&path).is_err(), "{label} disguised as a disc");
        }

        // A real ISO header is accepted.
        let iso = temp.path().join("real.iso");
        fs::write(&iso, [0u8; 16]).unwrap();
        issue(&iso).unwrap();
    }

    #[test]
    fn a_redundant_cue_describing_a_plain_iso_points_at_the_iso() {
        let temp = TempDir::new("redundant");
        let iso = temp.path().join("Game.iso");
        fs::write(&iso, [0u8; 2048]).unwrap();

        let cue = temp.path().join("Game.cue");
        fs::write(
            &cue,
            "FILE \"Game.iso\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n",
        )
        .unwrap();
        assert_eq!(redundant_cue(&cue).unwrap(), Some(iso.clone()));

        // Comments and descriptive lines are allowed around it.
        fs::write(
            &cue,
            "REM generated by a tool\nCATALOG 1234567890123\nTITLE \"Game\"\n\
             FILE \"Game.iso\" BINARY\nTRACK 1 MODE1/2048\nINDEX 1 00:00:00\n",
        )
        .unwrap();
        assert_eq!(redundant_cue(&cue).unwrap(), Some(iso));
    }

    #[test]
    fn a_cue_that_is_more_than_a_plain_iso_wrapper_is_kept() {
        let temp = TempDir::new("not-redundant");
        let bin = temp.path().join("Game.bin");
        fs::write(&bin, [0u8; 2352]).unwrap();

        // A BIN rather than an ISO.
        let cue = temp.path().join("Game.cue");
        fs::write(
            &cue,
            "FILE \"Game.bin\" BINARY\nTRACK 1 MODE1/2048\nINDEX 1 00:00:00\n",
        )
        .unwrap();
        assert_eq!(redundant_cue(&cue).unwrap(), None);

        // A second track.
        let iso = temp.path().join("Game.iso");
        fs::write(&iso, [0u8; 2048]).unwrap();
        let audio = temp.path().join("track2.wav");
        fs::write(&audio, [0u8; 100]).unwrap();
        fs::write(
            &cue,
            "FILE \"Game.iso\" BINARY\nTRACK 1 MODE1/2048\nINDEX 1 00:00:00\n\
             TRACK 2 AUDIO\nINDEX 1 04:00:00\n",
        )
        .unwrap();
        assert_eq!(redundant_cue(&cue).unwrap(), None);

        // The directives in the wrong order.
        fs::write(
            &cue,
            "TRACK 1 MODE1/2048\nFILE \"Game.iso\" BINARY\nINDEX 1 00:00:00\n",
        )
        .unwrap();
        assert_eq!(redundant_cue(&cue).unwrap(), None);

        // An audio index, which is not the plain data layout.
        fs::write(
            &cue,
            "FILE \"Game.iso\" BINARY\nTRACK 1 MODE1/2048\nINDEX 1 03:00:00\n",
        )
        .unwrap();
        assert_eq!(redundant_cue(&cue).unwrap(), None);
    }

    #[test]
    fn a_cue_that_cannot_be_read_is_not_redundant_rather_than_an_error() {
        let temp = TempDir::new("broken-redundant");
        let cue = temp.path().join("Broken.cue");
        // Names a file that is not there.
        fs::write(
            &cue,
            "FILE \"missing.iso\" BINARY\nTRACK 1 MODE1/2048\nINDEX 1 00:00:00\n",
        )
        .unwrap();
        assert_eq!(redundant_cue(&cue).unwrap(), None);
    }

    #[test]
    fn a_raw_sector_carries_the_sync_header_data_and_checksum() {
        let cooked = vec![0x5au8; DATA_BYTES];
        let sector = encode_sector(&cooked, 0).unwrap();
        assert_eq!(sector.len(), RAW_SECTOR_BYTES);

        // Sync pattern: 00, ten FFs, 00.
        assert_eq!(sector[0], 0);
        assert!(sector[1..11].iter().all(|&b| b == 0xff));
        assert_eq!(sector[11], 0);

        // Header: BCD address for sector 0 (150 with the lead-in), mode 1.
        assert_eq!(&sector[12..16], &[0x00, 0x02, 0x00, 0x01]);
        // The next sector advances the frame.
        let next = encode_sector(&cooked, 1).unwrap();
        assert_eq!(&next[12..16], &[0x00, 0x02, 0x01, 0x01]);

        // The data appears unchanged at offset 16.
        assert!(sector[16..2064].iter().all(|&b| b == 0x5a));
        // The gap between the EDC and the ECC is zero.
        assert!(sector[2068..2076].iter().all(|&b| b == 0));

        // A different sector number changes the header and so the EDC.
        let edc =
            |s: &[u8; RAW_SECTOR_BYTES]| u32::from_le_bytes(s[2064..2068].try_into().unwrap());
        assert_ne!(edc(&sector), edc(&next));

        // The EDC really is the CRC-32 of the bytes before it.
        let tables = sector_tables();
        let mut crc = 0u32;
        for byte in &sector[..2064] {
            crc = (crc >> 8) ^ tables.crc[((crc ^ *byte as u32) & 0xff) as usize];
        }
        assert_eq!(edc(&sector), crc);
    }

    #[test]
    fn a_raw_sector_is_stable_and_depends_on_its_data() {
        let one = encode_sector(&vec![1u8; DATA_BYTES], 5).unwrap();
        let same = encode_sector(&vec![1u8; DATA_BYTES], 5).unwrap();
        assert_eq!(one, same);

        let other_data = encode_sector(&vec![2u8; DATA_BYTES], 5).unwrap();
        assert_ne!(one, other_data);

        // The whole raw block is filled in: nothing past the EDC gap is left
        // zero by accident.
        assert!(one[2076..RAW_SECTOR_BYTES].iter().any(|&b| b != 0));
    }

    #[test]
    fn an_invalid_sector_is_refused() {
        // The data must be exactly one MODE1 block.
        assert!(encode_sector(&vec![0u8; DATA_BYTES - 1], 0).is_err());
        assert!(encode_sector(&vec![0u8; DATA_BYTES + 1], 0).is_err());
        // And the sector number must fit on a disc.
        assert!(encode_sector(&vec![0u8; DATA_BYTES], MAX_SECTORS).is_err());
        encode_sector(&vec![0u8; DATA_BYTES], MAX_SECTORS - 1).unwrap();
    }

    #[test]
    fn a_fingerprint_reports_the_checksums_of_a_small_image() {
        let temp = TempDir::new("fingerprint");
        let path = temp.path().join("small.iso");
        let data = b"not really an iso, but 2048 bytes of something".to_vec();
        fs::write(&path, &data).unwrap();

        let print = fingerprint(&path, false, &Job::default()).unwrap();
        assert_eq!(print.size, data.len() as u64);
        assert_eq!(print.sha1, format!("{:x}", Sha1::digest(&data)));
        assert_eq!(print.sha256, format!("{:x}", Sha256::digest(&data)));
        // Not asked to reconstruct, so no raw checksum.
        assert_eq!(print.raw_sha1, None);
    }

    #[test]
    fn a_fingerprint_can_rebuild_the_raw_image_checksum_for_a_sector_image() {
        let temp = TempDir::new("fingerprint-raw");
        // Exactly two 2048-byte sectors.
        let path = temp.path().join("two-sectors.iso");
        let data: Vec<u8> = (0..DATA_BYTES * 2).map(|i| (i % 251) as u8).collect();
        fs::write(&path, &data).unwrap();

        let print = fingerprint(&path, true, &Job::default()).unwrap();
        let expected_raw: Vec<u8> = data
            .chunks(DATA_BYTES)
            .enumerate()
            .flat_map(|(lba, chunk)| encode_sector(chunk, lba as u32).unwrap())
            .collect();
        assert_eq!(
            print.raw_sha1,
            Some(format!("{:x}", Sha1::digest(&expected_raw)))
        );
    }

    #[test]
    fn a_fingerprint_does_not_rebuild_raw_sectors_for_a_partially_filled_image() {
        let temp = TempDir::new("fingerprint-partial");
        // Not a whole number of sectors, so there is no raw form to build.
        let path = temp.path().join("odd.iso");
        fs::write(&path, vec![7u8; DATA_BYTES + 1]).unwrap();

        let print = fingerprint(&path, true, &Job::default()).unwrap();
        assert_eq!(print.raw_sha1, None);
    }

    #[test]
    fn a_fingerprint_refuses_an_empty_or_archive_like_file() {
        let temp = TempDir::new("fingerprint-bad");
        let empty = temp.path().join("empty.iso");
        fs::write(&empty, b"").unwrap();
        assert!(fingerprint(&empty, false, &Job::default()).is_err());

        let archive = temp.path().join("zip.iso");
        fs::write(&archive, b"PK\x03\x04not a disc").unwrap();
        assert!(fingerprint(&archive, true, &Job::default()).is_err());
    }

    #[test]
    fn a_fingerprint_reads_a_file_larger_than_its_read_chunk() {
        let temp = TempDir::new("fingerprint-chunked");
        // Bigger than one chunk, so the checksum is built from several reads.
        let data = vec![2u8; READ_CHUNK_BYTES * 2 + 7];
        let big = temp.path().join("big.iso");
        fs::write(&big, &data).unwrap();
        let print = fingerprint(&big, false, &Job::default()).unwrap();
        assert_eq!(print.size, data.len() as u64);
        assert_eq!(print.sha1, format!("{:x}", Sha1::digest(&data)));

        // The stamp is taken again after reading, so a file changed during the
        // scan would be caught; an unchanged one reads cleanly.
        let path = temp.path().join("small.iso");
        fs::write(&path, vec![3u8; DATA_BYTES]).unwrap();
        fingerprint(&path, false, &Job::default()).unwrap();
    }

    /// A fresh folder removed when the test ends, so no test here can reach a
    /// real library or drive.
    struct TempDir(PathBuf);
    impl TempDir {
        fn new(label: &str) -> Self {
            use std::cell::Cell;
            thread_local! {
                static COUNTER: Cell<u32> = const { Cell::new(0) };
            }
            let n = COUNTER.with(|c| {
                let n = c.get();
                c.set(n + 1);
                n
            });
            let path = std::env::temp_dir()
                .join(format!("ohmy3do-disc-{}-{label}-{n}", std::process::id()));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
        fn path(&self) -> &Path {
            &self.0
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
}
