//! Categories: the top-level folders of a library or drive (Games, Demos
//! and so on), which the person can rename, add to, and hide on the
//! console. They are kept in .ohmy3do/categories.json, which the console
//! menu reads too (and rewrites when a category is shown or hidden there).
//! A library or drive without the file has the built-in categories.

use crate::{fsutil as f, library, model::CATEGORIES, Job};
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, HashSet},
    fs,
    path::{Path, PathBuf},
};
/// Where the categories are kept, in a library or on a drive.
pub const PATH: &str = ".ohmy3do/categories.json";

/// The largest categories file (the size of the console's buffer for it).
pub const LIMIT: usize = 8192;

/// One category.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Category {
    /// A permanent ID, so a renamed category is still recognized.
    pub id: String,
    /// Its folder's name.
    pub name: String,
    /// Not shown on the console (the desktop always shows it).
    pub hidden: bool,
}

/// The contents of categories.json.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Categories {
    /// Always 1.
    pub schema: u32,
    pub categories: Vec<Category>,
    /// For each built-in category name, the ID of the category that discs
    /// of that kind are imported into.
    pub defaults: BTreeMap<String, String>,
}

impl Default for Categories {
    /// The built-in categories, each its own import destination.
    fn default() -> Self {
        let categories: Vec<_> = CATEGORIES
            .iter()
            .enumerate()
            .map(|(i, n)| Category {
                id: format!("category-{i}"),
                name: n.to_string(),
                hidden: false,
            })
            .collect();
        let defaults = CATEGORIES
            .iter()
            .zip(&categories)
            .map(|(n, c)| (n.to_string(), c.id.clone()))
            .collect();
        Self {
            schema: 1,
            categories,
            defaults,
        }
    }
}
impl Categories {
    /// Checks the categories can be used: 1 to 32 of them, with valid
    /// unique IDs, names that are unique (ignoring case) and usable as
    /// folder names on a FAT drive, and an import destination for every
    /// built-in category.
    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.schema == 1 && !self.categories.is_empty() && self.categories.len() <= 32,
            "Use between 1 and 32 categories"
        );
        let mut ids = HashSet::new();
        let mut names = HashSet::new();
        for c in &self.categories {
            ensure!(
                !c.id.is_empty()
                    && c.id.len() <= 40
                    && c.id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-'),
                "Invalid category identifier"
            );
            ensure!(ids.insert(&c.id), "Duplicate category identifier");
            ensure!(
                !c.name.is_empty()
                    && c.name.len() <= 48
                    && c.name.trim() == c.name
                    && !c.name.ends_with('.')
                    && c.name
                        .bytes()
                        .all(|b| (32..127).contains(&b) && !b"<>:\"/\\|?*".contains(&b)),
                "Category names need 1–48 printable characters without filename punctuation"
            );
            let base = c.name.split('.').next().unwrap().to_ascii_uppercase();
            ensure!(
                ![
                    "CON",
                    "PRN",
                    "AUX",
                    "NUL",
                    "BOOT.ISO",
                    "MENU-DEMO.ISO",
                    ".OHMY3DO"
                ]
                .contains(&c.name.to_ascii_uppercase().as_str())
                    && !["CON", "PRN", "AUX", "NUL"].contains(&base.as_str())
                    && !(base.len() == 4
                        && (base.starts_with("COM") || base.starts_with("LPT"))
                        && matches!(base.as_bytes()[3], b'1'..=b'9'))
                    && c.name != "."
                    && c.name != "..",
                "Reserved category name"
            );
            ensure!(
                names.insert(c.name.to_ascii_lowercase()),
                "Category names must be unique ignoring case"
            );
        }
        ensure!(
            self.defaults.len() == CATEGORIES.len(),
            "Choose an import destination for every built-in category"
        );
        for name in CATEGORIES {
            ensure!(
                self.defaults.get(name).is_some_and(|id| ids.contains(id)),
                "Missing import destination for {name}"
            );
        }
        Ok(())
    }
    /// The categories as the bytes of categories.json.
    pub fn encode(&self) -> Result<Vec<u8>> {
        self.validate()?;
        let mut bytes = serde_json::to_vec_pretty(self)?;
        ensure!(
            bytes.len() + 1 < LIMIT,
            "Category configuration is too large"
        );
        bytes.push(b'\n');
        Ok(bytes)
    }
    /// The categories' folder names.
    pub fn names(&self) -> Vec<String> {
        self.categories.iter().map(|c| c.name.clone()).collect()
    }
    /// Whether a category has this name (ignoring case).
    pub fn contains(&self, name: &str) -> bool {
        self.categories
            .iter()
            .any(|c| c.name.eq_ignore_ascii_case(name))
    }
    /// The folder that discs of built-in category `role` are imported into.
    pub fn destination(&self, role: &str) -> Option<&str> {
        let id = self.defaults.get(role)?;
        self.categories
            .iter()
            .find(|c| &c.id == id)
            .map(|c| c.name.as_str())
    }
}
/// The categories of the library or drive at `root`. Without a
/// categories.json, the built-in ones, with Adult hidden if the console's
/// settings hide adult games.
pub fn load(root: &Path) -> Result<Categories> {
    let path = f::safe(root, PATH)?;
    if !path.exists() {
        let mut config = Categories::default();
        if let Ok(ini) = crate::console_config::load(root) {
            if ini.settings.hide_adult {
                if let Some(c) = config.categories.iter_mut().find(|c| c.name == "Adult") {
                    c.hidden = true;
                }
            }
        }
        return Ok(config);
    }
    let raw = fs::read(&path)?;
    ensure!(raw.len() <= LIMIT, "Category configuration exceeds 8 KiB");
    // When the console rewrites the file it ends the JSON with a zero
    // byte: the ODE cannot shorten a file, so old bytes may follow it.
    let raw = raw.split(|b| *b == 0).next().unwrap_or(&[]);
    let config: Categories = serde_json::from_slice(raw).context("Invalid categories.json")?;
    config.validate()?;
    Ok(config)
}
/// Saves new categories to the library at `root`, if its categories are
/// still `expected` (what the editor opened with), renaming folders to
/// match (see `library::configure_categories`). Returns what that returns.
pub fn save(
    root: &Path,
    expected: &Categories,
    config: &Categories,
    cache: &Path,
    job: &Job,
) -> Result<PathBuf> {
    config.validate()?;
    ensure!(
        &load(root)? == expected,
        "Categories changed on disk; reopen the editor"
    );
    library::configure_categories(root, expected, config, cache, job)
}

/// The categories to write to the drive at `target` when copying from the
/// library at `source`: the library's categories, but keeping what the
/// drive had chosen to hide, and keeping categories that exist only on the
/// drive.
pub fn for_transfer(source: &Path, target: &Path) -> Result<Categories> {
    let mut config = load(source)?;
    let previous = load(target)?;
    let configured = f::safe(target, PATH)?.exists();
    for category in &mut config.categories {
        if let Some(old) = previous.categories.iter().find(|old| old.id == category.id) {
            if configured || old.hidden {
                category.hidden = old.hidden;
            }
        }
    }
    for old in previous.categories {
        if !config.categories.iter().any(|c| c.id == old.id)
            && (configured || f::safe(target, &old.name)?.exists())
        {
            config.categories.push(old);
        }
    }
    config.validate().context(
        "Library and drive categories conflict; resolve them in Manage categories before copying",
    )?;
    Ok(config)
}
