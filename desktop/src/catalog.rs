use crate::{
    disc::{self as d, rx},
    fsutil as f,
    model::*,
    Job,
};
use anyhow::{bail, Context, Result};
use quick_xml::{events::Event, Reader};
use serde_json::{json, Value};
use sha1::{Digest, Sha1};
use std::{
    collections::{HashMap, HashSet},
    fs::{self, File},
    io::{BufRead, BufReader, Read, Write},
    path::Path,
    time::Duration,
};
pub const REDUMP: &str = "https://redump.info/datfile/3do/";
pub const LAUNCHBOX: &str = "https://gamesdb.launchbox-app.com/Metadata.zip";
pub const ART: &str =
    "https://thumbnails.libretro.com/The%203DO%20Company%20-%203DO/Named_Boxarts/";
pub fn download(url: &str, path: &Path, max: u64, job: &Job) -> Result<()> {
    if path.is_file() {
        if fs::metadata(path)?.len() > max {
            bail!("Oversized cached download")
        };
        return Ok(());
    }
    if !url.starts_with("https://") {
        bail!("Downloads require HTTPS")
    }
    fs::create_dir_all(path.parent().context("No cache parent")?)?;
    job.say(format!("Downloading {url}"));
    let client = reqwest::blocking::Client::builder()
        .user_agent("Mozilla/5.0 OhMy3DO-Library/1.0 (https://github.com/Scyllinice/oh-my-3do)")
        .connect_timeout(Duration::from_secs(15))
        .timeout(Duration::from_secs(600))
        .build()?;
    let mut response = client.get(url).send()?.error_for_status()?;
    let expected = response.content_length();
    if expected.is_some_and(|n| n > max) {
        bail!("Download exceeds size limit")
    }
    let mut tmp = tempfile::NamedTempFile::new_in(path.parent().unwrap())?;
    let mut buf = vec![0; 1024 * 1024];
    let mut total = 0u64;
    let mut last_progress = 0u64;
    loop {
        job.check()?;
        let n = response.read(&mut buf)?;
        if n == 0 {
            break;
        }
        total += n as u64;
        if total > max {
            bail!("Download exceeds size limit")
        };
        tmp.write_all(&buf[..n])?;
        if total / (8 * 1024 * 1024) > last_progress {
            last_progress = total / (8 * 1024 * 1024);
            job.say(format!("Downloaded {} MiB", total / 1024 / 1024));
        }
    }
    if expected.is_some_and(|n| n != total) {
        bail!("Incomplete download")
    };
    tmp.as_file().sync_all()?;
    tmp.persist(path).map_err(|e| e.error)?;
    Ok(())
}
fn attributes(e: &quick_xml::events::BytesStart<'_>) -> Result<HashMap<String, String>> {
    e.attributes()
        .map(|a| {
            let a = a?;
            Ok((
                String::from_utf8(a.key.as_ref().to_vec())?,
                a.unescape_value()?.into_owned(),
            ))
        })
        .collect()
}
pub fn parse_dat(text: &str) -> Result<Vec<Game>> {
    let mut result = vec![];
    if text
        .trim_start_matches(['\u{feff}', ' ', '\r', '\n', '\t'])
        .starts_with('<')
    {
        let mut r = Reader::from_str(text);
        r.config_mut().trim_text(true);
        let mut game: Option<Game> = None;
        let mut field = String::new();
        loop {
            match r.read_event()? {
                Event::Start(e) => {
                    let name = String::from_utf8_lossy(e.name().as_ref()).to_string();
                    if name == "game" || name == "machine" {
                        let a = attributes(&e)?;
                        game = Some(Game {
                            name: a.get("name").cloned().unwrap_or_default(),
                            ..Default::default()
                        });
                    } else if name == "rom" {
                        if let Some(g) = game.as_mut() {
                            let a = attributes(&e)?;
                            g.roms.push(rom(&a)?);
                        }
                    } else {
                        field = name;
                    }
                }
                Event::Empty(e) if e.name().as_ref() == b"rom" => {
                    if let Some(g) = game.as_mut() {
                        g.roms.push(rom(&attributes(&e)?)?);
                    }
                }
                Event::Text(e) => {
                    if let Some(g) = game.as_mut() {
                        let v = quick_xml::escape::unescape(&e.decode()?)?.into_owned();
                        match field.as_str() {
                            "category" => g.category = v,
                            "serial" => g.serial = v,
                            "region" => g.region = Some(v),
                            _ => {}
                        }
                    }
                }
                Event::End(e) => {
                    if e.name().as_ref() == b"game" || e.name().as_ref() == b"machine" {
                        if let Some(g) = game.take() {
                            result.push(g)
                        }
                    }
                    field.clear();
                }
                Event::Eof => break,
                _ => {}
            }
        }
    } else {
        #[derive(Clone)]
        enum Node {
            Atom(String),
            Block(HashMap<String, Vec<Node>>),
        }
        fn block(
            tokens: &[String],
            pos: &mut usize,
            nested: bool,
        ) -> Result<HashMap<String, Vec<Node>>> {
            let mut out = HashMap::new();
            while *pos < tokens.len() {
                let key = tokens[*pos].clone();
                *pos += 1;
                if key == ")" {
                    if !nested {
                        bail!("Unexpected DAT bracket")
                    };
                    return Ok(out);
                }
                let value = tokens.get(*pos).context("Truncated DAT")?;
                *pos += 1;
                let node = if value == "(" {
                    Node::Block(block(tokens, pos, true)?)
                } else {
                    Node::Atom(
                        value
                            .trim_matches('"')
                            .replace("\\\"", "\"")
                            .replace("\\\\", "\\"),
                    )
                };
                out.entry(key).or_insert_with(Vec::new).push(node);
            }
            if nested {
                bail!("Truncated DAT block")
            };
            Ok(out)
        }
        let tokens = rx(r#""(?:\\.|[^"\\])*"|[()]|[^\s()]+"#)
            .find_iter(text)
            .map(|m| m.as_str().to_owned())
            .collect::<Vec<_>>();
        let tree = block(&tokens, &mut 0, false)?;
        for node in tree.get("game").into_iter().flatten() {
            if let Node::Block(fields) = node {
                let scalar = |key: &str| {
                    fields
                        .get(key)
                        .and_then(|v| v.first())
                        .and_then(|n| {
                            if let Node::Atom(s) = n {
                                Some(s.clone())
                            } else {
                                None
                            }
                        })
                        .unwrap_or_default()
                };
                let mut g = Game {
                    name: scalar("name"),
                    category: scalar("category"),
                    serial: scalar("serial"),
                    ..Default::default()
                };
                for n in fields.get("rom").into_iter().flatten() {
                    if let Node::Block(fields) = n {
                        let attrs = fields
                            .iter()
                            .filter_map(|(k, v)| match v.first() {
                                Some(Node::Atom(s)) => Some((k.clone(), s.clone())),
                                _ => None,
                            })
                            .collect();
                        g.roms.push(rom(&attrs)?);
                    }
                }
                result.push(g);
            }
        }
    }
    for g in &mut result {
        g.roms
            .retain(|r| !r.name.to_ascii_lowercase().ends_with(".cue"));
    }
    if result.is_empty()
        || result
            .iter()
            .any(|g| g.name.is_empty() || g.roms.is_empty())
    {
        bail!("DAT contains no usable disc records")
    }
    Ok(result)
}
fn rom(a: &HashMap<String, String>) -> Result<Rom> {
    Ok(Rom {
        name: a.get("name").cloned().unwrap_or_default(),
        size: a.get("size").context("Missing ROM size")?.parse()?,
        sha1: a
            .get("sha1")
            .cloned()
            .unwrap_or_default()
            .to_ascii_lowercase(),
        serial: a.get("serial").cloned().unwrap_or_default(),
    })
}
fn read_dat(path: &Path) -> Result<Vec<Game>> {
    let mut raw = fs::read(path)?;
    if raw.starts_with(b"PK") {
        let mut z = zip::ZipArchive::new(std::io::Cursor::new(raw))?;
        let names = (0..z.len())
            .filter_map(|n| {
                let e = z.by_index(n).ok()?;
                (!e.is_dir() && (e.name().ends_with(".dat") || e.name().ends_with(".xml")))
                    .then_some(n)
            })
            .collect::<Vec<_>>();
        if names.len() != 1 {
            bail!("DAT ZIP must contain exactly one DAT/XML")
        };
        let mut entry = z.by_index(names[0])?;
        if entry.size() > 8 * 1024 * 1024 {
            bail!("Oversized DAT")
        };
        raw = Vec::new();
        entry.read_to_end(&mut raw)?;
    }
    if raw.len() > 8 * 1024 * 1024 {
        bail!("Oversized DAT")
    };
    parse_dat(std::str::from_utf8(&raw)?)
}
fn metadata<R: BufRead>(input: R, job: &Job) -> Result<HashMap<String, Value>> {
    let mut r = Reader::from_reader(input);
    r.config_mut().trim_text(true);
    let mut buf = vec![];
    let (mut depth, mut count) = (0usize, 0usize);
    let mut kind = String::new();
    let mut field = String::new();
    let mut data = HashMap::<String, String>::new();
    let mut games = vec![];
    let mut aliases = vec![];
    loop {
        match r.read_event_into(&mut buf)? {
            Event::Start(e) => {
                depth += 1;
                let name = String::from_utf8_lossy(e.name().as_ref()).to_string();
                if depth == 2 {
                    kind = name;
                    data.clear();
                } else if depth == 3 {
                    field = name;
                }
            }
            Event::Text(e) if depth == 3 => {
                data.entry(field.clone())
                    .or_default()
                    .push_str(&quick_xml::escape::unescape(&e.decode()?)?);
            }
            Event::CData(e) if depth == 3 => {
                data.entry(field.clone())
                    .or_default()
                    .push_str(&e.decode()?);
            }
            Event::End(_) => {
                if depth == 2 {
                    count += 1;
                    if count % 10000 == 0 {
                        job.check()?;
                    }
                    if kind == "Game" && data.get("Platform").is_some_and(|s| s.contains("3DO")) {
                        games.push(data.clone());
                    }
                    if kind == "GameAlternateName" && aliases.len() < 1_000_000 {
                        if let (Some(id), Some(name)) =
                            (data.get("DatabaseID"), data.get("AlternateName"))
                        {
                            aliases.push((id.clone(), name.clone()));
                        }
                    }
                }
                depth = depth.saturating_sub(1);
            }
            Event::Eof => break,
            _ => {}
        }
        buf.clear();
    }
    let mut candidates: HashMap<String, Vec<Value>> = HashMap::new();
    for g in games {
        let get = |s: &str| g.get(s).cloned().unwrap_or_default();
        let year = rx(r"\d{4}")
            .find(&format!("{} {}", get("ReleaseYear"), get("ReleaseDate")))
            .and_then(|m| m.as_str().parse::<u32>().ok());
        let value = json!({"title":get("Name"),"genre":get("Genres"),"publisher":get("Publisher"),"developer":get("Developer"),"year":year,"release_type":get("ReleaseType"),"release_date":get("ReleaseDate"),"launchbox_id":get("DatabaseID"),"source":LAUNCHBOX});
        let mut names = vec![get("Name")];
        names.extend(
            aliases
                .iter()
                .filter(|(id, _)| id == &get("DatabaseID"))
                .map(|(_, s)| s.clone()),
        );
        for name in names {
            let list = candidates.entry(d::title_key(&name)).or_default();
            if !list.contains(&value) {
                list.push(value.clone());
            }
        }
    }
    Ok(candidates
        .into_iter()
        .filter_map(|(key, v)| (v.len() == 1).then(|| (key, v[0].clone())))
        .collect())
}
pub fn prepare(catalog: &mut Catalog) {
    let mut signatures: HashMap<String, HashSet<String>> = HashMap::new();
    let mut codes = vec![];
    for game in &mut catalog.games {
        let stems = game
            .roms
            .iter()
            .map(|r| {
                rx(r"(?i)\s*\(Track\s+\d+\)\s*$")
                    .replace(
                        Path::new(&r.name)
                            .file_stem()
                            .unwrap_or_default()
                            .to_string_lossy()
                            .as_ref(),
                        "",
                    )
                    .trim()
                    .to_owned()
            })
            .collect::<HashSet<_>>();
        game.release = if stems.len() == 1 {
            let stem = stems.iter().next().unwrap();
            if d::title_key(stem) == d::title_key(&game.name)
                && d::disc_number(stem) == d::disc_number(&game.name)
            {
                stem.clone()
            } else {
                game.name.clone()
            }
        } else {
            game.name.clone()
        };
        let mut identity = game
            .roms
            .iter()
            .map(|r| {
                (
                    r.size.to_string(),
                    if r.sha1.is_empty() {
                        "unknown".into()
                    } else {
                        format!("sha1:{}", r.sha1.to_ascii_lowercase())
                    },
                )
            })
            .collect::<Vec<_>>();
        identity.sort();
        let signature = serde_json::to_string(&identity).unwrap();
        signatures
            .entry(game.release.to_lowercase())
            .or_default()
            .insert(signature.clone());
        codes.push(signature);
    }
    for (game, signature) in catalog.games.iter_mut().zip(codes) {
        game.variant = None;
        if signatures[&game.release.to_lowercase()].len() > 1 {
            let code =
                format!("{:x}", Sha1::digest(signature.as_bytes()))[..8].to_ascii_uppercase();
            game.release = format!(
                "{} [Variant {}]{}",
                d::without_disc(&game.release),
                code,
                d::disc_number(&game.release)
                    .map(|n| format!(" (Disc {n})"))
                    .unwrap_or_default()
            );
            game.variant = Some(code);
        }
    }
}
pub fn load(cache: &Path, offline: bool, job: &Job) -> Result<Catalog> {
    fs::create_dir_all(cache)?;
    let path = cache.join("catalog.json");
    if path.is_file() {
        let mut data: Catalog = serde_json::from_reader(File::open(&path)?)?;
        if data.complete_catalog || offline {
            prepare(&mut data);
            return Ok(data);
        }
    }
    if offline {
        bail!("No complete catalog is cached. Enable downloads for the first scan.")
    }
    let dat = cache.join("redump-full.zip");
    download(REDUMP, &dat, 8 * 1024 * 1024, job)?;
    let mut games = read_dat(&dat)?;
    let launchbox = cache.join("launchbox.zip");
    download(LAUNCHBOX, &launchbox, 256 * 1024 * 1024, job)?;
    job.say("Reading game metadata");
    let mut z = zip::ZipArchive::new(File::open(&launchbox)?)?;
    let xml = z.by_name("Metadata.xml")?;
    if xml.size() > 1200 * 1024 * 1024 {
        bail!("Oversized metadata XML")
    }
    let meta = metadata(BufReader::new(xml), job)?;
    for game in &mut games {
        game.metadata = meta
            .get(&d::title_key(&game.name))
            .cloned()
            .unwrap_or_else(|| json!({}));
    }
    let sources = vec![
        json!({"url":REDUMP,"sha256":f::sha256(&dat,job)?}),
        json!({"url":LAUNCHBOX,"sha256":f::sha256(&launchbox,job)?}),
        json!({"url":ART}),
    ];
    let mut catalog = Catalog {
        games,
        sources,
        complete_catalog: true,
    };
    let mut value = serde_json::to_value(&catalog)?;
    value["schema"] = json!(3);
    f::atomic(&path, &serde_json::to_vec_pretty(&value)?)?;
    prepare(&mut catalog);
    Ok(catalog)
}
