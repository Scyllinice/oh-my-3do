use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::path::PathBuf;
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
fn size_value<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u64, D::Error> {
    let v = Value::deserialize(d)?;
    v.as_u64()
        .or_else(|| v.as_str().and_then(|s| s.parse().ok()))
        .ok_or_else(|| serde::de::Error::custom("Invalid ROM size"))
}
#[derive(Clone, Debug, Serialize, Deserialize, Default)]
pub struct Game {
    pub name: String,
    #[serde(default)]
    pub roms: Vec<Rom>,
    #[serde(default)]
    pub metadata: Value,
    #[serde(default)]
    pub category: String,
    #[serde(default)]
    pub serial: String,
    #[serde(default)]
    pub region: Option<String>,
    #[serde(default)]
    pub release: String,
    #[serde(default)]
    pub variant: Option<String>,
}
#[derive(Clone, Debug, Serialize, Deserialize, Default)]
pub struct Catalog {
    #[serde(default)]
    pub sources: Vec<Value>,
    pub games: Vec<Game>,
    #[serde(default)]
    pub complete_catalog: bool,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Fingerprint {
    pub size: u64,
    pub mtime_ns: u128,
    pub sha1: String,
    pub sha256: String,
    pub raw_sha1: Option<String>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DiscFile {
    pub source: PathBuf,
    pub fingerprint: Fingerprint,
    pub target: String,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Entry {
    pub source: PathBuf,
    pub title: String,
    pub category: String,
    pub region: String,
    pub target: String,
    pub confidence: String,
    pub reason: String,
    pub selected: bool,
    pub files: Vec<DiscFile>,
    pub candidates: Vec<usize>,
    pub game: Option<usize>,
    pub cover: Option<PathBuf>,
    pub preview: Option<PathBuf>,
    pub category_reason: String,
    pub blocked: bool,
}
impl Entry {
    pub fn resolvable(&self) -> bool {
        !self.blocked && !self.target.is_empty() && !self.category.is_empty()
    }
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Plan {
    pub root: PathBuf,
    pub catalog: Catalog,
    pub entries: Vec<Entry>,
}
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Preferences {
    pub library: String,
    pub emumovies: String,
    pub destination: String,
    pub video_tools: String,
    pub offline: bool,
    pub disable_update_checks: bool,
    pub quick: bool,
    pub download_covers: bool,
    pub software_rendering: bool,
    pub theme: Theme,
    pub library_views: std::collections::BTreeMap<String, LibraryView>,
}

#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct LibraryView {
    pub selected: Vec<String>,
    pub search: String,
    pub category: String,
    pub region: String,
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
