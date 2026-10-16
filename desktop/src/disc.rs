use crate::{fsutil as f, model::*, Job};
use anyhow::{bail, Context, Result};
use regex::Regex;
use sha1::{Digest, Sha1};
use sha2::Sha256;
use std::{
    collections::{HashMap, HashSet},
    fs::{self, File},
    io::Read,
    path::{Path, PathBuf},
};
use unicode_normalization::UnicodeNormalization;
pub fn rx(s: &str) -> Regex {
    Regex::new(s).expect("constant regex")
}
pub fn title_key(s: &str) -> String {
    let s = rx(r"\([^)]*\)|\[[^]]*\]").replace_all(s, "");
    let s = rx(r"(?i),\s*(the|a|an)$").replace(s.trim(), "");
    let s = rx(r"(?i)^(the|a|an)\s+").replace(&s, "");
    s.nfkd()
        .filter(|c| !unicode_normalization::char::is_combining_mark(*c))
        .flat_map(char::to_lowercase)
        .filter(|c| c.is_alphanumeric())
        .collect()
}
pub fn disc_number(s: &str) -> Option<u32> {
    rx(r"(?i)(?:\(\s*(?:disc|disk|cd)\s*0*(\d+)(?:\s+of\s+\d+)?\s*\)|(?:\s*[-_.]\s*|\s+)(?:disc|disk|cd)[\s_.-]*0*(\d+)(?:\s+of\s+\d+)?$)").captures(s).and_then(|c|c.get(1).or_else(||c.get(2))).and_then(|m|m.as_str().parse().ok())
}
pub fn without_disc(s: &str) -> String {
    rx(r"(?i)\s*\(\s*(?:disc|disk|cd)\s*\d+(?:\s+of\s+\d+)?\s*\)|(?:\s*[-_.]\s*|\s+)(?:disc|disk|cd)[\s_.-]*\d+(?:\s+of\s+\d+)?$").replace_all(s,"").trim().to_owned()
}
pub fn region(s: &str) -> Option<&'static str> {
    match s.trim().to_ascii_lowercase().as_str() {
        "us" | "usa" | "na" => Some("USA"),
        "eu" | "europe" | "pal" | "uk" | "gb" => Some("Europe"),
        "jp" | "japan" | "j" => Some("Japan"),
        "korea" | "kr" => Some("Korea"),
        "germany" => Some("Germany"),
        "france" => Some("France"),
        "world" => Some("World"),
        "australia" => Some("Australia"),
        "canada" => Some("Canada"),
        _ => None,
    }
}
pub fn regions(s: &str) -> HashSet<String> {
    rx(r"\(([^)]*)\)")
        .captures_iter(s)
        .flat_map(|c| {
            c[1].split([',', '/', '-'])
                .filter_map(region)
                .map(str::to_owned)
                .collect::<Vec<_>>()
        })
        .collect()
}
pub fn primary_region(s: &str) -> String {
    rx(r"\(([^)]*)\)")
        .captures_iter(s)
        .find_map(|c| region(c[1].split(',').next().unwrap_or("")))
        .unwrap_or("Unknown")
        .to_owned()
}
pub fn safe_name(s: &str, max: usize) -> String {
    let mut s = s
        .chars()
        .map(|c| {
            if c < ' ' || "<>:\"/\\|?*".contains(c) {
                '_'
            } else {
                c
            }
        })
        .collect::<String>()
        .trim()
        .trim_end_matches([' ', '.'])
        .to_owned();
    if s.is_empty() {
        s = "Untitled".into()
    }
    if rx(r"(?i)^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)").is_match(&s) {
        s.insert(0, '_')
    }
    if s.len() > max {
        let tail = format!("-{:x}", Sha1::digest(s.as_bytes()));
        let mut end = max.saturating_sub(9);
        while !s.is_char_boundary(end) {
            end -= 1
        }
        s = format!("{}{}", s[..end].trim_end_matches([' ', '.']), &tail[..9]);
    }
    s
}
pub fn disc_stem(s: &str) -> String {
    let mut base = without_disc(s);
    let disc = disc_number(s)
        .map(|n| format!(" (Disc {n})"))
        .unwrap_or_default();
    let tags = rx(r"\s*((?:\[[^\[\]]+\]\s*)+)$")
        .captures(&base)
        .and_then(|c| c.get(1).map(|m| (m.start(), m.as_str().trim().to_owned())));
    let mut suffix = disc;
    if let Some((pos, tags)) = tags {
        if tags.len() + suffix.len() + 20 < 110 {
            suffix = format!(" {tags}{suffix}");
            base = base[..pos].trim().to_owned();
        }
    }
    format!("{}{}", safe_name(&base, 110 - suffix.len()), suffix)
}

