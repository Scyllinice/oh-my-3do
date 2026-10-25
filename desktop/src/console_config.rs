//! The console INI, shared by the desktop editor and USB validation.
use crate::{fsutil as f, library, Job};
use anyhow::{bail, Context, Result};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
};

pub const PATH: &str = ".ohmy3do/ohmy3do.ini";
const LIMIT: usize = 512;
const END: &str = "; End of Oh My 3DO settings.";
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

#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct Settings {
    /// 0 = automatic (FCare), 1 = always, 2 = off.
    pub folder_workaround: u8,
    pub usb_covers: bool,
    pub screen_margin: u8,
    pub large_titles: bool,
    pub remember_position: bool,
    pub recent_games: bool,
    pub video_previews: bool,
    pub preview_sound: bool,
    pub hide_adult: bool,
}
impl Default for Settings {
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
    pub fn parse(data: &[u8]) -> Result<Self> {
        if data.len() > LIMIT {
            bail!("Config exceeds 512 bytes")
        }
        // The console stops at the first NUL, or at its explicit end marker.
        let data = data.split(|b| *b == 0).next().unwrap_or_default();
        let length = data.len();
        let data = data.strip_prefix(b"\xef\xbb\xbf").unwrap_or(data);
        let mut values = [0u8, 1, 0, 0, 1, 1, 1, 1, 0];
        let mut seen = [false; 9];
        let mut ended = false;
        for raw in data.split(|b| *b == b'\n') {
            let raw = trim(raw);
            if raw == END.as_bytes() {
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
            let maximum = match index {
                0 => 2,
                2 => 12,
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

/// Retain exact original bytes, including comments and console padding, for
/// conflict detection. The normal undo journal restores those bytes on undo.
#[derive(Clone)]
pub struct Loaded {
    pub root: PathBuf,
    pub settings: Settings,
    original: Option<Vec<u8>>,
}
impl Loaded {
    pub fn exists(&self) -> bool {
        self.original.is_some()
    }
}
pub struct Saved {
    pub loaded: Loaded,
    pub journal: PathBuf,
}
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
