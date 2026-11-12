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

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    /// A fresh folder that is removed when the test ends, so no test here
    /// can reach a real library or drive.
    struct TempDir(PathBuf);
    impl TempDir {
        fn new(label: &str) -> Self {
            thread_local! {
                static COUNTER: Cell<u32> = const { Cell::new(0) };
            }
            let n = COUNTER.with(|c| {
                let n = c.get();
                c.set(n + 1);
                n
            });
            let path = std::env::temp_dir().join(format!(
                "ohmy3do-categories-{}-{}-{n}",
                std::process::id(),
                label
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
        fn path(&self) -> &Path {
            &self.0
        }
        /// Writes categories.json under .ohmy3do/, as the console leaves it:
        /// padded with zero bytes, since the ODE cannot shorten a file.
        fn write_padded(&self, json: &str) {
            let dir = self.0.join(".ohmy3do");
            fs::create_dir_all(&dir).unwrap();
            let mut bytes = json.as_bytes().to_vec();
            bytes.resize(LIMIT, 0);
            fs::write(dir.join("categories.json"), bytes).unwrap();
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn the_default_categories_are_the_built_in_ones_each_its_own_destination() {
        let config = Categories::default();
        assert_eq!(config.schema, 1);
        assert_eq!(
            config.names(),
            CATEGORIES.iter().map(|n| n.to_string()).collect::<Vec<_>>()
        );
        assert_eq!(config.categories.len(), CATEGORIES.len());
        for (i, c) in config.categories.iter().enumerate() {
            assert_eq!(c.id, format!("category-{i}"));
            assert!(!c.hidden);
        }
        // Every built-in role imports into the category of the same name.
        for name in CATEGORIES {
            assert_eq!(config.destination(name), Some(name));
        }
    }

    #[test]
    fn a_renamed_category_keeps_import_destinations_that_point_at_its_id() {
        let mut config = Categories::default();
        config.categories[0].name = "Retail".into();
        config.validate().unwrap();
        assert_eq!(config.destination("Games"), Some("Retail"));
        // The old name is no longer a folder on the drive.
        assert!(!config.contains("Games"));
        assert!(config.contains("RETAIL"));
    }

    #[test]
    fn an_import_destination_naming_an_unknown_id_is_refused() {
        let mut config = Categories::default();
        config
            .defaults
            .insert("Games".into(), "no-such-category".into());
        let error = config.validate().unwrap_err().to_string();
        assert!(error.contains("Games"), "{error}");
    }

    #[test]
    fn destination_of_an_unknown_role_is_none() {
        assert_eq!(Categories::default().destination("Not A Category"), None);
    }

    #[test]
    fn names_are_compared_ignoring_case() {
        let config = Categories::default();
        assert!(config.contains("games"));
        assert!(config.contains("Bonus Discs"));
        assert!(!config.contains("Games2"));
    }

    #[test]
    fn encoding_writes_a_trailing_newline_and_load_reads_it_back() {
        let temp = TempDir::new("roundtrip");
        let config = Categories::default();
        let bytes = config.encode().unwrap();
        assert_eq!(*bytes.last().unwrap(), b'\n');

        let dir = temp.path().join(".ohmy3do");
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("categories.json"), &bytes).unwrap();
        assert_eq!(load(temp.path()).unwrap(), config);
    }

    #[test]
    fn a_file_the_console_padded_with_zero_bytes_loads() {
        let temp = TempDir::new("padded");
        let mut config = Categories::default();
        config.categories[0].hidden = true;
        temp.write_padded(&serde_json::to_string(&config).unwrap());
        let loaded = load(temp.path()).unwrap();
        assert_eq!(loaded, config);
        // The padding is not part of the configuration.
        assert_eq!(loaded.categories.len(), CATEGORIES.len());
    }

    #[test]
    fn a_library_without_the_file_gets_the_built_in_categories() {
        let temp = TempDir::new("absent");
        assert_eq!(load(temp.path()).unwrap(), Categories::default());
    }

    #[test]
    fn an_over_large_file_is_refused_before_it_is_parsed() {
        let temp = TempDir::new("oversize");
        let dir = temp.path().join(".ohmy3do");
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("categories.json"), vec![b' '; LIMIT + 1]).unwrap();
        let error = load(temp.path()).unwrap_err().to_string();
        assert!(error.contains("8 KiB"), "{error}");
    }

    #[test]
    fn malformed_json_is_reported_as_an_invalid_file() {
        let temp = TempDir::new("malformed");
        let dir = temp.path().join(".ohmy3do");
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("categories.json"), b"{ not json").unwrap();
        let error = load(temp.path()).unwrap_err().to_string();
        assert!(error.contains("Invalid categories.json"), "{error}");
    }

    #[test]
    fn a_file_that_parses_but_does_not_validate_is_refused() {
        let temp = TempDir::new("unvalidated");
        let dir = temp.path().join(".ohmy3do");
        fs::create_dir_all(&dir).unwrap();
        // Two categories sharing a name, which validate() rejects.
        fs::write(
            dir.join("categories.json"),
            r#"{"schema":1,"categories":[
                {"id":"a","name":"Games","hidden":false},
                {"id":"b","name":"games","hidden":false}
            ],"defaults":{}}"#,
        )
        .unwrap();
        assert!(load(temp.path()).is_err());
    }

    #[test]
    fn the_largest_valid_categories_file_still_fits_the_console_buffer() {
        // encode() refuses a file over the console's 8 KiB buffer. This records
        // how much room that leaves: with IDs at their 40-character maximum and
        // names at 48, and the most categories validate() allows (32), the file
        // is still well under the limit. So encode() cannot reject a set of
        // categories that validate() has accepted; the size check only matters
        // for a file written elsewhere, which load() checks against LIMIT.
        let mut config = Categories::default();
        let largest = |i: usize| {
            (
                format!("{i}{}", "z".repeat(40 - i.to_string().len())),
                format!("{i}{}", "w".repeat(48 - i.to_string().len())),
            )
        };
        for i in 0..32 {
            let (id, name) = largest(i);
            if let Some(category) = config.categories.get_mut(i) {
                category.id = id;
                category.name = name;
            } else {
                config.categories.push(Category {
                    id,
                    name,
                    hidden: false,
                });
            }
        }
        for (role, category) in CATEGORIES.iter().zip(&config.categories) {
            config
                .defaults
                .insert((*role).to_string(), category.id.clone());
        }
        assert_eq!(config.categories.len(), 32);
        assert!(config.categories.iter().all(|c| c.id.len() == 40));
        assert!(config.categories.iter().all(|c| c.name.len() == 48));
        config.validate().unwrap();

        let bytes = config.encode().unwrap();
        assert_eq!(
            bytes.len(),
            serde_json::to_vec_pretty(&config).unwrap().len() + 1
        );
        assert!(bytes.len() < LIMIT, "{} bytes", bytes.len());
    }

    #[test]
    fn a_transfer_keeps_what_the_drive_chose_to_hide() {
        let source = TempDir::new("src");
        let target = TempDir::new("dst-hidden");
        // The library hides nothing; the drive has hidden Demos.
        let mut library_config = Categories::default();
        library_config.categories[1].name = "Demos".into();
        let dir = source.path().join(".ohmy3do");
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join("categories.json"),
            serde_json::to_vec(&library_config).unwrap(),
        )
        .unwrap();

        let mut drive_config = Categories::default();
        drive_config.categories[1].hidden = true;
        let dir = target.path().join(".ohmy3do");
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join("categories.json"),
            serde_json::to_vec(&drive_config).unwrap(),
        )
        .unwrap();

        let merged = for_transfer(source.path(), target.path()).unwrap();
        assert!(merged.categories[1].hidden, "the drive's choice is kept");
        assert!(!merged.categories[0].hidden);
    }

    #[test]
    fn a_transfer_keeps_a_category_that_exists_only_on_the_drive() {
        let source = TempDir::new("src-only");
        let target = TempDir::new("dst-extra");
        let dir = source.path().join(".ohmy3do");
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join("categories.json"),
            serde_json::to_vec(&Categories::default()).unwrap(),
        )
        .unwrap();

        // The drive has an extra category the library does not know about.
        let mut drive_config = Categories::default();
        drive_config.categories.push(Category {
            id: "drive-only".into(),
            name: "Homebrew".into(),
            hidden: true,
        });
        let dir = target.path().join(".ohmy3do");
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join("categories.json"),
            serde_json::to_vec(&drive_config).unwrap(),
        )
        .unwrap();

        let merged = for_transfer(source.path(), target.path()).unwrap();
        assert!(merged.contains("Homebrew"));
        let kept = merged
            .categories
            .iter()
            .find(|c| c.id == "drive-only")
            .unwrap();
        assert!(kept.hidden);
    }

    #[test]
    fn a_transfer_carries_the_libraries_categories_when_the_drive_is_unconfigured() {
        // A drive with no categories.json: hiding is not carried over from
        // the built-in defaults, only from a category the drive really has.
        let source = TempDir::new("src-hide");
        let target = TempDir::new("dst-bare");
        let mut library_config = Categories::default();
        library_config.categories[1].hidden = true;
        let dir = source.path().join(".ohmy3do");
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join("categories.json"),
            serde_json::to_vec(&library_config).unwrap(),
        )
        .unwrap();

        let merged = for_transfer(source.path(), target.path()).unwrap();
        assert!(merged.categories[1].hidden, "the library's choice applies");
        assert_eq!(merged.categories.len(), CATEGORIES.len());
    }
}
