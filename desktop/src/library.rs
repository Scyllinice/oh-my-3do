use crate::{
    disc::{self as d, rx},
    fsutil as f, media,
    model::*,
    Job,
};
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, HashMap, HashSet},
    fs,
    path::{Path, PathBuf},
};

pub fn lock_library(root: &Path, cache: &Path) -> Result<fs::File> {
    let directory = cache.join("native-locks");
    fs::create_dir_all(&directory)?;
    let key = format!("{:x}", Sha256::digest(root.to_string_lossy().as_bytes()));
    let file = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(directory.join(key))?;
    fs2::FileExt::try_lock_exclusive(&file)
        .context("Another native operation is already using this library; wait for it to finish")?;
    Ok(file)
}
pub fn classify(game: &Game) -> (String, String) {
    let cat = game.category.to_lowercase();
    let known = CATEGORIES
        .iter()
        .find(|c| c.to_lowercase() == cat)
        .copied()
        .or_else(|| match cat.as_str() {
            "video" | "audio" => Some("Multimedia"),
            "coverdiscs" => Some("Samplers"),
            "add-ons" => Some("Games"),
            _ => None,
        });
    if let Some(c) = known {
        return (c.into(), format!("Redump category: {}", game.category));
    }
    let genres = game.metadata["genre"]
        .as_str()
        .unwrap_or("")
        .split([';', ','])
        .map(|s| s.trim().to_lowercase())
        .collect::<HashSet<_>>();
    let release = game.metadata["release_type"]
        .as_str()
        .unwrap_or("")
        .to_lowercase();
    let mut evidence = HashSet::new();
    for (category, pattern, types) in [
        (
            "Bonus Discs",
            r"(?i)\bbonus\s+(?:disc|cd)\b",
            &["bonus disc", "bonus cd"][..],
        ),
        (
            "Samplers",
            r"(?i)\b(?:sampler|sample\s+(?:disc|cd))\b",
            &["sampler"][..],
        ),
        (
            "Demos",
            r"(?i)\((?:demo|demonstration|trial)[^)]*\)|\b(?:demo|demonstration|trial\s+version)\b",
            &["demo", "demonstration", "trial"][..],
        ),
        (
            "Applications",
            r"(?i)\b(?:storage\s+manager|system\s+utility|utilities|operating\s+system)\b",
            &[
                "utility",
                "utilities",
                "application",
                "applications",
                "operating system",
            ][..],
        ),
        (
            "Multimedia",
            r"(?i)\b(?:atlas|almanac|encyclop(?:e|ae)dia|dictionary|museum|art\s+gallery|documentary|photo\s*(?:cd|shoot)|slide\s*show|interactive\s+(?:book|magazine)|music\s+video|karaoke)\b",
            &[
                "multimedia",
                "reference",
                "documentary",
                "encyclopedia",
                "music video",
                "karaoke",
            ][..],
        ),
    ] {
        if rx(pattern).is_match(&game.release)
            || types.contains(&release.as_str())
            || types.iter().any(|s| genres.contains(*s))
        {
            evidence.insert(category);
        }
    }
    if (genres.contains("education")
        || genres.contains("educational")
        || rx(r"(?i)\beducational\b").is_match(&game.release))
        && !evidence
            .iter()
            .any(|c| ["Demos", "Samplers", "Multimedia", "Bonus Discs"].contains(c))
    {
        evidence.insert("Educational");
    }
    let game_genres = [
        "action",
        "adventure",
        "arcade",
        "board game",
        "casino",
        "compilation",
        "fighting",
        "flight simulator",
        "horror",
        "pinball",
        "platform",
        "puzzle",
        "racing",
        "role-playing",
        "shooter",
        "simulation",
        "sports",
        "strategy",
        "visual novel",
        "quiz",
        "sandbox",
        "party",
        "construction and management simulation",
    ];
    if evidence.contains("Multimedia")
        && game_genres.iter().any(|g| genres.contains(*g))
        && !genres.iter().any(|g| {
            [
                "education",
                "educational",
                "multimedia",
                "reference",
                "documentary",
                "encyclopedia",
            ]
            .contains(&g.as_str())
        })
    {
        return (
            "".into(),
            "Conflicting game/multimedia evidence; choose a category".into(),
        );
    }
    if evidence.len() == 1 {
        return (
            evidence.into_iter().next().unwrap().into(),
            "Catalog title/genre category".into(),
        );
    }
    if !evidence.is_empty() {
        return (
            "".into(),
            "Mixed category evidence; choose a category".into(),
        );
    }
    if rx(r"(?i)\((?:sample|preview)[^)]*\)").is_match(&game.release)
        || genres.contains("music")
        || genres.contains("life simulation")
    {
        return ("".into(), "Choose a software category".into());
    }
    if game_genres.iter().any(|g| genres.contains(*g)) {
        return ("Games".into(), "Catalog genre identifies a game".into());
    }
    ("".into(), "Choose a category".into())
}
pub fn assign(entry: &mut Entry, game: &Game, index: Option<usize>, category: Option<&str>) {
    entry.title = game.release.clone();
    entry.region = game
        .region
        .clone()
        .filter(|r| !r.is_empty())
        .unwrap_or_else(|| d::primary_region(&game.release));
    let (c, reason) = classify(game);
    entry.category = category.unwrap_or(&c).into();
    entry.category_reason = if category.is_some() {
        "Explicit category override".into()
    } else {
        reason
    };
    entry.game = index;
    if entry.category.is_empty() {
        entry.target.clear();
        entry.selected = false;
        entry.reason.push_str("; category needs review");
        return;
    }
    let group = rx(r"\s*\[Variant [0-9A-F]{8}\]")
        .replace_all(&d::without_disc(&entry.title), "")
        .into_owned();
    let directory = format!(
        "{}/{}/{}",
        entry.category,
        d::safe_name(&entry.region, 80),
        d::safe_name(&group, 110)
    );
    let stem = d::disc_stem(&entry.title);
    let count = entry.files.len();
    for (n, file) in entry.files.iter_mut().enumerate() {
        let ext = file
            .source
            .extension()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase();
        let track = if n > 0 && count > 2 {
            format!(" (Track {n:02})")
        } else {
            String::new()
        };
        file.target = format!("{directory}/{stem}{track}.{ext}");
    }
    entry.target = entry
        .files
        .first()
        .map(|f| f.target.clone())
        .unwrap_or_default();
}
fn serials(game: &Game) -> HashSet<String> {
    std::iter::once(game.serial.as_str())
        .chain(game.roms.iter().map(|r| r.serial.as_str()))
        .flat_map(|s| s.split([',', ';', '/']))
        .map(serial_key)
        .filter(|s| !s.is_empty())
        .collect()
}
fn serial_key(s: &str) -> String {
    s.to_lowercase()
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .collect()
}
fn identify(entry: &mut Entry, catalog: &Catalog, quick: bool) {
    let data = entry
        .files
        .iter()
        .filter(|f| {
            !f.source
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .eq_ignore_ascii_case("cue")
        })
        .collect::<Vec<_>>();
    let mut exact = vec![];
    let mut confidence = "checksum";
    if !quick {
        let mut hashes = data
            .iter()
            .map(|f| (f.fingerprint.size, f.fingerprint.sha1.clone()))
            .collect::<Vec<_>>();
        hashes.sort();
        exact = catalog
            .games
            .iter()
            .enumerate()
            .filter_map(|(n, g)| {
                let mut h = g
                    .roms
                    .iter()
                    .map(|r| (r.size, r.sha1.clone()))
                    .collect::<Vec<_>>();
                h.sort();
                (h == hashes).then_some(n)
            })
            .collect();
        if exact.is_empty() && data.len() == 1 {
            if let Some(raw) = &data[0].fingerprint.raw_sha1 {
                let size = data[0].fingerprint.size / 2048 * 2352;
                let complete = catalog
                    .games
                    .iter()
                    .enumerate()
                    .filter(|(_, g)| {
                        g.roms.len() == 1 && g.roms[0].size == size && &g.roms[0].sha1 == raw
                    })
                    .map(|(n, _)| n)
                    .collect::<Vec<_>>();
                let partial = catalog
                    .games
                    .iter()
                    .enumerate()
                    .filter(|(_, g)| {
                        g.roms.len() > 1
                            && g.roms
                                .iter()
                                .filter(|r| {
                                    rx(r"(?i)(?:\(Track\s+0*1\)|\btrack0*1)\.bin$")
                                        .is_match(&r.name)
                                        && r.size == size
                                        && &r.sha1 == raw
                                })
                                .count()
                                == 1
                    })
                    .map(|(n, _)| n)
                    .collect::<Vec<_>>();
                let all = complete
                    .iter()
                    .chain(partial.iter())
                    .copied()
                    .collect::<Vec<_>>();
                if all
                    .iter()
                    .map(|n| &catalog.games[*n].release)
                    .collect::<HashSet<_>>()
                    .len()
                    > 1
                {
                    entry.candidates = all;
                    entry.reason =
                        "Reconstructed checksum matches multiple releases; choose explicitly"
                            .into();
                    return;
                }
                if !complete.is_empty() {
                    exact = complete;
                    confidence = "converted-checksum";
                } else {
                    exact = partial;
                    confidence = "data-track";
                }
            }
        }
    }
    if !exact.is_empty() {
        let names = exact
            .iter()
            .map(|i| &catalog.games[*i].release)
            .collect::<HashSet<_>>();
        if names.len() == 1 {
            entry.confidence = confidence.into();
            entry.reason=match confidence{"converted-checksum"=>"Complete reconstructed MODE1 checksum match; original ISO unchanged","data-track"=>"First data-track checksum matched; missing audio tracks are unverified; review required",_=>"Complete data-file SHA-1 and size match"}.into();
            let i = exact[0];
            assign(entry, &catalog.games[i], Some(i), None);
            entry.selected = entry.resolvable() && confidence != "data-track";
            return;
        }
        entry.candidates = exact;
        entry.reason = "Checksums match multiple releases; choose one".into();
        return;
    }
    let name = entry
        .source
        .file_stem()
        .unwrap_or_default()
        .to_string_lossy();
    let norm = d::title_key(&name);
    let exact = catalog
        .games
        .iter()
        .enumerate()
        .filter(|(_, g)| {
            g.release.eq_ignore_ascii_case(&name)
                || d::disc_stem(&g.release).eq_ignore_ascii_case(&name)
        })
        .map(|(i, _)| i)
        .collect::<Vec<_>>();
    let mut candidates = if exact.is_empty() {
        catalog
            .games
            .iter()
            .enumerate()
            .filter(|(_, g)| {
                d::title_key(&g.name) == norm && d::disc_number(&g.name) == d::disc_number(&name)
            })
            .map(|(i, _)| i)
            .collect::<Vec<_>>()
    } else {
        exact
    };
    let mut regions = d::regions(&name);
    if regions.is_empty() {
        let hints = entry
            .source
            .parent()
            .into_iter()
            .flat_map(|p| p.components())
            .filter_map(|c| {
                let text = c.as_os_str().to_string_lossy();
                let name = rx(r"^\d+[\s._-]+").replace(&text, "");
                d::region(&name).map(str::to_owned)
            })
            .collect::<HashSet<_>>();
        if hints.len() == 1 {
            regions = hints
        }
    }
    if !regions.is_empty() {
        candidates.retain(|i| regions.is_subset(&d::regions(&catalog.games[*i].name)));
    }
    let codes = rx(r"\[([^]]+)\]")
        .captures_iter(&name)
        .map(|c| c[1].to_owned())
        .filter(|s| {
            s.len() >= 4
                && !s.to_lowercase().starts_with("variant ")
                && s.chars().any(|c| c.is_ascii_alphabetic())
                && s.chars().any(|c| c.is_ascii_digit())
        })
        .map(|s| serial_key(&s))
        .collect::<HashSet<_>>();
    if !codes.is_empty() {
        let serial = candidates
            .iter()
            .filter(|i| codes.is_subset(&serials(&catalog.games[**i])))
            .copied()
            .collect::<Vec<_>>();
        if !serial.is_empty() {
            candidates = serial
        } else {
            entry.candidates = candidates;
            entry.reason =
                "Product code does not uniquely identify a catalog release; choose explicitly"
                    .into();
            return;
        }
    }
    let names = candidates
        .iter()
        .map(|i| &catalog.games[*i].release)
        .collect::<HashSet<_>>();
    let tags = |s: &str| {
        rx(r"(?i)\(([^)]*(?:rev|beta|proto|demo|sample|unl|alt)[^)]*)\)")
            .captures_iter(s)
            .map(|c| c[1].to_lowercase())
            .collect::<HashSet<_>>()
    };
    if names.len() == 1 {
        let i = candidates[0];
        if tags(&name) == tags(&catalog.games[i].release) {
            entry.confidence = "filename".into();
            entry.reason="Title/region/disc match; checksum identity is unverified. Check explicitly to accept.".into();
            assign(entry, &catalog.games[i], Some(i), None);
            entry.selected = false;
            return;
        }
    }
    entry.candidates = candidates;
    if entry.candidates.is_empty() {
        entry.reason =
            "No safe catalog match. Choose a release or enter an unverified title.".into()
    } else {
        entry.reason = "Several releases or variants match; choose explicitly".into()
    }
}
pub fn scan(
    root: &Path,
    inputs: &[PathBuf],
    catalog: Catalog,
    quick: bool,
    job: &Job,
) -> Result<Plan> {
    let root = f::root(root)?;
    let sets = d::collect(inputs, Some(&root), job)?;
    if sets.is_empty() {
        bail!("No disc entries found")
    }
    let sizes = catalog
        .games
        .iter()
        .flat_map(|g| g.roms.iter())
        .map(|r| r.size)
        .collect::<HashSet<_>>();
    let mut entries = vec![];
    for (n, (source, files)) in sets.iter().enumerate() {
        job.check()?;
        job.say(format!(
            "Scanning {}/{}: {}",
            n + 1,
            sets.len(),
            source.display()
        ));
        let mut entry = Entry {
            source: source.clone(),
            title: source
                .file_stem()
                .unwrap_or_default()
                .to_string_lossy()
                .into_owned(),
            category: "".into(),
            region: "Unknown".into(),
            target: "".into(),
            confidence: "unmatched".into(),
            reason: "".into(),
            selected: false,
            files: vec![],
            candidates: vec![],
            game: None,
            cover: None,
            preview: None,
            category_reason: "".into(),
            blocked: false,
        };
        if source
            .extension()
            .unwrap_or_default()
            .to_string_lossy()
            .eq_ignore_ascii_case("cue")
        {
            let (text, _) = d::cue_refs(source)?;
            let modes = rx(r"(?im)^\s*TRACK\s+\d+\s+(\S+)")
                .captures_iter(&text)
                .map(|c| c[1].to_ascii_uppercase())
                .collect::<Vec<_>>();
            if !modes.is_empty() && modes.iter().all(|m| m == "AUDIO") {
                entry.reason =
                    "Audio-only disc: use the audio-disc workflow; no game identity assigned"
                        .into();
                entry.blocked = true;
                entries.push(entry);
                continue;
            }
        }
        for file in files {
            let size = fs::metadata(file)?.len();
            let reconstruct = !quick
                && files.len() == 1
                && file
                    .extension()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .eq_ignore_ascii_case("iso")
                && size % 2048 == 0
                && sizes.contains(&(size / 2048 * 2352));
            let fingerprint = if quick {
                let (size, mtime_ns) = f::stamp(file)?;
                Fingerprint {
                    size,
                    mtime_ns,
                    sha1: String::new(),
                    sha256: String::new(),
                    raw_sha1: None,
                }
            } else {
                d::fingerprint(file, reconstruct, job)?
            };
            entry.files.push(DiscFile {
                source: file.clone(),
                fingerprint,
                target: String::new(),
            });
        }
        identify(&mut entry, &catalog, quick);
        entries.push(entry);
    }
    let mut plan = Plan {
        root,
        catalog,
        entries,
    };
    collisions(&mut plan)?;
    Ok(plan)
}
pub fn collisions(plan: &mut Plan) -> Result<()> {
    for entry in &mut plan.entries {
        if entry.reason == "Destination is occupied or shared by another selected disc"
            || entry.reason == "Destination is already occupied"
            || entry.reason == "Destination shared by multiple source entries; check only one"
        {
            entry.blocked = false;
            entry.selected = false;
            entry.reason = "Review destination and check to import".into();
        }
    }
    let mut used: HashMap<String, usize> = HashMap::new();
    let mut occupied = vec![];
    let mut shared = vec![];
    for (i, e) in plan.entries.iter().enumerate() {
        if e.target.is_empty() {
            continue;
        }
        for file in &e.files {
            if f::safe(&plan.root, &file.target)?.exists() {
                occupied.push(i)
            }
            if let Some(other) = used.insert(f::fold(&file.target), i) {
                if other != i {
                    shared.extend([other, i]);
                }
            }
        }
    }
    for i in shared {
        plan.entries[i].selected = false;
        plan.entries[i].reason =
            "Destination shared by multiple source entries; check only one".into();
    }
    for i in occupied {
        plan.entries[i].blocked = true;
        plan.entries[i].selected = false;
        plan.entries[i].reason = "Destination is already occupied".into();
    }
    Ok(())
}

