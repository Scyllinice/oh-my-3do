use crate::{
    disc as d, fsutil as f,
    library::{self, Journal},
    media, Job,
};
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeSet, HashMap, HashSet},
    fs,
    path::{Path, PathBuf},
};
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CopyItem {
    pub path: String,
    pub status: String,
    pub size: u64,
    pub source_stamp: u128,
    pub destination_stamp: Option<u128>,
    #[serde(default)]
    pub verified: bool,
}
#[derive(Clone, Serialize, Deserialize)]
pub struct CopyPlan {
    pub source: PathBuf,
    pub destination: PathBuf,
    pub files: Vec<CopyItem>,
    pub games: usize,
    pub bytes: u64,
}
fn word(data: &[u8], pos: usize) -> Result<usize> {
    Ok(u32::from_be_bytes(
        data.get(pos..pos + 4)
            .context("Truncated index")?
            .try_into()?,
    ) as usize)
}
pub fn config(path: &Path) -> Result<()> {
    if fs::metadata(path)?.len() > 512 {
        bail!("Config exceeds 512 bytes")
    }
    let data = fs::read(path)?;
    if data.len() > 512 {
        bail!("Config exceeds 512 bytes")
    };
    let text = std::str::from_utf8(&data)?.trim_start_matches('\u{feff}');
    let mut keys = HashSet::new();
    let mut ended = false;
    let limits = HashMap::from([
        ("folder_workaround", 1),
        ("usb_covers", 1),
        ("screen_margin", 12),
        ("large_titles", 1),
        ("theme", 2),
        ("remember_position", 1),
        ("recent_games", 1),
        ("video_previews", 1),
        ("menu_music", 1),
        ("preview_sound", 1),
    ]);
    for line in text.split('\n') {
        let line = line.trim_matches([' ', '\t', '\r']);
        if line == "; End of Oh My 3DO settings." {
            if ![
                "folder_workaround",
                "usb_covers",
                "screen_margin",
                "large_titles",
                "theme",
                "remember_position",
                "recent_games",
                "video_previews",
            ]
            .iter()
            .all(|k| keys.contains(*k))
                || !keys.contains("preview_sound") && !keys.contains("menu_music")
            {
                bail!("Incomplete generated configuration")
            };
            ended = true;
            break;
        }
        if line.is_empty() || line.starts_with([';', '#']) {
            continue;
        }
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
        let c = d::rx(r"^([a-z_]+)[ \t]*=[ \t]*([0-9]{1,2})[ \t]*(?:[#;].*)?$")
            .captures(line)
            .context("Invalid configuration line")?;
        let key = c[1].to_owned();
        let limit = limits
            .get(key.as_str())
            .context("Unknown configuration key")?;
        if c[2].parse::<u32>()? > *limit || !keys.insert(key) {
            bail!("Duplicate or out-of-range config setting")
        }
    }
    if data.len() == 512 && !ended {
        bail!("Full config buffer needs an end marker")
    };
    Ok(())
}
pub fn find(path: &Path) -> Result<Vec<String>> {
    if fs::metadata(path)?.len() > 128 * 1024 + 32 {
        bail!("Oversized Find index")
    }
    let data = fs::read(path)?;
    if data.len() < 32 || &data[..8] != b"OM3DLIB1" || &data[data.len() - 8..] != b"END3DL01" {
        bail!("Invalid Find index header/end")
    }
    let count = word(&data, 8)?;
    let size = word(&data, 12)?;
    if count > 2048
        || size + 9 * count > 128 * 1024
        || size + 32 != data.len()
        || word(&data, 20)? != 0
        || crc32fast::hash(&data[24..data.len() - 8]) as usize != word(&data, 16)?
    {
        bail!("Invalid Find index checksum/bounds")
    }
    let mut pos = 24;
    let mut names = vec![];
    for _ in 0..count {
        let b = data.get(pos..pos + 2).context("Truncated Find path")?;
        let n = u16::from_be_bytes(b.try_into()?) as usize;
        pos += 2;
        let s = data.get(pos..pos + n).context("Truncated Find path")?;
        if n < 2 || s[n - 1] != 0 || s[..n - 1].contains(&0) {
            bail!("Invalid Find path")
        };
        names.push(std::str::from_utf8(&s[..n - 1])?.to_owned());
        pos += n;
    }
    if pos != data.len() - 8
        || names
            .iter()
            .map(|s| f::fold(s))
            .collect::<HashSet<_>>()
            .len()
            != names.len()
    {
        bail!("Duplicate/trailing Find paths")
    };
    Ok(names)
}
pub fn inventory(root: &Path, job: &Job) -> Result<(Vec<String>, usize)> {
    let boot = f::safe(root, "boot.iso")?;
    d::issue(&boot)?;
    if fs::metadata(&boot)?.len() > 16 * 1024 * 1024 {
        bail!("Oversized menu boot image")
    }
    let raw = fs::read(&boot)?;
    if raw.len() < 84
        || raw[..7] != [1, 90, 90, 90, 90, 90, 1]
        || word(&raw, 76)? != 2048
        || word(&raw, 80)? * 2048 != raw.len()
    {
        bail!("Invalid boot volume header/size")
    }
    config(&f::safe(root, ".ohmy3do/ohmy3do.ini")?)?;
    let indexed = find(&f::safe(root, ".ohmy3do/library.3di")?)?;
    let sets = d::collect(&[root.to_owned()], None, job)?;
    let games = sets
        .iter()
        .map(|(p, _)| library::relative(root, p))
        .collect::<Result<BTreeSet<_>>>()?;
    if indexed.iter().cloned().collect::<BTreeSet<_>>() != games {
        bail!("Find index is stale. Rebuild indexes on the Library tab first.")
    }
    let mut files = BTreeSet::from([
        "boot.iso".into(),
        ".ohmy3do/ohmy3do.ini".into(),
        ".ohmy3do/library.3di".into(),
    ]);
    for (_, set) in &sets {
        for p in set {
            files.insert(library::relative(root, p)?);
        }
    }
    let folded = games.iter().map(|s| f::fold(s)).collect::<HashSet<_>>();
    let metadata = f::safe(root, ".ohmy3do/metadata.3di")?;
    if metadata.is_file() {
        if fs::metadata(&metadata)?.len() > 192 * 1024 + 32 {
            bail!("Oversized metadata index");
        }
        for path in library::metadata_paths(&fs::read(metadata)?)? {
            if !folded.contains(&f::fold(&path)) {
                bail!("Metadata index refers to a missing game; rebuild indexes");
            }
        }
        files.insert(".ohmy3do/metadata.3di".into());
    }
    for folder in ["covers", "previews"] {
        let map = library::read_media_index(root, folder)?;
        let index = format!(".ohmy3do/{folder}/index.3di");
        if f::safe(root, &index)?.is_file() {
            files.insert(index);
        }
        for (game, name) in map {
            if !folded.contains(&game) {
                bail!("Media index refers to a missing game")
            };
            let extension = Path::new(&name)
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .to_ascii_lowercase();
            if !(if folder == "covers" {
                ["png", "3dc"].contains(&extension.as_str())
            } else {
                extension == "3dv"
            }) {
                bail!("Indexed media type does not match its folder")
            }
            let path = format!(".ohmy3do/{folder}/{name}");
            if files.insert(path.clone()) {
                job.check()?;
                media::validate(&f::safe(root, &path)?)?;
            }
        }
    }
    for name in ["favorites.txt", "recent.txt", "last-position.txt"] {
        let path = format!(".ohmy3do/{name}");
        if f::safe(root, &path)?.is_file() {
            files.insert(path);
        }
    }
    let saves = f::safe(root, ".ohmy3do/saves")?;
    if saves.exists() {
        for e in walkdir::WalkDir::new(saves).follow_links(false) {
            let e = e?;
            if f::is_link(e.path()) {
                bail!("Links in saves are unsupported")
            };
            if e.file_type().is_file() {
                files.insert(library::relative(root, e.path())?);
            }
        }
    }
    for name in &files {
        let path = f::safe(root, name)?;
        if !path.is_file() || fs::metadata(path)?.len() >= 4 * 1024u64.pow(3) {
            bail!("Missing or oversized FAT32 file: {name}")
        }
    }
    Ok((files.into_iter().collect(), games.len()))
}
fn personal(name: &str) -> bool {
    name.starts_with(".ohmy3do/saves/")
        || [
            ".ohmy3do/ohmy3do.ini",
            ".ohmy3do/favorites.txt",
            ".ohmy3do/recent.txt",
            ".ohmy3do/last-position.txt",
        ]
        .contains(&name)
}
fn report_path(source: &Path, target: &Path, cache: &Path) -> PathBuf {
    let key = format!(
        "{:x}",
        Sha256::digest(format!("{}\n{}", source.display(), target.display()).as_bytes())
    );
    cache.join("usb-copy").join(format!("{key}.json"))
}
pub fn plan(
    source: &Path,
    target: &Path,
    cache: &Path,
    verify: bool,
    job: &Job,
) -> Result<CopyPlan> {
    let source = f::root(source)?;
    let target = f::root(target)?;
    if source.starts_with(&target) || target.starts_with(&source) {
        bail!("Choose separate, non-overlapping staging and USB roots")
    }
    let _source_lock = library::lock_library(&source, cache)?;
    let _destination_lock = library::lock_library(&target, cache)?;
    job.say("Validating staging and media");
    let (files, games) = inventory(&source, job)?;
    let report = report_path(&source, &target, cache);
    let previous = fs::read(&report)
        .ok()
        .and_then(|b| serde_json::from_slice::<CopyPlan>(&b).ok());
    let mut items = vec![];
    let mut bytes = 0;
    for name in files {
        job.check()?;
        let src = f::safe(&source, &name)?;
        let dest = f::safe(&target, &name)?;
        let (size, source_stamp) = f::stamp(&src)?;
        let legacy = name
            .strip_prefix(".ohmy3do/")
            .filter(|s| !s.contains('/'))
            .map(|s| f::safe(&target, s))
            .transpose()?;
        let destination_stamp = if dest.is_file() {
            Some(f::stamp(&dest)?.1)
        } else {
            None
        };
        if dest.exists() && !dest.is_file() {
            bail!("Destination is occupied by a directory: {name}")
        }
        if name == ".ohmy3do/ohmy3do.ini" {
            if dest.is_file() {
                config(&dest)?
            } else if let Some(old) = legacy.as_ref().filter(|p| p.exists()) {
                config(old)?
            }
        }
        let mut trusted = false;
        let status =
            if personal(&name) && (dest.exists() || legacy.as_ref().is_some_and(|p| p.exists())) {
                "preserve"
            } else if !dest.exists() {
                "copy"
            } else if !dest.is_file() {
                bail!("Destination is occupied: {name}")
            } else {
                let ds = f::stamp(&dest)?;
                let disc = !name.starts_with(".ohmy3do/") && name != "boot.iso";
                let receipt = previous
                    .as_ref()
                    .and_then(|p| {
                        p.files
                            .iter()
                            .find(|r| r.path == name && (r.verified || r.status == "verified"))
                    })
                    .is_some_and(|r| {
                        r.size == size
                            && r.source_stamp == source_stamp
                            && r.destination_stamp == Some(ds.1)
                            && ds.0 == size
                    });
                if disc && !verify && size == ds.0 && (source_stamp == ds.1 || receipt) {
                    trusted = receipt;
                    "unchanged"
                } else if size == ds.0 && f::sha256(&src, job)? == f::sha256(&dest, job)? {
                    trusted = true;
                    "unchanged"
                } else if disc {
                    bail!("Different existing disc on destination: {name}")
                } else {
                    "update"
                }
            };
        if ["copy", "update"].contains(&status) {
            bytes += size
        }
        items.push(CopyItem {
            path: name,
            status: status.into(),
            size,
            source_stamp,
            destination_stamp,
            verified: trusted,
        });
    }
    if fs2::available_space(&target)? < bytes {
        bail!("Insufficient free space for the reviewed USB copy")
    }
    let plan = CopyPlan {
        source,
        destination: target,
        files: items,
        games,
        bytes,
    };
    f::atomic(&report, &serde_json::to_vec_pretty(&plan)?)?;
    job.say(format!(
        "USB preview: {} files; {} disc entries; {:.2} GiB to copy",
        plan.files.len(),
        games,
        bytes as f64 / 1024f64.powi(3)
    ));
    Ok(plan)
}
pub fn apply(plan: &CopyPlan, cache: &Path, job: &Job) -> Result<PathBuf> {
    if f::root(&plan.source)? != plan.source || f::root(&plan.destination)? != plan.destination {
        bail!("Copy roots changed since preview")
    }
    let _source_lock = library::lock_library(&plan.source, cache)?;
    let _destination_lock = library::lock_library(&plan.destination, cache)?;
    let mut tx = Journal::new(&plan.destination, cache)?;
    let mut receipt = plan.clone();
    let result = (|| -> Result<()> {
        let mut order = (0..plan.files.len()).collect::<Vec<_>>();
        order.sort_by_key(|i| {
            let name = &plan.files[*i].path;
            (
                if name == "boot.iso" {
                    2
                } else if name.ends_with(".3di") {
                    1
                } else {
                    0
                },
                name.clone(),
            )
        });
        for i in order {
            let file = &plan.files[i];
            if !["copy", "update"].contains(&file.status.as_str()) {
                continue;
            };
            job.check()?;
            let src = f::safe(&plan.source, &file.path)?;
            let dest = f::safe(&plan.destination, &file.path)?;
            if f::stamp(&src)? != (file.size, file.source_stamp) {
                bail!("Staging changed since preview: {}", file.path)
            }
            job.say(format!("Copying {}", file.path));
            if file.status == "copy" {
                tx.copy(&src, &file.path, None, job)?;
            } else {
                if f::stamp(&dest)?.1
                    != file
                        .destination_stamp
                        .context("Missing destination stamp")?
                {
                    bail!("Destination changed since preview")
                };
                let bytes = fs::read(&src)?;
                tx.write(&file.path, &bytes)?;
                if f::sha256(&dest, job)? != f::sha256(&src, job)? {
                    bail!("Copy readback failed")
                }
            }
            receipt.files[i].status = "verified".into();
            receipt.files[i].verified = true;
            receipt.files[i].destination_stamp = Some(f::stamp(&dest)?.1);
        }
        for file in &mut receipt.files {
            if file.status == "unchanged" && file.verified {
                file.status = "verified".into();
            }
        }
        tx.finish()?;
        f::atomic(
            &report_path(&plan.source, &plan.destination, cache),
            &serde_json::to_vec_pretty(&receipt)?,
        )?;
        Ok(())
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
    job.say("USB copy complete; existing settings and saves preserved");
    Ok(tx.path)
}
pub fn summary(plan: &CopyPlan) -> String {
    let mut counts = std::collections::BTreeMap::new();
    for f in &plan.files {
        *counts.entry(&f.status).or_insert(0) += 1
    }
    format!(
        "{} disc entries · {} files · {:.2} GiB to copy\n{}",
        plan.games,
        plan.files.len(),
        plan.bytes as f64 / 1024f64.powi(3),
        counts
            .into_iter()
            .map(|(s, n)| format!("{s}: {n}"))
            .collect::<Vec<_>>()
            .join(" · ")
    )
}
