//! The PC library: importing discs into it, and keeping its database and
//! the console's index files up to date.
//!
//! - Identifying: [`scan`] finds discs in the chosen files and folders,
//!   matches each to a catalog release (by checksum, or by name), and works
//!   out its category and where it goes in the library
//!   (Category/Region/Title/disc).
//! - Changing files safely: every change to a library or drive goes through
//!   a [`Journal`], which records how to undo it, so an interruption or a
//!   failure can always be rolled back (or finished).
//! - Indexes: the console menu reads small index files instead of scanning
//!   the drive: `library.3di` (every disc, for Find), `metadata.3di` (each
//!   disc's details), and the cover and preview indexes.
//! - The database (`.ohmy3do/library.json`) records every disc, where it
//!   came from, and its media and information.
//! - [`import`] copies the chosen discs in; the media functions prepare
//!   covers and previews for discs already in the library; and
//!   [`configure_categories`] and [`recategorize`] move discs between
//!   category folders.

use crate::{
    categories,
    disc::{self as d, rx},
    fsutil as f, media,
    model::*,
    Job,
};
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, HashMap, HashSet},
    fs,
    path::{Path, PathBuf},
};

/// Takes the library's lock, so only one job changes it at a time. The lock
/// is a file in the cache named by a checksum of the library's path, held
/// until the returned file is dropped. Fails at once if another job holds
/// it.
pub fn lock_library(root: &Path, cache: &Path) -> Result<fs::File> {
    let directory = cache.join("native-locks");
    fs::create_dir_all(&directory)?;
    let key = format!("{:x}", Sha256::digest(root.to_string_lossy().as_bytes()));
    let file = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(directory.join(key))?;
    fs2::FileExt::try_lock_exclusive(&file)
        .context("Another native operation is already using this library; wait for it to finish")?;
    Ok(file)
}

// ---- Categories ----

/// Evidence for a category: a pattern in the release name, or a LaunchBox
/// release type or genre.
struct CategoryEvidence {
    category: &'static str,
    name_pattern: &'static str,
    types_and_genres: &'static [&'static str],
}

const CATEGORY_EVIDENCE: [CategoryEvidence; 5] = [
    CategoryEvidence {
        category: "Bonus Discs",
        name_pattern: r"(?i)\bbonus\s+(?:disc|cd)\b",
        types_and_genres: &["bonus disc", "bonus cd"],
    },
    CategoryEvidence {
        category: "Samplers",
        name_pattern: r"(?i)\b(?:sampler|sample\s+(?:disc|cd))\b",
        types_and_genres: &["sampler"],
    },
    CategoryEvidence {
        category: "Demos",
        name_pattern: r"(?i)\((?:demo|demonstration|trial)[^)]*\)|\b(?:demo|demonstration|trial\s+version)\b",
        types_and_genres: &["demo", "demonstration", "trial"],
    },
    CategoryEvidence {
        category: "Applications",
        name_pattern: r"(?i)\b(?:storage\s+manager|system\s+utility|utilities|operating\s+system)\b",
        types_and_genres: &[
            "utility",
            "utilities",
            "application",
            "applications",
            "operating system",
        ],
    },
    CategoryEvidence {
        category: "Multimedia",
        name_pattern: r"(?i)\b(?:atlas|almanac|encyclop(?:e|ae)dia|dictionary|museum|art\s+gallery|documentary|photo\s*(?:cd|shoot)|slide\s*show|interactive\s+(?:book|magazine)|music\s+video|karaoke)\b",
        types_and_genres: &[
            "multimedia",
            "reference",
            "documentary",
            "encyclopedia",
            "music video",
            "karaoke",
        ],
    },
];

/// LaunchBox genres that mean a disc is a game.
const GAME_GENRES: [&str; 23] = [
    "action",
    "adventure",
    "arcade",
    "board game",
    "casino",
    "compilation",
    "fighting",
    "flight simulator",
    "horror",
    "pinball",
    "platform",
    "puzzle",
    "racing",
    "role-playing",
    "shooter",
    "simulation",
    "sports",
    "strategy",
    "visual novel",
    "quiz",
    "sandbox",
    "party",
    "construction and management simulation",
];

/// LaunchBox genres that mean a disc is not a game, even if it also has a
/// game genre.
const NON_GAME_GENRES: [&str; 6] = [
    "education",
    "educational",
    "multimedia",
    "reference",
    "documentary",
    "encyclopedia",
];

/// The library category for a release, and why. The category is "" when
/// the evidence is missing or contradictory and the person must choose.
///
/// Redump's own category is used when it names one of the library's
/// categories (or an alias). Otherwise the release name and LaunchBox's
/// release type and genres are weighed: exactly one kind of evidence
/// decides the category; a game genre with no other evidence means Games.
pub fn classify(game: &Game) -> (String, String) {
    let redump_category = game.category.to_lowercase();
    let library_category = CATEGORIES
        .iter()
        .find(|category| category.to_lowercase() == redump_category)
        .copied();
    let redump_alias = match redump_category.as_str() {
        "video" | "audio" => Some("Multimedia"),
        "coverdiscs" => Some("Samplers"),
        "add-ons" => Some("Games"),
        _ => None,
    };
    if let Some(category) = library_category.or(redump_alias) {
        return (
            category.into(),
            format!("Redump category: {}", game.category),
        );
    }

    let genres: HashSet<String> = game.metadata["genre"]
        .as_str()
        .unwrap_or("")
        .split([';', ','])
        .map(|genre| genre.trim().to_lowercase())
        .collect();
    let release_type = game.metadata["release_type"]
        .as_str()
        .unwrap_or("")
        .to_lowercase();

    // Every category with some evidence for it.
    let mut evidence = HashSet::new();
    for rule in &CATEGORY_EVIDENCE {
        if rx(rule.name_pattern).is_match(&game.release)
            || rule.types_and_genres.contains(&release_type.as_str())
            || rule
                .types_and_genres
                .iter()
                .any(|kind| genres.contains(*kind))
        {
            evidence.insert(rule.category);
        }
    }

    // Educational, unless it is already a demo, sampler, multimedia disc or
    // bonus disc.
    let looks_educational = genres.contains("education")
        || genres.contains("educational")
        || rx(r"(?i)\beducational\b").is_match(&game.release);
    let already_non_game = evidence
        .iter()
        .any(|category| ["Demos", "Samplers", "Multimedia", "Bonus Discs"].contains(category));
    if looks_educational && !already_non_game {
        evidence.insert("Educational");
    }

    let has_game_genre = GAME_GENRES.iter().any(|genre| genres.contains(*genre));
    let has_non_game_genre = genres
        .iter()
        .any(|genre| NON_GAME_GENRES.contains(&genre.as_str()));
    if evidence.contains("Multimedia") && has_game_genre && !has_non_game_genre {
        return (
            "".into(),
            "Conflicting game/multimedia evidence; choose a category".into(),
        );
    }

    if evidence.len() == 1 {
        let category = evidence.into_iter().next().unwrap();
        return (category.into(), "Catalog title/genre category".into());
    }
    if !evidence.is_empty() {
        return (
            "".into(),
            "Mixed category evidence; choose a category".into(),
        );
    }

    let maybe_software = rx(r"(?i)\((?:sample|preview)[^)]*\)").is_match(&game.release)
        || genres.contains("music")
        || genres.contains("life simulation");
    if maybe_software {
        return ("".into(), "Choose a software category".into());
    }
    if has_game_genre {
        return ("Games".into(), "Catalog genre identifies a game".into());
    }
    ("".into(), "Choose a category".into())
}

/// Identifies an entry as catalog release `game` (number `index` in the
/// catalog, if it is one) and works out where its files go:
/// Category/Region/Title/disc name. `category` overrides the release's own.
///
/// An entry left without a category gets no destination and is unselected.
/// A disc with several tracks names each track file "(Track NN)".
pub fn assign(entry: &mut Entry, game: &Game, index: Option<usize>, category: Option<&str>) {
    entry.title = game.release.clone();
    entry.region = game
        .region
        .clone()
        .filter(|region| !region.is_empty())
        .unwrap_or_else(|| d::primary_region(&game.release));

    let (classified, reason) = classify(game);
    entry.category = category.unwrap_or(&classified).into();
    entry.category_reason = if category.is_some() {
        "Explicit category override".into()
    } else {
        reason
    };
    entry.game = index;

    if entry.category.is_empty() {
        entry.target.clear();
        entry.selected = false;
        entry.reason.push_str("; category needs review");
        return;
    }

    // Every disc of a set (and every variant) shares the title's folder.
    let folder_title = rx(r"\s*\[Variant [0-9A-F]{8}\]")
        .replace_all(&d::without_disc(&entry.title), "")
        .into_owned();
    let folder = format!(
        "{}/{}/{}",
        entry.category,
        d::safe_name(&entry.region, 80),
        d::safe_name(&folder_title, 110)
    );

    let stem = d::disc_stem(&entry.title);
    let file_count = entry.files.len();
    for (index, file) in entry.files.iter_mut().enumerate() {
        let extension = file
            .source
            .extension()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase();
        // The CUE sheet is file 0; with more than one track, each track
        // file is numbered.
        let track = if index > 0 && file_count > 2 {
            format!(" (Track {index:02})")
        } else {
            String::new()
        };
        file.target = format!("{folder}/{stem}{track}.{extension}");
    }
    entry.target = entry
        .files
        .first()
        .map(|file| file.target.clone())
        .unwrap_or_default();
}

/// Sets an entry's category as the person chose it. A category is also an
/// explicit destination choice for an unmatched title, so an unmatched
/// entry keeps its entered name and region, without claiming it was
/// identified.
pub fn choose_import_category(entry: &mut Entry, catalog: &Catalog, category: &str) {
    // Changing folders may resolve an occupied destination. The caller checks
    // collisions again against the newly assigned paths.
    if entry.reason == "Destination is already occupied" {
        entry.blocked = false;
        entry.selected = false;
    }

    if let Some(index) = entry.game {
        assign(entry, &catalog.games[index], Some(index), Some(category));
    } else {
        let game = Game {
            name: entry.title.clone(),
            release: entry.title.clone(),
            region: Some(entry.region.clone()),
            ..Default::default()
        };
        assign(entry, &game, None, Some(category));
        entry.confidence = "unverified".into();
        entry.reason = "Title kept as entered; no catalog match verified".into();
    }
}

// ---- Identifying discs ----

/// Every product code (serial) a release lists, in comparable form. A field
/// can hold several, separated by commas, semicolons or slashes.
fn serials(game: &Game) -> HashSet<String> {
    std::iter::once(game.serial.as_str())
        .chain(game.roms.iter().map(|rom| rom.serial.as_str()))
        .flat_map(|field| field.split([',', ';', '/']))
        .map(serial_key)
        .filter(|serial| !serial.is_empty())
        .collect()
}

/// A product code in comparable form: lowercase letters and digits only.
fn serial_key(serial: &str) -> String {
    serial
        .to_lowercase()
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .collect()
}