pub fn issue(p: &Path) -> Result<()> {
    let mut h = [0; 16];
    let n = File::open(p)?.read(&mut h)?;
    if n == 0 {
        bail!("Empty disc: {}", p.display())
    }
    for sig in [
        &b"PK\x03\x04"[..],
        b"PK\x05\x06",
        b"PK\x07\x08",
        b"7z\xbc\xaf\x27\x1c",
        b"Rar!\x1a\x07",
        b"\x1f\x8b",
        b"BZh",
        b"\xfd7zXZ\0",
    ] {
        if h[..n].starts_with(sig) {
            bail!(
                "Archive disguised as disc data: {}. Extract it first.",
                p.display()
            )
        }
    }
    Ok(())
}
pub fn decode_cue(raw: &[u8]) -> Result<String> {
    let text = match std::str::from_utf8(raw) {
        Ok(text) => text.to_owned(),
        Err(_) => {
            let (text, errors) = encoding_rs::WINDOWS_1252.decode_without_bom_handling(raw);
            if errors {
                bail!("Unsupported CUE text encoding")
            };
            text.into_owned()
        }
    };
    Ok(text.trim_start_matches('\u{feff}').to_owned())
}
pub fn cue_refs(p: &Path) -> Result<(String, Vec<PathBuf>)> {
    if fs::metadata(p)?.len() > 256 * 1024 {
        bail!("Oversized CUE")
    }
    let raw = fs::read(p)?;
    let text = decode_cue(&raw)?;
    let pattern = rx(r#"(?im)^\s*FILE\s+(?:"([^"\r\n]+)"|(\S+))\s+BINARY\s*$"#);
    let parent = p.parent().context("CUE parent missing")?;
    let mut refs = vec![];
    for c in pattern.captures_iter(&text) {
        let name = c.get(1).or_else(|| c.get(2)).unwrap().as_str();
        if name.contains(['/', '\\', ':']) || name == ".." {
            bail!("CUE FILE must name a sibling file")
        }
        let matches = fs::read_dir(parent)?
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().eq_ignore_ascii_case(name))
            .map(|e| e.path())
            .collect::<Vec<_>>();
        if matches.len() != 1 || !matches[0].is_file() || f::is_link(&matches[0]) {
            bail!("Missing or ambiguous CUE track: {name}")
        }
        let q = matches[0].canonicalize()?;
        if !["bin", "iso", "wav", "aiff", "aif"].contains(
            &q.extension()
                .unwrap_or_default()
                .to_string_lossy()
                .to_ascii_lowercase()
                .as_str(),
        ) {
            bail!("Unsupported CUE track type")
        }
        if !refs.contains(&q) {
            refs.push(q)
        }
    }
    if refs.is_empty()
        || rx(r"(?im)^\s*FILE\b").find_iter(&text).count() != pattern.captures_iter(&text).count()
    {
        bail!("Unsupported CUE directives; use BINARY tracks")
    }
    Ok((text, refs))
}
pub fn redundant_cue(p: &Path) -> Result<Option<PathBuf>> {
    let Ok((text, refs)) = cue_refs(p) else {
        return Ok(None);
    };
    if refs.len() != 1
        || refs[0]
            .extension()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase()
            != "iso"
    {
        return Ok(None);
    }
    let mut directives = vec![];
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || rx(r"(?i)^REM(?:\s|$)").is_match(line) {
            continue;
        }
        let token = if rx(r#"(?i)^FILE\s+"[^"\r\n]+"\s+BINARY$"#).is_match(line) {
            "file"
        } else if rx(r"(?i)^TRACK\s+0*1\s+MODE1/2048$").is_match(line) {
            "track"
        } else if rx(r"(?i)^INDEX\s+0*1\s+00:00:00$").is_match(line) {
            "index"
        } else if rx(r#"(?i)^(?:CATALOG\s+\d{13}|(?:TITLE|PERFORMER|SONGWRITER)\s+"[^"\r\n]*")$"#)
            .is_match(line)
        {
            continue;
        } else {
            return Ok(None);
        };
        directives.push(token);
    }
    Ok((directives == ["file", "track", "index"]).then(|| refs[0].clone()))
}
pub fn collect(
    inputs: &[PathBuf],
    library: Option<&Path>,
    job: &Job,
) -> Result<Vec<(PathBuf, Vec<PathBuf>)>> {
    let mut candidates = HashSet::new();
    for input in inputs {
        job.check()?;
        if f::is_link(input) {
            bail!("Input is a link or junction")
        };
        let p = input.canonicalize()?;
        if let Some(root) = library {
            if p.starts_with(root) || (p.is_dir() && root.starts_with(&p)) {
                bail!("Import sources must be outside the destination library")
            }
        }
        if p.is_dir() {
            for entry in walkdir::WalkDir::new(&p)
                .follow_links(false)
                .into_iter()
                .filter_entry(|e| {
                    !f::is_link(e.path())
                        && (e.depth() == 0
                            || !e.file_type().is_dir()
                            || !e.file_name().to_string_lossy().starts_with('.')
                                && !["covers", "previews", "saves", "music", "$boot", "system"]
                                    .contains(
                                        &e.file_name()
                                            .to_string_lossy()
                                            .to_ascii_lowercase()
                                            .as_str(),
                                    ))
                })
            {
                let e = entry?;
                if e.file_type().is_file() {
                    let ext = e
                        .path()
                        .extension()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .to_ascii_lowercase();
                    if ["iso", "cue"].contains(&ext.as_str())
                        && !(e.depth() == 1
                            && ["boot.iso", "menu-demo.iso"].contains(
                                &e.file_name()
                                    .to_string_lossy()
                                    .to_ascii_lowercase()
                                    .as_str(),
                            ))
                    {
                        candidates.insert(e.path().canonicalize()?);
                    }
                }
            }
        } else {
            let ext = p
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .to_ascii_lowercase();
            if !["iso", "cue"].contains(&ext.as_str()) {
                bail!("Select ISO/CUE files or directories")
            };
            candidates.insert(p);
        }
    }
    let mut consumed = HashSet::new();
    let mut sets = vec![];
    let mut ordered = candidates.into_iter().collect::<Vec<_>>();
    ordered.sort();
    for p in &ordered {
        if p.extension()
            .unwrap_or_default()
            .to_string_lossy()
            .eq_ignore_ascii_case("cue")
        {
            if let Some(iso) = redundant_cue(p)? {
                if !ordered.contains(&iso) {
                    ordered_contains_iso(&mut sets, &mut consumed, &iso)?;
                }
                consumed.insert(p.clone());
                continue;
            }
            let (_, refs) = cue_refs(p)?;
            let mut files = vec![p.clone()];
            for r in refs {
                consumed.insert(r.clone());
                files.push(r)
            }
            sets.push((p.clone(), files));
            consumed.insert(p.clone());
        }
    }
    for p in ordered {
        if !consumed.contains(&p) {
            issue(&p)?;
            sets.push((p.clone(), vec![p]));
        }
    }
    let mut owners = HashMap::new();
    for (_, files) in &sets {
        for p in files {
            *owners.entry(p).or_insert(0) += 1;
        }
    }
    if owners.values().any(|n| *n > 1) {
        bail!("Selected CUE/ISO entries share data files; select one representation")
    }
    sets.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(sets)
}
fn ordered_contains_iso(
    sets: &mut Vec<(PathBuf, Vec<PathBuf>)>,
    seen: &mut HashSet<PathBuf>,
    p: &Path,
) -> Result<()> {
    if seen.insert(p.to_owned()) {
        issue(p)?;
        sets.push((p.to_owned(), vec![p.to_owned()]));
    }
    Ok(())
}

