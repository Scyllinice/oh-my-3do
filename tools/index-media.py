#!/usr/bin/env python3
"""Index games for Find and associate PNG covers/prepared previews on USB.

Usage: index-media.py USB_ROOT

Scans a prepared USB drive and writes the index files the menu reads from
its .ohmy3do/ folder:
  library.3di          every game's path, for Find and folder browsing
  metadata.3di         genre, publisher and date per game (only when
                       library.json, written by organize-library.py, exists)
  save-names.txt       which games use which save files, read from the discs
  covers/index.3di     which cover belongs to which game
  previews/index.3di   which preview clip belongs to which game

A cover or preview is matched to a game by file name (Game.iso goes with
Game.png or Game.3dv), and earlier matches in an existing index are kept.
Everything is checked before any index is replaced, and media files are
never changed.
"""
import argparse
import binascii
import datetime
import json
from pathlib import Path
import re
import struct
import unicodedata

# The menu's limits.
MAX_GAMES = 2048
MAX_PATH_BYTES = 8192
MAX_PATH_PARTS = 33
MAX_PATH_PART_BYTES = 1023
MAX_METADATA_TEXT = 96
MAX_MEDIA_NAME_BYTES = 96
MAX_PNG_BYTES = 512 * 1024
NATIVE_COVER_SIZE = (96, 120)

# Memory budgets: the index's strings plus the menu's per-entry overhead.
LIBRARY_BUDGET = 128 * 1024  # 9 bytes per game
LIBRARY_ENTRY_OVERHEAD = 9
METADATA_BUDGET = 192 * 1024  # 16 bytes per game
METADATA_ENTRY_OVERHEAD = 16
MEDIA_INDEX_BUDGET = 128 * 1024

# Media index files: the "OM3DIDX1" magic plus four header words.
MEDIA_HEADER_SIZE = 24

MAX_LIBRARY_DATABASE_BYTES = 16 * 1024 * 1024
MAX_CUE_BYTES = 256 * 1024

# Windows marks junctions and other reparse points with this attribute.
FILE_ATTRIBUTE_REPARSE_POINT = 0x400

# Disc images on the drive that aren't games.
NAVIGATION_HELPER = "oh my 3do - folder navigation helper (keep).iso"
MENU_IMAGES = ("boot.iso", "menu-demo.iso")
SYSTEM_FOLDERS = ("$boot", "system")

# The media files each folder can hold.
MEDIA_SUFFIXES = {"covers": (".png", ".3dc"), "previews": (".3dv",)}

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# File signatures of archives sometimes renamed to .iso by mistake.
ARCHIVE_SIGNATURES = (
    (b"PK\x03\x04", "ZIP"),
    (b"PK\x05\x06", "ZIP"),
    (b"PK\x07\x08", "ZIP"),
    (b"7z\xbc\xaf\x27\x1c", "7-Zip"),
    (b"Rar!\x1a\x07", "RAR"),
    (b"\x1f\x8b", "gzip"),
    (b"BZh", "bzip2"),
    (b"\xfd7zXZ\0", "xz"),
)

ASCII_LOWERCASE_TABLE = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def fold(value):
    """Lowercase A-Z only, matching the menu's case-insensitive comparison."""
    return value.translate(ASCII_LOWERCASE_TABLE)


def has_control_character(text, extra=""):
    return any(ord(c) < 32 or ord(c) == 127 or c in extra for c in text)


def is_safe_media_name(name):
    """A plain file name the menu can store and show."""
    return not (
        len(name.encode()) > MAX_MEDIA_NAME_BYTES
        or name.startswith(".")
        or has_control_character(name, "\\:|")
    )


def write_replacing(temporary, target, data):
    """Write through a temporary file, so a reader never sees half of it."""
    temporary.write_bytes(data)
    temporary.replace(target)


def index_string(text):
    """A string as the index files store it: its length (16-bit, counting
    the terminator), then the UTF-8 text and a NUL."""
    raw = text.encode("utf-8") + b"\0"
    return struct.pack(">H", len(raw)) + raw


def index_file(magic, footer, payload, count):
    """An index file: magic, then the entry count, payload size, payload
    CRC-32 and a reserved zero (big-endian words), the payload and a footer."""
    header = struct.pack(">IIII", count, len(payload), binascii.crc32(payload), 0)
    return magic + header + payload + footer


# Metadata