/// What comparing a disc's checksums with the catalog found.
enum ChecksumMatch {
    /// No release matched (or the checksums were not taken).
    None,
    /// The disc's rebuilt raw checksum matches several different releases.
    Ambiguous(Vec<usize>),
    /// These releases match, with this confidence: "checksum" (every data
    /// file), "converted-checksum" (the rebuilt raw image), or "data-track"
    /// (only the first track of a multi-track release).
    Matches(Vec<usize>, &'static str),
}

/// Compares a disc's file checksums with the catalog.
///
/// First every data file's size and SHA-1 together. Failing that, for a
/// single ISO with a rebuilt raw checksum (see [`d::fingerprint`]): a
/// release whose one file is that raw image, or else a multi-track release
/// whose first track is.
fn checksum_matches(entry: &Entry, catalog: &Catalog) -> ChecksumMatch {
    let data_files: Vec<&DiscFile> = entry
        .files
        .iter()
        .filter(|file| {
            !file
                .source
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .eq_ignore_ascii_case("cue")
        })
        .collect();

    // Every data file's (size, SHA-1), in sorted order, against each
    // release's files.
    let mut disc_hashes: Vec<(u64, String)> = data_files
        .iter()
        .map(|file| (file.fingerprint.size, file.fingerprint.sha1.clone()))
        .collect();
    disc_hashes.sort();
    let exact: Vec<usize> = catalog
        .games
        .iter()
        .enumerate()
        .filter_map(|(index, game)| {
            let mut release_hashes: Vec<(u64, String)> = game
                .roms
                .iter()
                .map(|rom| (rom.size, rom.sha1.clone()))
                .collect();
            release_hashes.sort();
            (release_hashes == disc_hashes).then_some(index)
        })
        .collect();
    if !exact.is_empty() {
        return ChecksumMatch::Matches(exact, "checksum");
    }

    if data_files.len() != 1 {
        return ChecksumMatch::None;
    }
    let Some(raw_sha1) = &data_files[0].fingerprint.raw_sha1 else {
        return ChecksumMatch::None;
    };
    let raw_size = data_files[0].fingerprint.size / 2048 * 2352;

    let complete: Vec<usize> = catalog
        .games
        .iter()
        .enumerate()
        .filter(|(_, game)| {
            game.roms.len() == 1 && game.roms[0].size == raw_size && &game.roms[0].sha1 == raw_sha1
        })
        .map(|(index, _)| index)
        .collect();
    let first_track = rx(r"(?i)(?:\(Track\s+0*1\)|\btrack0*1)\.bin$");
    let partial: Vec<usize> = catalog
        .games
        .iter()
        .enumerate()
        .filter(|(_, game)| {
            let matching_first_tracks = game
                .roms
                .iter()
                .filter(|rom| {
                    first_track.is_match(&rom.name) && rom.size == raw_size && &rom.sha1 == raw_sha1
                })
                .count();
            game.roms.len() > 1 && matching_first_tracks == 1
        })
        .map(|(index, _)| index)
        .collect();

    let all: Vec<usize> = complete.iter().chain(partial.iter()).copied().collect();
    let releases: HashSet<&String> = all
        .iter()
        .map(|index| &catalog.games[*index].release)
        .collect();
    if releases.len() > 1 {
        return ChecksumMatch::Ambiguous(all);
    }
    if !complete.is_empty() {
        ChecksumMatch::Matches(complete, "converted-checksum")
    } else {
        ChecksumMatch::Matches(partial, "data-track")
    }
}

/// Identifies an entry against the catalog: by checksum unless `quick`,
/// and by name if that finds nothing. Sets its title, category and
/// destination if it is identified, or else its candidates and the reason
/// the person must choose.
fn identify(entry: &mut Entry, catalog: &Catalog, quick: bool) {
    if !quick {
        match checksum_matches(entry, catalog) {
            ChecksumMatch::Ambiguous(all) => {
                entry.candidates = all;
                entry.reason =
                    "Reconstructed checksum matches multiple releases; choose explicitly".into();
                return;
            }
            ChecksumMatch::Matches(found, confidence) if !found.is_empty() => {
                accept_checksum_match(entry, catalog, found, confidence);
                return;
            }
            _ => {}
        }
    }
    identify_by_name(entry, catalog);
}

/// Uses a checksum match: if every matching release has the same name, the
/// entry is that release (selected, unless only its first track was
/// verified); otherwise the person chooses among them.
fn accept_checksum_match(
    entry: &mut Entry,
    catalog: &Catalog,
    found: Vec<usize>,
    confidence: &'static str,
) {
    let releases: HashSet<&String> = found
        .iter()
        .map(|index| &catalog.games[*index].release)
        .collect();
    if releases.len() != 1 {
        entry.candidates = found;
        entry.reason = "Checksums match multiple releases; choose one".into();
        return;
    }

    entry.confidence = confidence.into();
    entry.reason = match confidence {
        "converted-checksum" => {
            "Complete reconstructed MODE1 checksum match; original ISO unchanged"
        }
        "data-track" => {
            "First data-track checksum matched; missing audio tracks are unverified; review required"
        }
        _ => "Complete data-file SHA-1 and size match",
    }
    .into();
    let index = found[0];
    assign(entry, &catalog.games[index], Some(index), None);
    entry.selected = entry.resolvable() && confidence != "data-track";
}

/// Identifies an entry by its file name: the release with exactly that name,
/// or else the releases with the same title key and disc number, narrowed by
/// region (from the name, or a region-named folder above it) and by product
/// code ([code] in the name). A single release is accepted, unselected, if
/// its special tags (Rev, Beta, Proto, Demo, ...) agree.
fn identify_by_name(entry: &mut Entry, catalog: &Catalog) {
    let name = entry
        .source
        .file_stem()
        .unwrap_or_default()
        .to_string_lossy()
        .into_owned();
    let title_key = d::title_key(&name);

    let same_name: Vec<usize> = catalog
        .games
        .iter()
        .enumerate()
        .filter(|(_, game)| {
            game.release.eq_ignore_ascii_case(&name)
                || d::disc_stem(&game.release).eq_ignore_ascii_case(&name)
        })
        .map(|(index, _)| index)
        .collect();
    let mut candidates: Vec<usize> = if same_name.is_empty() {
        catalog
            .games
            .iter()
            .enumerate()
            .filter(|(_, game)| {
                d::title_key(&game.name) == title_key
                    && d::disc_number(&game.name) == d::disc_number(&name)
            })
            .map(|(index, _)| index)
            .collect()
    } else {
        same_name
    };

    // Narrow by region: from the name, or else from the one region-named
    // folder above the disc ("USA", or "2 - Europe").
    let mut regions = d::regions(&name);
    if regions.is_empty() {
        let folder_regions: HashSet<String> = entry
            .source
            .parent()
            .into_iter()
            .flat_map(|parent| parent.components())
            .filter_map(|component| {
                let text = component.as_os_str().to_string_lossy();
                let without_number = rx(r"^\d+[\s._-]+").replace(&text, "");
                d::region(&without_number).map(str::to_owned)
            })
            .collect();
        if folder_regions.len() == 1 {
            regions = folder_regions;
        }
    }
    if !regions.is_empty() {
        candidates.retain(|index| regions.is_subset(&d::regions(&catalog.games[*index].name)));
    }

    // Narrow by product code: a [tag] with letters and digits that is not a
    // variant label.
    let codes: HashSet<String> = rx(r"\[([^]]+)\]")
        .captures_iter(&name)
        .map(|captures| captures[1].to_owned())
        .filter(|tag| {
            tag.len() >= 4
                && !tag.to_lowercase().starts_with("variant ")
                && tag.chars().any(|c| c.is_ascii_alphabetic())
                && tag.chars().any(|c| c.is_ascii_digit())
        })
        .map(|tag| serial_key(&tag))
        .collect();
    if !codes.is_empty() {
        let with_codes: Vec<usize> = candidates
            .iter()
            .filter(|index| codes.is_subset(&serials(&catalog.games[**index])))
            .copied()
            .collect();
        if with_codes.is_empty() {
            entry.candidates = candidates;
            entry.reason =
                "Product code does not uniquely identify a catalog release; choose explicitly"
                    .into();
            return;
        }
        candidates = with_codes;
    }

    let releases: HashSet<&String> = candidates
        .iter()
        .map(|index| &catalog.games[*index].release)
        .collect();
    let special_tags = |text: &str| -> HashSet<String> {
        rx(r"(?i)\(([^)]*(?:rev|beta|proto|demo|sample|unl|alt)[^)]*)\)")
            .captures_iter(text)
            .map(|captures| captures[1].to_lowercase())
            .collect()
    };
    if releases.len() == 1 {
        let index = candidates[0];
        if special_tags(&name) == special_tags(&catalog.games[index].release) {
            entry.confidence = "filename".into();
            entry.reason = "Title/region/disc match; checksum identity is unverified. \
                            Check explicitly to accept."
                .into();
            assign(entry, &catalog.games[index], Some(index), None);
            entry.selected = false;
            return;
        }
    }

    entry.candidates = candidates;
    entry.reason = if entry.candidates.is_empty() {
        "No safe catalog match. Choose a release or enter an unverified title.".into()
    } else {
        "Several releases or variants match; choose explicitly".into()
    };
}

/// A new, unidentified entry for the disc at `source`.
fn new_entry(source: &Path) -> Entry {
    Entry {
        source: source.to_owned(),
        title: source
            .file_stem()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned(),
        category: "".into(),
        region: "Unknown".into(),
        target: "".into(),
        confidence: "unmatched".into(),
        reason: "".into(),
        selected: false,
        files: vec![],
        candidates: vec![],
        game: None,
        cover: None,
        preview: None,
        category_reason: "".into(),
        blocked: false,
    }
}

/// Whether a disc is a CUE sheet whose tracks are all audio (a music CD).
fn is_audio_only(source: &Path) -> Result<bool> {
    let is_cue = source
        .extension()
        .unwrap_or_default()
        .to_string_lossy()
        .eq_ignore_ascii_case("cue");
    if !is_cue {
        return Ok(false);
    }
    let (text, _) = d::cue_refs(source)?;
    let modes: Vec<String> = rx(r"(?im)^\s*TRACK\s+\d+\s+(\S+)")
        .captures_iter(&text)
        .map(|captures| captures[1].to_ascii_uppercase())
        .collect();
    Ok(!modes.is_empty() && modes.iter().all(|mode| mode == "AUDIO"))
}

/// Finds the discs in `inputs` and identifies each for importing into the
/// library at `root`.
///
/// A `quick` scan reads only file sizes and times, and identifies by name.
/// A full scan checksums every file, and for a lone ISO whose raw size
/// could match a catalog release, also the checksum of its rebuilt raw
/// image (see [`d::fingerprint`]).
pub fn scan(
    root: &Path,
    inputs: &[PathBuf],
    catalog: Catalog,
    quick: bool,
    job: &Job,
) -> Result<Plan> {
    let root = f::root(root)?;
    let sets = d::collect(inputs, Some(&root), job)?;
    if sets.is_empty() {
        bail!("No disc entries found")
    }
    // Every file size in the catalog, to tell which ISOs are worth
    // rebuilding as raw images.
    let catalog_sizes: HashSet<u64> = catalog
        .games
        .iter()
        .flat_map(|game| game.roms.iter())
        .map(|rom| rom.size)
        .collect();

    let mut entries = vec![];
    for (index, (source, files)) in sets.iter().enumerate() {
        job.check()?;
        job.say(format!(
            "Scanning {}/{}: {}",
            index + 1,
            sets.len(),
            source.display()
        ));
        let mut entry = new_entry(source);
        let audio_only = is_audio_only(source)?;

        for file in files {
            let size = fs::metadata(file)?.len();
            let is_iso = file
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .eq_ignore_ascii_case("iso");
            let rebuild_raw = !quick
                && files.len() == 1
                && is_iso
                && size % 2048 == 0
                && catalog_sizes.contains(&(size / 2048 * 2352));
            let fingerprint = if quick {
                let (size, mtime_ns) = f::stamp(file)?;
                Fingerprint {
                    size,
                    mtime_ns,
                    sha1: String::new(),
                    sha256: String::new(),
                    raw_sha1: None,
                }
            } else {
                d::fingerprint(file, rebuild_raw, job)?
            };
            entry.files.push(DiscFile {
                source: file.clone(),
                fingerprint,
                target: String::new(),
            });
        }

        if audio_only {
            // Keep the CUE and every track available for explicit import, but
            // do not match an audio disc against the game catalog.
            entry.region = d::primary_region(&entry.title);
            entry.confidence = "audio-only".into();
            entry.reason =
                "Audio-only disc: choose a category to import without a verified catalog match"
                    .into();
        } else {
            identify(&mut entry, &catalog, quick);
        }
        entries.push(entry);
    }

    let mut plan = Plan {
        root,
        catalog,
        entries,
    };
    let config = categories::load(&plan.root)?;
    for entry in &mut plan.entries {
        apply_category_defaults(entry, &config);
    }
    collisions(&mut plan)?;
    Ok(plan)
}

/// Moves an entry into the folder its category is configured to use (the
/// library's categories can be renamed or merged), adjusting its
/// destination paths.
pub fn apply_category_defaults(entry: &mut Entry, config: &categories::Categories) {
    let Some(destination) = config.destination(&entry.category) else {
        return;
    };
    let destination = destination.to_owned();
    if destination == entry.category {
        return;
    }

    let old_prefix = format!("{}/", entry.category);
    let new_prefix = format!("{destination}/");
    entry.target = entry.target.replacen(&old_prefix, &new_prefix, 1);
    for file in &mut entry.files {
        file.target = file.target.replacen(&old_prefix, &new_prefix, 1);
    }
    entry.category = destination;
}

/// Marks entries whose destination is already taken (blocked) or shared
/// with another entry (unselected, so the person picks one). Clears the
/// marks from an earlier check first, so it can be run again after changes.
pub fn collisions(plan: &mut Plan) -> Result<()> {
    for entry in &mut plan.entries {
        let marked_before = [
            "Destination is occupied or shared by another selected disc",
            "Destination is already occupied",
            "Destination shared by multiple source entries; check only one",
        ]
        .contains(&entry.reason.as_str());
        if marked_before {
            entry.blocked = false;
            entry.selected = false;
            entry.reason = "Review destination and check to import".into();
        }
    }

    // Which entry claims each destination (case-insensitively).
    let mut claimed: HashMap<String, usize> = HashMap::new();
    let mut occupied = vec![];
    let mut shared = vec![];
    for (index, entry) in plan.entries.iter().enumerate() {
        if entry.target.is_empty() {
            continue;
        }
        for file in &entry.files {
            if f::safe(&plan.root, &file.target)?.exists() {
                occupied.push(index);
            }
            if let Some(other) = claimed.insert(f::fold(&file.target), index) {
                if other != index {
                    shared.extend([other, index]);
                }
            }
        }
    }

    for index in shared {
        plan.entries[index].selected = false;
        plan.entries[index].reason =
            "Destination shared by multiple source entries; check only one".into();
    }
    for index in occupied {
        plan.entries[index].blocked = true;
        plan.entries[index].selected = false;
        plan.entries[index].reason = "Destination is already occupied".into();
    }
    Ok(())
}

// ---- The journal ----

/// The largest support file the journal will back up (it keeps the old
/// contents in the journal itself).
const MAX_JOURNALED_FILE_BYTES: u64 = 16 * 1024 * 1024;

/// A new file the journal copied in: undo deletes it, if it is unchanged.
#[derive(Clone, Serialize, Deserialize)]
struct Created {
    path: String,
    sha256: String,
}

/// A small file the journal wrote or removed: undo puts back `before`
/// (deleting the file if it did not exist), if the file is still as the
/// journal left it.
#[derive(Clone, Serialize, Deserialize)]
struct Written {
    path: String,
    before: Option<Vec<u8>>,
    /// SHA-256 of what was written, or "" for a removed file.
    after: String,
}

/// A file or folder renamed within the library, such as a game changing
/// category: undo renames it back.
#[derive(Clone, Serialize, Deserialize)]
struct Moved {
    from: String,
    to: String,
}

/// A record of every change one job makes to a library or drive, saved to
/// the cache before each change is made, so the job can always be undone
/// (by [`Journal::undo`], or later by [`undo`] from the saved file).
///
/// Its `state` is "applying" while the job runs, "complete" when it
/// finished, "undone" after an undo, or "removal-committed" once title
/// removal has passed the point where it can only be finished (see
/// `title_removal`).
#[derive(Serialize, Deserialize)]
pub struct Journal {
    schema: u32,
    pub root: PathBuf,
    pub state: String,
    created: Vec<Created>,
    writes: Vec<Written>,
    /// Folders the journal created, so undo can remove them if empty.
    directories: Vec<String>,
    #[serde(default)]
    moves: Vec<Moved>,
    /// Where the journal is saved (not part of the saved file).
    #[serde(skip)]
    pub path: PathBuf,
}

impl Journal {
    /// Starts a journal for changes to `root`, saved in the cache's
    /// `native-transactions` folder. Backs up the person's own game
    /// information first.
    pub fn new(root: &Path, cache: &Path) -> Result<Self> {
        crate::game_info::backup_custom(root, cache)?;
        let path = cache
            .join("native-transactions")
            .join(format!("{}.json", uuid::Uuid::new_v4()));
        let journal = Self {
            schema: 1,
            root: f::root(root)?,
            state: "applying".into(),
            created: vec![],
            writes: vec![],
            directories: vec![],
            moves: vec![],
            path,
        };
        journal.save()?;
        Ok(journal)
    }