#[derive(Clone, Serialize, Deserialize)]
struct Created {
    path: String,
    sha256: String,
}
#[derive(Clone, Serialize, Deserialize)]
struct Written {
    path: String,
    before: Option<Vec<u8>>,
    after: String,
}
#[derive(Serialize, Deserialize)]
pub struct Journal {
    schema: u32,
    pub root: PathBuf,
    pub state: String,
    created: Vec<Created>,
    writes: Vec<Written>,
    directories: Vec<String>,
    #[serde(skip)]
    pub path: PathBuf,
}
impl Journal {
    pub fn new(root: &Path, cache: &Path) -> Result<Self> {
        let path = cache
            .join("native-transactions")
            .join(format!("{}.json", uuid::Uuid::new_v4()));
        let j = Self {
            schema: 1,
            root: f::root(root)?,
            state: "applying".into(),
            created: vec![],
            writes: vec![],
            directories: vec![],
            path,
        };
        j.save()?;
        Ok(j)
    }
    pub fn save(&self) -> Result<()> {
        f::atomic(&self.path, &serde_json::to_vec_pretty(self)?)?;
        Ok(())
    }
    fn parents(&mut self, path: &Path) -> Result<()> {
        let mut missing = vec![];
        let mut p = path.parent();
        while let Some(q) = p {
            if q == self.root || q.exists() {
                break;
            }
            missing.push(q.to_owned());
            p = q.parent();
        }
        for p in missing.into_iter().rev() {
            let name = relative(&self.root, &p)?;
            self.directories.push(name);
            self.save()?;
            fs::create_dir(&p)?;
        }
        Ok(())
    }
    pub fn write(&mut self, name: &str, data: &[u8]) -> Result<()> {
        let p = f::safe(&self.root, name)?;
        let before = if p.is_file() {
            if fs::metadata(&p)?.len() > 16 * 1024 * 1024 {
                bail!("Oversized support file")
            };
            Some(fs::read(&p)?)
        } else {
            None
        };
        if before.as_deref() == Some(data) {
            return Ok(());
        }
        self.parents(&p)?;
        self.writes.push(Written {
            path: name.into(),
            before,
            after: format!("{:x}", Sha256::digest(data)),
        });
        self.save()?;
        f::atomic(&p, data)
    }
    pub fn copy(
        &mut self,
        source: &Path,
        name: &str,
        expected: Option<&str>,
        job: &Job,
    ) -> Result<()> {
        let p = f::safe(&self.root, name)?;
        if p.exists() {
            bail!("Refusing to overwrite {}", p.display())
        }
        self.parents(&p)?;
        let hash = if let Some(h) = expected.filter(|h| !h.is_empty()) {
            h.to_owned()
        } else {
            f::sha256(source, job)?
        };
        self.created.push(Created {
            path: name.into(),
            sha256: hash.clone(),
        });
        self.save()?;
        let actual = f::copy_new(source, &p, job, Some(&hash))?;
        if actual != hash {
            bail!("Source contents changed since scan: {}", source.display())
        }
        self.save()?;
        Ok(())
    }
    pub fn finish(&mut self) -> Result<()> {
        self.state = "complete".into();
        self.save()
    }
    pub fn undo(&mut self) -> Result<()> {
        if self.state == "undone" {
            bail!("Transaction is already undone")
        }
        let job = Job::default();
        let mut simulated: HashMap<String, Option<Vec<u8>>> = HashMap::new();
        for entry in self.writes.iter().rev() {
            let path = f::safe(&self.root, &entry.path)?;
            let current = simulated
                .entry(entry.path.clone())
                .or_insert(if path.exists() {
                    Some(fs::read(&path)?)
                } else {
                    None
                });
            if *current != entry.before
                && current.as_ref().map(|b| format!("{:x}", Sha256::digest(b)))
                    != Some(entry.after.clone())
            {
                bail!("Undo refused: file changed after import: {}", entry.path)
            }
            *current = entry.before.clone();
        }
        for entry in &self.created {
            let path = f::safe(&self.root, &entry.path)?;
            if path.exists() {
                let original = simulated.get(&entry.path);
                let actual = match original {
                    Some(Some(b)) => format!("{:x}", Sha256::digest(b)),
                    Some(None) => String::new(),
                    None => f::sha256(&path, &job)?,
                };
                if actual != entry.sha256 {
                    bail!("Undo refused: imported file changed: {}", entry.path)
                }
            }
        }
        for entry in self.writes.iter().rev() {
            let path = f::safe(&self.root, &entry.path)?;
            if let Some(before) = &entry.before {
                f::atomic(&path, before)?
            } else if path.exists() {
                fs::remove_file(path)?
            }
        }
        for entry in self.created.iter().rev() {
            let path = f::safe(&self.root, &entry.path)?;
            if path.exists() {
                fs::remove_file(path)?
            }
        }
        for name in self.directories.iter().rev() {
            let path = f::safe(&self.root, name)?;
            if path.is_dir() {
                let _ = fs::remove_dir(path);
            }
        }
        self.state = "undone".into();
        self.save()
    }
}
pub fn undo(path: &Path) -> Result<()> {
    let mut journal: Journal = serde_json::from_slice(&fs::read(path)?)?;
    journal.path = path.to_owned();
    if journal.schema != 1 {
        bail!("Unsupported native transaction version")
    }
    let cache = path
        .parent()
        .and_then(Path::parent)
        .context("Invalid native journal location")?;
    let _lock = lock_library(&journal.root, cache)?;
    if f::root(&journal.root)? != journal.root {
        bail!("Transaction root changed")
    };
    journal.undo()
}
pub fn relative(root: &Path, p: &Path) -> Result<String> {
    let canonical_root;
    let canonical_path;
    let relative = if let Ok(r) = p.strip_prefix(root) {
        r
    } else {
        canonical_root = root.canonicalize()?;
        canonical_path = p.canonicalize()?;
        canonical_path.strip_prefix(&canonical_root)?
    };
    Ok(relative
        .components()
        .map(|c| c.as_os_str().to_str().context("Non-UTF8 filename"))
        .collect::<Result<Vec<_>>>()?
        .join("/"))
}

