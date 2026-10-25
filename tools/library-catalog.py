#!/usr/bin/env python3
"""Runtime data adapters for organize-library.py. No third-party data is bundled."""
import hashlib
import io
import json
from pathlib import Path
import re
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

REDUMP = "https://redump.info/datfile/3do/"
CATALOG_SCHEMA = 3
LAUNCHBOX = "https://gamesdb.launchbox-app.com/Metadata.zip"
ART = "https://thumbnails.libretro.com/The%203DO%20Company%20-%203DO/Named_Boxarts/"
AGENT = "Mozilla/5.0 OhMy3DO-Library/1.0 (https://github.com/Scyllinice/oh-my-3do)"


def download(url, target, maximum, offline=False):
    """Cache complete responses only; interrupted downloads never become cache hits."""
    target = Path(target)
    if target.is_file():
        if target.stat().st_size > maximum:
            raise ValueError(f"Oversized cached download: {target}")
        return target
    if offline:
        raise ValueError(f"Offline cache is missing: {target.name}")
    if not url.startswith("https://"):
        raise ValueError("Downloads require HTTPS")
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(target.suffix + ".download")
    print(f"Downloading {url}", flush=True)
    try:
        req = urllib.request.Request(url, headers={"User-Agent": AGENT})
        with urllib.request.urlopen(req, timeout=45) as response, temporary.open("wb") as output:
            size = 0
            expected = response.headers.get("Content-Length")
            while True:
                block = response.read(1024 * 1024)
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


def dat_games(text):
    if text.lstrip("\ufeff \r\n\t").startswith("<"):
        root = ET.fromstring(text)
        if root.tag != "datafile":
            raise ValueError("Expected a Logiqx/Redump datafile")
        result = []
        for item in root.findall("game") + root.findall("machine"):
            row = dict(item.attrib)
            for key in ("description", "category", "serial", "version", "region"):
                if item.findtext(key):
                    row[key] = item.findtext(key).strip()
            row["roms"] = [dict(rom.attrib) for rom in item.findall("rom")]
            if row.get("name") and row["roms"]:
                result.append(row)
        if not result:
            raise ValueError("No games in identification DAT")
        return data_roms(result)
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+', text)
    pos = 0

    def block(nested=False):
        nonlocal pos
        fields = {}
        while pos < len(tokens):
            key = tokens[pos]
            pos += 1
            if key == ")":
                if not nested:
                    raise ValueError("Unexpected DAT closing bracket")
                return fields
            if pos >= len(tokens):
                raise ValueError("Truncated DAT")
            value = tokens[pos]
            pos += 1
            if value == "(":
                value = block(True)
            elif value.startswith('"'):
                value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
            fields.setdefault(key, []).append(value)
        if nested:
            raise ValueError("Truncated DAT block")
        return fields

    result = []
    for item in block().get("game", []):
        row = {k: v[0] for k, v in item.items() if k != "rom"}
        row["roms"] = [{k: v[0] for k, v in rom.items()} for rom in item.get("rom", [])]
        if row.get("name") and row["roms"]:
            result.append(row)
    if not result:
        raise ValueError("No games in identification DAT")
    return data_roms(result)


def data_roms(games):
    # CUE bytes depend on names/formatting and are not disc sectors. Keep every
    # actual track; removing audio tracks would turn partial dumps into matches.
    for game in games:
        game["roms"] = [r for r in game["roms"] if not r.get("name", "").lower().endswith(".cue")]
        if not game["roms"]:
            raise ValueError("Catalog entry has no disc data: " + game["name"])
    return games


def read_dat(path):
    data = Path(path).read_bytes()
    if len(data) > 8 * 1024 * 1024:
        raise ValueError("Identification DAT exceeds 8 MiB")
    if zipfile.is_zipfile(io.BytesIO(data)):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            candidates = [
                i
                for i in archive.infolist()
                if not i.is_dir() and i.filename.lower().endswith((".dat", ".xml"))
            ]
            if len(candidates) != 1 or candidates[0].file_size > 8 * 1024 * 1024:
                raise ValueError("DAT ZIP must contain one DAT/XML of at most 8 MiB")
            # Read directly; never extract archive paths onto disk.
            data = archive.read(candidates[0])
    return dat_games(data.decode("utf-8-sig"))


