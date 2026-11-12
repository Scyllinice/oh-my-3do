//! The console menu's settings file, .ohmy3do/ohmy3do.ini: read and checked
//! exactly as the menu reads it (src/menu_config.c), for the desktop's
//! settings editor and for checking drives.

use crate::{fsutil as f, library, Job};
use anyhow::{bail, Context, Result};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
};

/// Where the settings are kept, in a library or on a drive.
pub const PATH: &str = ".ohmy3do/ohmy3do.ini";

/// The largest settings file the menu reads.
const LIMIT: usize = 512;

/// The line that ends a settings file the app or menu wrote.
const END: &str = "; End of Oh My 3DO settings.";

/// The settings' names, in the order `Settings::parse` keeps their values.
/// hide_adult came last and may be missing from older files.
const KEYS: [&str; 9] = [
    "folder_workaround",
    "usb_covers",
    "screen_margin",
    "large_titles",
    "remember_position",
    "recent_games",
    "video_previews",
    "preview_sound",
    "hide_adult",
];
/// A line without spaces, tabs and carriage returns at either end.
fn trim(mut text: &[u8]) -> &[u8] {
    while text
        .first()
        .is_some_and(|b| matches!(*b, b' ' | b'\t' | b'\r'))
    {
        text = &text[1..];
    }
    while text
        .last()
        .is_some_and(|b| matches!(*b, b' ' | b'\t' | b'\r'))
    {
        text = &text[..text.len() - 1];
    }
    text
}

/// The menu's settings, as the Settings screen on the console shows them.
#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct Settings {
    /// 0 = automatic (FCare), 1 = always, 2 = off.
    pub folder_workaround: u8,
    pub usb_covers: bool,
    /// 0 to 12.
    pub screen_margin: u8,
    pub large_titles: bool,
    pub remember_position: bool,
    pub recent_games: bool,
    pub video_previews: bool,
    pub preview_sound: bool,
    pub hide_adult: bool,
}
impl Default for Settings {
    /// The menu's defaults, used when there is no settings file.
    fn default() -> Self {
        Self {
            folder_workaround: 0,
            usb_covers: true,
            screen_margin: 0,
            large_titles: false,
            remember_position: true,
            recent_games: true,
            video_previews: true,
            preview_sound: true,
            hide_adult: false,
        }
    }
}
impl Settings {
    /// Reads a settings file as the menu does: "key=value" lines (values of
    /// one or two digits, optionally followed by a comment), comments
    /// starting with ';' or '#', and section headers, which are ignored.
    /// Unknown, repeated or out-of-range settings are errors; missing ones
    /// keep their defaults. A file that fills the menu's whole buffer must
    /// end with the end line, so a cut-off file is noticed.
    pub fn parse(data: &[u8]) -> Result<Self> {
        if data.len() > LIMIT {
            bail!("Config exceeds 512 bytes")
        }
        // The console stops at the first NUL, or at its explicit end marker.
        let data = data.split(|b| *b == 0).next().unwrap_or_default();
        let length = data.len();
        let data = data.strip_prefix(b"\xef\xbb\xbf").unwrap_or(data);
        // The defaults, in KEYS order.
        let mut values = [0u8, 1, 0, 0, 1, 1, 1, 1, 0];
        let mut seen = [false; 9];
        let mut ended = false;
        for raw in data.split(|b| *b == b'\n') {
            let raw = trim(raw);
            if raw == END.as_bytes() {
                // A written file has every setting (hide_adult excepted,
                // for files written before it existed).
                if !seen[..8].iter().all(|s| *s) {
                    bail!("Incomplete generated configuration")
                }
                ended = true;
                break;
            }
            if raw.is_empty() || matches!(raw[0], b';' | b'#') {
                continue;
            }
            let line = std::str::from_utf8(raw).context("INI settings must use UTF-8 text")?;
            if [
                "[audio]",
                "[compatibility]",
                "[artwork]",
                "[display]",
                "[library]",
            ]
            .contains(&line)
            {
                continue;
            }
            let (key, value) = line.split_once('=').context("Invalid configuration line")?;
            let key = key.trim_end_matches([' ', '\t', '\r']);
            let index = KEYS
                .iter()
                .position(|k| *k == key)
                .with_context(|| format!("Unknown configuration key: {key}"))?;
            if seen[index] {
                bail!("Duplicate configuration key: {key}")
            }
            let value = value.trim_start_matches([' ', '\t', '\r']);
            let digits = value.bytes().take_while(u8::is_ascii_digit).count();
            if !(1..=2).contains(&digits) {
                bail!("Invalid value for {key}")
            }
            let tail = value[digits..].trim_start_matches([' ', '\t', '\r']);
            if !tail.is_empty() && !tail.starts_with(['#', ';']) {
                bail!("Invalid value for {key}")
            }
            let number = value[..digits].parse::<u8>()?;
            let maximum = match KEYS[index] {
                "folder_workaround" => 2,
                "screen_margin" => 12,
                _ => 1,
            };
            if number > maximum {
                bail!("Out-of-range configuration setting: {key}")
            }
            values[index] = number;
            seen[index] = true;
        }
        // Include the BOM in the buffer boundary, just as menu_config_parse does.
        if length == LIMIT && !ended {
            bail!("Full config buffer needs an end marker")
        }
        Ok(Self {
            folder_workaround: values[0],
            usb_covers: values[1] != 0,
            screen_margin: values[2],
            large_titles: values[3] != 0,
            remember_position: values[4] != 0,
            recent_games: values[5] != 0,
            video_previews: values[6] != 0,
            preview_sound: values[7] != 0,
            hide_adult: values[8] != 0,
        })
    }
    /// The settings as a settings file, checked by reading it back.
    pub fn encode(&self) -> Result<Vec<u8>> {
        let text = format!(
            "; Saved by Oh My 3DO Library.\r\n\
             [compatibility]\r\nfolder_workaround={}\r\n\
             [artwork]\r\nusb_covers={}\r\nvideo_previews={}\r\n\
             [display]\r\nscreen_margin={}\r\nlarge_titles={}\r\n\
             [library]\r\nremember_position={}\r\nrecent_games={}\r\nhide_adult={}\r\n\
             [audio]\r\npreview_sound={}\r\n{END}\r\n",
            self.folder_workaround,
            u8::from(self.usb_covers),
            u8::from(self.video_previews),
            self.screen_margin,
            u8::from(self.large_titles),
            u8::from(self.remember_position),
            u8::from(self.recent_games),
            u8::from(self.hide_adult),
            u8::from(self.preview_sound),
        );
        Self::parse(text.as_bytes())?;
        Ok(text.into_bytes())
    }
}