pub fn media_index(map: &BTreeMap<String, String>) -> Result<Vec<u8>> {
    let mut payload = vec![];
    for (key, name) in map {
        let k = key.as_bytes();
        let n = name.as_bytes();
        if k.len() + 1 > u16::MAX as usize || n.len() > 96 {
            bail!("Oversized media index entry")
        };
        payload.extend_from_slice(&((k.len() + 1) as u16).to_be_bytes());
        payload.extend_from_slice(&((n.len() + 1) as u16).to_be_bytes());
        payload.extend_from_slice(k);
        payload.push(0);
        payload.extend_from_slice(n);
        payload.push(0);
    }
    if map.len() > 2048 || payload.len() > 128 * 1024 {
        bail!("Media index exceeds menu capacity")
    }
    let mut out = b"OM3DIDX1".to_vec();
    for n in [
        map.len() as u32,
        payload.len() as u32,
        crc32fast::hash(&payload),
        0,
    ] {
        out.extend_from_slice(&n.to_be_bytes())
    }
    out.extend(payload);
    Ok(out)
}
pub fn read_media_index(root: &Path, folder: &str) -> Result<BTreeMap<String, String>> {
    let path = f::safe(root, &format!(".ohmy3do/{folder}/index.3di"))?;
    if !path.exists() {
        return Ok(BTreeMap::new());
    }
    if fs::metadata(&path)?.len() > 128 * 1024 + 24 {
        bail!("Oversized media index")
    }
    let data = fs::read(path)?;
    if data.len() < 24 || &data[..8] != b"OM3DIDX1" {
        bail!("Invalid {folder} index")
    }
    let word = |p| u32::from_be_bytes(data[p..p + 4].try_into().unwrap()) as usize;
    let count = word(8);
    if count > 2048
        || word(12) > 128 * 1024
        || word(12) + 24 != data.len()
        || word(20) != 0
        || crc32fast::hash(&data[24..]) as usize != word(16)
    {
        bail!("Invalid {folder} index checksum or bounds")
    }
    let mut map = BTreeMap::new();
    let mut pos = 24;
    for _ in 0..count {
        if pos + 4 > data.len() {
            bail!("Truncated media index")
        };
        let a = u16::from_be_bytes(data[pos..pos + 2].try_into().unwrap()) as usize;
        let b = u16::from_be_bytes(data[pos + 2..pos + 4].try_into().unwrap()) as usize;
        pos += 4;
        if a < 2 || b < 6 || pos + a + b > data.len() {
            bail!("Invalid media index string")
        };
        let key = &data[pos..pos + a];
        let name = &data[pos + a..pos + a + b];
        if key[a - 1] != 0
            || name[b - 1] != 0
            || key[..a - 1].contains(&0)
            || name[..b - 1].contains(&0)
        {
            bail!("Invalid media index terminator")
        };
        let key = std::str::from_utf8(&key[..a - 1])?;
        let name = std::str::from_utf8(&name[..b - 1])?;
        if name.len() > 96 || name.contains(['/', '\\', ':', '|']) || name.starts_with('.') {
            bail!("Unsafe media name")
        };
        f::safe(root, &format!(".ohmy3do/{folder}/{name}"))?;
        if map.insert(f::fold(key), name.into()).is_some() {
            bail!("Duplicate media association")
        };
        pos += a + b;
    }
    if pos != data.len() {
        bail!("Trailing media index data")
    };
    Ok(map)
}
pub fn build_indexes(root: &Path, tx: &mut Journal, job: &Job) -> Result<()> {
    let sets = d::collect(&[root.to_owned()], None, job)?;
    let mut names = sets
        .iter()
        .map(|(p, _)| relative(root, p))
        .collect::<Result<Vec<_>>>()?;
    names.sort_by_key(|s| f::fold(s));
    if names
        .iter()
        .map(|s| f::fold(s))
        .collect::<HashSet<_>>()
        .len()
        != names.len()
    {
        bail!("Case-colliding disc names")
    }
    let mut payload = vec![];
    for name in &names {
        if name.len() > 8192 || name.split('/').count() > 33 {
            bail!("Disc path exceeds navigation limits")
        };
        payload.extend_from_slice(&((name.len() + 1) as u16).to_be_bytes());
        payload.extend_from_slice(name.as_bytes());
        payload.push(0);
    }
    if names.len() > 2048 || payload.len() + 9 * names.len() > 128 * 1024 {
        bail!("Find index exceeds menu capacity")
    }
    let mut data = b"OM3DLIB1".to_vec();
    for n in [
        names.len() as u32,
        payload.len() as u32,
        crc32fast::hash(&payload),
        0,
    ] {
        data.extend_from_slice(&n.to_be_bytes())
    }
    data.extend(payload);
    data.extend_from_slice(b"END3DL01");
    tx.write(".ohmy3do/library.3di", &data)?;
    tx.write(
        ".ohmy3do/metadata.3di",
        &metadata_bytes(&database(root)?, &names)?,
    )?;
    Ok(())
}