def metadata_text(value):
    """Text the menu's font can show: accents removed, printable ASCII only,
    whitespace collapsed, at most 96 characters."""
    text = unicodedata.normalize("NFKD", str(value or "")).encode("ascii", "ignore").decode()
    printable = "".join(c if 32 <= ord(c) <= 126 else " " for c in text)
    return re.sub(r"\s+", " ", printable).strip()[:MAX_METADATA_TEXT]


def metadata_date(meta):
    """The release date as YYYY-MM-DD when valid, else the year, else ""."""
    date = str(meta.get("release_date") or "")[:10]
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
        try:
            return datetime.date.fromisoformat(date).isoformat()
        except ValueError:
            pass
    year = str(meta.get("year") or "")
    return year if re.fullmatch(r"[12]\d{3}", year) else ""


def metadata_index(records, games):
    """Index every disc for game-folder recognition; catalog fields are optional.

    `records` are library.json's game records and `games` the paths found on
    the drive. Returns metadata.3di: one row per game, sorted by folded path,
    holding its path, genre, publisher and date ("" when unknown).
    """
    allowed = {fold(path): path for path in games}
    rows = {key: [path, "", "", ""] for key, path in allowed.items()}
    seen = set()
    for record in records:
        path = allowed.get(fold(record.get("path") or ""))
        if path is None:
            continue
        meta = record.get("metadata") or {}
        fields = [
            metadata_text(meta.get("genre")),
            metadata_text(meta.get("publisher")),
            metadata_date(meta),
        ]
        key = fold(path)
        if key in seen:
            raise ValueError("Duplicate metadata path: " + path)
        seen.add(key)
        rows[key] = [path] + fields

    payload = bytearray()
    for key in sorted(rows):
        for field in rows[key]:
            if len(field.encode("utf-8")) > MAX_PATH_BYTES:
                raise ValueError("Oversized metadata field")
            payload.extend(index_string(field))

    over_budget = len(payload) + METADATA_ENTRY_OVERHEAD * len(rows) > METADATA_BUDGET
    if len(rows) > MAX_GAMES or over_budget:
        raise ValueError("Console metadata index exceeds capacity")
    return index_file(b"OM3DMET1", b"END3DM01", bytes(payload), len(rows))


def is_valid_game_path(path):
    """A path the menu can navigate to: not too long or deep, no empty, "."
    or ".." parts, no control characters, backslashes or colons, and an .iso
    or .cue extension."""
    parts = path.split("/")
    return not (
        len(path.encode()) > MAX_PATH_BYTES
        or len(parts) > MAX_PATH_PARTS
        or any(p in ("", ".", "..") or len(p.encode()) > MAX_PATH_PART_BYTES for p in parts)
        or has_control_character(path, "\\:")
        or not path.lower().endswith((".iso", ".cue"))
    )


def metadata_paths(data):
    """Check a metadata.3di file thoroughly and return its game paths.

    Raises ValueError for a bad header, footer, size or CRC, malformed
    strings, text the menu can't show, invalid paths, or paths that aren't
    unique and sorted.
    """
    if len(data) < 32 or data[:8] != b"OM3DMET1" or data[-8:] != b"END3DM01":
        raise ValueError("Invalid metadata header/footer")
    count, size, crc, reserved = struct.unpack_from(">IIII", data, 8)
    payload = data[24:-8]
    if (
        reserved
        or count > MAX_GAMES
        or size + METADATA_ENTRY_OVERHEAD * count > METADATA_BUDGET
        or size != len(payload)
        or binascii.crc32(payload) != crc
    ):
        raise ValueError("Invalid metadata bounds/checksum")

    paths = []
    position = 0
    for _ in range(count):
        # Each row: the path, then genre, publisher and date.
        for field in range(4):
            if position + 2 > size:
                raise ValueError("Truncated metadata")
            length = struct.unpack_from(">H", payload, position)[0]
            position += 2
            raw = payload[position : position + length]
            position += length
            if not length or len(raw) != length or raw[-1:] != b"\0" or b"\0" in raw[:-1]:
                raise ValueError("Invalid metadata string")

            if field:
                if length > MAX_METADATA_TEXT + 1 or any(c < 32 or c > 126 for c in raw[:-1]):
                    raise ValueError("Invalid metadata text")
                continue

            path = raw[:-1].decode("utf-8")
            if not is_valid_game_path(path):
                raise ValueError("Invalid metadata path")
            if paths and fold(paths[-1]) >= fold(path):
                raise ValueError("Metadata paths must be unique and sorted")
            paths.append(path)

    if position != size:
        raise ValueError("Trailing metadata")
    return paths


