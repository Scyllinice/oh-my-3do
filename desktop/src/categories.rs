//! Shared, library-local category names and console visibility.
use crate::{fsutil as f, library, model::CATEGORIES, Job};
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, HashSet},
    fs,
    path::{Path, PathBuf},
};
pub const PATH: &str = ".ohmy3do/categories.json";
pub const LIMIT: usize = 8192;
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Category {
    pub id: String,
    pub name: String,
    pub hidden: bool,
}
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Categories {
    pub schema: u32,
    pub categories: Vec<Category>,
    pub defaults: BTreeMap<String, String>,
}
impl Default for Categories {
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
    pub fn names(&self) -> Vec<String> {
        self.categories.iter().map(|c| c.name.clone()).collect()
    }
    pub fn contains(&self, name: &str) -> bool {
        self.categories
            .iter()
            .any(|c| c.name.eq_ignore_ascii_case(name))
    }
    pub fn destination(&self, role: &str) -> Option<&str> {
        let id = self.defaults.get(role)?;
        self.categories
            .iter()
            .find(|c| &c.id == id)
            .map(|c| c.name.as_str())
    }
}
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
    let raw = raw.split(|b| *b == 0).next().unwrap_or(&[]);
    let config: Categories = serde_json::from_slice(raw).context("Invalid categories.json")?;
    config.validate()?;
    Ok(config)
}
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

/// Category definitions follow staging; per-drive visibility and drive-only
/// categories survive subsequent (including selected-game) transfers.
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
