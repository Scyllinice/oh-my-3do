#!/usr/bin/env python3
"""Runtime data adapters for organize-library.py. No third-party data is bundled.

organize-library.py identifies discs and fills in their details from three
public sources, downloaded on first use and cached:

  Redump's 3DO DAT     the checksums of every known disc, used to recognize
                       a dump exactly (REDUMP)
  LaunchBox metadata   titles, genres, publishers and release dates, matched
                       to Redump's names by normalized title (LAUNCHBOX)
  libretro thumbnails  box art, named after the Redump titles (ART)

load_catalog combines the first two into catalog.json in the cache folder.
cover_names and cover_url find box art. Every download is capped in size
and cached only once complete.
"""
import hashlib
import io
import json
from pathlib import Path
import re
import time
import unicodedata
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

REDUMP = "https://redump.info/datfile/3do/"
CATALOG_SCHEMA = 3
LAUNCHBOX = "https://gamesdb.launchbox-app.com/Metadata.zip"
ART = "https://thumbnails.libretro.com/The%203DO%20Company%20-%203DO/Named_Boxarts/"
AGENT = "Mozilla/5.0 OhMy3DO-Library/1.0 (https://github.com/Scyllinice/oh-my-3do)"

MIB = 1024 * 1024
MAX_DAT_BYTES = 8 * MIB
MAX_LAUNCHBOX_ZIP_BYTES = 256 * MIB
MAX_LAUNCHBOX_XML_BYTES = 1200 * MIB
MAX_COVER_LIST_BYTES = 4 * MIB
DOWNLOAD_TIMEOUT_SECONDS = 45

# The LaunchBox fields kept for each game.
LAUNCHBOX_FIELDS = (
    "Name",
    "DatabaseID",
    "Genres",
    "Publisher",
    "Developer",
    "ReleaseDate",
    "ReleaseYear",
    "ReleaseType",
)

# Child elements of an XML DAT game copied into its row as text.
DAT_TEXT_FIELDS = ("description", "category", "serial", "version", "region")

LEGACY_OFFLINE_WARNING = (
    "Offline legacy catalog: demos and other releases may be missing. "
    "Run an online scan to upgrade."
)


def download(url, target, maximum, offline=False):
    """Cache complete responses only; interrupted downloads never become cache hits.

    Returns `target`, downloading `url` into it first unless it is already
    cached. Raises ValueError when offline with nothing cached, for a
    non-HTTPS URL, or when the download exceeds `maximum` bytes or ends
    early.
    """
    target = Path(target)
    if target.is_file():
        if target.stat().st_size > maximum:
            raise ValueError(f"Oversized cached download: {target}")
        return target
    if offline:
        raise ValueError(f"Offline cache is missing: {target.name}")
    if not url.startswith("https://"):
        raise ValueError("Downloads require HTTPS")

    # Download under a temporary name and rename only once complete.
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(target.suffix + ".download")
    print(f"Downloading {url}", flush=True)
    try:
        request = urllib.request.Request(url, headers={"User-Agent": AGENT})
        response = urllib.request.urlopen(request, timeout=DOWNLOAD_TIMEOUT_SECONDS)
        with response, temporary.open("wb") as output:
            size = 0
            expected = response.headers.get("Content-Length")
            while True:
                block = response.read(MIB)
                if not block:
                    break
                size += len(block)
                if size > maximum:
                    raise ValueError(f"Download exceeds {maximum} bytes: {url}")
                output.write(block)
            if expected is not None and size != int(expected):
                raise ValueError(f"Incomplete download: {url}")
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def xml_dat_games(text):
    """Read a Logiqx XML datafile: one row per <game> or <machine>, holding its
    attributes, the DAT_TEXT_FIELDS present, and "roms" (each <rom>'s
    attributes)."""
    root = ET.fromstring(text)
    if root.tag != "datafile":
        raise ValueError("Expected a Logiqx/Redump datafile")
    result = []
    for item in root.findall("game") + root.findall("machine"):
        row = dict(item.attrib)
        for key in DAT_TEXT_FIELDS:
            if item.findtext(key):
                row[key] = item.findtext(key).strip()
        row["roms"] = [dict(rom.attrib) for rom in item.findall("rom")]
        if row.get("name") and row["roms"]:
            result.append(row)
    return result


# A ClrMamePro token: a quoted string (with \" and \\ escapes), a bracket,
# or a bare word.
CLRMAMEPRO_TOKEN = r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+'