# Disc checks


def disc_data_issue(path):
    """Reject empty data and recognizable archives, without guessing a game.

    Returns a message describing the problem, or None when the file looks
    like disc data.
    """
    if not path.stat().st_size:
        return "Empty disc data file: " + path.name
    with path.open("rb") as stream:
        header = stream.read(16)
    for signature, name in ARCHIVE_SIGNATURES:
        if header.startswith(signature):
            return (
                name
                + " archive disguised as disc data: "
                + path.name
                + "; extract it before scanning"
            )
    return None


# Save names read from disc programs

OPERA_BLOCK = 2048
SAVE_NAMES_BYTES = 32768
SAVE_PATH = re.compile(rb"/nvram/([\x21-\x7e][\x20-\x7e]{0,30})", re.I)
SAVE_NAME = re.compile(r"[A-Za-z0-9_.\- \[\]'&+!()]+")

# The Opera (3DO disc) file system's volume label starts with these bytes:
# record type 1, five "Z" sync bytes, and version 1.
OPERA_LABEL_START = b"\x01ZZZZZ\x01"

# Fields of the volume label.
LABEL_ROOT_BLOCK_COUNT = 88
LABEL_ROOT_FIRST_COPY = 100

# Fields of a directory block's header.
DIRECTORY_FIRST_FREE_BYTE = 12
DIRECTORY_FIRST_ENTRY = 16
DIRECTORY_HEADER_SIZE = 20

# A directory entry is 68 bytes plus 4 per copy ("avatar") of the file;
# the copy count minus one is at offset 64, the first copy's block at 68.
ENTRY_BASE_SIZE = 72
ENTRY_LAST_AVATAR = 64
MAX_AVATARS = 16
ENTRY_TYPE_DIRECTORY = 7
ENTRY_FLAGS_LAST = 0xC0000000  # last entry in the block or in the directory

# Limits so a damaged image can't make the scan run away.
MAX_DIRECTORIES = 4096
MAX_DIRECTORY_BLOCKS = 64
MIN_PROGRAM_BYTES = 64
MAX_PROGRAM_BYTES = 3 * 1024 * 1024


def opera_files(stream):
    """(byte offset, size) of every file on a 3DO (Opera) disc image.

    Walks the directory tree from the volume label's root directory. Yields
    nothing if the image doesn't start with an Opera label.
    """
    label = stream.read(OPERA_BLOCK)
    if len(label) < OPERA_BLOCK or label[:7] != OPERA_LABEL_START:
        return

    root_block = struct.unpack_from(">I", label, LABEL_ROOT_FIRST_COPY)[0]
    root_block_count = struct.unpack_from(">I", label, LABEL_ROOT_BLOCK_COUNT)[0]
    pending = [(root_block, root_block_count)]  # directories still to read
    seen = set()
    while pending:
        start, count = pending.pop()
        if start in seen or len(seen) > MAX_DIRECTORIES:
            continue
        seen.add(start)

        for block in range(min(count, MAX_DIRECTORY_BLOCKS)):
            stream.seek((start + block) * OPERA_BLOCK)
            data = stream.read(OPERA_BLOCK)
            if len(data) < OPERA_BLOCK:
                break
            end = min(struct.unpack_from(">I", data, DIRECTORY_FIRST_FREE_BYTE)[0], OPERA_BLOCK)
            position = struct.unpack_from(">I", data, DIRECTORY_FIRST_ENTRY)[0]

            while DIRECTORY_HEADER_SIZE <= position <= end - ENTRY_BASE_SIZE:
                # flags, id, type, block size, byte count, block count
                flags, _, _, block_size, size, blocks = struct.unpack_from(
                    ">II4sIII", data, position
                )
                last_avatar, first_avatar = struct.unpack_from(
                    ">II", data, position + ENTRY_LAST_AVATAR
                )
                if last_avatar >= MAX_AVATARS:
                    break
                if flags & 0xFF == ENTRY_TYPE_DIRECTORY:
                    pending.append((first_avatar, blocks))
                else:
                    yield first_avatar * (block_size or OPERA_BLOCK), size
                position += ENTRY_BASE_SIZE + 4 * last_avatar
                if flags & ENTRY_FLAGS_LAST:
                    break