fn metadata_text(value: &Value) -> String {
    use unicode_normalization::UnicodeNormalization;
    let text: String = value
        .as_str()
        .unwrap_or("")
        .nfkd()
        .filter_map(|c| {
            c.is_ascii()
                .then_some(if c.is_ascii_control() { ' ' } else { c })
        })
        .collect();
    text.split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(96)
        .collect()
}
fn metadata_date(meta: &Value) -> String {
    if let Some(date) = meta["release_date"].as_str().and_then(|s| s.get(..10)) {
        if rx(r"^\d{4}-\d{2}-\d{2}$").is_match(date) {
            let year = date[..4].parse::<u32>().unwrap_or(0);
            let month = date[5..7].parse::<usize>().unwrap_or(0);
            let day = date[8..10].parse::<u32>().unwrap_or(0);
            let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
            let days = [
                0,
                31,
                if leap { 29 } else { 28 },
                31,
                30,
                31,
                30,
                31,
                31,
                30,
                31,
                30,
                31,
            ];
            if year > 0 && month <= 12 && day > 0 && day <= days[month] {
                return date.into();
            }
        }
    }
    let year = meta["year"]
        .as_i64()
        .or_else(|| meta["year"].as_str().and_then(|s| s.parse().ok()));
    year.filter(|y| (1000..3000).contains(y))
        .map(|y| y.to_string())
        .unwrap_or_default()
}
pub fn metadata_bytes(db: &Value, names: &[String]) -> Result<Vec<u8>> {
    let allowed = names
        .iter()
        .map(|p| (f::fold(p), p))
        .collect::<HashMap<_, _>>();
    let mut records = BTreeMap::new();
    for record in db["games"].as_array().context("Invalid library records")? {
        let key = f::fold(record["path"].as_str().unwrap_or(""));
        let Some(path) = allowed.get(&key) else {
            continue;
        };
        let meta = &record["metadata"];
        let fields = [
            metadata_text(&meta["genre"]),
            metadata_text(&meta["publisher"]),
            metadata_date(meta),
        ];
        if fields.iter().all(String::is_empty) {
            continue;
        }
        if records.insert(key, ((*path).clone(), fields)).is_some() {
            bail!("Duplicate metadata path");
        }
    }
    let count = records.len();
    let mut payload = vec![];
    for (path, fields) in records.values() {
        for field in std::iter::once(path).chain(fields.iter()) {
            if field.len() > 8192 || field.contains('\0') {
                bail!("Invalid metadata field");
            }
            payload.extend_from_slice(&((field.len() + 1) as u16).to_be_bytes());
            payload.extend_from_slice(field.as_bytes());
            payload.push(0);
        }
    }
    if count > 2048 || payload.len() + 16 * count > 192 * 1024 {
        bail!("Console metadata index exceeds capacity");
    }
    let mut result = b"OM3DMET1".to_vec();
    for n in [
        count as u32,
        payload.len() as u32,
        crc32fast::hash(&payload),
        0,
    ] {
        result.extend_from_slice(&n.to_be_bytes());
    }
    result.extend_from_slice(&payload);
    result.extend_from_slice(b"END3DM01");
    metadata_paths(&result)?;
    Ok(result)
}
pub fn metadata_paths(data: &[u8]) -> Result<Vec<String>> {
    if data.len() < 32 || &data[..8] != b"OM3DMET1" || &data[data.len() - 8..] != b"END3DM01" {
        bail!("Invalid metadata header/footer");
    }
    let word = |i| u32::from_be_bytes(data[i..i + 4].try_into().unwrap()) as usize;
    let count = word(8);
    let bytes = word(12);
    if count > 2048
        || bytes > 192 * 1024
        || bytes + 16 * count > 192 * 1024
        || data.len() != bytes + 32
        || word(20) != 0
        || crc32fast::hash(&data[24..data.len() - 8]) as usize != word(16)
    {
        bail!("Invalid metadata checksum/bounds");
    }
    let mut paths = Vec::<String>::new();
    let mut pos = 24;
    for _ in 0..count {
        for field in 0..4 {
            let length = data.get(pos..pos + 2).context("Truncated metadata")?;
            let length = u16::from_be_bytes(length.try_into().unwrap()) as usize;
            pos += 2;
            let text = data
                .get(pos..pos + length)
                .context("Truncated metadata string")?;
            pos += length;
            if pos > data.len() - 8
                || length == 0
                || text[length - 1] != 0
                || text[..length - 1].contains(&0)
            {
                bail!("Invalid metadata string");
            }
            let text = &text[..length - 1];
            if field == 0 {
                let path = std::str::from_utf8(text)?;
                if text.len() > 8192
                    || path.split('/').count() > 33
                    || path
                        .split('/')
                        .any(|p| p.is_empty() || p == "." || p == ".." || p.len() >= 1024)
                    || path
                        .chars()
                        .any(|c| c.is_control() || c == '\\' || c == ':')
                    || ![".iso", ".cue"].iter().any(|e| f::fold(path).ends_with(e))
                    || paths.last().is_some_and(|p| f::fold(p) >= f::fold(path))
                {
                    bail!("Invalid or unsorted metadata path");
                }
                paths.push(path.into());
            } else if length > 97 || text.iter().any(|b| !(32..=126).contains(b)) {
                bail!("Invalid metadata text");
            }
        }
    }
    if pos != data.len() - 8 {
        bail!("Trailing metadata");
    }
    Ok(paths)
}
pub fn database(root: &Path) -> Result<Value> {
    let path = f::safe(root, ".ohmy3do/library.json")?;
    if !path.exists() {
        return Ok(json!({"schema":1,"sources":[],"games":[]}));
    }
    if fs::metadata(&path)?.len() > 16 * 1024 * 1024 {
        bail!("Oversized library database")
    };
    let data: Value = serde_json::from_slice(&fs::read(path)?)?;
    if data["schema"] != 1 || !data["games"].is_array() {
        bail!("Invalid library database")
    };
    Ok(data)
}
pub fn write_database(
    tx: &mut Journal,
    mut db: Value,
    records: Vec<Value>,
    sources: &[Value],
) -> Result<()> {
    let games = db["games"]
        .as_array_mut()
        .context("Invalid library records")?;
    for record in records {
        let path = record["path"].as_str().context("Missing disc path")?;
        games.retain(|r| r["path"].as_str() != Some(path));
        games.push(record);
    }
    games.sort_by(|a, b| a["path"].as_str().cmp(&b["path"].as_str()));
    let urls = db["sources"]
        .as_array_mut()
        .context("Missing database sources")?;
    for source in sources {
        if !urls.contains(source) {
            urls.push(source.clone())
        }
    }
    db["generated"] = json!(std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs_f64());
    tx.write(".ohmy3do/library.json", &serde_json::to_vec_pretty(&db)?)?;
    let dir = tempfile::tempdir()?;
    let path = dir.path().join("library.sqlite");
    {
        let mut sql = rusqlite::Connection::open(&path)?;
        let transaction = sql.transaction()?;
        transaction.execute_batch("CREATE TABLE games (path TEXT PRIMARY KEY, title TEXT, region TEXT, disc INTEGER, genre TEXT, publisher TEXT, year INTEGER, developer TEXT, identification TEXT, cover TEXT, metadata_source TEXT, variant TEXT, catalog_name TEXT, edition_unresolved INTEGER, category TEXT, category_review INTEGER, category_reason TEXT); CREATE TABLE sources (url TEXT, sha256 TEXT);")?;
        for source in db["sources"].as_array().unwrap() {
            transaction.execute(
                "INSERT INTO sources VALUES (?,?)",
                rusqlite::params![source["url"].as_str(), source["sha256"].as_str()],
            )?;
        }
        for record in db["games"].as_array().unwrap() {
            let r = record;
            let meta = &r["metadata"];
            transaction.execute(
                "INSERT INTO games VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                rusqlite::params![
                    r["path"].as_str(),
                    r["title"].as_str(),
                    r["region"].as_str(),
                    r["disc"].as_i64(),
                    meta["genre"].as_str(),
                    meta["publisher"].as_str(),
                    meta["year"].as_i64(),
                    meta["developer"].as_str(),
                    r["identification"].as_str(),
                    r["cover"].as_str(),
                    meta["source"].as_str(),
                    r["variant"].as_str(),
                    r["catalog_name"].as_str(),
                    r["edition_unresolved"].as_bool().unwrap_or(false) as i32,
                    r["category"].as_str(),
                    r["category_review"].as_bool().unwrap_or(true) as i32,
                    r["category_reason"].as_str()
                ],
            )?;
        }
        transaction.commit()?;
    }
    tx.write(".ohmy3do/library.sqlite", &fs::read(path)?)?;
    Ok(())
}
pub fn import(plan: &Plan, prefs: &Preferences, cache: &Path, job: &Job) -> Result<PathBuf> {
    let root = f::root(&plan.root)?;
    let _lock = lock_library(&root, cache)?;
    let mut entries = plan
        .entries
        .iter()
        .filter(|e| e.selected)
        .cloned()
        .collect::<Vec<_>>();
    if entries.is_empty() {
        bail!("Check at least one resolved entry")
    }
    let mut targets = HashSet::new();
    let mut sources = HashSet::new();
    for entry in &entries {
        if !entry.resolvable() {
            bail!("Resolve {} first", entry.title)
        }
        for file in &entry.files {
            let path = f::safe(&root, &file.target)?;
            if path.exists() || !targets.insert(f::fold(&file.target)) {
                bail!("Destination is occupied: {}", file.target)
            }
            if !sources.insert(file.source.clone()) {
                bail!("Entries share a source data file")
            };
            if file.source.canonicalize()?.starts_with(&root) {
                bail!("Import source is already inside the library")
            }
            if f::stamp(&file.source)? != (file.fingerprint.size, file.fingerprint.mtime_ns) {
                bail!("Source changed since scan")
            }
        }
    }
    let required = entries
        .iter()
        .flat_map(|e| e.files.iter())
        .map(|f| f.fingerprint.size)
        .sum::<u64>();
    if fs2::available_space(&root)? < required + 16 * 1024 * 1024 {
        bail!("Insufficient free space for the import")
    }
    let mut db = database(&root)?;
    let mut covers = read_media_index(&root, "covers")?;
    let mut previews = read_media_index(&root, "previews")?;
    let mut prepared = media::Outputs::new();
    for entry in &mut entries {
        job.check()?;
        media::prepare_entry(entry, prefs, cache, &mut prepared, job)?;
    }
    let marker = f::safe(&root, ".ohmy3do/ohmy3do.ini")?;
    if !marker.exists()
        && ["ohmy3do.ini", "covers", "previews", "library.3di"]
            .iter()
            .any(|p| root.join(p).exists())
    {
        bail!("Migrate legacy support files before importing into this library")
    }
    let mut tx = Journal::new(&root, cache)?;
    job.say(format!("Undo journal: {}", tx.path.display()));
    let result = (|| -> Result<()> {
        if !marker.exists() {
            tx.write(".ohmy3do/ohmy3do.ini", include_bytes!("../../ohmy3do.ini"))?;
        }
        let mut records = vec![];
        for (n, entry) in entries.iter().enumerate() {
            job.check()?;
            job.say(format!(
                "Importing {}/{}: {}",
                n + 1,
                entries.len(),
                entry.title
            ));
            for file in &entry.files {
                job.say(format!("Copying and verifying {}", file.source.display()));
                if f::stamp(&file.source)? != (file.fingerprint.size, file.fingerprint.mtime_ns) {
                    bail!("Source changed since scan: {}", file.source.display())
                }
                tx.copy(
                    &file.source,
                    &file.target,
                    Some(&file.fingerprint.sha256),
                    job,
                )?;
            }
            if entry
                .source
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .eq_ignore_ascii_case("cue")
            {
                let copied = fs::read(root.join(&entry.target))?;
                let expected = tx
                    .created
                    .iter()
                    .find(|file| file.path == entry.target)
                    .context("Missing copied CUE receipt")?;
                if format!("{:x}", Sha256::digest(&copied)) != expected.sha256 {
                    bail!("Copied CUE changed during import")
                }
                let text = d::decode_cue(&copied)?;
                let mapping = entry
                    .files
                    .iter()
                    .map(|f| {
                        (
                            f.source
                                .file_name()
                                .unwrap()
                                .to_string_lossy()
                                .to_lowercase(),
                            Path::new(&f.target)
                                .file_name()
                                .unwrap()
                                .to_string_lossy()
                                .into_owned(),
                        )
                    })
                    .collect::<HashMap<_, _>>();
                let text = rx(r#"(?im)^(\s*FILE\s+)(?:"([^"\r\n]+)"|(\S+))(\s+BINARY[^\r\n]*)"#)
                    .replace_all(&text, |c: &regex::Captures<'_>| {
                        format!(
                            "{}\"{}\"{}",
                            &c[1],
                            mapping
                                .get(
                                    &c.get(2)
                                        .or_else(|| c.get(3))
                                        .unwrap()
                                        .as_str()
                                        .to_lowercase()
                                )
                                .unwrap(),
                            &c[4]
                        )
                    })
                    .into_owned();
                tx.write(&entry.target, text.as_bytes())?;
            }
            let game = entry.game.map(|i| &plan.catalog.games[i]);
            let metadata = game
                .map(|g| g.metadata.clone())
                .unwrap_or_else(|| json!({}));
            let mut record = json!({"path":entry.target,"title":entry.title,"region":entry.region,"category":entry.category,"category_review":false,"category_reason":entry.category_reason,"catalog_name":game.map(|g|&g.name),"variant":game.and_then(|g|g.variant.as_ref()),"edition_unresolved":false,"disc":d::disc_number(&entry.title),"identification":entry.confidence,"status":if entry.confidence=="unverified"{"registered"}else{"organized"},"metadata":metadata,"cover":null,"cover_source":null,"preview":null,"preview_source":null,"suggestions":[]});
            if entry.confidence == "unverified" {
                let mut proof = serde_json::Map::new();
                for file in &entry.files {
                    let fingerprint = d::fingerprint(&root.join(&file.target), false, job)?;
                    proof.insert(file.target.clone(),json!({"size":fingerprint.size,"mtime_ns":fingerprint.mtime_ns,"sha1":fingerprint.sha1}));
                }
                record["registration"] = Value::Object(proof);
            }
            for (kind, map) in [("cover", &mut covers), ("preview", &mut previews)] {
                if let Some(asset) = prepared.get(&(entry.target.clone(), kind.to_owned())) {
                    map.insert(f::fold(&entry.target), asset.name.clone());
                    record[kind] = json!(asset.name);
                    record[format!("{kind}_source")] = json!(asset.source);
                }
            }
            records.push(record);
        }
        let mut written = HashSet::new();
        for ((_, kind), asset) in &prepared {
            let folder = if kind == "cover" {
                "covers"
            } else {
                "previews"
            };
            let relative = format!(".ohmy3do/{folder}/{}", asset.name);
            if written.insert(relative.clone()) {
                let bytes = fs::read(&asset.path)?;
                let path = f::safe(&root, &relative)?;
                if path.exists() && fs::read(&path)? != bytes {
                    bail!("Existing media differs")
                };
                tx.write(&relative, &bytes)?;
            }
        }
        if !covers.is_empty() {
            tx.write(".ohmy3do/covers/index.3di", &media_index(&covers)?)?
        }
        if !previews.is_empty() {
            tx.write(".ohmy3do/previews/index.3di", &media_index(&previews)?)?
        }
        write_database(&mut tx, db.take(), records, &plan.catalog.sources)?;
        build_indexes(&root, &mut tx, job)?;
        job.check()?;
        tx.finish()?;
        Ok(())
    })();
    if let Err(error) = result {
        job.say("Restoring the library after interrupted import");
        if let Err(undo) = tx.undo() {
            bail!(
                "{error:#}. Automatic rollback could not finish: {undo:#}. Keep {}",
                tx.path.display()
            )
        }
        return Err(error);
    }
    job.say(format!(
        "Imported {} disc entries; originals unchanged",
        entries.len()
    ));
    Ok(tx.path)
}