def clrmamepro_dat_games(text):
    """Read a ClrMamePro text DAT, made of `key value` pairs where a value can
    be a bracketed block of more pairs, as in:

        game ( name "Title" rom ( name "Title.bin" size 1234 crc ... ) )

    Returns the same rows as xml_dat_games.
    """
    tokens = re.findall(CLRMAMEPRO_TOKEN, text)
    position = 0

    def read_block(nested=False):
        """Read pairs until the matching ")" (or the end of the file at the
        top level) and return {key: [values]}."""
        nonlocal position
        fields = {}
        while position < len(tokens):
            key = tokens[position]
            position += 1
            if key == ")":
                if not nested:
                    raise ValueError("Unexpected DAT closing bracket")
                return fields
            if position >= len(tokens):
                raise ValueError("Truncated DAT")

            value = tokens[position]
            position += 1
            if value == "(":
                value = read_block(True)
            elif value.startswith('"'):
                value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
            fields.setdefault(key, []).append(value)
        if nested:
            raise ValueError("Truncated DAT block")
        return fields

    result = []
    for item in read_block().get("game", []):
        # Keep the first value of each key.
        row = {key: values[0] for key, values in item.items() if key != "rom"}
        row["roms"] = [
            {key: values[0] for key, values in rom.items()} for rom in item.get("rom", [])
        ]
        if row.get("name") and row["roms"]:
            result.append(row)
    return result


def dat_games(text):
    """Parse an identification DAT, XML or ClrMamePro text, into game rows
    whose "roms" list only disc data (see data_roms)."""
    is_xml = text.lstrip("\ufeff \r\n\t").startswith("<")
    result = xml_dat_games(text) if is_xml else clrmamepro_dat_games(text)
    if not result:
        raise ValueError("No games in identification DAT")
    return data_roms(result)


def data_roms(games):
    """Drop each game's .cue entries, keeping only its disc tracks."""
    # CUE bytes depend on names/formatting and are not disc sectors. Keep every
    # actual track; removing audio tracks would turn partial dumps into matches.
    for game in games:
        game["roms"] = [r for r in game["roms"] if not r.get("name", "").lower().endswith(".cue")]
        if not game["roms"]:
            raise ValueError("Catalog entry has no disc data: " + game["name"])
    return games


def read_dat(path):
    """Read a DAT file, or a ZIP holding exactly one, and return its games."""
    data = Path(path).read_bytes()
    if len(data) > MAX_DAT_BYTES:
        raise ValueError("Identification DAT exceeds 8 MiB")
    if zipfile.is_zipfile(io.BytesIO(data)):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            candidates = [
                info
                for info in archive.infolist()
                if not info.is_dir() and info.filename.lower().endswith((".dat", ".xml"))
            ]
            if len(candidates) != 1 or candidates[0].file_size > MAX_DAT_BYTES:
                raise ValueError("DAT ZIP must contain one DAT/XML of at most 8 MiB")
            # Read directly; never extract archive paths onto disk.
            data = archive.read(candidates[0])
    return dat_games(data.decode("utf-8-sig"))


def title_key(name):
    """Normalize formatting, never invent fuzzy matches.

    Drops bracketed notes ("(USA)", "[b]"), moves or drops a leading article
    ("Horde, The" and "The Horde" both become "horde"), strips accents, and
    keeps only lowercase letters and digits.
    """
    name = re.sub(r"\([^)]*\)|\[[^]]*\]", "", name)
    name = name.strip()
    moved = re.match(r"^(.*),\s*(The|A|An)$", name, re.I)
    if moved:
        name = moved[2] + " " + moved[1]
    name = re.sub(r"^(the|a|an)\s+", "", name, flags=re.I)
    name = "".join(c for c in unicodedata.normalize("NFKD", name) if not unicodedata.combining(c))
    return "".join(c for c in name.casefold() if c.isalnum())


def launchbox_games(path):
    """Stream the large XML; discard every completed top-level record.

    LaunchBox's Metadata.xml covers every platform and is over a gigabyte,
    so it is read element by element and each record is freed once seen.
    Returns the 3DO games (LAUNCHBOX_FIELDS plus "aliases", their alternate
    names).
    """
    games = {}
    aliases = []
    with zipfile.ZipFile(path) as archive:
        info = archive.getinfo("Metadata.xml")
        if info.file_size > MAX_LAUNCHBOX_XML_BYTES:
            raise ValueError("LaunchBox XML is oversized")
        with archive.open(info) as stream:
            depth = 0
            root = None
            for event, element in ET.iterparse(stream, events=("start", "end")):
                if event == "start":
                    depth += 1
                    if depth == 1:
                        root = element
                    continue

                # Records are the root's children (depth 2).
                if depth == 2:
                    is_3do_game = element.tag == "Game" and "3DO" in (
                        element.findtext("Platform") or ""
                    )
                    if is_3do_game:
                        game = {key: element.findtext(key) or "" for key in LAUNCHBOX_FIELDS}
                        game["aliases"] = []
                        games[game["DatabaseID"]] = game
                    elif element.tag == "GameAlternateName":
                        aliases.append(
                            (element.findtext("DatabaseID"), element.findtext("AlternateName"))
                        )
                    root.remove(element)
                    element.clear()
                depth -= 1

    # Alternate names come in their own records, for every platform.
    for key, name in aliases:
        if key in games and name:
            games[key]["aliases"].append(name)
    if not games:
        raise ValueError("No 3DO games in LaunchBox database; source schema may have changed")
    return list(games.values())