def disc_save_names(path):
    """NVRAM files a disc's programs name: '=FILE' exact, '*PREFIX' from FILE%d-style patterns.

    Searches every program-sized file on the disc for "/nvram/NAME" strings.
    A name built with a printf pattern ("Save%d") becomes a prefix; names
    with unusual characters are ignored. Returns the names sorted.
    """
    found = set()
    with path.open("rb") as stream:
        for offset, size in list(opera_files(stream)):
            if size < MIN_PROGRAM_BYTES or size > MAX_PROGRAM_BYTES:
                continue
            stream.seek(offset)
            data = stream.read(size)
            if b"/nvram/" not in data.lower():
                continue
            for match in SAVE_PATH.finditer(data):
                name = match[1].decode("ascii")
                if "%" in name:
                    prefix = name.split("%", 1)[0]
                    if len(prefix) >= 3 and SAVE_NAME.fullmatch(prefix):
                        found.add("*" + prefix)
                elif len(name) >= 2 and SAVE_NAME.fullmatch(name):
                    found.add("=" + name)
    return sorted(found)


def short_title(title):
    """A list title without trailing (Region) / (Disc 1) groups, as the menu shows it."""
    title = re.sub(r"\.(iso|cue)$", "", title, flags=re.I)
    while True:
        shorter = re.sub(r" \([^()]*\)$", "", title)
        if shorter == title or not shorter:
            return metadata_text(title)
        title = shorter


def load_save_names_cache(cache_path):
    """The per-disc cache of save names, or {} when missing or unreadable."""
    try:
        cache = json.loads(cache_path.read_text(encoding="utf-8")) if cache_path.is_file() else {}
    except (OSError, ValueError):
        return {}
    return cache if isinstance(cache, dict) and cache.get("schema") == 2 else {}


def save_names_text(root, support, games, titles, write_cache=True):
    """save-names.txt for the console's Save manager, reusing a per-disc cache.

    Each line is a save name, a tab, and the titles of the games that use it.
    Discs are recognized in the cache by name, size and modification time,
    so only new or changed discs are read. Returns (text, number of names),
    or with `write_cache` false (text, number of names, the cache's bytes)
    without writing the cache.
    """
    cache_path = support / "save-names-cache.json"
    cache = load_save_names_cache(cache_path)
    discs = {}
    rows = {}  # save name -> titles
    scanned = 0
    for _, game, _ in sorted(games):
        if not game.lower().endswith(".iso"):
            continue
        path = root / game
        stat = path.stat()
        stamp = "%s|%d|%d" % (path.name, stat.st_size, stat.st_mtime_ns)
        names = cache.get("discs", {}).get(stamp)
        if not isinstance(names, list):
            if not scanned:
                print("Reading save names from discs; later runs reuse the results...", flush=True)
            scanned += 1
            try:
                names = disc_save_names(path)
            except (OSError, struct.error):
                names = []
        discs[stamp] = names

        title = short_title(titles.get(fold(game)) or Path(game).name)
        for name in names:
            row = rows.setdefault(name, [])
            if title and title not in row:
                row.append(title)

    # Sorted by name, ignoring the leading = or *; titles cut to 63
    # characters; lines that don't fit the menu's buffer are left out.
    text = "# Save names read from the discs by index-media.py\n"
    dropped = 0
    for name in sorted(rows, key=lambda n: (n[1:].lower(), n)):
        line = name + "\t" + " / ".join(rows[name])[:63] + "\n"
        if len(text) + len(line) >= SAVE_NAMES_BYTES:
            dropped += 1
            continue
        text += line
    if dropped:
        print(f"Save names: {dropped} names left out to stay within {SAVE_NAMES_BYTES} bytes")

    cache_data = json.dumps(dict(schema=2, discs=discs), sort_keys=True).encode("utf-8")
    if not write_cache:
        return text, len(rows), cache_data
    write_replacing(cache_path.with_name(cache_path.name + ".tmp"), cache_path, cache_data)
    return text, len(rows)


