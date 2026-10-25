//! The data the app keeps: the catalog of known 3DO releases, the plan for
//! importing disc images into the PC library, and the person's preferences.
//! All of it is saved as JSON, so field names are part of the file formats.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::PathBuf;

/// The library's top-level folders, in the order the app shows them.
pub const CATEGORIES: [&str; 8] = [
    "Games",
    "Demos",
    "Samplers",
    "Applications",
    "Multimedia",
    "Educational",
    "Bonus Discs",
    "Adult",
];

/// One file of a catalog release (a disc image or a track), as a DAT file
/// lists it.
#[derive(Clone, Debug, Serialize, Deserialize, Default)]
pub struct Rom {
    #[serde(default)]
    pub name: String,
    #[serde(deserialize_with = "size_value")]
    pub size: u64,
    #[serde(default)]
    pub sha1: String,
    #[serde(default)]
    pub serial: String,
}

/// Reads a ROM size written either as a number or as a string of digits.
fn size_value<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u64, D::Error> {
    let v = Value::deserialize(d)?;
    v.as_u64()
        .or_else(|| v.as_str().and_then(|s| s.parse().ok()))
        .ok_or_else(|| serde::de::Error::custom("Invalid ROM size"))
}

/// A known 3DO release in the catalog.
#[derive(Clone, Debug, Serialize, Deserialize, Default)]
pub struct Game {
    pub name: String,
    #[serde(default)]
    pub roms: Vec<Rom>,
    /// Extra information from the catalog's sources (genre, publisher,
    /// description and so on), kept as it came.
    #[serde(default)]
    pub metadata: Value,
    #[serde(default)]
    pub category: String,
    #[serde(default)]
    pub serial: String,
    #[serde(default)]
    pub region: Option<String>,
    /// The full release name, like "Road Rash (USA)", which becomes the
    /// title in the library.
    #[serde(default)]
    pub release: String,
    #[serde(default)]
    pub variant: Option<String>,
}

/// The catalog of known releases (see `catalog`).
#[derive(Clone, Debug, Serialize, Deserialize, Default)]
pub struct Catalog {
    /// Where the catalog came from (download URLs and checksums), recorded
    /// in the library's database.
    #[serde(default)]
    pub sources: Vec<Value>,
    pub games: Vec<Game>,
    /// Whether this is a catalog fully built from the downloads (see
    /// `catalog::load`); a cached one without it is rebuilt when online.
    #[serde(default)]
    pub complete_catalog: bool,
}

/// What identifies a disc image file's contents.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Fingerprint {
    pub size: u64,
    /// The file's modification time, in nanoseconds; with `size`, tells
    /// whether a file changed since it was fingerprinted.
    pub mtime_ns: u128,
    /// Empty after a quick scan, which skips reading the files.
    pub sha1: String,
    pub sha256: String,
    /// For a 2048-byte-sector ISO: the SHA-1 it would have as a raw
    /// 2352-byte-sector image, which is how some catalogs list discs.
    pub raw_sha1: Option<String>,
}

/// One file of a disc being imported, and where it goes in the library.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DiscFile {
    pub source: PathBuf,
    pub fingerprint: Fingerprint,
    /// The file's path in the library, like
    /// "Games/USA/Road Rash/Road Rash (USA).iso".
    pub target: String,
}

/// One disc (or multi-file disc) found when scanning folders to import, and
/// what the app made of it.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Entry {
    /// The disc image (.iso), or the .cue sheet of a disc with several files.
    pub source: PathBuf,
    pub title: String,
    /// The library folder it goes in (one of [`CATEGORIES`]); empty until
    /// one is chosen.
    pub category: String,
    pub region: String,
    /// The library path of its first file; empty until it can be placed.
    pub target: String,
    /// How it was identified: "checksum", "converted-checksum",
    /// "data-track", "filename", "audio-only", "unverified" or "unmatched".
    pub confidence: String,
    /// Why it was identified (or not) as it was, shown in the import list.
    pub reason: String,
    /// Whether it will be imported.
    pub selected: bool,
    pub files: Vec<DiscFile>,
    /// Catalog releases it might be, when the match is not certain.
    pub candidates: Vec<usize>,
    /// The catalog release it was identified as.
    pub game: Option<usize>,
    pub cover: Option<PathBuf>,
    pub preview: Option<PathBuf>,
    /// Why it was put in its category.
    pub category_reason: String,
    /// Its library path is already taken, so it cannot be imported there.
    pub blocked: bool,
}

impl Entry {
    /// Whether it has everything needed to be imported.
    pub fn resolvable(&self) -> bool {
        !self.blocked && !self.target.is_empty() && !self.category.is_empty()
    }
}

/// What importing into a library will do: every disc found, and the
/// catalog they were matched against.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Plan {
    /// The library's folder.
    pub root: PathBuf,
    pub catalog: Catalog,
    pub entries: Vec<Entry>,
}

/// The person's preferences, saved between runs. Every field has a default,
/// so older preference files still load.
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Preferences {
    /// The PC library in use.
    pub library: String,
    /// Every PC library the person added; `library` is the one in use.
    pub libraries: Vec<String>,
    /// The folder of EmuMovies media, for covers and previews.
    pub emumovies: String,
    /// The USB / SD drive in use.
    pub destination: String,
    /// A folder holding ffmpeg, if not on the PATH or installed by the app.
    pub video_tools: String,
    /// Never use the network.
    pub offline: bool,
    pub disable_update_checks: bool,
    pub hide_update_notifications: bool,
    pub developer_mode: bool,
    /// In developer mode: a folder of release files to update from instead
    /// of the published releases.
    pub developer_updates_directory: String,
    /// Scan without checksums: identify discs by name and size only.
    pub quick: bool,
    pub download_covers: bool,
    /// Draw the window without the GPU (for systems where it fails).
    pub software_rendering: bool,
    pub theme: Theme,
    /// Text size as a percentage, if the person changed it.
    pub text_scale: Option<u16>,
    /// The identity of each drive prepared for the menu, by drive path.
    pub drives: BTreeMap<String, crate::drive_identity::Identity>,
    /// Every USB / SD drive the person added; `destination` is the one in use.
    pub browser_drives: Vec<String>,
    /// How each library's list was last left (filters and selection), by
    /// library path.
    pub library_views: BTreeMap<String, LibraryView>,
}

/// The library list's selection, search and filters, restored when the
/// library is opened again.
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct LibraryView {
    pub selected: Vec<String>,
    pub search: String,
    pub category: String,
    pub region: String,
    pub media: String,
    pub information: String,
    #[serde(rename = "searchScope")]
    pub search_scope: String,
}

/// Desktop appearance; older preference files follow the operating system.
#[derive(Clone, Copy, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Theme {
    #[default]
    System,
    Light,
    Dark,
}