pub fn update_media(
    root: &Path,
    prefs: &Preferences,
    cache: &Path,
    provided: Option<(&str, Option<PathBuf>, Option<PathBuf>)>,
    job: &Job,
) -> Result<PathBuf> {
    update_media_mode(root, prefs, cache, provided, false, job)
}
pub fn rebuild_media(root: &Path, prefs: &Preferences, cache: &Path, job: &Job) -> Result<PathBuf> {
    update_media_mode(root, prefs, cache, None, true, job)
}
fn update_media_mode(
    root: &Path,
    prefs: &Preferences,
    cache: &Path,
    provided: Option<(&str, Option<PathBuf>, Option<PathBuf>)>,
    rebuild: bool,
    job: &Job,
) -> Result<PathBuf> {
    let root = f::root(root)?;
    let _lock = lock_library(&root, cache)?;
    let mut db = database(&root)?;
    let mut covers = read_media_index(&root, "covers")?;
    let mut previews = read_media_index(&root, "previews")?;
    let mut outputs = media::Outputs::new();
    let mut records = vec![];
    for record in db["games"].as_array().context("Invalid library records")? {
        job.check()?;
        let path = record["path"].as_str().context("Missing disc path")?;
        let source = f::safe(&root, path)?;
        if !source.is_file() {
            bail!("Missing database disc: {path}")
        }
        if let Some((selected, _, _)) = &provided {
            if path != *selected {
                continue;
            }
        }
        let mut entry = Entry {
            source,
            title: record["title"].as_str().unwrap_or(path).into(),
            category: record["category"].as_str().unwrap_or("Games").into(),
            region: record["region"].as_str().unwrap_or("Unknown").into(),
            target: path.into(),
            confidence: String::new(),
            reason: String::new(),
            selected: false,
            files: vec![],
            candidates: vec![],
            game: None,
            cover: provided.as_ref().and_then(|(_, c, _)| c.clone()),
            preview: provided.as_ref().and_then(|(_, _, p)| p.clone()),
            category_reason: String::new(),
            blocked: false,
        };
        if rebuild {
            for (kind, destination) in
                [("cover", &mut entry.cover), ("preview", &mut entry.preview)]
            {
                if let Some(origin) = record[format!("{kind}_source")]
                    .as_str()
                    .filter(|s| !s.is_empty())
                {
                    match media::original_source(kind, origin, prefs, cache, job) {
                        Ok(source) => {
                            if source.is_none() {
                                job.say(format!(
                                    "Original {kind} unavailable; kept existing: {path}"
                                ));
                            }
                            *destination = source;
                        }
                        Err(e) => {
                            job.check()?;
                            job.say(format!(
                                "Original {kind} unavailable; kept existing: {path}: {e:#}"
                            ));
                        }
                    }
                }
            }
        }
        let existing =
            |kind: &str, map: &std::collections::BTreeMap<String, String>| -> Result<bool> {
                let name = map
                    .get(&f::fold(path))
                    .map(String::as_str)
                    .or_else(|| record[kind].as_str());
                Ok(if let Some(name) = name {
                    f::safe(
                        &root,
                        &format!(
                            ".ohmy3do/{}/{name}",
                            if kind == "cover" {
                                "covers"
                            } else {
                                "previews"
                            }
                        ),
                    )?
                    .is_file()
                } else {
                    false
                })
            };
        let want_cover = entry.cover.is_some() || (!rebuild && !existing("cover", &covers)?);
        let want_preview =
            entry.preview.is_some() || (!rebuild && !existing("preview", &previews)?);
        media::prepare_missing(
            &mut entry,
            prefs,
            cache,
            &mut outputs,
            want_cover,
            want_preview,
            job,
        )?;
        let mut changed = record.clone();
        let mut dirty = false;
        for (kind, map) in [("cover", &mut covers), ("preview", &mut previews)] {
            if let Some(asset) = outputs.get(&(path.into(), kind.into())) {
                map.insert(f::fold(path), asset.name.clone());
                changed[kind] = json!(asset.name);
                changed[format!("{kind}_source")] = if rebuild {
                    record[format!("{kind}_source")].clone()
                } else {
                    json!(asset.source)
                };
                dirty = true;
            }
        }
        if dirty {
            records.push(changed)
        }
    }
    let mut tx = Journal::new(&root, cache)?;
    let result = (|| -> Result<()> {
        let mut written = HashSet::new();
        for ((_, kind), asset) in &outputs {
            let name = format!(
                ".ohmy3do/{}/{}",
                if kind == "cover" {
                    "covers"
                } else {
                    "previews"
                },
                asset.name
            );
            if written.insert(name.clone()) {
                let bytes = fs::read(&asset.path)?;
                let path = f::safe(&root, &name)?;
                if path.exists() && fs::read(&path)? != bytes {
                    bail!("Existing media differs")
                };
                tx.write(&name, &bytes)?;
            }
        }
        if !records.is_empty() {
            if !covers.is_empty() {
                tx.write(".ohmy3do/covers/index.3di", &media_index(&covers)?)?;
            }
            if !previews.is_empty() {
                tx.write(".ohmy3do/previews/index.3di", &media_index(&previews)?)?;
            }
            let sources = db["sources"].as_array().cloned().unwrap_or_default();
            write_database(&mut tx, db.take(), records, &sources)?;
        }
        job.check()?;
        tx.finish()
    })();
    if let Err(error) = result {
        tx.undo()?;
        return Err(error);
    }
    job.say(format!(
        "Media update complete: {} associations prepared; existing media retained",
        outputs.len()
    ));
    Ok(tx.path)
}
pub fn review(root: &Path, cache: &Path) -> Result<PathBuf> {
    use base64::Engine;
    let db = database(root)?;
    let mut page=String::from("<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\"><title>3DO artwork review</title><style>body{font:16px system-ui;background:#101c28;color:#e2eaf0;margin:2rem}table{width:100%;border-collapse:collapse}td,th{padding:.8rem;border:1px solid #486273;text-align:left}img{width:84px;height:105px}input,button{font:inherit;padding:.5rem}</style><h1>3DO artwork review</h1><input id=\"q\" placeholder=\"Find a title or path\" oninput=\"filter()\"><button onclick=\"missing=!missing;filter()\">Toggle missing only</button><table><thead><tr><th>Cover</th><th>Game / disc</th><th>Preview</th></tr></thead><tbody>");
    let cover = read_media_index(root, "covers")?;
    let preview = read_media_index(root, "previews")?;
    let esc = |s: &str| {
        s.replace('&', "&amp;")
            .replace('<', "&lt;")
            .replace('>', "&gt;")
            .replace('"', "&quot;")
    };
    for r in db["games"].as_array().context("Invalid library")? {
        let path = r["path"].as_str().context("Missing path")?;
        let image = cover.get(&f::fold(path));
        let mut thumbnail = String::from("Missing cover");
        if let Some(name) = image {
            let file = f::safe(root, &format!(".ohmy3do/covers/{name}"))?;
            if fs::metadata(&file)?.len() < 512 * 1024 {
                let mut png = std::io::Cursor::new(Vec::new());
                media::cover_display(&file)?.write_to(&mut png, image::ImageFormat::Png)?;
                thumbnail = format!(
                    "<img alt=\"Cover\" src=\"data:image/png;base64,{}\">",
                    base64::engine::general_purpose::STANDARD.encode(png.get_ref())
                );
            }
        }
        let clip = preview.get(&f::fold(path));
        page.push_str(&format!(
            "<tr data-missing=\"{}\"><td>{}</td><td>{}<br><small>{}</small></td><td>{}</td></tr>",
            image.is_none() || clip.is_none(),
            thumbnail,
            esc(r["title"].as_str().unwrap_or(path)),
            esc(path),
            esc(clip.map(String::as_str).unwrap_or("Missing preview"))
        ));
    }
    page.push_str("</tbody></table><script>let missing=false;function filter(){const q=document.getElementById('q').value.toLowerCase();document.querySelectorAll('tbody tr').forEach(r=>r.hidden=(missing&&r.dataset.missing!=='true')||!r.textContent.toLowerCase().includes(q));}</script></html>");
    let path = cache.join("reports/artwork-review.html");
    f::atomic(&path, page.as_bytes())?;
    Ok(path)
}