def support_base(root):
    """The drive's .ohmy3do folder, refusing links (symbolic links, or
    junctions on Windows) that could redirect writes elsewhere."""
    directory = root / ".ohmy3do"
    marker = directory / "ohmy3do.ini"
    is_reparse_point = directory.exists() and (
        getattr(directory.lstat(), "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT
    )
    if directory.is_symlink() or marker.is_symlink() or is_reparse_point:
        raise ValueError("Support directory/config must not be a symlink")
    return directory


# CUE sheets that only wrap an .iso


def is_safe_iso_name(name):
    return not (
        Path(name).suffix.lower() != ".iso"
        or name in (".", "..")
        or has_control_character(name, "/\\:")
    )


def redundant_iso_name(text):
    """Recognize only a plain, single-track MODE1/2048 ISO wrapper.

    Returns the .iso's name when the CUE sheet holds nothing but one FILE,
    TRACK 01 MODE1/2048 and INDEX 01 00:00:00 (plus comments and titles),
    so indexing the .iso alone loses nothing. Otherwise returns None.
    """
    directives = []
    name = None
    for line in text.splitlines():
        line = line.strip()
        if not line or re.match(r"^REM(?:\s|$)", line, re.I):
            continue
        match = re.fullmatch(r'FILE\s+"([^"\r\n]+)"\s+BINARY', line, re.I)
        if match:
            name = match[1]
            if not is_safe_iso_name(name):
                return None
            directives.append("FILE")
        elif re.fullmatch(r"TRACK\s+0*1\s+MODE1/2048", line, re.I):
            directives.append("TRACK")
        elif re.fullmatch(r"INDEX\s+0*1\s+00:00:00", line, re.I):
            directives.append("INDEX")
        elif re.fullmatch(
            r'CATALOG\s+\d{13}|(?:TITLE|PERFORMER|SONGWRITER)\s+"[^"\r\n]*"', line, re.I
        ):
            continue
        else:
            return None
    return name if directives == ["FILE", "TRACK", "INDEX"] else None


def redundant_cue_iso(path):
    """The .iso beside a CUE sheet that only wraps it, or None."""
    if path.stat().st_size > MAX_CUE_BYTES:
        return None
    raw = path.read_bytes()
    if len(raw) > MAX_CUE_BYTES:
        return None  # it grew after the size check
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeError:
        text = raw.decode("cp1252", errors="replace")
    name = redundant_iso_name(text)
    if not name:
        return None
    matches = [p for p in path.parent.iterdir() if p.name.casefold() == name.casefold()]
    if len(matches) != 1 or not matches[0].is_file() or matches[0].is_symlink():
        return None
    return matches[0]


# Media indexes


def existing_index(directory):
    """Read a media folder's current index.3di as {folded game path: file name}.

    Returns {} when there is none (or it is empty). Raises ValueError for a
    damaged index, so a bad file is backed up rather than silently replaced.
    """
    path = directory / "index.3di"
    if path.is_symlink():
        raise ValueError("Index must not be a symlink")
    if path.exists() and path.stat().st_size > MEDIA_INDEX_BUDGET + MEDIA_HEADER_SIZE:
        raise ValueError("Existing index is oversized")
    if not path.exists() or path.stat().st_size == 0:
        return {}

    data = path.read_bytes()
    if len(data) < MEDIA_HEADER_SIZE or data[:8] != b"OM3DIDX1":
        raise ValueError(f"Invalid existing index: {path}; back it up before replacing it")
    count, size, crc, reserved = struct.unpack_from(">IIII", data, 8)
    if (
        count > MAX_GAMES
        or size > MEDIA_INDEX_BUDGET
        or len(data) != size + MEDIA_HEADER_SIZE
        or reserved
        or binascii.crc32(data[MEDIA_HEADER_SIZE:]) != crc
    ):
        raise ValueError(f"Invalid existing index: {path}")

    result = {}
    position = MEDIA_HEADER_SIZE
    for _ in range(count):
        # Each row: the key and name lengths, then both NUL-terminated.
        if position + 4 > len(data):
            raise ValueError("Truncated existing index")
        key_length, name_length = struct.unpack_from(">HH", data, position)
        position += 4
        # The shortest name is "a.png" plus its NUL.
        if key_length < 2 or name_length < 6 or position + key_length + name_length > len(data):
            raise ValueError("Invalid existing index entry")
        key = data[position : position + key_length]
        name = data[position + key_length : position + key_length + name_length]
        position += key_length + name_length

        if key[-1:] != b"\0" or name[-1:] != b"\0" or b"\0" in key[:-1] + name[:-1]:
            raise ValueError("Invalid index strings")
        key = key[:-1].decode()
        name = name[:-1].decode()
        if not is_safe_media_name(name) or "/" in name:
            raise ValueError("Invalid media filename in existing index")
        if fold(key) in result:
            raise ValueError("Duplicate existing association")
        result[fold(key)] = name

    if position != len(data):
        raise ValueError("Trailing index data")
    return result


def png_size(path):
    """A PNG's (width, height) from its header, or None if it isn't one."""
    with path.open("rb") as stream:
        header = stream.read(24)
    if len(header) == 24 and header[:8] == PNG_SIGNATURE and header[12:16] == b"IHDR":
        return struct.unpack_from(">II", header, 16)
    return None


def media_files(directory, folder, oversized):
    """{folded file stem: file name} for the media files in a folder.

    Adds PNG covers that aren't 96x120 to `oversized` as (name, width,
    height). Full-screen "-large.3dc" covers are skipped: they belong to
    the cover of the same name.
    """
    assets = {}
    for path in directory.iterdir():
        if (
            path.is_symlink()
            or not path.is_file()
            or path.suffix.lower() not in MEDIA_SUFFIXES[folder]
        ):
            continue
        if path.name.lower().endswith("-large.3dc"):
            continue  # the full-screen version of another cover

        name = path.name
        if not is_safe_media_name(name):
            raise ValueError(f"Unsupported media basename: {name}")
        if path.suffix.lower() == ".png":
            size = png_size(path)
            if size and size != NATIVE_COVER_SIZE:
                oversized.append((name, *size))
        key = fold(path.stem)
        if key in assets:
            raise ValueError(f"Media filenames collide: {name}")
        if path.suffix.lower() == ".png" and path.stat().st_size > MAX_PNG_BYTES:
            raise ValueError(f"PNG exceeds 512 KiB: {name}")
        assets[key] = name
    return assets


def media_index(directory, folder, games, oversized):
    """Build a media folder's index.3di and return (its bytes, row count)."""
    previous = existing_index(directory)
    assets = media_files(directory, folder, oversized)

    rows = []
    for folded, game, stem in sorted(games):
        # A file named after the game, or else the earlier association.
        name = assets.get(stem, previous.get(folded))
        # Explicit preview imports can supersede an older same-name clip.
        # Keep that association across indexing without deleting originals.
        associated = previous.get(folded)
        if folder == "previews" and associated and (directory / associated).is_file():
            name = associated
        if not name or not (directory / name).is_file() or (directory / name).is_symlink():
            continue
        if Path(name).suffix.lower() not in MEDIA_SUFFIXES[folder]:
            raise ValueError(f"Unsupported indexed media type: {name}")

        key = game.encode() + b"\0"
        file_name = name.encode() + b"\0"
        rows.append(struct.pack(">HH", len(key), len(file_name)) + key + file_name)

    payload = b"".join(rows)
    if len(rows) > MAX_GAMES or len(payload) > MEDIA_INDEX_BUDGET:
        raise ValueError(f"{folder} index exceeds 2048 entries or 128 KiB")
    header = struct.pack(">IIII", len(rows), len(payload), binascii.crc32(payload), 0)
    return b"OM3DIDX1" + header + payload, len(rows)


# The whole drive


def is_skipped(path, relative):
    """Whether a disc image on the drive isn't a game to index: the menu's
    own images, system folders, hidden files and folders, and CUE sheets
    that only wrap an .iso (which is indexed instead)."""
    name = path.name.lower()
    if name == NAVIGATION_HELPER:
        return True
    if len(relative.parts) == 1 and name in MENU_IMAGES:
        return True
    if relative.parts[0].lower() in SYSTEM_FOLDERS:
        return True
    if any(part.startswith(".") for part in relative.parts):
        return True
    return path.suffix.lower() == ".cue" and redundant_cue_iso(path) is not None


def find_games(root):
    """Every game disc on the drive, as (folded path, path, folded file stem),
    with paths relative to the drive's root."""
    games = []
    for path in root.rglob("*"):
        if path.is_symlink() or not path.is_file() or path.suffix.lower() not in (".iso", ".cue"):
            continue
        relative = path.relative_to(root)
        if is_skipped(path, relative):
            continue
        if path.suffix.lower() == ".iso":
            issue = disc_data_issue(path)
            if issue:
                print("Skipped: " + issue)
                continue
        game = relative.as_posix()
        if has_control_character(game, "\\:") or len(game.encode()) > MAX_PATH_BYTES:
            raise ValueError(f"Unsupported game path: {game}")
        games.append((fold(game), game, fold(path.stem)))

    if len({key for key, _, _ in games}) != len(games):
        raise ValueError("Game paths collide when ignoring ASCII case")
    return games


def library_index(games):
    """library.3di: every game's path, sorted by folded path."""
    rows = []
    for _, game, _ in sorted(games):
        parts = game.split("/")
        if any(len(part.encode()) > MAX_PATH_PART_BYTES for part in parts) or (
            len(parts) > MAX_PATH_PARTS
        ):
            raise ValueError(f"Game path exceeds menu navigation limits: {game}")
        rows.append(index_string(game))
    payload = b"".join(rows)
    over_budget = len(payload) + LIBRARY_ENTRY_OVERHEAD * len(games) > LIBRARY_BUDGET
    if len(games) > MAX_GAMES or over_budget:
        raise ValueError("Search index exceeds 2048 games or 128 KiB of paths")
    return index_file(b"OM3DLIB1", b"END3DL01", payload, len(games))


def load_library_database(support, games):
    """Read library.json, if organize-library.py wrote one.

    Returns (metadata.3di bytes or None, {folded path: title}).
    """
    database = support / "library.json"
    if not database.is_file():
        return None, {}
    if database.is_symlink() or database.stat().st_size > MAX_LIBRARY_DATABASE_BYTES:
        raise ValueError("Invalid library metadata database")
    records = json.loads(database.read_text(encoding="utf-8"))
    if records.get("schema") != 1 or not isinstance(records.get("games"), list):
        raise ValueError("Invalid library metadata database")

    metadata = metadata_index(records["games"], [game for _, game, _ in games])
    titles = {
        fold(record.get("path") or ""): record.get("title") or ""
        for record in records["games"]
        if isinstance(record, dict)
    }
    target = support / "metadata.3di"
    if (
        target.is_symlink()
        or (target.exists() and not target.is_file())
        or (support / "metadata.3di.tmp").is_symlink()
    ):
        raise ValueError("Metadata index must be a regular file")
    return metadata, titles


def report_oversized(oversized):
    if not oversized:
        return
    print(f"{len(oversized)} PNG cover(s) differ from the native 96x120 cover size:")
    for name, width, height in oversized[:5]:
        print(f"  {name}: {width}x{height}")
    print(
        "Use resize-covers.py with a separate output folder for 96x120 PNGs; "
        "originals were not changed."
    )


def index(root, include_save_names=True):
    """Write every index for the drive at `root` (see the module summary).

    All of them are built and checked first; only then are the files
    replaced, so a problem leaves the drive's indexes as they were.
    """
    if not root.is_dir():
        raise ValueError("USB root must be an existing directory")
    support = support_base(root)
    support.mkdir(exist_ok=True)

    games = find_games(root)
    library_data = library_index(games)
    library = support / "library.3di"
    metadata, titles = load_library_database(support, games)
    if library.is_symlink() or (library.exists() and not library.is_file()):
        raise ValueError("Search index must be a regular file")

    # (folder, index bytes, row count) for each media folder present.
    oversized = []
    outputs = []
    for folder in ("covers", "previews"):
        directory = support / folder
        if not directory.exists():
            continue
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError(f"Media folder must be a real directory: {directory}")
        data, count = media_index(directory, folder, games, oversized)
        outputs.append((directory, data, count))
    report_oversized(oversized)

    # Validate every index before replacing any existing output.
    temporary = support / "library.3di.tmp"
    if temporary.is_symlink():
        raise ValueError("Temporary search index must not be a symlink")
    write_replacing(temporary, library, library_data)
    if metadata is not None:
        write_replacing(support / "metadata.3di.tmp", support / "metadata.3di", metadata)
    if include_save_names:
        names, count = save_names_text(root, support, games, titles)
        write_replacing(
            support / "save-names.txt.tmp", support / "save-names.txt", names.encode("ascii")
        )
        print(f"Indexed {count} save names for the Save manager")
    print(f"Indexed {len(games)} games for Find in {library}")

    # Validate both indices before replacing either. Media files stay untouched.
    for directory, data, count in outputs:
        write_replacing(directory / "index.3di.tmp", directory / "index.3di", data)
        print(f"Indexed {count} games in {directory}; media files unchanged")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("usb_root", type=Path)
    args = parser.parse_args()
    try:
        index(args.usb_root)
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
