//! Editing saves whose layout is known from the game's source code, with
//! the game's own checksum kept right (separate from the backup file's
//! checksums).
//!
//! DOOM source: Olde-Skuul/doom3do @ 8a5cdae476e09f2bedd2cc4dcd105dae4b35d4af,
//! source/threedo.c ReadPrefsFile / WritePrefsFile; lib/burger/burger.h.

use crate::save_archive::Entry;
use anyhow::{ensure, Result};
/// DOOM's "DoomPrefs" save: ten big-endian words, a signature (0x4c57),
/// eight settings, and a checksum (12345 plus the other nine words).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DoomPrefs {
    /// Difficulty, starting map, sound volume, music volume, control
    /// layout, unlocked level, screen size and low detail, in that order
    /// (the doom3do-prefs-v1 profile in the cheat database).
    pub values: [u32; 8],
}

impl DoomPrefs {
    /// Reads the settings from a DoomPrefs save, checking its signature,
    /// checksum and that every setting is in range.
    pub fn parse(entry: &Entry) -> Result<Self> {
        ensure!(
            entry.name == b"DoomPrefs" && entry.data.len() == 40,
            "Not a supported DOOM preferences save"
        );
        let words: Vec<u32> = entry
            .data
            .chunks_exact(4)
            .map(|b| u32::from_be_bytes(b.try_into().unwrap()))
            .collect();
        ensure!(
            words[0] == 0x4c57
                && words[..9].iter().fold(12345u32, |a, b| a.wrapping_add(*b)) == words[9],
            "DOOM signature or checksum is invalid"
        );
        let value = Self {
            values: words[1..9].try_into().unwrap(),
        };
        value.validate()?;
        Ok(value)
    }
    /// Checks every setting against its range in the cheat database.
    fn validate(&self) -> Result<()> {
        let p = crate::save_cheats::profiles()
            .iter()
            .find(|p| p.id == "doom3do-prefs-v1")
            .unwrap();
        for (value, field) in self.values.iter().zip(&p.fields) {
            ensure!(
                (field.min..=field.max).contains(value),
                "Unsupported DOOM preference value"
            );
        }
        Ok(())
    }
    /// Writes these settings into a DoomPrefs save, with a new checksum.
    pub fn apply(&self, entry: &mut Entry) -> Result<()> {
        Self::parse(entry)?;
        self.validate()?;
        let mut words = vec![0x4c57u32];
        words.extend(self.values);
        words.push(words.iter().fold(12345u32, |a, b| a.wrapping_add(*b)));
        entry.data = words.iter().flat_map(|w| w.to_be_bytes()).collect();
        Ok(())
    }
}