def enrich(games, metadata):
    """Give each DAT game a "metadata" dict from the LaunchBox game whose
    name or alias has the same title_key. A title matching no game, or more
    than one, gets an empty dict rather than a guess."""
    lookup = {}
    for row in metadata:
        for name in [row["Name"]] + row["aliases"]:
            key = title_key(name)
            if row not in lookup.setdefault(key, []):
                lookup[key].append(row)

    for game in games:
        matches = lookup.get(title_key(game["name"]), [])
        game["metadata"] = {}
        if len(matches) != 1:
            continue
        match = matches[0]
        date = match["ReleaseYear"] or match["ReleaseDate"]
        year = re.match(r"^\d{4}", date)
        game["metadata"] = dict(
            title=match["Name"],
            genre=match["Genres"] or None,
            publisher=match["Publisher"] or None,
            developer=match["Developer"] or None,
            release_type=match.get("ReleaseType") or None,
            year=int(year[0]) if year else None,
            release_date=match["ReleaseDate"] or None,
            launchbox_id=match["DatabaseID"],
            description=match.get("Overview") or None,
            source=LAUNCHBOX,
        )
    return games


def cached_catalog(output, offline):
    """Return the cached catalog.json if it can be used as it is, else None.

    A complete catalog of the current schema is always usable. Offline, an
    incomplete one (from a custom DAT) or an older schema-2 one is used with
    a warning, since nothing better can be fetched.
    """
    result = json.loads(output.read_text(encoding="utf-8"))
    if result.get("schema") == CATALOG_SCHEMA and (offline or result.get("complete_catalog")):
        if not result.get("complete_catalog"):
            print(
                "Offline/custom catalog may omit releases. "
                "Run an online scan to load the full Redump catalog.",
                flush=True,
            )
        return result
    if offline and result.get("schema") == 2:
        print(LEGACY_OFFLINE_WARNING, flush=True)
        return result
    return None


def load_catalog(cache, offline=False, refresh=False, dat=None, launchbox=None):
    """Return the catalog: Redump's games, each with LaunchBox metadata.

    Uses cache/catalog.json when possible; otherwise builds it from the
    cached or downloaded sources and saves it. `dat` and `launchbox` name
    local files to use instead of downloads (a custom DAT makes the catalog
    "incomplete", so a later online scan replaces it). `refresh` deletes the
    downloaded sources first so they are fetched again.
    """
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    output = cache / "catalog.json"
    if not refresh and dat is None and launchbox is None and output.is_file():
        result = cached_catalog(output, offline)
        if result is not None:
            return result

    local_dat = dat is not None
    local_launchbox = launchbox is not None
    dat = Path(dat) if local_dat else cache / "redump-full.zip"
    launchbox = Path(launchbox) if local_launchbox else cache / "launchbox.zip"
    if refresh:
        (cache / "cover-list.html").unlink(missing_ok=True)
        # Explicit local source files are never removed by refresh.
        for file, local in ((dat, local_dat), (launchbox, local_launchbox)):
            if not local:
                file.unlink(missing_ok=True)

    # An older cache may hold redump.dat, which lacks demos and other
    # releases; offline, it is better than nothing.
    limited = False
    if not dat.exists():
        if local_dat:
            raise ValueError("Local DAT does not exist: " + str(dat))
        legacy = cache / "redump.dat"
        if offline and legacy.is_file():
            dat = legacy
            limited = True
            print(LEGACY_OFFLINE_WARNING, flush=True)
        else:
            download(REDUMP, dat, MAX_DAT_BYTES, offline)
    if not launchbox.exists():
        print(
            "First metadata download is about 104 MiB; later scans use the local cache.", flush=True
        )
        download(LAUNCHBOX, launchbox, MAX_LAUNCHBOX_ZIP_BYTES, offline)

    result = dict(
        schema=CATALOG_SCHEMA,
        complete_catalog=not limited and not local_dat,
        fetched=time.time(),
        sources=[
            dict(
                url=str(dat) if local_dat or limited else REDUMP,
                sha256=hashlib.sha256(dat.read_bytes()).hexdigest(),
            ),
            dict(url=LAUNCHBOX, sha256=hashlib.sha256(launchbox.read_bytes()).hexdigest()),
            dict(url=ART),
        ],
        games=enrich(read_dat(dat), launchbox_games(launchbox)),
    )
    temporary = output.with_suffix(".tmp")
    temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(output)
    return result


def cover_names(cache, offline=False):
    """The box-art file names listed on the libretro thumbnail page (cached)."""
    page = download(ART, Path(cache) / "cover-list.html", MAX_COVER_LIST_BYTES, offline)
    names = []
    for href in re.findall(r'href="([^"]+)"', page.read_text(encoding="utf-8")):
        name = urllib.parse.unquote(href)
        if name.lower().endswith(".png") and "/" not in name and "\\" not in name:
            names.append(name)
    return names


def cover_url(name):
    return ART + urllib.parse.quote(name, safe="")
