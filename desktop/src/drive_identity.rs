//! Telling drives apart. A drive prepared for the menu gets an identity (a
//! random ID and a name the person chose) stored on the drive itself, so it
//! is recognized whatever letter or mount point it gets, and a different
//! drive mounted in the same place is noticed before anything is written.

use crate::{fsutil as f, model::Preferences};
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::{fs, path::Path};
/// Where a drive's identity is stored on it.
pub const MARKER: &str = ".ohmy3do/drive.json";

/// A drive's identity, as stored in [`MARKER`].
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Identity {
    /// A UUID.
    pub id: String,
    /// The person's name for the drive (1 to 64 characters).
    pub name: String,
}

/// What is known about the drive mounted at a path.
#[derive(Serialize)]
pub struct Inspection {
    pub root: String,
    /// The identity stored on the drive, if it has one.
    pub identity: Option<Identity>,
    /// The identity last remembered for this path.
    pub expected: Option<Identity>,
    /// A remembered drive was expected here, but this is another one.
    pub mismatch: bool,
    /// The drive's ID, or "unnamed": passed back with a later write to make
    /// sure the drive was not swapped meanwhile.
    pub token: String,
}

/// Reads the identity of the drive at `root`, and compares it with the one
/// remembered for that path.
pub fn inspect(root: &Path, prefs: &Preferences) -> Result<Inspection> {
    let root = f::root(root)?;
    let key = root.to_string_lossy().into_owned();
    let path = f::safe(&root, MARKER)?;
    let identity = if path.exists() {
        ensure!(
            fs::metadata(&path)?.len() <= 1024,
            "Oversized drive identity"
        );
        let value: Identity = serde_json::from_slice(&fs::read(&path)?)?;
        validate(&value)?;
        Some(value)
    } else {
        None
    };
    let expected = prefs.drives.get(&key).cloned();
    let mismatch = expected
        .as_ref()
        .is_some_and(|old| identity.as_ref().is_none_or(|now| now.id != old.id));
    let token = identity
        .as_ref()
        .map(|v| v.id.clone())
        .unwrap_or_else(|| "unnamed".into());
    Ok(Inspection {
        root: key,
        identity,
        expected,
        mismatch,
        token,
    })
}
/// Checks an identity read from a drive or about to be written.
fn validate(value: &Identity) -> Result<()> {
    ensure!(uuid::Uuid::parse_str(&value.id).is_ok(), "Invalid drive ID");
    ensure!(
        !value.name.trim().is_empty()
            && value.name.chars().count() <= 64
            && !value.name.chars().any(char::is_control),
        "Use a drive name of 1–64 characters"
    );
    Ok(())
}
/// Names the drive at `root` (giving it an ID if it has none), writes its
/// identity to it and remembers it for that path. `expected` is the token
/// from the inspection the person saw.
pub fn remember(
    root: &Path,
    name: &str,
    prefs: &mut Preferences,
    expected: &str,
) -> Result<Identity> {
    let view = inspect(root, prefs)?;
    ensure!(view.token == expected, "Drive changed; inspect it again");
    let value = Identity {
        id: view
            .identity
            .map(|i| i.id)
            .unwrap_or_else(|| uuid::Uuid::new_v4().to_string()),
        name: name.trim().into(),
    };
    validate(&value)?;
    let path = f::safe(&f::root(root)?, MARKER)?;
    fs::create_dir_all(path.parent().context("Missing support directory")?)?;
    f::atomic(&path, &serde_json::to_vec_pretty(&value)?)?;
    prefs.drives.insert(view.root, value.clone());
    Ok(value)
}
/// Before writing to a drive: fails if it is not the drive that was
/// reviewed (`token`), or not the drive remembered for that path.
pub fn check(root: &Path, prefs: &Preferences, token: &str) -> Result<()> {
    let view = inspect(root, prefs)?;
    ensure!(
        view.token == token,
        "Destination drive changed since review. Review it again before writing."
    );
    ensure!(
        !view.mismatch,
        "A different drive is mounted here. Check and remember this drive in Drive setup first."
    );
    Ok(())
}