    /// Saves the journal (replacing the saved file in one step).
    pub fn save(&self) -> Result<()> {
        f::atomic(&self.path, &serde_json::to_vec_pretty(self)?)?;
        Ok(())
    }

    /// Creates the missing folders above `path`, top first, recording each
    /// before it is made.
    fn parents(&mut self, path: &Path) -> Result<()> {
        let mut missing = vec![];
        let mut parent = path.parent();
        while let Some(folder) = parent {
            if folder == self.root || folder.exists() {
                break;
            }
            missing.push(folder.to_owned());
            parent = folder.parent();
        }

        for folder in missing.into_iter().rev() {
            let name = relative(&self.root, &folder)?;
            self.directories.push(name);
            self.save()?;
            fs::create_dir(&folder)?;
        }
        Ok(())
    }

    /// Makes sure folder `name` exists, creating it (and its parents) if
    /// needed.
    pub fn ensure_directory(&mut self, name: &str) -> Result<()> {
        let path = f::safe(&self.root, name)?;
        if path.exists() {
            if !path.is_dir() {
                bail!("Expected a directory: {name}")
            }
            return Ok(());
        }
        self.parents(&path.join("placeholder"))
    }

    /// Writes a small file (at most 16 MiB), keeping its old contents for
    /// undo. Does nothing if it already holds `data`.
    pub fn write(&mut self, name: &str, data: &[u8]) -> Result<()> {
        let path = f::safe(&self.root, name)?;
        if path.exists() && !path.is_file() {
            bail!("Expected a regular file: {name}")
        }

        let before = if path.is_file() {
            if fs::metadata(&path)?.len() > MAX_JOURNALED_FILE_BYTES {
                bail!("Oversized support file")
            }
            Some(fs::read(&path)?)
        } else {
            None
        };
        if before.as_deref() == Some(data) {
            return Ok(());
        }

        self.parents(&path)?;
        self.writes.push(Written {
            path: name.into(),
            before,
            after: format!("{:x}", Sha256::digest(data)),
        });
        self.save()?;
        f::atomic(&path, data)
    }

    /// Deletes a small file; undo writes it back.
    pub fn remove(&mut self, name: &str) -> Result<()> {
        let path = f::safe(&self.root, name)?;
        if !path.is_file() {
            bail!("Expected a regular file: {name}")
        }
        if fs::metadata(&path)?.len() > MAX_JOURNALED_FILE_BYTES {
            bail!("Oversized support file")
        }

        self.writes.push(Written {
            path: name.into(),
            before: Some(fs::read(&path)?),
            after: String::new(),
        });
        self.save()?;
        fs::remove_file(path)?;
        Ok(())
    }

    /// Copies `source` to the new file `name` (never over an existing one),
    /// checking the copy has the SHA-256 `expected`, or the source's
    /// checksum taken first if none is given. Fails if the source changed
    /// since it was scanned.
    pub fn copy(
        &mut self,
        source: &Path,
        name: &str,
        expected: Option<&str>,
        job: &Job,
    ) -> Result<()> {
        let path = f::safe(&self.root, name)?;
        if path.exists() {
            bail!("Refusing to overwrite {}", path.display())
        }
        self.parents(&path)?;

        let mut copy_job = job.clone();
        let hash = match expected.filter(|hash| !hash.is_empty()) {
            Some(hash) => hash.to_owned(),
            None => {
                // Hashing first is a third of the work: show it in the same
                // progress bar, so the bar does not reach the end and
                // restart before the copy and its check.
                let bytes = fs::metadata(source)?.len().max(1);
                copy_job =
                    job.progress_range(bytes, bytes * 2, bytes * 3, "Copying and verifying file");
                let hashing_job =
                    job.progress_range(0, bytes, bytes * 3, "Copying and verifying file");
                f::sha256(source, &hashing_job)?
            }
        };

        self.created.push(Created {
            path: name.into(),
            sha256: hash.clone(),
        });
        self.save()?;
        let actual = f::copy_new(source, &path, &copy_job, Some(&hash))?;
        if actual != hash {
            bail!("Source contents changed since scan: {}", source.display())
        }
        self.save()?;
        Ok(())
    }

    /// Renames a file or folder within the root; undo renames it back.
    pub fn rename(&mut self, from: &str, to: &str) -> Result<()> {
        let source = f::safe(&self.root, from)?;
        let target = f::safe(&self.root, to)?;
        if !source.exists() || f::is_link(&source) {
            bail!("Missing or linked: {from}")
        }
        if target.exists() {
            bail!("Destination is already occupied: {to}")
        }

        self.parents(&target)?;
        self.moves.push(Moved {
            from: from.into(),
            to: to.into(),
        });
        self.save()?;
        fs::rename(&source, &target)?;
        Ok(())
    }

    /// Marks the job complete.
    pub fn finish(&mut self) -> Result<()> {
        self.state = "complete".into();
        self.save()
    }

