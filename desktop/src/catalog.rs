//! The catalog of known 3DO releases, built from two downloads:
//!
//! - Redump's 3DO DAT file: every verified disc release, with its name and
//!   the size and SHA-1 of each file. Used to recognize discs by checksum.
//! - LaunchBox's game database (`Metadata.xml`, inside a ZIP): genre,
//!   publisher, release date, description and so on, matched to releases by
//!   title.
//!
//! Both are downloaded once into the cache folder, and the combined catalog
//! is saved there as `catalog.json` so later scans work offline. Box art
//! comes separately from the libretro thumbnail server ([`ART`]).

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

/// Redump's 3DO DAT file (a ZIP holding one DAT).
pub const REDUMP: &str = "https://redump.info/datfile/3do/";

/// LaunchBox's game database (a ZIP holding `Metadata.xml`).
pub const LAUNCHBOX: &str = "https://gamesdb.launchbox-app.com/Metadata.zip";

/// The libretro box art folder for the 3DO, named by release.
pub const ART: &str =
    "https://thumbnails.libretro.com/The%203DO%20Company%20-%203DO/Named_Boxarts/";

/// The largest DAT download, and the largest DAT inside it.
const MAX_DAT_BYTES: u64 = 8 * 1024 * 1024;

/// The largest LaunchBox ZIP download.
const MAX_LAUNCHBOX_ZIP_BYTES: u64 = 256 * 1024 * 1024;

/// The largest `Metadata.xml` inside the LaunchBox ZIP (it holds every
/// platform's games, not just the 3DO's).
const MAX_LAUNCHBOX_XML_BYTES: u64 = 1200 * 1024 * 1024;

/// The most alternate names kept from LaunchBox, as a memory limit.
const MAX_ALTERNATE_NAMES: usize = 1_000_000;

/// The version of `catalog.json`'s layout.
const CATALOG_SCHEMA: u32 = 3;

// ---- Downloading ----

/// Downloads `url` over HTTPS to `path`, refusing anything larger than `max`
/// bytes. If `path` already exists it is used as it is (a cached download).
///
/// The download goes to a temporary file first and is only moved into place
/// once complete, so an interrupted download never leaves a partial file.
pub fn download(url: &str, path: &Path, max: u64, job: &Job) -> Result<()> {
    if path.is_file() {
        if fs::metadata(path)?.len() > max {
            bail!("Oversized cached download")
        }
        return Ok(());
    }
    if !url.starts_with("https://") {
        bail!("Downloads require HTTPS")
    }

    let folder = path.parent().context("No cache parent")?;
    fs::create_dir_all(folder)?;
    job.say(format!("Downloading {url}"));

    let client = reqwest::blocking::Client::builder()
        .user_agent("Mozilla/5.0 OhMy3DO-Library/1.0 (https://github.com/Scyllinice/oh-my-3do)")
        .connect_timeout(Duration::from_secs(15))
        .timeout(Duration::from_secs(600))
        .build()?;
    let mut response = client.get(url).send()?.error_for_status()?;

    // The server may say how big the file is; if not, progress shows 0 of 0.
    let expected = response.content_length();
    job.report(0, expected.unwrap_or(0), "Downloading");
    if expected.is_some_and(|size| size > max) {
        bail!("Download exceeds size limit")
    }

    let mut temporary = tempfile::NamedTempFile::new_in(folder)?;
    let mut buffer = vec![0; 1024 * 1024];
    let mut total = 0u64;
    // Progress messages are written every 8 MiB.
    const MESSAGE_EVERY: u64 = 8 * 1024 * 1024;
    let mut messages_written = 0u64;

    loop {
        job.check()?;
        let read = response.read(&mut buffer)?;
        if read == 0 {
            break;
        }

        total += read as u64;
        if total > max {
            bail!("Download exceeds size limit")
        }
        temporary.write_all(&buffer[..read])?;

        job.report(total, expected.unwrap_or(0), "Downloading");
        if total / MESSAGE_EVERY > messages_written {
            messages_written = total / MESSAGE_EVERY;
            job.say(format!("Downloaded {} MiB", total / 1024 / 1024));
        }
    }

    if expected.is_some_and(|size| size != total) {
        bail!("Incomplete download")
    }
    temporary.as_file().sync_all()?;
    temporary.persist(path).map_err(|error| error.error)?;
    Ok(())
}