/// Settings loaded for editing. The file's exact bytes (comments, padding
/// and all) are kept, so saving can tell if the file changed meanwhile; the
/// undo journal puts those bytes back on undo.
#[derive(Clone)]
pub struct Loaded {
    pub root: PathBuf,
    pub settings: Settings,
    original: Option<Vec<u8>>,
}
impl Loaded {
    /// Whether there was a settings file (rather than just the defaults).
    pub fn exists(&self) -> bool {
        self.original.is_some()
    }
}
/// The result of a save: the settings as now on disk, and the undo journal.
pub struct Saved {
    pub loaded: Loaded,
    pub journal: PathBuf,
}

/// The settings file's bytes, or None if there is none.
fn read(root: &Path) -> Result<Option<Vec<u8>>> {
    let path = f::safe(root, PATH)?;
    let file = match fs::File::open(&path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(e).with_context(|| format!("Cannot read {}", path.display())),
    };
    if !file.metadata()?.is_file() {
        bail!("Console settings must be a regular file")
    }
    let mut data = Vec::new();
    file.take((LIMIT + 1) as u64).read_to_end(&mut data)?;
    if data.len() > LIMIT {
        bail!("Config exceeds 512 bytes")
    }
    Ok(Some(data))
}
/// Loads the settings of the library or drive at `root` (the defaults if it
/// has no settings file).
pub fn load(root: &Path) -> Result<Loaded> {
    let root = f::root(root)?;
    let original = read(&root)?;
    let settings = original
        .as_deref()
        .map(Settings::parse)
        .transpose()?
        .unwrap_or_default();
    Ok(Loaded {
        root,
        settings,
        original,
    })
}
/// Saves `settings` over what `loaded` read, if the file has not changed
/// since. The write goes through the library's undo journal and is read back;
/// if anything fails, the old file is put back.
pub fn save(loaded: &Loaded, settings: &Settings, cache: &Path, job: &Job) -> Result<Saved> {
    let bytes = settings.encode()?;
    let root = f::root(&loaded.root)?;
    let _lock = library::lock_library(&root, cache)?;
    job.check()?;
    if read(&root)? != loaded.original {
        bail!("Console settings changed since loading. Reload before saving.")
    }
    let mut tx = library::Journal::new(&root, cache)?;
    job.say(format!("Undo journal: {}", tx.path.display()));
    let result = (|| -> Result<()> {
        tx.write(PATH, &bytes)?;
        if read(&root)?.as_deref() != Some(bytes.as_slice()) {
            bail!("Console settings readback failed")
        }
        tx.finish()
    })();
    if let Err(error) = result {
        if let Err(undo) = tx.undo() {
            bail!(
                "{error:#}; rollback needs attention: {undo:#}. Keep {}",
                tx.path.display()
            )
        }
        return Err(error);
    }
    Ok(Saved {
        loaded: Loaded {
            root,
            settings: settings.clone(),
            original: Some(bytes),
        },
        journal: tx.path,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_empty_file_gives_the_menu_defaults() {
        for empty in [b"".as_slice(), b"\r\n", b"; only a comment\r\n"] {
            assert_eq!(Settings::parse(empty).unwrap(), Settings::default());
        }
    }

    #[test]
    fn each_setting_is_read_and_booleans_are_any_nonzero_digit() {
        let settings = Settings::parse(
            b"folder_workaround=2\nusb_covers=1\nscreen_margin=12\nlarge_titles=1\n\
              remember_position=0\nrecent_games=0\nvideo_previews=1\npreview_sound=0\n\
              hide_adult=1\n",
        )
        .unwrap();
        assert_eq!(
            settings,
            Settings {
                folder_workaround: 2,
                usb_covers: true,
                screen_margin: 12,
                large_titles: true,
                remember_position: false,
                recent_games: false,
                video_previews: true,
                preview_sound: false,
                hide_adult: true,
            }
        );
    }

    #[test]
    fn a_missing_setting_keeps_its_default() {
        // hide_adult came last and may be absent from an older file.
        let settings = Settings::parse(b"hide_adult=1\n").unwrap();
        assert!(settings.hide_adult);
        assert!(settings.usb_covers, "the others keep the menu's defaults");
        assert_eq!(settings.screen_margin, 0);
    }

    #[test]
    fn section_headers_comments_and_blank_lines_are_ignored() {
        let text = b"; a comment\n# another\n[audio]\n[compatibility]\n[artwork]\n[display]\n\
                     [library]\n\nusb_covers=0\n; trailing\n";
        let settings = Settings::parse(text).unwrap();
        assert!(!settings.usb_covers);
    }

    #[test]
    fn surrounding_blanks_and_a_value_comment_are_tolerated() {
        let settings =
            Settings::parse(b"  \tusb_covers = 0 \t; off\r\n\tscreen_margin\t=\t7 ; wide\r\n")
                .unwrap();
        assert!(!settings.usb_covers);
        assert_eq!(settings.screen_margin, 7);
    }

    #[test]
    fn a_utf8_byte_order_mark_is_allowed_and_counted_towards_the_limit() {
        let settings = Settings::parse("\u{feff}usb_covers=0\n".as_bytes()).unwrap();
        assert!(!settings.usb_covers);
    }

    #[test]
    fn the_first_nul_ends_the_file_as_the_console_does() {
        // The ODE cannot shorten a file, so bytes written earlier may follow.
        let settings = Settings::parse(b"usb_covers=0\n\0usb_covers=1\n").unwrap();
        assert!(!settings.usb_covers);
    }

    #[test]
    fn the_range_checks_are_the_menu_s() {
        // Booleans take 0 or 1; two settings have their own ranges.
        for key in ["usb_covers", "large_titles", "recent_games", "hide_adult"] {
            assert!(
                Settings::parse(format!("{key}=2").as_bytes()).is_err(),
                "{key}=2"
            );
        }
        assert!(Settings::parse(b"folder_workaround=3").is_err());
        assert!(Settings::parse(b"screen_margin=13").is_err());
        assert!(Settings::parse(b"folder_workaround=2").is_ok());
        assert!(Settings::parse(b"screen_margin=12").is_ok());
    }

    #[test]
    fn a_value_needs_one_or_two_digits_and_nothing_else_after_them() {
        for value in ["", "abc", "0x1", "1.5", "1e0", "123", "-1", "+1"] {
            assert!(
                Settings::parse(format!("screen_margin={value}").as_bytes()).is_err(),
                "{value:?}"
            );
        }
        // Two digits is the widest a setting goes.
        assert_eq!(
            Settings::parse(b"screen_margin=10").unwrap().screen_margin,
            10
        );
    }

    #[test]
    fn unknown_and_repeated_keys_are_errors() {
        let error = Settings::parse(b"cover_flow=1\n").unwrap_err().to_string();
        assert!(error.contains("Unknown configuration key"), "{error}");
        let error = Settings::parse(b"usb_covers=1\nusb_covers=0\n")
            .unwrap_err()
            .to_string();
        assert!(error.contains("Duplicate configuration key"), "{error}");
    }

    #[test]
    fn a_line_without_an_equals_is_an_error() {
        let error = Settings::parse(b"usb_covers\n").unwrap_err().to_string();
        assert!(error.contains("Invalid configuration line"), "{error}");
    }

    #[test]
    fn a_file_filling_the_buffer_needs_the_end_marker() {
        // The menu reads a fixed buffer, so a file that exactly fills it is
        // only accepted when the end marker proves nothing was cut off.
        let written = Settings::default().encode().unwrap();
        assert!(written.len() < LIMIT, "the fixture should leave room");

        // Cut off before the end marker and padded to exactly the limit: refused.
        let mut cut_off = written[..written.len() - END.len() - 2].to_vec();
        cut_off.resize(LIMIT, b' ');
        assert_eq!(cut_off.len(), LIMIT);
        let error = Settings::parse(&cut_off).unwrap_err().to_string();
        assert!(error.contains("end marker"), "{error}");

        // The same length with the end marker present: accepted.
        let mut complete = written.clone();
        complete.resize(LIMIT, b' ');
        assert_eq!(complete.len(), LIMIT);
        Settings::parse(&complete).unwrap();
    }

    #[test]
    fn a_file_over_the_buffer_is_refused_without_parsing_it() {
        let oversized = vec![b';'; LIMIT + 1];
        let error = Settings::parse(&oversized).unwrap_err().to_string();
        assert!(error.contains("512 bytes"), "{error}");
    }

    #[test]
    fn an_end_marker_before_every_setting_is_written_is_refused() {
        // A generated file that stops early would silently lose settings.
        let error = Settings::parse(format!("usb_covers=0\n{END}\n").as_bytes())
            .unwrap_err()
            .to_string();
        assert!(
            error.contains("Incomplete generated configuration"),
            "{error}"
        );
    }

    #[test]
    fn the_end_marker_is_not_required_for_a_hand_written_file() {
        // The marker only guards files the app or menu wrote in full.
        assert!(Settings::parse(b"usb_covers=0\nhide_adult=1\n").is_ok());
    }

    #[test]
    fn every_setting_survives_a_round_trip_through_encode() {
        let settings = Settings {
            folder_workaround: 2,
            usb_covers: false,
            screen_margin: 9,
            large_titles: true,
            remember_position: false,
            recent_games: false,
            video_previews: false,
            preview_sound: true,
            hide_adult: true,
        };
        let bytes = settings.encode().unwrap();
        assert_eq!(Settings::parse(&bytes).unwrap(), settings);
        // encode() checks its own output by parsing it.
        Settings::default().encode().unwrap();
    }

    #[test]
    fn an_encoded_file_ends_its_lines_with_crlf_as_the_console_expects() {
        let bytes = Settings::default().encode().unwrap();
        let text = String::from_utf8(bytes.clone()).unwrap();
        assert!(text.contains("\r\nfolder_workaround="), "{text}");
        assert!(text.ends_with(&format!("{END}\r\n")));
        // Every setting the menu knows about appears exactly once.
        for key in KEYS {
            assert_eq!(
                text.matches(&format!("{key}=")).count(),
                1,
                "{key} in {text}"
            );
        }
    }

    #[test]
    fn a_library_without_a_settings_file_reports_that_it_has_none() {
        let temp = TempDir::new("console-config");
        let loaded = load(temp.path()).unwrap();
        assert!(!loaded.exists());
        assert_eq!(loaded.settings, Settings::default());
    }

    #[test]
    fn a_settings_file_on_disk_is_found_and_read() {
        let temp = TempDir::new("console-config-read");
        temp.write(PATH, &Settings::default().encode().unwrap());
        let loaded = load(temp.path()).unwrap();
        assert!(loaded.exists());
        assert_eq!(loaded.settings, Settings::default());
    }

    #[test]
    fn an_invalid_settings_file_on_disk_is_reported_rather_than_ignored() {
        let temp = TempDir::new("console-config-bad");
        temp.write(PATH, b"not_a_setting=1\n");
        assert!(load(temp.path()).is_err());
    }

    #[test]
    fn a_settings_file_larger_than_the_buffer_is_refused_on_read() {
        let temp = TempDir::new("console-config-oversize");
        // Written on disk beyond what the menu would read.
        temp.write(PATH, &vec![b';'; LIMIT + 1]);
        let error = match load(temp.path()) {
            Ok(_) => panic!("an over-long settings file should be refused"),
            Err(error) => error.to_string(),
        };
        assert!(error.contains("512 bytes"), "{error}");
    }

    /// A fresh folder that is removed when the test ends, so no test here can
    /// reach a real library or drive.
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
            let path = std::env::temp_dir().join(format!(
                "ohmy3do-console-{}-{label}-{n}",
                std::process::id()
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(path.join(".ohmy3do")).unwrap();
            Self(path)
        }
        fn path(&self) -> &Path {
            &self.0
        }
        fn write(&self, name: &str, data: &[u8]) {
            let target = self.0.join(name);
            fs::create_dir_all(target.parent().unwrap()).unwrap();
            fs::write(target, data).unwrap();
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
}