fn sector_tables() -> &'static ([u32; 256], [u8; 256], [u8; 256]) {
    static TABLES: std::sync::OnceLock<([u32; 256], [u8; 256], [u8; 256])> =
        std::sync::OnceLock::new();
    TABLES.get_or_init(|| {
        let mut crc = [0u32; 256];
        let mut double = [0u8; 256];
        let mut inverse = [0u8; 256];
        for v in 0..256 {
            let mut n = v as u32;
            for _ in 0..8 {
                n = (n >> 1) ^ if n & 1 != 0 { 0xd8018001 } else { 0 }
            }
            crc[v] = n;
            let n = ((v << 1) ^ if v & 128 != 0 { 0x11d } else { 0 }) as u8;
            double[v] = n;
            inverse[(n ^ (v as u8)) as usize] = v as u8;
        }
        (crc, double, inverse)
    })
}
pub fn encode_sector(cooked: &[u8], lba: u32) -> Result<[u8; 2352]> {
    if cooked.len() != 2048 || lba >= 449850 {
        bail!("Invalid MODE1 sector")
    }
    let mut out = [0u8; 2352];
    out[1..11].fill(255);
    let frame = lba + 150;
    let bcd = |n: u32| ((n / 10) * 16 + n % 10) as u8;
    out[12..16].copy_from_slice(&[bcd(frame / 4500), bcd(frame / 75 % 60), bcd(frame % 75), 1]);
    out[16..2064].copy_from_slice(cooked);
    let (crc_table, double, inverse) = sector_tables();
    let mut crc = 0u32;
    for byte in &out[..2064] {
        crc = (crc >> 8) ^ crc_table[((crc ^ *byte as u32) & 255) as usize];
    }
    out[2064..2068].copy_from_slice(&crc.to_le_bytes());
    for (columns, rows, step, stride, output) in [(86, 24, 2, 86, 2076), (52, 43, 86, 88, 2248)] {
        for column in 0..columns {
            let mut pos = (column / 2) * step + column % 2;
            let (mut a, mut b) = (0u8, 0u8);
            for _ in 0..rows {
                let v = out[12 + pos];
                a ^= v;
                b = double[(b ^ v) as usize];
                pos = (pos + stride) % (columns * rows);
            }
            let p = inverse[(double[b as usize] ^ a) as usize];
            out[output + column] = p;
            out[output + columns + column] = p ^ a;
        }
    }
    Ok(out)
}
pub fn fingerprint(p: &Path, reconstruct: bool, job: &Job) -> Result<Fingerprint> {
    issue(p)?;
    let before = f::stamp(p)?;
    let raw = reconstruct && before.0 % 2048 == 0 && before.0 / 2048 <= 449850;
    let (mut original, mut sha, mut converted) = (Sha1::new(), Sha256::new(), Sha1::new());
    let mut file = File::open(p)?;
    let mut bytes = vec![0; 2048 * 512];
    let mut sectors = 0u32;
    loop {
        job.check()?;
        let mut got = 0;
        while got < bytes.len() {
            let n = file.read(&mut bytes[got..])?;
            if n == 0 {
                break;
            }
            got += n;
        }
        if got == 0 {
            break;
        }
        original.update(&bytes[..got]);
        sha.update(&bytes[..got]);
        if raw {
            for chunk in bytes[..got].chunks(2048) {
                converted.update(encode_sector(chunk, sectors)?);
                sectors += 1;
            }
            if sectors % 8192 == 0 {
                job.say(format!(
                    "Verifying converted sectors: {}%",
                    sectors as u64 * 2048 * 100 / before.0
                ));
            }
        }
    }
    if f::stamp(p)? != before {
        bail!("Source changed while scanning")
    }
    Ok(Fingerprint {
        size: before.0,
        mtime_ns: before.1,
        sha1: format!("{:x}", original.finalize()),
        sha256: format!("{:x}", sha.finalize()),
        raw_sha1: raw.then(|| format!("{:x}", converted.finalize())),
    })
}