// ---- Redump DAT files ----

/// Reads a DAT file's games. Redump publishes DATs as XML (Logiqx
/// format), but the older clrmamepro text format is read too.
///
/// CUE sheets are dropped from each game's files (they are generated, so
/// their checksums never match a person's own). Fails if any game has no
/// name or no files left.
pub fn parse_dat(text: &str) -> Result<Vec<Game>> {
    let is_xml = text
        .trim_start_matches(['\u{feff}', ' ', '\r', '\n', '\t'])
        .starts_with('<');
    let mut games = if is_xml {
        parse_xml_dat(text)?
    } else {
        parse_text_dat(text)?
    };

    for game in &mut games {
        game.roms
            .retain(|rom| !rom.name.to_ascii_lowercase().ends_with(".cue"));
    }

    let unusable = games
        .iter()
        .any(|game| game.name.is_empty() || game.roms.is_empty());
    if games.is_empty() || unusable {
        bail!("DAT contains no usable disc records")
    }
    Ok(games)
}

/// Reads an XML DAT, where each release looks like:
///
/// ```xml
/// <game name="Road Rash (USA)">
///   <category>Games</category>
///   <rom name="Road Rash (USA).bin" size="..." sha1="..."/>
/// </game>
/// ```
///
/// (`machine` is accepted in place of `game`.)
fn parse_xml_dat(text: &str) -> Result<Vec<Game>> {
    let mut reader = Reader::from_str(text);
    reader.config_mut().trim_text(true);

    let mut games = vec![];
    // The game being read, and the element whose text comes next.
    let mut game: Option<Game> = None;
    let mut field = String::new();

    loop {
        match reader.read_event()? {
            Event::Start(element) => {
                let name = String::from_utf8_lossy(element.name().as_ref()).to_string();
                if name == "game" || name == "machine" {
                    let attributes = xml_attributes(&element)?;
                    game = Some(Game {
                        name: attributes.get("name").cloned().unwrap_or_default(),
                        ..Default::default()
                    });
                } else if name == "rom" {
                    if let Some(game) = game.as_mut() {
                        game.roms.push(rom(&xml_attributes(&element)?)?);
                    }
                } else {
                    field = name;
                }
            }
            Event::Empty(element) if element.name().as_ref() == b"rom" => {
                if let Some(game) = game.as_mut() {
                    game.roms.push(rom(&xml_attributes(&element)?)?);
                }
            }
            Event::Text(text) => {
                if let Some(game) = game.as_mut() {
                    let value = quick_xml::escape::unescape(&text.decode()?)?.into_owned();
                    match field.as_str() {
                        "category" => game.category = value,
                        "serial" => game.serial = value,
                        "region" => game.region = Some(value),
                        _ => {}
                    }
                }
            }
            Event::End(element) => {
                let name = element.name();
                if name.as_ref() == b"game" || name.as_ref() == b"machine" {
                    if let Some(game) = game.take() {
                        games.push(game);
                    }
                }
                field.clear();
            }
            Event::Eof => break,
            _ => {}
        }
    }

    Ok(games)
}

/// An XML element's attributes, by name.
fn xml_attributes(element: &quick_xml::events::BytesStart<'_>) -> Result<HashMap<String, String>> {
    let mut attributes = HashMap::new();
    for attribute in element.attributes() {
        let attribute = attribute?;
        let key = String::from_utf8(attribute.key.as_ref().to_vec())?;
        let value = attribute.unescape_value()?.into_owned();
        attributes.insert(key, value);
    }
    Ok(attributes)
}

/// A value in a clrmamepro text DAT: a word or quoted string, or a
/// ( block ) of `key value` pairs. A key can appear more than once (a game
/// has one `rom` per file).
#[derive(Clone)]
enum TextNode {
    Atom(String),
    Block(HashMap<String, Vec<TextNode>>),
}