def title_key(name):
    """Normalize formatting, never invent fuzzy matches."""
    import unicodedata

    name = re.sub(r"\([^)]*\)|\[[^]]*\]", "", name)
    name = name.strip()
    moved = re.match(r"^(.*),\s*(The|A|An)$", name, re.I)
    if moved:
        name = moved[2] + " " + moved[1]
    name = re.sub(r"^(the|a|an)\s+", "", name, flags=re.I)
    name = "".join(c for c in unicodedata.normalize("NFKD", name) if not unicodedata.combining(c))
    return "".join(c for c in name.casefold() if c.isalnum())


def launchbox_games(path):
    """Stream the large XML; discard every completed top-level record."""
    games, aliases = {}, []
    with zipfile.ZipFile(path) as archive:
        info = archive.getinfo("Metadata.xml")
        if info.file_size > 1200 * 1024 * 1024:
            raise ValueError("LaunchBox XML is oversized")
        with archive.open(info) as stream:
            depth, root = 0, None
            for event, element in ET.iterparse(stream, events=("start", "end")):
                if event == "start":
                    depth += 1
                    if depth == 1:
                        root = element
                    continue
                if depth == 2:
                    if element.tag == "Game" and "3DO" in (element.findtext("Platform") or ""):
                        game = {
                            k: element.findtext(k) or ""
                            for k in (
                                "Name",
                                "DatabaseID",
                                "Genres",
                                "Publisher",
                                "Developer",
                                "ReleaseDate",
                                "ReleaseYear",
                                "ReleaseType",
                            )
                        }
                        game["aliases"] = []
                        games[game["DatabaseID"]] = game
                    elif element.tag == "GameAlternateName":
                        aliases.append(
                            (element.findtext("DatabaseID"), element.findtext("AlternateName"))
                        )
                    root.remove(element)
                    element.clear()
                depth -= 1
    for key, name in aliases:
        if key in games and name:
            games[key]["aliases"].append(name)
    if not games:
        raise ValueError("No 3DO games in LaunchBox database; source schema may have changed")
    return list(games.values())


def enrich(games, metadata):
    lookup = {}
    for row in metadata:
        for name in [row["Name"]] + row["aliases"]:
            key = title_key(name)
            if row not in lookup.setdefault(key, []):
                lookup[key].append(row)
    for game in games:
        matches = lookup.get(title_key(game["name"]), [])
        game["metadata"] = {}
        if len(matches) == 1:
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


def load_catalog(cache, offline=False, refresh=False, dat=None, launchbox=None):
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    output = cache / "catalog.json"
    if not refresh and dat is None and launchbox is None and output.is_file():
        result = json.loads(output.read_text(encoding="utf-8"))
        if result.get("schema") == CATALOG_SCHEMA and (offline or result.get("complete_catalog")):
            if not result.get("complete_catalog"):
                print(
                    "Offline/custom catalog may omit releases. Run an online scan to load the full Redump catalog.",
                    flush=True,
                )
            return result
        if offline and result.get("schema") == 2:
            print(
                "Offline legacy catalog: demos and other releases may be missing. Run an online scan to upgrade.",
                flush=True,
            )
            return result
    local_dat, local_launchbox = dat is not None, launchbox is not None
    dat = Path(dat) if local_dat else cache / "redump-full.zip"
    launchbox = Path(launchbox) if local_launchbox else cache / "launchbox.zip"
    if refresh:
        (cache / "cover-list.html").unlink(missing_ok=True)
        # Explicit local source files are never removed by refresh.
        for file, local in ((dat, local_dat), (launchbox, local_launchbox)):
            if not local:
                file.unlink(missing_ok=True)
    limited = False
    if not dat.exists():
        if local_dat:
            raise ValueError("Local DAT does not exist: " + str(dat))
        legacy = cache / "redump.dat"
        if offline and not local_dat and legacy.is_file():
            dat = legacy
            limited = True
            print(
                "Offline legacy catalog: demos and other releases may be missing. Run an online scan to upgrade.",
                flush=True,
            )
        else:
            download(REDUMP, dat, 8 * 1024 * 1024, offline)
    if not launchbox.exists():
        print(
            "First metadata download is about 104 MiB; later scans use the local cache.", flush=True
        )
        download(LAUNCHBOX, launchbox, 256 * 1024 * 1024, offline)
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
    page = download(ART, Path(cache) / "cover-list.html", 4 * 1024 * 1024, offline)
    names = []
    for href in re.findall(r'href="([^"]+)"', page.read_text(encoding="utf-8")):
        name = urllib.parse.unquote(href)
        if name.lower().endswith(".png") and "/" not in name and "\\" not in name:
            names.append(name)
    return names


def cover_url(name):
    return ART + urllib.parse.quote(name, safe="")