    /// Undoes every change, newest first: written files get their old
    /// contents, copied files are deleted, renames are reversed, and created
    /// folders are removed if empty.
    ///
    /// Everything is checked before anything is changed: if any file has
    /// been changed by something else since the journal wrote it, nothing is
    /// undone, so the person's newer work is never lost.
    pub fn undo(&mut self) -> Result<()> {
        anyhow::ensure!(
            self.state != "removal-committed",
            "Permanent title removal cannot be undone"
        );
        if self.state == "undone" {
            bail!("Transaction is already undone")
        }

        self.check_undo_is_safe()?;

        for entry in self.writes.iter().rev() {
            let path = f::safe(&self.root, &entry.path)?;
            if let Some(before) = &entry.before {
                f::atomic(&path, before)?;
            } else if path.exists() {
                fs::remove_file(path)?;
            }
        }
        for entry in self.created.iter().rev() {
            let path = f::safe(&self.root, &entry.path)?;
            if path.exists() {
                fs::remove_file(path)?;
            }
        }
        for entry in self.moves.iter().rev() {
            let from = f::safe(&self.root, &entry.from)?;
            let to = f::safe(&self.root, &entry.to)?;
            if to.exists() && !from.exists() {
                fs::rename(to, from)?;
            } else if !from.exists() {
                bail!("Undo refused: moved item is missing: {}", entry.to)
            }
        }
        for name in self.directories.iter().rev() {
            let path = f::safe(&self.root, name)?;
            if path.is_dir() {
                let _ = fs::remove_dir(path);
            }
        }

        self.state = "undone".into();
        self.save()
    }