/// Reads a clrmamepro text DAT, where each release looks like:
///
/// ```text
/// game (
///     name "Road Rash (USA)"
///     rom ( name "Road Rash (USA).bin" size 12345 sha1 abc... )
/// )
/// ```
fn parse_text_dat(text: &str) -> Result<Vec<Game>> {
    // Tokens: quoted strings (with \" and \\ escapes), brackets, and words.
    let tokens: Vec<String> = rx(r#""(?:\\.|[^"\\])*"|[()]|[^\s()]+"#)
        .find_iter(text)
        .map(|token| token.as_str().to_owned())
        .collect();
    let mut position = 0;
    let top_level = parse_text_block(&tokens, &mut position, false)?;

    let mut games = vec![];
    for node in top_level.get("game").into_iter().flatten() {
        let TextNode::Block(fields) = node else {
            continue;
        };

        // The first value of `key`, if it is a plain value; "" otherwise.
        let text_field = |key: &str| match fields.get(key).and_then(|values| values.first()) {
            Some(TextNode::Atom(value)) => value.clone(),
            _ => String::new(),
        };
        let mut game = Game {
            name: text_field("name"),
            category: text_field("category"),
            serial: text_field("serial"),
            ..Default::default()
        };

        for rom_node in fields.get("rom").into_iter().flatten() {
            let TextNode::Block(rom_fields) = rom_node else {
                continue;
            };
            let attributes: HashMap<String, String> = rom_fields
                .iter()
                .filter_map(|(key, values)| match values.first() {
                    Some(TextNode::Atom(value)) => Some((key.clone(), value.clone())),
                    _ => None,
                })
                .collect();
            game.roms.push(rom(&attributes)?);
        }

        games.push(game);
    }

    Ok(games)
}

/// Reads `key value` pairs from `tokens`, starting at `position`, up to the
/// closing bracket of a `nested` block or the end of the file. Moves
/// `position` past what was read.
fn parse_text_block(
    tokens: &[String],
    position: &mut usize,
    nested: bool,
) -> Result<HashMap<String, Vec<TextNode>>> {
    let mut fields: HashMap<String, Vec<TextNode>> = HashMap::new();

    while *position < tokens.len() {
        let key = tokens[*position].clone();
        *position += 1;
        if key == ")" {
            if !nested {
                bail!("Unexpected DAT bracket")
            }
            return Ok(fields);
        }

        let value = tokens.get(*position).context("Truncated DAT")?;
        *position += 1;
        let node = if value == "(" {
            TextNode::Block(parse_text_block(tokens, position, true)?)
        } else {
            let unquoted = value
                .trim_matches('"')
                .replace("\\\"", "\"")
                .replace("\\\\", "\\");
            TextNode::Atom(unquoted)
        };
        fields.entry(key).or_default().push(node);
    }

    if nested {
        bail!("Truncated DAT block")
    }
    Ok(fields)
}

/// A disc file from a DAT entry's attributes (name, size, sha1, serial).
/// The size is required.
fn rom(attributes: &HashMap<String, String>) -> Result<Rom> {
    let text = |key: &str| attributes.get(key).cloned().unwrap_or_default();
    Ok(Rom {
        name: text("name"),
        size: attributes
            .get("size")
            .context("Missing ROM size")?
            .parse()?,
        sha1: text("sha1").to_ascii_lowercase(),
        serial: text("serial"),
    })
}

/// Reads a DAT file from disk: plain, or inside a ZIP holding exactly one
/// `.dat` or `.xml` file (as Redump serves it).
fn read_dat(path: &Path) -> Result<Vec<Game>> {
    let mut raw = fs::read(path)?;

    if raw.starts_with(b"PK") {
        let mut zip = zip::ZipArchive::new(std::io::Cursor::new(raw))?;
        let dat_entries: Vec<usize> = (0..zip.len())
            .filter(|&index| {
                zip.by_index(index).is_ok_and(|entry| {
                    !entry.is_dir()
                        && (entry.name().ends_with(".dat") || entry.name().ends_with(".xml"))
                })
            })
            .collect();
        if dat_entries.len() != 1 {
            bail!("DAT ZIP must contain exactly one DAT/XML")
        }

        let mut entry = zip.by_index(dat_entries[0])?;
        if entry.size() > MAX_DAT_BYTES {
            bail!("Oversized DAT")
        }
        raw = Vec::new();
        entry.read_to_end(&mut raw)?;
    }

    if raw.len() as u64 > MAX_DAT_BYTES {
        bail!("Oversized DAT")
    }
    parse_dat(std::str::from_utf8(&raw)?)
}

// ---- LaunchBox game information ----

/// Keeps only the titles that match exactly one LaunchBox game, so automatic
/// matching never guesses between two.
fn only_unique(candidates: HashMap<String, Vec<Value>>) -> HashMap<String, Value> {
    candidates
        .into_iter()
        .filter(|(_, values)| values.len() == 1)
        .map(|(key, mut values)| (key, values.remove(0)))
        .collect()
}

/// The fields of one top-level record in `Metadata.xml`.
type LaunchBoxRecord = HashMap<String, String>;

/// What is kept from `Metadata.xml`.
struct LaunchBoxData {
    /// Every 3DO game's fields.
    games: Vec<LaunchBoxRecord>,
    /// Every alternate title, as (game id, name).
    alternate_names: Vec<(String, String)>,
}

/// Reads LaunchBox's `Metadata.xml` and returns the information for each
/// 3DO game, keyed by title key (see [`d::title_key`]). A game is listed
/// under its name and each of its alternate names, so a title can match
/// several games.
fn metadata_candidates<R: BufRead>(input: R, job: &Job) -> Result<HashMap<String, Vec<Value>>> {
    let LaunchBoxData {
        games,
        alternate_names,
    } = read_launchbox_records(input, job)?;

    let mut candidates: HashMap<String, Vec<Value>> = HashMap::new();
    for game in games {
        let field = |name: &str| game.get(name).cloned().unwrap_or_default();

        // The year: the first four digits in the release year or date.
        let year_and_date = format!("{} {}", field("ReleaseYear"), field("ReleaseDate"));
        let year = rx(r"\d{4}")
            .find(&year_and_date)
            .and_then(|digits| digits.as_str().parse::<u32>().ok());

        let information = json!({
            "title": field("Name"),
            "genre": field("Genres"),
            "publisher": field("Publisher"),
            "developer": field("Developer"),
            "description": field("Overview"),
            "year": year,
            "release_type": field("ReleaseType"),
            "release_date": field("ReleaseDate"),
            "launchbox_id": field("DatabaseID"),
            "source": LAUNCHBOX,
        });

        let id = field("DatabaseID");
        let mut names = vec![field("Name")];
        for (alternate_id, name) in &alternate_names {
            if *alternate_id == id {
                names.push(name.clone());
            }
        }

        for name in names {
            let list = candidates.entry(d::title_key(&name)).or_default();
            if !list.contains(&information) {
                list.push(information.clone());
            }
        }
    }

    Ok(candidates)
}

/// Streams through `Metadata.xml`, which looks like:
///
/// ```xml
/// <LaunchBox>
///   <Game><Name>...</Name><Platform>...</Platform>...</Game>
///   <GameAlternateName><DatabaseID>...</DatabaseID>
///     <AlternateName>...</AlternateName></GameAlternateName>
///   ...
/// </LaunchBox>
/// ```
///
/// Keeps the 3DO games and every alternate name. The file is far too large
/// to load whole, so it is read one record at a time.
fn read_launchbox_records<R: BufRead>(input: R, job: &Job) -> Result<LaunchBoxData> {
    let mut reader = Reader::from_reader(input);
    reader.config_mut().trim_text(false);
    let mut buffer = vec![];

    // Depth 1 is <LaunchBox>, 2 a record such as <Game>, 3 one of its fields.
    let mut depth = 0usize;
    let mut records_read = 0usize;
    let mut record_kind = String::new();
    let mut field = String::new();
    let mut record = LaunchBoxRecord::new();

    let mut games = vec![];
    let mut alternate_names = vec![];

    loop {
        match reader.read_event_into(&mut buffer)? {
            Event::Start(element) => {
                depth += 1;
                let name = String::from_utf8_lossy(element.name().as_ref()).to_string();
                if depth == 2 {
                    record_kind = name;
                    record.clear();
                } else if depth == 3 {
                    field = name;
                }
            }

            // A field's text can arrive in pieces: plain text, entities such
            // as &amp; or &#233;, and CDATA sections.
            Event::Text(text) if depth == 3 => {
                let decoded = text.decode()?;
                let value = quick_xml::escape::unescape(&decoded)?;
                record.entry(field.clone()).or_default().push_str(&value);
            }
            Event::GeneralRef(reference) if depth == 3 => {
                let value = record.entry(field.clone()).or_default();
                if let Some(character) = reference.resolve_char_ref()? {
                    value.push(character);
                } else {
                    let name = reference.decode()?;
                    let resolved = quick_xml::escape::resolve_predefined_entity(&name)
                        .context("Unknown XML entity")?;
                    value.push_str(resolved);
                }
            }
            Event::CData(data) if depth == 3 => {
                record
                    .entry(field.clone())
                    .or_default()
                    .push_str(&data.decode()?);
            }

            Event::End(_) => {
                if depth == 2 {
                    for value in record.values_mut() {
                        *value = value.trim().to_owned();
                    }

                    records_read += 1;
                    if records_read.is_multiple_of(10000) {
                        job.check()?;
                    }

                    let is_3do_game = record_kind == "Game"
                        && record
                            .get("Platform")
                            .is_some_and(|platform| platform.contains("3DO"));
                    if is_3do_game {
                        games.push(record.clone());
                    }

                    if record_kind == "GameAlternateName"
                        && alternate_names.len() < MAX_ALTERNATE_NAMES
                    {
                        if let (Some(id), Some(name)) =
                            (record.get("DatabaseID"), record.get("AlternateName"))
                        {
                            alternate_names.push((id.clone(), name.clone()));
                        }
                    }
                }
                depth = depth.saturating_sub(1);
            }

            Event::Eof => break,
            _ => {}
        }
        buffer.clear();
    }

    Ok(LaunchBoxData {
        games,
        alternate_names,
    })
}

/// Opens `Metadata.xml` inside a LaunchBox ZIP and reads its candidates.
/// `oversized` is the error for an XML file over the size limit.
fn read_launchbox_zip(
    path: &Path,
    oversized: &str,
    job: &Job,
) -> Result<HashMap<String, Vec<Value>>> {
    let mut zip = zip::ZipArchive::new(File::open(path)?)?;
    let xml = zip.by_name("Metadata.xml")?;
    if xml.size() > MAX_LAUNCHBOX_XML_BYTES {
        bail!("{oversized}")
    }
    metadata_candidates(BufReader::new(xml), job)
}

/// Game information by title key, from the cached LaunchBox ZIP (downloaded
/// first unless `offline`). Titles that match more than one game are left
/// out, so nothing is matched by guesswork.
pub fn information(cache: &Path, offline: bool, job: &Job) -> Result<HashMap<String, Value>> {
    Ok(only_unique(information_candidates(cache, offline, job)?))
}

/// Like [`information`], but keeps every game a title matches, so the
/// person can choose between them.
pub fn information_candidates(
    cache: &Path,
    offline: bool,
    job: &Job,
) -> Result<HashMap<String, Vec<Value>>> {
    let path = cache.join("launchbox.zip");
    if !path.is_file() {
        if offline {
            bail!("No game information cached. Turn off offline mode to download it first.");
        }
        download(LAUNCHBOX, &path, MAX_LAUNCHBOX_ZIP_BYTES, job)?;
    }
    read_launchbox_zip(&path, "Oversized game information XML", job)
}

// ---- The combined catalog ----

/// Works out each game's release name (the title used in the library) and
/// tells apart releases that share a name.
///
/// The release name is the name of the game's files when they all agree and
/// match the game (Redump sometimes writes the game name differently),
/// otherwise the game's own name. Two releases with the same name but
/// different data each get " [Variant XXXXXXXX]" (from a checksum of their
/// files' sizes and SHA-1s) added before any disc number.
pub fn prepare(catalog: &mut Catalog) {
    // Each release name, with the data signatures seen for it.
    let mut signatures_by_release: HashMap<String, HashSet<String>> = HashMap::new();
    let mut signatures = vec![];

    for game in &mut catalog.games {
        game.release = release_name(game);
        let signature = data_signature(game);
        signatures_by_release
            .entry(game.release.to_lowercase())
            .or_default()
            .insert(signature.clone());
        signatures.push(signature);
    }

    for (game, signature) in catalog.games.iter_mut().zip(signatures) {
        game.variant = None;
        let has_variants = signatures_by_release[&game.release.to_lowercase()].len() > 1;
        if !has_variants {
            continue;
        }

        let hash = format!("{:x}", Sha1::digest(signature.as_bytes()));
        let code = hash[..8].to_ascii_uppercase();
        let disc = match d::disc_number(&game.release) {
            Some(number) => format!(" (Disc {number})"),
            None => String::new(),
        };
        game.release = format!("{} [Variant {code}]{disc}", d::without_disc(&game.release));
        game.variant = Some(code);
    }
}

/// A game's release name: the shared name of its files (without " (Track
/// N)"), if they all have one and it is the same title and disc as the game;
/// otherwise the game's name.
fn release_name(game: &Game) -> String {
    let file_stems: HashSet<String> = game
        .roms
        .iter()
        .map(|rom| {
            let stem = Path::new(&rom.name)
                .file_stem()
                .unwrap_or_default()
                .to_string_lossy()
                .into_owned();
            rx(r"(?i)\s*\(Track\s+\d+\)\s*$")
                .replace(&stem, "")
                .trim()
                .to_owned()
        })
        .collect();

    if file_stems.len() == 1 {
        let stem = file_stems.iter().next().unwrap();
        let same_title = d::title_key(stem) == d::title_key(&game.name);
        let same_disc = d::disc_number(stem) == d::disc_number(&game.name);
        if same_title && same_disc {
            return stem.clone();
        }
    }
    game.name.clone()
}

/// A text that is the same for two games exactly when their files have the
/// same sizes and SHA-1s (in any order).
fn data_signature(game: &Game) -> String {
    let mut files: Vec<(String, String)> = game
        .roms
        .iter()
        .map(|rom| {
            let checksum = if rom.sha1.is_empty() {
                "unknown".to_owned()
            } else {
                format!("sha1:{}", rom.sha1.to_ascii_lowercase())
            };
            (rom.size.to_string(), checksum)
        })
        .collect();
    files.sort();
    serde_json::to_string(&files).unwrap()
}

/// The catalog: from `catalog.json` in `cache` if it is complete (or if
/// `offline`, whatever is cached), otherwise built from fresh downloads of
/// the Redump DAT and the LaunchBox database, and saved.
pub fn load(cache: &Path, offline: bool, job: &Job) -> Result<Catalog> {
    fs::create_dir_all(cache)?;
    let path = cache.join("catalog.json");
    if path.is_file() {
        let mut cached: Catalog = serde_json::from_reader(File::open(&path)?)?;
        if cached.complete_catalog || offline {
            prepare(&mut cached);
            return Ok(cached);
        }
    }
    if offline {
        bail!("No complete catalog is cached. Enable downloads for the first scan.")
    }

    let dat = cache.join("redump-full.zip");
    download(REDUMP, &dat, MAX_DAT_BYTES, job)?;
    let mut games = read_dat(&dat)?;

    let launchbox = cache.join("launchbox.zip");
    download(LAUNCHBOX, &launchbox, MAX_LAUNCHBOX_ZIP_BYTES, job)?;
    job.say("Reading game metadata");
    let information = only_unique(read_launchbox_zip(
        &launchbox,
        "Oversized metadata XML",
        job,
    )?);
    for game in &mut games {
        game.metadata = information
            .get(&d::title_key(&game.name))
            .cloned()
            .unwrap_or_else(|| json!({}));
    }

    let sources = vec![
        json!({"url": REDUMP, "sha256": f::sha256(&dat, job)?}),
        json!({"url": LAUNCHBOX, "sha256": f::sha256(&launchbox, job)?}),
        json!({"url": ART}),
    ];
    let mut catalog = Catalog {
        games,
        sources,
        complete_catalog: true,
    };

    let mut saved = serde_json::to_value(&catalog)?;
    saved["schema"] = json!(CATALOG_SCHEMA);
    f::atomic(&path, &serde_json::to_vec_pretty(&saved)?)?;

    prepare(&mut catalog);
    Ok(catalog)
}