    /// Checks every written and copied file is as the journal left it,
    /// without changing anything.
    ///
    /// A file can be written more than once, so the writes are replayed
    /// backwards in memory: each must find either its `before` contents
    /// (already undone by a later write) or its `after` contents. A copied
    /// file that a write also touched is checked against the replayed
    /// contents.
    fn check_undo_is_safe(&self) -> Result<()> {
        let job = Job::default();
        let mut replayed: HashMap<String, Option<Vec<u8>>> = HashMap::new();

        for entry in self.writes.iter().rev() {
            let path = f::safe(&self.root, &entry.path)?;
            let current = replayed
                .entry(entry.path.clone())
                .or_insert(if path.exists() {
                    Some(fs::read(&path)?)
                } else {
                    None
                });
            // An empty `after` marks a removed file.
            let after = (!entry.after.is_empty()).then(|| entry.after.clone());
            let current_hash = current
                .as_ref()
                .map(|bytes| format!("{:x}", Sha256::digest(bytes)));
            if *current != entry.before && current_hash != after {
                bail!("Undo refused: file changed after import: {}", entry.path)
            }
            *current = entry.before.clone();
        }

        for entry in &self.created {
            let path = f::safe(&self.root, &entry.path)?;
            if !path.exists() {
                continue;
            }
            let actual = match replayed.get(&entry.path) {
                Some(Some(bytes)) => format!("{:x}", Sha256::digest(bytes)),
                Some(None) => String::new(),
                None => f::sha256(&path, &job)?,
            };
            if actual != entry.sha256 {
                bail!("Undo refused: imported file changed: {}", entry.path)
            }
        }
        Ok(())
    }
}

/// Undoes the job recorded in the saved journal at `path` (in the cache's
/// `native-transactions` folder), holding the library's lock.
pub fn undo(path: &Path) -> Result<()> {
    let mut journal: Journal = serde_json::from_slice(&fs::read(path)?)?;
    journal.path = path.to_owned();
    if journal.schema != 1 {
        bail!("Unsupported native transaction version")
    }

    let cache = path
        .parent()
        .and_then(Path::parent)
        .context("Invalid native journal location")?;
    let _lock = lock_library(&journal.root, cache)?;
    if f::root(&journal.root)? != journal.root {
        bail!("Transaction root changed")
    }
    crate::game_info::backup_custom(&journal.root, cache)?;
    journal.undo()
}

/// `path` relative to `root`, with "/" between folders (as the console and
/// the database write paths). Works through links and different spellings
/// of the root by comparing full paths if needed. Fails for a path outside
/// the root or a name that is not valid Unicode.
pub fn relative(root: &Path, path: &Path) -> Result<String> {
    let canonical_root;
    let canonical_path;
    let relative = match path.strip_prefix(root) {
        Ok(relative) => relative,
        Err(_) => {
            canonical_root = root.canonicalize()?;
            canonical_path = path.canonicalize()?;
            canonical_path.strip_prefix(&canonical_root)?
        }
    };

    let parts = relative
        .components()
        .map(|part| part.as_os_str().to_str().context("Non-UTF8 filename"))
        .collect::<Result<Vec<_>>>()?;
    Ok(parts.join("/"))
}

// ---- Cover and preview indexes ----

/// The most discs the menu's indexes can hold.
const MAX_INDEXED_DISCS: usize = 2048;

/// The largest media index payload the menu accepts.
const MAX_MEDIA_INDEX_PAYLOAD: usize = 128 * 1024;

/// The longest media file name in a media index.
const MAX_MEDIA_NAME: usize = 96;

/// Appends a big-endian 32-bit number.
fn push_u32(bytes: &mut Vec<u8>, value: u32) {
    bytes.extend_from_slice(&value.to_be_bytes());
}

/// Appends a big-endian 16-bit length (counting the terminating zero), the
/// text, and a zero byte: how the console's indexes store text.
fn push_text(bytes: &mut Vec<u8>, text: &[u8]) {
    bytes.extend_from_slice(&((text.len() + 1) as u16).to_be_bytes());
    bytes.extend_from_slice(text);
    bytes.push(0);
}

/// A 24-byte index header: the signature, the entry count, the payload's
/// size and CRC-32, and a flags word (0).
fn index_header(signature: &[u8; 8], count: usize, payload: &[u8]) -> Vec<u8> {
    let mut header = signature.to_vec();
    push_u32(&mut header, count as u32);
    push_u32(&mut header, payload.len() as u32);
    push_u32(&mut header, crc32fast::hash(payload));
    push_u32(&mut header, 0);
    header
}

/// A cover or preview index ("OM3DIDX1") for the menu: which media file
/// belongs to each disc, given as (case-folded disc path, file name).
///
/// After the header (see [`index_header`]), each entry is the two lengths
/// (16 bits each, counting the zero byte) and then the disc path and the
/// file name, each ending in a zero byte.
pub fn media_index(map: &BTreeMap<String, String>) -> Result<Vec<u8>> {
    let mut payload = vec![];
    for (key, name) in map {
        let key = key.as_bytes();
        let name = name.as_bytes();
        if key.len() + 1 > u16::MAX as usize || name.len() > MAX_MEDIA_NAME {
            bail!("Oversized media index entry")
        }
        payload.extend_from_slice(&((key.len() + 1) as u16).to_be_bytes());
        payload.extend_from_slice(&((name.len() + 1) as u16).to_be_bytes());
        payload.extend_from_slice(key);
        payload.push(0);
        payload.extend_from_slice(name);
        payload.push(0);
    }
    if map.len() > MAX_INDEXED_DISCS || payload.len() > MAX_MEDIA_INDEX_PAYLOAD {
        bail!("Media index exceeds menu capacity")
    }

    let mut index = index_header(b"OM3DIDX1", map.len(), &payload);
    index.extend(payload);
    Ok(index)
}

/// The USB drive's copy of a media index: the same entries, flagged so the
/// menu opens each disc's media from the .ohmy3do folder beside that disc.
/// The FCare ODE opens files relative to the current folder, so the menu
/// then reads covers and previews without navigating to the root and back.
pub fn beside_index(data: &[u8]) -> Result<Vec<u8>> {
    if data.len() < 24 || &data[..8] != b"OM3DIDX1" {
        bail!("Invalid media index")
    }
    let mut index = data.to_vec();
    index[20..24].copy_from_slice(&1u32.to_be_bytes());
    Ok(index)
}

/// Reads a library's cover or preview index (`folder`): file name by
/// case-folded disc path. Empty if there is none.
pub fn read_media_index(root: &Path, folder: &str) -> Result<BTreeMap<String, String>> {
    read_media_index_impl(root, folder, false)
}

/// Reads a prepared drive's cover or preview index, whose media sit beside
/// each disc (see [`beside_index`]).
pub(crate) fn read_drive_media_index(
    root: &Path,
    folder: &str,
) -> Result<BTreeMap<String, String>> {
    read_media_index_impl(root, folder, true)
}

/// Reads and checks a media index (see [`media_index`]): its size, CRC,
/// layout flag (`beside`), every entry's lengths and zero bytes, safe file
/// names, and no disc listed twice.
fn read_media_index_impl(
    root: &Path,
    folder: &str,
    beside: bool,
) -> Result<BTreeMap<String, String>> {
    let path = f::safe(root, &format!(".ohmy3do/{folder}/index.3di"))?;
    if !path.exists() || (beside && fs::metadata(&path)?.len() == 0) {
        return Ok(BTreeMap::new());
    }
    if fs::metadata(&path)?.len() > (MAX_MEDIA_INDEX_PAYLOAD + 24) as u64 {
        bail!("Oversized media index")
    }
    let data = fs::read(path)?;
    if data.len() < 24 || &data[..8] != b"OM3DIDX1" {
        bail!("Invalid {folder} index")
    }

    let word = |position: usize| {
        u32::from_be_bytes(data[position..position + 4].try_into().unwrap()) as usize
    };
    let count = word(8);
    let payload_size = word(12);
    let flags = word(20);
    if flags == 1 && !beside {
        bail!(
            "This is a prepared USB drive with media beside each game. \
             Edit the staging library, then prepare the drive again."
        )
    }
    if count > MAX_INDEXED_DISCS
        || payload_size > MAX_MEDIA_INDEX_PAYLOAD
        || payload_size + 24 != data.len()
        || flags != u32::from(beside) as usize
        || crc32fast::hash(&data[24..]) as usize != word(16)
    {
        bail!("Invalid {folder} index checksum or bounds")
    }

    let mut map = BTreeMap::new();
    let mut position = 24;
    for _ in 0..count {
        if position + 4 > data.len() {
            bail!("Truncated media index")
        }
        let key_length =
            u16::from_be_bytes(data[position..position + 2].try_into().unwrap()) as usize;
        let name_length =
            u16::from_be_bytes(data[position + 2..position + 4].try_into().unwrap()) as usize;
        position += 4;
        // The shortest name is "x.3dc" plus its zero byte.
        if key_length < 2 || name_length < 6 || position + key_length + name_length > data.len() {
            bail!("Invalid media index string")
        }

        let key = &data[position..position + key_length];
        let name = &data[position + key_length..position + key_length + name_length];
        let properly_ended = key[key_length - 1] == 0
            && name[name_length - 1] == 0
            && !key[..key_length - 1].contains(&0)
            && !name[..name_length - 1].contains(&0);
        if !properly_ended {
            bail!("Invalid media index terminator")
        }

        let key = std::str::from_utf8(&key[..key_length - 1])?;
        let name = std::str::from_utf8(&name[..name_length - 1])?;
        if name.len() > MAX_MEDIA_NAME
            || name.contains(['/', '\\', ':', '|'])
            || name.starts_with('.')
        {
            bail!("Unsafe media name")
        }
        f::safe(root, &format!(".ohmy3do/{folder}/{name}"))?;
        if map.insert(f::fold(key), name.into()).is_some() {
            bail!("Duplicate media association")
        }
        position += key_length + name_length;
    }

    if position != data.len() {
        bail!("Trailing media index data")
    }
    Ok(map)
}

// ---- The menu's index files ----

/// Rebuilds every index the menu reads (see [`build_indexes_impl`]), and
/// adds any missing folder navigation helpers.
pub fn build_indexes(root: &Path, journal: &mut Journal, job: &Job) -> Result<()> {
    crate::setup::ensure_navigation(root, journal, job)?;
    build_indexes_impl(root, journal, job, false, true)
}

/// Complete a minimal drive without replacing existing user indexes or name
/// mappings: only index files that are missing or empty are written.
pub fn build_missing_indexes(root: &Path, journal: &mut Journal, job: &Job) -> Result<()> {
    let all_present = [
        ".ohmy3do/library.3di",
        ".ohmy3do/metadata.3di",
        ".ohmy3do/save-names.txt",
    ]
    .iter()
    .all(|name| {
        f::safe(root, name).is_ok_and(|path| {
            path.is_file() && path.metadata().is_ok_and(|metadata| metadata.len() > 0)
        })
    });
    if all_present {
        return Ok(());
    }
    build_indexes_impl(root, journal, job, true, true)
}

/// Writes an index file through the journal; with `missing_only`, only if
/// it is missing or empty.
fn write_index(
    root: &Path,
    journal: &mut Journal,
    name: &str,
    data: &[u8],
    missing_only: bool,
) -> Result<()> {
    if missing_only {
        let present = f::safe(root, name)?
            .metadata()
            .is_ok_and(|metadata| metadata.is_file() && metadata.len() > 0);
        if present {
            return Ok(());
        }
    }
    journal.write(name, data)
}

/// Removing discs adds no save names. Keep the existing mappings because the
/// saves themselves remain, and avoid rereading every remaining disc.
pub fn build_indexes_after_removal(root: &Path, journal: &mut Journal, job: &Job) -> Result<()> {
    crate::setup::ensure_navigation(root, journal, job)?;
    build_indexes_impl(root, journal, job, false, false)
}

/// Writes the menu's index files for the discs under `root`:
///
/// - `library.3di`: every disc's path, for Find (see [`find_index`]);
/// - each disc's description file (see `game_info::files`);
/// - `metadata.3di`: each disc's genre, publisher and date
///   (see [`metadata_bytes`]);
/// - with `scan_save_names`, `save-names.txt` and its cache: the save file
///   names each game uses, for the Save manager (see `saves`).
///
/// With `missing_only`, existing non-empty files are kept.
fn build_indexes_impl(
    root: &Path,
    journal: &mut Journal,
    job: &Job,
    missing_only: bool,
    scan_save_names: bool,
) -> Result<()> {
    let discs = d::collect(&[root.to_owned()], None, job)?;
    let mut names = discs
        .iter()
        .map(|(path, _)| relative(root, path))
        .collect::<Result<Vec<_>>>()?;
    names.sort_by_key(|name| f::fold(name));
    let distinct: HashSet<String> = names.iter().map(|name| f::fold(name)).collect();
    if distinct.len() != names.len() {
        bail!("Case-colliding disc names")
    }

    write_index(
        root,
        journal,
        ".ohmy3do/library.3di",
        &find_index(&names)?,
        missing_only,
    )?;

    let database = database(root)?;
    for (name, data) in crate::game_info::files(&database)? {
        write_index(root, journal, &name, &data, missing_only)?;
    }
    write_index(
        root,
        journal,
        ".ohmy3do/metadata.3di",
        &metadata_bytes(&database, &names)?,
        missing_only,
    )?;

    if !scan_save_names {
        return Ok(());
    }
    let titles = database["games"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|game| {
            Some((
                f::fold(game["path"].as_str()?),
                game["title"].as_str()?.to_owned(),
            ))
        })
        .collect();
    let saves = crate::saves::save_names_text(root, &names, &titles, job)?;
    write_index(
        root,
        journal,
        ".ohmy3do/save-names-cache.json",
        &saves.cache,
        missing_only,
    )?;
    write_index(
        root,
        journal,
        ".ohmy3do/save-names.txt",
        saves.text.as_bytes(),
        missing_only,
    )?;
    job.say(format!(
        "Indexed {} save names for the Save manager",
        saves.count
    ));
    Ok(())
}

/// The Find index ("OM3DLIB1"): the header (see [`index_header`]), each
/// disc's path as text (see [`push_text`]) in case-folded order, and
/// "END3DL01". Checks the menu's limits: path length and depth, and the
/// index's size with room for the menu's own per-disc data.
fn find_index(names: &[String]) -> Result<Vec<u8>> {
    let mut payload = vec![];
    for name in names {
        if name.len() > 8192 || name.split('/').count() > 33 {
            bail!("Disc path exceeds navigation limits")
        }
        push_text(&mut payload, name.as_bytes());
    }
    if names.len() > MAX_INDEXED_DISCS || payload.len() + 9 * names.len() > 128 * 1024 {
        bail!("Find index exceeds menu capacity")
    }

    let mut index = index_header(b"OM3DLIB1", names.len(), &payload);
    index.extend(payload);
    index.extend_from_slice(b"END3DL01");
    Ok(index)
}

/// The longest metadata text field, in characters.
const MAX_METADATA_TEXT: usize = 96;

/// The largest metadata index payload, with room for the menu's own
/// per-disc data.
const MAX_METADATA_PAYLOAD: usize = 192 * 1024;

/// A metadata field as the console's font can show it: plain ASCII
/// (accents dropped, control characters as spaces), single-spaced, at most
/// 96 characters.
fn metadata_text(value: &Value) -> String {
    use unicode_normalization::UnicodeNormalization;
    let ascii: String = value
        .as_str()
        .unwrap_or("")
        .nfkd()
        .filter(|c| c.is_ascii())
        .map(|c| if c.is_ascii_control() { ' ' } else { c })
        .collect();
    ascii
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(MAX_METADATA_TEXT)
        .collect()
}

/// A disc's release date for the console: "YYYY-MM-DD" if the metadata has
/// a real calendar date, else its year if sensible (1000-2999), else "".
fn metadata_date(meta: &Value) -> String {
    let date = meta["release_date"]
        .as_str()
        .and_then(|text| text.get(..10))
        .filter(|date| rx(r"^\d{4}-\d{2}-\d{2}$").is_match(date));
    if let Some(date) = date {
        let year = date[..4].parse::<u32>().unwrap_or(0);
        let month = date[5..7].parse::<usize>().unwrap_or(0);
        let day = date[8..10].parse::<u32>().unwrap_or(0);
        let leap_year = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        let february = if leap_year { 29 } else { 28 };
        // Days in each month; index 0 is unused.
        let days_in_month = [0, 31, february, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
        if year > 0 && month <= 12 && day > 0 && day <= days_in_month[month] {
            return date.into();
        }
    }

    let year = meta["year"]
        .as_i64()
        .or_else(|| meta["year"].as_str().and_then(|text| text.parse().ok()));
    year.filter(|year| (1000..3000).contains(year))
        .map(|year| year.to_string())
        .unwrap_or_default()
}

/// The metadata index ("OM3DMET1") for the discs `names`: for each, in
/// case-folded path order, its path, genre, publisher and release date (see
/// [`push_text`]). After the header (see [`index_header`]) come the entries
/// and "END3DM01".
///
/// Every disc gets an entry, even without a database record (the menu uses
/// it to recognize folders); its fields are then empty. The result is
/// checked with [`metadata_paths`] before it is returned.
pub fn metadata_bytes(database: &Value, names: &[String]) -> Result<Vec<u8>> {
    let disc_paths: HashMap<String, &String> =
        names.iter().map(|path| (f::fold(path), path)).collect();
    let mut records: BTreeMap<String, (String, [String; 3])> = names
        .iter()
        .map(|path| {
            (
                f::fold(path),
                (path.clone(), [String::new(), String::new(), String::new()]),
            )
        })
        .collect();

    let mut seen = HashSet::new();
    for record in database["games"]
        .as_array()
        .context("Invalid library records")?
    {
        let key = f::fold(record["path"].as_str().unwrap_or(""));
        let Some(path) = disc_paths.get(&key) else {
            continue;
        };
        let meta = &record["metadata"];
        let fields = [
            metadata_text(&meta["genre"]),
            metadata_text(&meta["publisher"]),
            metadata_date(meta),
        ];
        if !seen.insert(key.clone()) {
            bail!("Duplicate metadata path");
        }
        records.insert(key, ((*path).clone(), fields));
    }

    let count = records.len();
    let mut payload = vec![];
    for (path, fields) in records.values() {
        for field in std::iter::once(path).chain(fields.iter()) {
            if field.len() > 8192 || field.contains('\0') {
                bail!("Invalid metadata field");
            }
            push_text(&mut payload, field.as_bytes());
        }
    }
    if count > MAX_INDEXED_DISCS || payload.len() + 16 * count > MAX_METADATA_PAYLOAD {
        bail!("Console metadata index exceeds capacity");
    }

    let mut index = index_header(b"OM3DMET1", count, &payload);
    index.extend_from_slice(&payload);
    index.extend_from_slice(b"END3DM01");
    metadata_paths(&index)?;
    Ok(index)
}

/// Checks a metadata index as the menu does (sizes, CRC, every text field,
/// safe paths in strictly increasing case-folded order) and returns the
/// disc paths it lists.
pub fn metadata_paths(data: &[u8]) -> Result<Vec<String>> {
    if data.len() < 32 || &data[..8] != b"OM3DMET1" || &data[data.len() - 8..] != b"END3DM01" {
        bail!("Invalid metadata header/footer");
    }
    let word = |position: usize| {
        u32::from_be_bytes(data[position..position + 4].try_into().unwrap()) as usize
    };
    let count = word(8);
    let payload_size = word(12);
    if count > MAX_INDEXED_DISCS
        || payload_size > MAX_METADATA_PAYLOAD
        || payload_size + 16 * count > MAX_METADATA_PAYLOAD
        || data.len() != payload_size + 32
        || word(20) != 0
        || crc32fast::hash(&data[24..data.len() - 8]) as usize != word(16)
    {
        bail!("Invalid metadata checksum/bounds");
    }

    let end_of_entries = data.len() - 8;
    let mut paths = Vec::<String>::new();
    let mut position = 24;
    for _ in 0..count {
        // Four fields: path, genre, publisher, date.
        for field in 0..4 {
            let length = data
                .get(position..position + 2)
                .context("Truncated metadata")?;
            let length = u16::from_be_bytes(length.try_into().unwrap()) as usize;
            position += 2;
            let text = data
                .get(position..position + length)
                .context("Truncated metadata string")?;
            position += length;
            if position > end_of_entries
                || length == 0
                || text[length - 1] != 0
                || text[..length - 1].contains(&0)
            {
                bail!("Invalid metadata string");
            }
            let text = &text[..length - 1];

            if field == 0 {
                let path = std::str::from_utf8(text)?;
                if !is_valid_metadata_path(path, paths.last()) {
                    bail!("Invalid or unsorted metadata path");
                }
                paths.push(path.into());
            } else if length > MAX_METADATA_TEXT + 1
                || text.iter().any(|byte| !(32..=126).contains(byte))
            {
                bail!("Invalid metadata text");
            }
        }
    }

    if position != end_of_entries {
        bail!("Trailing metadata");
    }
    Ok(paths)
}

/// Whether a metadata index path is one the menu can use: a relative path
/// to an ISO or CUE, at most 33 levels and 8 KiB long, with no empty, "."
/// or ".." parts, no part of 1 KiB or more, no control characters, "\" or
/// ":", and after `previous` in case-folded order.
fn is_valid_metadata_path(path: &str, previous: Option<&String>) -> bool {
    let parts_valid = path
        .split('/')
        .all(|part| !part.is_empty() && part != "." && part != ".." && part.len() < 1024);
    let characters_valid = !path
        .chars()
        .any(|c| c.is_control() || c == '\\' || c == ':');
    let folded = f::fold(path);
    let is_disc = folded.ends_with(".iso") || folded.ends_with(".cue");
    let in_order = previous.is_none_or(|previous| f::fold(previous) < folded);

    path.len() <= 8192
        && path.split('/').count() <= 33
        && parts_valid
        && characters_valid
        && is_disc
        && in_order
}

// ---- The library database ----

/// The largest library database accepted.
const MAX_DATABASE_BYTES: u64 = 16 * 1024 * 1024;

/// Reads the library database (`.ohmy3do/library.json`), or an empty one if
/// it does not exist yet.
pub fn database(root: &Path) -> Result<Value> {
    let path = f::safe(root, ".ohmy3do/library.json")?;
    if !path.exists() {
        return Ok(json!({"schema": 1, "sources": [], "games": []}));
    }
    if fs::metadata(&path)?.len() > MAX_DATABASE_BYTES {
        bail!("Oversized library database")
    }
    let data: Value = serde_json::from_slice(&fs::read(path)?)?;
    if data["schema"] != 1 || !data["games"].is_array() {
        bail!("Invalid library database")
    }
    Ok(data)
}

/// Saves the library database through the journal: `records` replace any
/// record with the same path (or are added), records are sorted by path,
/// and new `sources` are added. Also writes `library.sqlite`, the same
/// records as an SQLite database for people who want to query them.
pub fn write_database(
    journal: &mut Journal,
    mut database: Value,
    records: Vec<Value>,
    sources: &[Value],
) -> Result<()> {
    let games = database["games"]
        .as_array_mut()
        .context("Invalid library records")?;
    for record in records {
        let path = record["path"].as_str().context("Missing disc path")?;
        games.retain(|existing| existing["path"].as_str() != Some(path));
        games.push(record);
    }
    games.sort_by(|a, b| a["path"].as_str().cmp(&b["path"].as_str()));

    let known_sources = database["sources"]
        .as_array_mut()
        .context("Missing database sources")?;
    for source in sources {
        if !known_sources.contains(source) {
            known_sources.push(source.clone());
        }
    }

    database["generated"] = json!(std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs_f64());
    journal.write(
        ".ohmy3do/library.json",
        &serde_json::to_vec_pretty(&database)?,
    )?;

    let sqlite = sqlite_copy(&database)?;
    journal.write(".ohmy3do/library.sqlite", &sqlite)?;
    Ok(())
}

/// The database's records and sources as an SQLite file's bytes.
fn sqlite_copy(database: &Value) -> Result<Vec<u8>> {
    let folder = tempfile::tempdir()?;
    let path = folder.path().join("library.sqlite");
    {
        let mut connection = rusqlite::Connection::open(&path)?;
        let transaction = connection.transaction()?;
        transaction.execute_batch(
            "CREATE TABLE games (path TEXT PRIMARY KEY, title TEXT, region TEXT, disc INTEGER, \
             genre TEXT, publisher TEXT, year INTEGER, developer TEXT, identification TEXT, \
             cover TEXT, metadata_source TEXT, variant TEXT, catalog_name TEXT, \
             edition_unresolved INTEGER, category TEXT, category_review INTEGER, \
             category_reason TEXT); \
             CREATE TABLE sources (url TEXT, sha256 TEXT);",
        )?;

        for source in database["sources"].as_array().unwrap() {
            transaction.execute(
                "INSERT INTO sources VALUES (?,?)",
                rusqlite::params![source["url"].as_str(), source["sha256"].as_str()],
            )?;
        }

        for record in database["games"].as_array().unwrap() {
            let meta = &record["metadata"];
            transaction.execute(
                "INSERT INTO games VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                rusqlite::params![
                    record["path"].as_str(),
                    record["title"].as_str(),
                    record["region"].as_str(),
                    record["disc"].as_i64(),
                    meta["genre"].as_str(),
                    meta["publisher"].as_str(),
                    meta["year"].as_i64(),
                    meta["developer"].as_str(),
                    record["identification"].as_str(),
                    record["cover"].as_str(),
                    meta["source"].as_str(),
                    record["variant"].as_str(),
                    record["catalog_name"].as_str(),
                    record["edition_unresolved"].as_bool().unwrap_or(false) as i32,
                    record["category"].as_str(),
                    record["category_review"].as_bool().unwrap_or(true) as i32,
                    record["category_reason"].as_str()
                ],
            )?;
        }
        transaction.commit()?;
    }
    Ok(fs::read(path)?)
}

/// Adds a prepared media file; an existing file with other contents is an error.
fn write_asset(tx: &mut Journal, root: &Path, relative: &str, source: &Path) -> Result<()> {
    let bytes = fs::read(source)?;
    let path = f::safe(root, relative)?;
    if path.exists() && fs::read(&path)? != bytes {
        bail!("Existing media differs")
    };
    tx.write(relative, &bytes)
}
pub fn import(plan: &Plan, prefs: &Preferences, cache: &Path, job: &Job) -> Result<PathBuf> {
    let root = f::root(&plan.root)?;
    let _lock = lock_library(&root, cache)?;
    let mut entries = plan
        .entries
        .iter()
        .filter(|e| e.selected)
        .cloned()
        .collect::<Vec<_>>();
    if entries.is_empty() {
        bail!("Check at least one resolved entry")
    }
    let mut targets = HashSet::new();
    let mut sources = HashSet::new();
    for entry in &entries {
        if !entry.resolvable() {
            bail!(
                "Choose a category or identify {} before importing{}",
                entry.title,
                if entry.blocked {
                    format!(": {}", entry.reason)
                } else {
                    String::new()
                }
            )
        }
        for file in &entry.files {
            let path = f::safe(&root, &file.target)?;
            if path.exists() || !targets.insert(f::fold(&file.target)) {
                bail!("Destination is occupied: {}", file.target)
            }
            if !sources.insert(file.source.clone()) {
                bail!("Entries share a source data file")
            };
            if file.source.canonicalize()?.starts_with(&root) {
                bail!("Import source is already inside the library")
            }
            if f::stamp(&file.source)? != (file.fingerprint.size, file.fingerprint.mtime_ns) {
                bail!("Source changed since scan")
            }
        }
    }
    let required = entries
        .iter()
        .flat_map(|e| e.files.iter())
        .map(|f| f.fingerprint.size)
        .sum::<u64>();
    if fs2::available_space(&root)? < required + 16 * 1024 * 1024 {
        bail!("Insufficient free space for the import")
    }
    let mut db = database(&root)?;
    let mut covers = read_media_index(&root, "covers")?;
    let mut previews = read_media_index(&root, "previews")?;
    let mut prepared = media::Outputs::new();
    for entry in &mut entries {
        job.check()?;
        media::prepare_entry(entry, prefs, cache, &mut prepared, job)?;
    }
    let category_config = categories::load(&root)?;
    for entry in &entries {
        anyhow::ensure!(
            category_config.contains(&entry.category),
            "Unknown category: {}",
            entry.category
        );
    }
    let marker = f::safe(&root, ".ohmy3do/ohmy3do.ini")?;
    let mut tx = Journal::new(&root, cache)?;
    job.say(format!("Undo journal: {}", tx.path.display()));
    let result = (|| -> Result<()> {
        if !root.join(categories::PATH).exists() {
            tx.write(categories::PATH, &category_config.encode()?)?;
        }
        if !marker.exists() {
            tx.write(".ohmy3do/ohmy3do.ini", include_bytes!("../../ohmy3do.ini"))?;
        }
        let mut records = vec![];
        let import_total = entries
            .iter()
            .flat_map(|entry| &entry.files)
            .map(|file| file.fingerprint.size)
            .sum::<u64>()
            .max(1);
        let mut imported_bytes = 0u64;
        job.report(0, import_total, "Importing and verifying discs");
        for (n, entry) in entries.iter().enumerate() {
            job.check()?;
            job.say(format!(
                "Importing {}/{}: {}",
                n + 1,
                entries.len(),
                entry.title
            ));
            for file in &entry.files {
                job.say(format!("Copying and verifying {}", file.source.display()));
                if f::stamp(&file.source)? != (file.fingerprint.size, file.fingerprint.mtime_ns) {
                    bail!("Source changed since scan: {}", file.source.display())
                }
                tx.copy(
                    &file.source,
                    &file.target,
                    Some(&file.fingerprint.sha256),
                    &job.progress_range(
                        imported_bytes,
                        file.fingerprint.size,
                        import_total,
                        "Importing and verifying discs",
                    ),
                )?;
                imported_bytes += file.fingerprint.size;
                job.report(
                    imported_bytes,
                    import_total,
                    "Importing and verifying discs",
                );
            }
            if entry
                .source
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .eq_ignore_ascii_case("cue")
            {
                let copied = fs::read(root.join(&entry.target))?;
                let expected = tx
                    .created
                    .iter()
                    .find(|file| file.path == entry.target)
                    .context("Missing copied CUE receipt")?;
                if format!("{:x}", Sha256::digest(&copied)) != expected.sha256 {
                    bail!("Copied CUE changed during import")
                }
                let text = d::decode_cue(&copied)?;
                let mapping = entry
                    .files
                    .iter()
                    .map(|f| {
                        (
                            f.source
                                .file_name()
                                .unwrap()
                                .to_string_lossy()
                                .to_lowercase(),
                            Path::new(&f.target)
                                .file_name()
                                .unwrap()
                                .to_string_lossy()
                                .into_owned(),
                        )
                    })
                    .collect::<HashMap<_, _>>();
                let text = rx(r#"(?im)^(\s*FILE\s+)(?:"([^"\r\n]+)"|(\S+))(\s+\S+[^\r\n]*)"#)
                    .replace_all(&text, |c: &regex::Captures<'_>| {
                        format!(
                            "{}\"{}\"{}",
                            &c[1],
                            mapping
                                .get(
                                    &c.get(2)
                                        .or_else(|| c.get(3))
                                        .unwrap()
                                        .as_str()
                                        .to_lowercase()
                                )
                                .unwrap(),
                            &c[4]
                        )
                    })
                    .into_owned();
                tx.write(&entry.target, text.as_bytes())?;
            }
            let game = entry.game.map(|i| &plan.catalog.games[i]);
            let metadata = game
                .map(|g| g.metadata.clone())
                .unwrap_or_else(|| json!({}));
            let mut record = json!({"path":entry.target,"title":entry.title,"region":entry.region,"category":entry.category,"category_review":false,"category_reason":entry.category_reason,"catalog_name":game.map(|g|&g.name),"variant":game.and_then(|g|g.variant.as_ref()),"edition_unresolved":false,"disc":d::disc_number(&entry.title),"identification":entry.confidence,"status":if entry.confidence=="unverified"{"registered"}else{"organized"},"metadata":metadata,"cover":null,"cover_source":null,"preview":null,"preview_source":null,"suggestions":[]});
            if entry.confidence == "unverified" {
                let mut proof = serde_json::Map::new();
                for file in &entry.files {
                    let fingerprint = d::fingerprint(&root.join(&file.target), false, job)?;
                    proof.insert(file.target.clone(),json!({"size":fingerprint.size,"mtime_ns":fingerprint.mtime_ns,"sha1":fingerprint.sha1}));
                }
                record["registration"] = Value::Object(proof);
            }
            for (kind, map) in [("cover", &mut covers), ("preview", &mut previews)] {
                if let Some(asset) = prepared.get(&(entry.target.clone(), kind.to_owned())) {
                    map.insert(f::fold(&entry.target), asset.name.clone());
                    record[kind] = json!(asset.name);
                    record[format!("{kind}_source")] = json!(asset.source);
                }
            }
            records.push(record);
        }
        let mut written = HashSet::new();
        job.phase("Saving library metadata and indexes");
        for ((_, kind), asset) in &prepared {
            let folder = if kind == "cover" {
                "covers"
            } else {
                "previews"
            };
            let relative = format!(".ohmy3do/{folder}/{}", asset.name);
            if written.insert(relative.clone()) {
                write_asset(&mut tx, &root, &relative, &asset.path)?;
                if let Some(large) = &asset.large {
                    write_asset(
                        &mut tx,
                        &root,
                        &format!(".ohmy3do/covers/{}", media::large_name(&asset.name)),
                        large,
                    )?;
                }
            }
        }
        if !covers.is_empty() {
            tx.write(".ohmy3do/covers/index.3di", &media_index(&covers)?)?
        }
        if !previews.is_empty() {
            tx.write(".ohmy3do/previews/index.3di", &media_index(&previews)?)?
        }
        write_database(&mut tx, db.take(), records, &plan.catalog.sources)?;
        build_indexes(&root, &mut tx, job)?;
        job.check()?;
        tx.finish()?;
        Ok(())
    })();
    if let Err(error) = result {
        job.say("Restoring the library after interrupted import");
        if let Err(undo) = tx.undo() {
            bail!(
                "{error:#}. Automatic rollback could not finish: {undo:#}. Keep {}",
                tx.path.display()
            )
        }
        return Err(error);
    }
    job.say(format!(
        "Imported {} disc entries; originals unchanged",
        entries.len()
    ));
    Ok(tx.path)
}

pub fn update_media(
    root: &Path,
    prefs: &Preferences,
    cache: &Path,
    provided: Option<(&str, Option<PathBuf>, Option<PathBuf>)>,
    job: &Job,
) -> Result<PathBuf> {
    update_media_mode(root, prefs, cache, provided, false, None, job)
}
/// One transaction for the chosen discs; unrelated records and media are retained.
pub fn update_selected_media(
    root: &Path,
    prefs: &Preferences,
    cache: &Path,
    discs: &[String],
    job: &Job,
) -> Result<PathBuf> {
    let db = database(root)?;
    let available: HashSet<_> = db["games"]
        .as_array()
        .context("Invalid library")?
        .iter()
        .filter_map(|r| r["path"].as_str().map(f::fold))
        .collect();
    let selected: HashSet<_> = discs.iter().map(|p| f::fold(p)).collect();
    anyhow::ensure!(
        !selected.is_empty() && selected.iter().all(|p| available.contains(p)),
        "Select existing library games first"
    );
    update_media_mode(root, prefs, cache, None, false, Some(&selected), job)
}
type MediaWork = (Value, String, Entry, bool, bool);
/// Prepares media for many discs at once, one disc per worker thread at a
/// time. A preview takes about 100 seconds of one core to encode.
fn prepare_parallel(
    work: &mut [MediaWork],
    prefs: &Preferences,
    cache: &Path,
    outputs: &mut media::Outputs,
    job: &Job,
) -> Result<()> {
    use std::sync::atomic::{AtomicBool, Ordering::Relaxed};
    let total = work.iter().filter(|w| w.3 || w.4).count();
    // Leave one core for the interface.
    let workers = std::thread::available_parallelism()
        .map_or(1, |n| n.get().saturating_sub(1).max(1))
        .min(total.max(1));
    let queue = std::sync::Mutex::new(work.iter_mut());
    let (done, failed) = (std::sync::Mutex::new(0usize), AtomicBool::new(false));
    let worker_job = job.quiet_progress();
    job.report(0, total as u64, "Preparing media");
    let results = std::thread::scope(|s| {
        let handles = (0..workers)
            .map(|_| {
                s.spawn(|| -> Result<media::Outputs> {
                    let mut out = media::Outputs::new();
                    while !failed.load(Relaxed) {
                        let Some((_, path, entry, cover, preview)) = queue.lock().unwrap().next()
                        else {
                            break;
                        };
                        if !(*cover || *preview) {
                            continue;
                        }
                        let result = media::prepare_missing(
                            entry,
                            prefs,
                            cache,
                            &mut out,
                            *cover,
                            *preview,
                            &worker_job,
                        );
                        if result.is_err() {
                            failed.store(true, Relaxed);
                            result?;
                        }
                        let mut completed = done.lock().unwrap();
                        *completed += 1;
                        let n = *completed;
                        job.report(n as u64, total as u64, "Preparing media");
                        job.say(format!("Media {n} of {total} ready: {path}"));
                    }
                    Ok(out)
                })
            })
            .collect::<Vec<_>>();
        handles
            .into_iter()
            .map(|h| {
                h.join()
                    .unwrap_or_else(|_| bail!("Media worker stopped unexpectedly"))
            })
            .collect::<Vec<_>>()
    });
    for result in results {
        outputs.extend(result?);
    }
    job.phase("Saving prepared media");
    Ok(())
}
pub fn rebuild_media(root: &Path, prefs: &Preferences, cache: &Path, job: &Job) -> Result<PathBuf> {
    update_media_mode(root, prefs, cache, None, true, None, job)
}
fn update_media_mode(
    root: &Path,
    prefs: &Preferences,
    cache: &Path,
    provided: Option<(&str, Option<PathBuf>, Option<PathBuf>)>,
    rebuild: bool,
    selected: Option<&HashSet<String>>,
    job: &Job,
) -> Result<PathBuf> {
    let root = f::root(root)?;
    let _lock = lock_library(&root, cache)?;
    let mut db = database(&root)?;
    let include_info = provided.is_none() && !rebuild;
    let info_changed = if include_info {
        crate::game_info::fill_missing(&mut db, prefs, cache, selected, job)?
    } else {
        0
    };
    let mut covers = read_media_index(&root, "covers")?;
    let mut previews = read_media_index(&root, "previews")?;
    let mut outputs = media::Outputs::new();
    let mut records = vec![];
    // Choose each disc's sources in library order, encode on every core, then
    // record the results in library order again.
    let mut work = vec![];
    for record in db["games"].as_array().context("Invalid library records")? {
        job.check()?;
        let path = record["path"].as_str().context("Missing disc path")?;
        if selected.is_some_and(|set| !set.contains(&f::fold(path))) {
            continue;
        }
        let source = f::safe(&root, path)?;
        if !source.is_file() {
            bail!("Missing database disc: {path}")
        }
        if let Some((selected, _, _)) = &provided {
            if path != *selected {
                continue;
            }
        }
        let mut entry = Entry {
            source,
            title: record["title"].as_str().unwrap_or(path).into(),
            category: record["category"].as_str().unwrap_or("Games").into(),
            region: record["region"].as_str().unwrap_or("Unknown").into(),
            target: path.into(),
            confidence: String::new(),
            reason: String::new(),
            selected: false,
            files: vec![],
            candidates: vec![],
            game: None,
            cover: provided.as_ref().and_then(|(_, c, _)| c.clone()),
            preview: provided.as_ref().and_then(|(_, _, p)| p.clone()),
            category_reason: String::new(),
            blocked: false,
        };
        if rebuild {
            for (kind, destination) in
                [("cover", &mut entry.cover), ("preview", &mut entry.preview)]
            {
                if let Some(origin) = record[format!("{kind}_source")]
                    .as_str()
                    .filter(|s| !s.is_empty())
                {
                    match media::original_source(kind, origin, prefs, cache, job) {
                        Ok(source) => {
                            if source.is_none() {
                                job.say(format!(
                                    "Original {kind} unavailable; kept existing: {path}"
                                ));
                            }
                            *destination = source;
                        }
                        Err(e) => {
                            job.check()?;
                            job.say(format!(
                                "Original {kind} unavailable; kept existing: {path}: {e:#}"
                            ));
                        }
                    }
                }
            }
        }
        let existing =
            |kind: &str, map: &std::collections::BTreeMap<String, String>| -> Result<bool> {
                let name = map
                    .get(&f::fold(path))
                    .map(String::as_str)
                    .or_else(|| record[kind].as_str());
                Ok(if let Some(name) = name {
                    f::safe(
                        &root,
                        &format!(
                            ".ohmy3do/{}/{name}",
                            if kind == "cover" {
                                "covers"
                            } else {
                                "previews"
                            }
                        ),
                    )?
                    .is_file()
                } else {
                    false
                })
            };
        let want_cover = entry.cover.is_some() || (!rebuild && !existing("cover", &covers)?);
        let want_preview =
            entry.preview.is_some() || (!rebuild && !existing("preview", &previews)?);
        work.push((
            record.clone(),
            path.to_owned(),
            entry,
            want_cover,
            want_preview,
        ));
    }
    prepare_parallel(&mut work, prefs, cache, &mut outputs, job)?;
    for (record, path, ..) in &work {
        let path = path.as_str();
        let mut changed = record.clone();
        let mut dirty = false;
        for (kind, map) in [("cover", &mut covers), ("preview", &mut previews)] {
            if let Some(asset) = outputs.get(&(path.into(), kind.into())) {
                map.insert(f::fold(path), asset.name.clone());
                changed[kind] = json!(asset.name);
                changed[format!("{kind}_source")] = if rebuild {
                    record[format!("{kind}_source")].clone()
                } else {
                    json!(asset.source)
                };
                dirty = true;
            }
        }
        if dirty {
            records.push(changed)
        }
    }
    let description_files = if include_info {
        crate::game_info::files_for(&db, selected)?
    } else {
        BTreeMap::new()
    };
    let metadata_file = if include_info
        && (info_changed > 0
            || (!description_files.is_empty() && !root.join(".ohmy3do/metadata.3di").is_file()))
    {
        Some(crate::game_info::console_metadata(&root, &db)?)
    } else {
        None
    };
    let mut tx = Journal::new(&root, cache)?;
    let result = (|| -> Result<()> {
        let mut written = HashSet::new();
        for ((_, kind), asset) in &outputs {
            let name = format!(
                ".ohmy3do/{}/{}",
                if kind == "cover" {
                    "covers"
                } else {
                    "previews"
                },
                asset.name
            );
            if written.insert(name.clone()) {
                write_asset(&mut tx, &root, &name, &asset.path)?;
                if let Some(large) = &asset.large {
                    write_asset(
                        &mut tx,
                        &root,
                        &format!(".ohmy3do/covers/{}", media::large_name(&asset.name)),
                        large,
                    )?;
                }
            }
        }
        for (name, data) in &description_files {
            job.check()?;
            let path = f::safe(&root, name)?;
            if !path.is_file()
                || fs::metadata(&path)?.len() != data.len() as u64
                || fs::read(&path)? != *data
            {
                tx.write(name, data)?;
            }
        }
        if let Some(bytes) = &metadata_file {
            tx.write(".ohmy3do/metadata.3di", bytes)?;
        }
        if !records.is_empty() || info_changed > 0 {
            if !covers.is_empty() {
                tx.write(".ohmy3do/covers/index.3di", &media_index(&covers)?)?;
            }
            if !previews.is_empty() {
                tx.write(".ohmy3do/previews/index.3di", &media_index(&previews)?)?;
            }
            let sources = db["sources"].as_array().cloned().unwrap_or_default();
            write_database(&mut tx, db.take(), records, &sources)?;
        }
        job.check()?;
        tx.finish()
    })();
    if let Err(error) = result {
        tx.undo()?;
        return Err(error);
    }
    job.say(format!(
        "Media update complete: {} artwork associations prepared; {} missing descriptions found; {} description files ready; existing media retained",
        outputs.len(),info_changed,description_files.len()
    ));
    Ok(tx.path)
}
/// Text with every whole-line path in `map` replaced; menu text ends at its first NUL.
pub fn remap_lines(raw: &[u8], map: &HashMap<String, String>) -> Option<Vec<u8>> {
    let text = String::from_utf8_lossy(raw.split(|b| *b == 0).next().unwrap_or(&[])).into_owned();
    let mut changed = false;
    let out: String = text
        .split_inclusive('\n')
        .map(|line| {
            let key = line.trim();
            match map.get(&f::fold(key.trim_start_matches('\u{feff}'))) {
                Some(new) => {
                    changed = true;
                    line.replacen(key.trim_start_matches('\u{feff}'), new, 1)
                }
                None => line.to_owned(),
            }
        })
        .collect();
    changed.then(|| out.into_bytes())
}

/// The saved menu position after discs move: it follows a moved disc, and
/// returns to the USB root when it was inside a moved folder.
pub fn remap_position(
    raw: &[u8],
    map: &HashMap<String, String>,
    folders: &[String],
) -> Option<Vec<u8>> {
    let text = String::from_utf8_lossy(raw.split(|b| *b == 0).next().unwrap_or(&[])).into_owned();
    let lines: Vec<&str> = text.lines().collect();
    if lines.len() != 3 || lines[0] != "OH3DO_POSITION1" {
        return None;
    }
    let folder = lines[1].strip_prefix("/Storage/").unwrap_or("");
    let current = if folder.is_empty() {
        lines[2].to_owned()
    } else {
        format!("{folder}/{}", lines[2])
    };
    if let Some(new) = map.get(&f::fold(&current)) {
        let (dir, name) = new.rsplit_once('/').unwrap_or(("", new));
        return Some(format!("OH3DO_POSITION1\n/Storage/{dir}\n{name}\n").into_bytes());
    }
    folders
        .iter()
        .any(|old| {
            f::fold(&current).starts_with(&f::fold(&format!("{old}/")))
                || f::fold(folder) == f::fold(old)
        })
        .then(|| b"OH3DO_POSITION1\n/Storage\n\n".to_vec())
}

/// Moves whole game folders (Category/Region/Game) to another category,
/// updating the database, media indexes, favorites, recent games, saved
/// position and Find. One undo journal covers it all.
pub fn recategorize(
    root: &Path,
    discs: &[String],
    category: &str,
    cache: &Path,
    job: &Job,
) -> Result<PathBuf> {
    let config = categories::load(root)?;
    let category = config
        .categories
        .iter()
        .find(|c| c.name.eq_ignore_ascii_case(category))
        .context("Unknown category")?
        .name
        .as_str();
    let _lock = lock_library(root, cache)?;
    let db = database(root)?;
    let mut folders = BTreeMap::new();
    for disc in discs {
        let parts: Vec<&str> = disc.split('/').collect();
        if parts.len() < 4 || !config.contains(parts[0]) {
            bail!("Not in a Category/Region/Game folder: {disc}")
        }
        if parts[0] != category {
            folders.insert(
                parts[..3].join("/"),
                format!("{category}/{}", parts[1..3].join("/")),
            );
        }
    }
    if folders.is_empty() {
        bail!("Already in {category}")
    }
    move_category_folders(root, db, folders, None, cache, job)
}
pub fn configure_categories(
    root: &Path,
    old: &categories::Categories,
    new: &categories::Categories,
    cache: &Path,
    job: &Job,
) -> Result<PathBuf> {
    let _lock = lock_library(root, cache)?;
    anyhow::ensure!(
        &categories::load(root)? == old,
        "Categories changed on disk; reopen the editor"
    );
    let mut folders = BTreeMap::new();
    for category in &old.categories {
        let previous = f::safe(root, &category.name)?;
        anyhow::ensure!(
            !previous.exists() || previous.is_dir(),
            "Category path is occupied by a file: {}",
            category.name
        );
        if let Some(next) = new.categories.iter().find(|c| c.id == category.id) {
            if next.name != category.name && previous.exists() {
                anyhow::ensure!(
                    !f::safe(root, &next.name)?.exists(),
                    "Destination category already exists: {}",
                    next.name
                );
                folders.insert(category.name.clone(), next.name.clone());
            }
        } else {
            anyhow::ensure!(
                !previous.exists()
                    || (previous.is_dir() && fs::read_dir(&previous)?.next().is_none()),
                "Move games out of {} before removing it",
                category.name
            );
            if previous.exists() {
                folders.insert(
                    category.name.clone(),
                    format!(".ohmy3do/removed-categories/{}", uuid::Uuid::new_v4()),
                );
            }
        }
    }
    move_category_folders(root, database(root)?, folders, Some(new), cache, job)
}
fn move_category_folders(
    root: &Path,
    mut db: Value,
    folders: BTreeMap<String, String>,
    config: Option<&categories::Categories>,
    cache: &Path,
    job: &Job,
) -> Result<PathBuf> {
    let mut map = HashMap::new();
    for record in db["games"]
        .as_array_mut()
        .context("Invalid library records")?
    {
        let path = record["path"].as_str().unwrap_or("").to_owned();
        if let Some((old, new)) = folders
            .iter()
            .find(|(old, _)| path.starts_with(&format!("{old}/")))
        {
            let moved = format!("{new}{}", &path[old.len()..]);
            map.insert(f::fold(&path), moved.clone());
            record["path"] = json!(moved);
            record["category"] = json!(new.split('/').next().unwrap());
            record["category_reason"] = json!("Explicit category override");
            record["category_review"] = json!(false);
        }
    }
    let mut covers = read_media_index(root, "covers")?;
    let mut previews = read_media_index(root, "previews")?;
    let mut tx = Journal::new(root, cache)?;
    job.say(format!("Undo journal: {}", tx.path.display()));
    let result = (|| -> Result<()> {
        for (old, new) in &folders {
            job.check()?;
            job.say(format!("Moving {old} to {new}"));
            tx.rename(old, new)?;
        }
        if let Some(config) = config {
            tx.write(categories::PATH, &config.encode()?)?;
        }
        write_database(&mut tx, db, vec![], &[])?;
        for (folder, index) in [("covers", &mut covers), ("previews", &mut previews)] {
            let moved: Vec<(String, String)> = index
                .iter()
                .filter_map(|(game, media)| {
                    Some((f::fold(map.get(&f::fold(game))?), media.clone()))
                })
                .collect();
            if !moved.is_empty() {
                index.retain(|game, _| !map.contains_key(&f::fold(game)));
                index.extend(moved);
                tx.write(
                    &format!(".ohmy3do/{folder}/index.3di"),
                    &media_index(index)?,
                )?;
            }
        }
        for name in ["favorites.txt", "recent.txt"] {
            let path = f::safe(root, &format!(".ohmy3do/{name}"))?;
            if let Some(text) = path
                .is_file()
                .then(|| fs::read(&path))
                .transpose()?
                .and_then(|raw| remap_lines(&raw, &map))
            {
                tx.write(&format!(".ohmy3do/{name}"), &text)?;
            }
        }
        let position = f::safe(root, ".ohmy3do/last-position.txt")?;
        if position.is_file() {
            let moved: Vec<String> = folders.keys().cloned().collect();
            if let Some(text) = remap_position(&fs::read(&position)?, &map, &moved) {
                tx.write(".ohmy3do/last-position.txt", &text)?;
            }
        }
        build_indexes(root, &mut tx, job)
    })();
    if let Err(e) = result {
        tx.save()?;
        let _ = tx.undo();
        return Err(e);
    }
    tx.finish()?;
    Ok(tx.path.clone())
}

pub fn review(root: &Path, cache: &Path) -> Result<PathBuf> {
    use base64::Engine;
    let db = database(root)?;
    let mut page=String::from("<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\"><title>3DO artwork review</title><style>body{font:16px system-ui;background:#101c28;color:#e2eaf0;margin:2rem}table{width:100%;border-collapse:collapse}td,th{padding:.8rem;border:1px solid #486273;text-align:left}img{width:84px;height:105px}input,button{font:inherit;padding:.5rem}</style><h1>3DO artwork review</h1><input id=\"q\" placeholder=\"Find a title or path\" oninput=\"filter()\"><button onclick=\"missing=!missing;filter()\">Toggle missing only</button><table><thead><tr><th>Cover</th><th>Game / disc</th><th>Preview</th></tr></thead><tbody>");
    let cover = read_media_index(root, "covers")?;
    let preview = read_media_index(root, "previews")?;
    let esc = |s: &str| {
        s.replace('&', "&amp;")
            .replace('<', "&lt;")
            .replace('>', "&gt;")
            .replace('"', "&quot;")
    };
    for r in db["games"].as_array().context("Invalid library")? {
        let path = r["path"].as_str().context("Missing path")?;
        let image = cover.get(&f::fold(path));
        let mut thumbnail = String::from("Missing cover");
        if let Some(name) = image {
            let file = f::safe(root, &format!(".ohmy3do/covers/{name}"))?;
            if fs::metadata(&file)?.len() < 512 * 1024 {
                let mut png = std::io::Cursor::new(Vec::new());
                media::cover_display(&file)?.write_to(&mut png, image::ImageFormat::Png)?;
                thumbnail = format!(
                    "<img alt=\"Cover\" src=\"data:image/png;base64,{}\">",
                    base64::engine::general_purpose::STANDARD.encode(png.get_ref())
                );
            }
        }
        let clip = preview.get(&f::fold(path));
        page.push_str(&format!(
            "<tr data-missing=\"{}\"><td>{}</td><td>{}<br><small>{}</small></td><td>{}</td></tr>",
            image.is_none() || clip.is_none(),
            thumbnail,
            esc(r["title"].as_str().unwrap_or(path)),
            esc(path),
            esc(clip.map(String::as_str).unwrap_or("Missing preview"))
        ));
    }
    page.push_str("</tbody></table><script>let missing=false;function filter(){const q=document.getElementById('q').value.toLowerCase();document.querySelectorAll('tbody tr').forEach(r=>r.hidden=(missing&&r.dataset.missing!=='true')||!r.textContent.toLowerCase().includes(q));}</script></html>");
    let path = cache.join("reports/artwork-review.html");
    f::atomic(&path, page.as_bytes())?;
    Ok(path)
}
