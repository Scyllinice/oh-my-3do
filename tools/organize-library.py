#!/usr/bin/env python3
"""Identify and organize a 3DO USB library; preview first, apply explicitly.

Usage:
  organize-library.py scan USB_ROOT [--plan PLAN.json] [--quick] [--overrides FILE]
                      [--categories FILE] [--offline] [--refresh-catalog]
                      [--dat FILE] [--launchbox FILE]
  organize-library.py apply USB_ROOT [--plan PLAN.json] [--accept-name-matches]
                      [--no-covers | --cover-dir DIR] [--preview-dir DIR ...]
  organize-library.py report PLAN.json [--output FILE.html]
  organize-library.py register USB_ROOT DISC --title TITLE --category CATEGORY
  organize-library.py undo USB_ROOT JOURNAL
  organize-library.py clean-empty USB_ROOT [--apply]

How it works:
  1. scan reads every disc image on the drive (.iso, or .cue with its
     tracks), identifies it against the Redump catalog (by checksum when
     possible, otherwise by file name), picks a category, and writes a plan
     (JSON) plus an HTML report. Nothing on the drive changes.
  2. apply carries out a reviewed plan: each identified disc moves to
     Category/Region/Game/, covers and previews are added, and the menu's
     indexes and the library database (.ohmy3do/library.json and
     library.sqlite) are rebuilt. Every change is recorded in an undo
     journal under .ohmy3do/transactions/.
  3. undo reverses an applied journal, refusing if files changed since.

register adds a disc the catalog doesn't know without moving it, and
clean-empty removes empty folders left behind. library-maintenance.py and
the desktop app build on the same files.
"""
import argparse
import base64
import collections
from contextlib import closing
import difflib
import errno
import hashlib
import html
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import sqlite3
import struct
import binascii
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile


def module(name):
    """Load a sibling tool (for example "index-media") as a module."""
    spec = importlib.util.spec_from_file_location(
        name.replace("-", "_"), Path(__file__).with_name(name + ".py")
    )
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


catalog_tools = module("library-catalog")
index_tools = module("index-media")
sector_tools = module("sector-codec")
SCANNER_VERSION = "1.0.3"

# Top-level folders that never hold games to organize.
SKIP = {".ohmy3do", "$boot", "system"}

# The menu's support files an apply may rewrite, checked before it starts.
STATE_FILES = [
    "favorites.txt",
    "recent.txt",
    "last-position.txt",
    "library.3di",
    "metadata.3di",
    "covers/index.3di",
    "previews/index.3di",
    ".ohmy3do/library.json",
    ".ohmy3do/library.sqlite",
]

# The built-in categories. A drive can rename, hide or add categories in
# .ohmy3do/categories.json; these names stay the "roles" that say where
# newly organized discs of each kind go.
CATEGORIES = (
    "Games",
    "Demos",
    "Samplers",
    "Applications",
    "Multimedia",
    "Educational",
    "Bonus Discs",
    "Adult",
)

MAX_CATEGORIES = 32
MAX_CATEGORY_CONFIG_BYTES = 8192

# Characters FAT and Windows forbid in names.
FORBIDDEN_NAME_CHARACTERS = '<>:"/\\|?*'

# Windows marks junctions and other reparse points with this attribute.
FILE_ATTRIBUTE_REPARSE_POINT = 0x400


def default_category_config():
    """The configuration used when a drive has no categories.json: the
    built-in categories, all visible, each the destination for its role."""
    rows = [
        dict(id="category-" + str(i), name=name, hidden=False) for i, name in enumerate(CATEGORIES)
    ]
    return dict(
        schema=1,
        categories=rows,
        defaults={name: row["id"] for name, row in zip(CATEGORIES, rows)},
    )


def is_valid_category_name(name, names_so_far):
    """A folder name FAT and the menu can use, not already taken (ignoring
    case), and not .ohmy3do or a name Windows reserves."""
    base = name.split(".")[0].upper()
    return not (
        not name
        or len(name) > 48
        or name.strip() != name
        or name.endswith(".")
        or any(ord(c) < 32 or ord(c) >= 127 or c in FORBIDDEN_NAME_CHARACTERS for c in name)
        or name.casefold() in names_so_far
        or name.casefold() == ".ohmy3do"
        or base in ("CON", "PRN", "AUX", "NUL")
        or re.fullmatch(r"(COM|LPT)[1-9]", base)
    )


def category_config(root):
    """The drive's category configuration (.ohmy3do/categories.json).

    Returns {"schema": 1, "categories": [{"id", "name", "hidden"}],
    "defaults": {built-in category: id}}. Raises ValueError for a damaged
    file: bad ids, duplicate or unusable names, or defaults that don't name
    a category for every built-in role.
    """
    path = relative_path(root, ".ohmy3do/categories.json")
    if not path.exists():
        return default_category_config()
    raw = path.read_bytes()
    if len(raw) > MAX_CATEGORY_CONFIG_BYTES:
        raise ValueError("Category configuration exceeds 8 KiB")
    # The menu writes into a fixed buffer, so the text may be NUL-padded.
    config = json.loads(raw.split(b"\0", 1)[0])

    rows = config.get("categories", [])
    if (
        config.get("schema") != 1
        or not isinstance(rows, list)
        or not 1 <= len(rows) <= MAX_CATEGORIES
    ):
        raise ValueError("Invalid category configuration")
    ids = set()
    names = set()
    for row in rows:
        name = row.get("name", "")
        identifier = row.get("id", "")
        if (
            not isinstance(row.get("hidden"), bool)
            or not re.fullmatch(r"[A-Za-z0-9-]{1,40}", identifier)
            or identifier in ids
        ):
            raise ValueError("Invalid category identifier or visibility")
        if not is_valid_category_name(name, names):
            raise ValueError("Invalid category name")
        ids.add(identifier)
        names.add(name.casefold())

    defaults = config.get("defaults", {})
    if set(defaults) != set(CATEGORIES) or any(
        identifier not in ids for identifier in defaults.values()
    ):
        raise ValueError("Invalid import category destinations")
    return config


def transfer_categories(source, target):
    """The category configuration to give the drive `target` when copying the
    library `source` to it.

    The library's categories win, but the drive keeps its own hidden
    settings, and categories only the drive has are kept when the drive was
    configured or has a folder for them. Raises ValueError when the result
    has too many categories or two with the same name.
    """
    config = category_config(source)
    previous = category_config(target)
    configured = relative_path(target, ".ohmy3do/categories.json").exists()

    hidden = {c["id"]: c["hidden"] for c in previous["categories"]}
    for category in config["categories"]:
        if configured or hidden.get(category["id"], False):
            category["hidden"] = hidden.get(category["id"], category["hidden"])

    known = {c["id"] for c in config["categories"]}
    for category in previous["categories"]:
        if category["id"] not in known and (
            configured or relative_path(target, category["name"]).exists()
        ):
            config["categories"].append(category)

    names = {c["name"].casefold() for c in config["categories"]}
    if len(config["categories"]) > MAX_CATEGORIES or len(names) != len(config["categories"]):
        raise ValueError(
            "Library and drive categories conflict; "
            "resolve them in Manage categories before copying"
        )
    return config


def category_names(root):
    return tuple(row["name"] for row in category_config(root)["categories"])


# Region words found in file names and folder names, and what they mean.
REGIONS = {
    "us": "USA",
    "usa": "USA",
    "na": "USA",
    "europe": "Europe",
    "eu": "Europe",
    "pal": "Europe",
    "uk": "Europe",
    "gb": "Europe",
    "jp": "Japan",
    "japan": "Japan",
    "j": "Japan",
    "korea": "Korea",
    "kr": "Korea",
    "germany": "Germany",
    "france": "France",
    "world": "World",
    "australia": "Australia",
    "canada": "Canada",
}

# A disc number in brackets: "(Disc 2)", "(CD 1 of 2)".
DISC = re.compile(r"\(\s*(?:disc|disk|cd)\s*0*(\d+)(?:\s+of\s+\d+)?\s*\)", re.I)

# A disc number at the end of a name: "Game - Disc 2", "Game_CD1".
DISC_SUFFIX = re.compile(
    r"(?:\s*[-_.]\s*|\s+)(?:disc|disk|cd)[\s_.-]*0*(\d+)(?:\s+of\s+\d+)?$", re.I
)

# A CUE sheet's FILE line, captured as: the "FILE " prefix, the name (quoted
# or bare), the rest of the line (the file type), and any carriage return.
FILE_LINE = re.compile(
    r'^([ \t]*FILE[ \t]+)(?:"([^"\r\n]+)"|([^\s]+))([ \t]+\S+[^\r\n]*)(\r?)$', re.I | re.M
)

MIB = 1024 * 1024
MAX_CUE_BYTES = 256 * 1024


def digest(path):
    """SHA-1 of a file, read a block at a time."""
    sha = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(MIB), b""):
            sha.update(chunk)
    return sha.hexdigest()


def fingerprint(path, hashes=True):
    """A file's size and modification time, plus its SHA-1 when `hashes`.

    Raises ValueError if the file changes while it is being read.
    """
    before = path.stat()
    result = dict(size=before.st_size, mtime_ns=before.st_mtime_ns)
    if hashes:
        result["sha1"] = digest(path)
    after = path.stat()
    if before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns:
        raise ValueError(f"File changed while reading: {path}")
    return result


def is_reparse_point(path):
    return bool(getattr(path.lstat(), "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT)


def relative_path(root, relative):
    """Reject traversal, symlinks and names Windows/FAT cannot distinguish.

    `relative` is a path inside the drive written with forward slashes.
    Returns root/relative after checking each part: no "." or "..", no
    trailing space or dot, no forbidden characters, no existing entry that
    differs only in case, and no links or junctions.
    """
    if not isinstance(relative, str) or not relative or "\\" in relative:
        raise ValueError(f"Invalid relative path: {relative!r}")
    parts = PurePosixPath(relative).parts
    if PurePosixPath(relative).is_absolute() or "/".join(parts) != relative:
        raise ValueError(f"Noncanonical relative path: {relative!r}")
    for part in parts:
        if (
            part in (".", "..")
            or part[-1:] in (" ", ".")
            or any(ord(c) < 32 or ord(c) == 127 or c in ':*?"<>|' for c in part)
        ):
            raise ValueError(f"Unsafe USB path: {relative}")

    path = root
    for part in parts:
        if path.exists() and not path.is_dir():
            raise ValueError(f"Parent is not a directory: {path}")
        if path.is_dir():
            matches = [p.name for p in path.iterdir() if p.name.casefold() == part.casefold()]
            if any(name != part for name in matches):
                raise ValueError(f"Path differs only in case from an existing entry: {relative}")
        path = path / part
        if path.is_symlink() or (path.exists() and is_reparse_point(path)):
            raise ValueError(f"Symlinks/junctions are not supported: {relative}")
    return path


def safe_name(name, maximum=110):
    """`name` made safe as a FAT file or folder name of at most `maximum`
    UTF-8 bytes. Forbidden characters become " - "; a name that is too long
    is cut and given a short hash so different long names stay different."""
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f\x7f]', " - ", name)
    name = re.sub(r"\s+", " ", name).strip(" .")
    if not name:
        name = "Untitled"
    if re.match(r"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)", name, re.I):
        name = "_" + name
    raw = name.encode("utf-8")
    if len(raw) > maximum:
        tail = "-" + hashlib.sha1(raw).hexdigest()[:8]
        name = raw[: maximum - len(tail)].decode("utf-8", errors="ignore").rstrip(" .") + tail
    return name


def region_of(name):
    """The regions named in a title's bracketed groups: "(USA, Europe)"."""
    result = set()
    for group in re.findall(r"\(([^)]*)\)", name):
        for part in re.split(r"[,/-]", group):
            if part.strip().casefold() in REGIONS:
                result.add(REGIONS[part.strip().casefold()])
    return result


def disc_of(name):
    """The disc number in a name, or None."""
    match = DISC.search(name) or DISC_SUFFIX.search(name)
    return int(match[1]) if match else None


def identification_title(name):
    """A name's normalized title (see library-catalog.py's title_key), with a
    trailing disc number removed."""
    return catalog_tools.title_key(DISC_SUFFIX.sub("", name).strip())


def without_disc(name):
    return DISC_SUFFIX.sub("", DISC.sub("", name)).strip()


def folder_regions(source):
    """Regions named by the folders a disc sits in ("USA/Game.iso"), ignoring
    a leading sort number such as "01 - "."""
    hints = set()
    for part in PurePosixPath(source).parts[:-1]:
        token = re.sub(r"^\d+[\s._-]+", "", part).strip().casefold()
        if len(token) > 1 and token in REGIONS:
            hints.add(REGIONS[token])
    return hints


def product_codes(name):
    """Product codes in square brackets ("[T-6801G]"): words of letters and
    digits containing both, at least 4 characters. "[Variant ...]" tags are
    not codes."""
    codes = []
    for tag in re.findall(r"\[([^]]+)\]", name):
        stripped = tag.strip()
        if (
            not tag.casefold().startswith("variant ")
            and len(stripped) >= 4
            and re.fullmatch(r"[A-Za-z0-9]+(?:[ ._-][A-Za-z0-9]+)*", stripped)
            and re.search(r"[A-Za-z]", tag)
            and re.search(r"\d", tag)
        ):
            codes.append(stripped)
    return codes


def serial_key(serial):
    """A serial reduced to lowercase letters and digits, for comparison."""
    return re.sub(r"[^a-z0-9]", "", serial.casefold())


def catalog_serials(game):
    """Every serial a catalog game lists (its own and its tracks'), as keys."""
    result = set()
    for serial in [game.get("serial", "")] + [r.get("serial", "") for r in game["roms"]]:
        for part in re.split(r"[,;/]", serial):
            if part.strip():
                result.add(serial_key(part))
    return result


def disc_stem(name, maximum=110):
    """A file name (without extension) for a disc: the title made safe, its
    trailing [tags] kept when they fit, and " (Disc N)" when numbered."""
    number = disc_of(name)
    base = without_disc(name)
    variant = re.search(r"\s*((?:\[[^][]+\]\s*)+)$", base)
    if variant and len(variant[1].encode()) > maximum - 20:
        variant = None
    suffix = (" " + variant[1].strip()) if variant else ""
    if variant:
        base = base[: variant.start()].strip()
    if number is not None:
        suffix += f" (Disc {number})"
    return safe_name(base, maximum - len(suffix)) + suffix


def dump_signature(game):
    """A game's tracks as sorted (size, best checksum) pairs, in JSON."""
    identity = []
    for rom in game["roms"]:
        checksum = next(
            (key + ":" + rom[key].lower() for key in ("sha1", "md5", "crc") if rom.get(key)),
            "unknown",
        )
        identity.append((str(rom["size"]), checksum))
    return json.dumps(sorted(identity), separators=(",", ":"))


def release_records(games):
    """Recover dump labels the DAT game title omits; disambiguate unnamed dumps.

    Serial-only duplicates with identical data describe the same dump. The
    fallback code identifies a disc dump, not an inferred multi-disc edition.

    Returns copies of the catalog games with "_release_name" (the name the
    disc is filed under), "_dump_signature" and "_variant" (an 8-digit code
    when several different dumps share one name, else None).
    """
    result = []
    signatures = collections.defaultdict(set)
    for game in games:
        row = dict(game)
        # The track file names may carry labels the game title lacks.
        stems = {
            re.sub(r"\s*\(Track\s+\d+\)\s*$", "", Path(rom["name"]).stem, flags=re.I).strip()
            for rom in game["roms"]
            if rom.get("name")
        }
        name = next(iter(stems)) if len(stems) == 1 else game["name"]
        same_title = catalog_tools.title_key(name) == catalog_tools.title_key(game["name"])
        if not same_title or disc_of(name) != disc_of(game["name"]):
            name = game["name"]
        row["_dump_signature"] = dump_signature(game)
        row["_release_name"] = name
        signatures[name.casefold()].add(row["_dump_signature"])
        result.append(row)

    for row in result:
        row["_variant"] = None
        if len(signatures[row["_release_name"].casefold()]) > 1:
            code = hashlib.sha1(row["_dump_signature"].encode()).hexdigest()[:8].upper()
            row["_variant"] = code
            number = disc_of(row["_release_name"])
            name = without_disc(row["_release_name"])
            disc = f" (Disc {number})" if number is not None else ""
            row["_release_name"] = name + f" [Variant {code}]" + disc
    return result


def decode_cue(raw):
    """CUE text and the encoding to write it back with: UTF-8 (keeping a
    byte-order mark if there was one), or Windows-1252 for older sheets."""
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        return raw.decode("cp1252"), "cp1252"
    encoding = "utf-8-sig" if raw.startswith(b"\xef\xbb\xbf") else "utf-8"
    return text, encoding


def cue_files(root, relative):
    """Read the CUE sheet at `relative` and check the files it names.

    Every FILE must be a sibling of the sheet (matched ignoring case) that
    exists, isn't another CUE sheet and isn't the menu's boot image.
    Returns (the files' paths relative to the drive, the sheet's text, its
    encoding, its raw bytes). The text keeps every non-FILE line verbatim.
    """
    path = relative_path(root, relative)
    if path.stat().st_size > MAX_CUE_BYTES:
        raise ValueError("CUE exceeds 256 KiB")
    raw = path.read_bytes()
    if len(raw) > MAX_CUE_BYTES:
        raise ValueError("CUE exceeds 256 KiB")
    # Preserve the original encoding and all non-FILE lines verbatim.
    text, encoding = decode_cue(raw)

    refs = []
    for match in FILE_LINE.finditer(text):
        name = match[2] or match[3]
        if "/" in name or "\\" in name or name in (".", ".."):
            raise ValueError("CUE FILE references must name sibling files (no paths)")
        siblings = [p.name for p in path.parent.iterdir() if p.name.casefold() == name.casefold()]
        if len(siblings) != 1:
            raise ValueError(f"CUE data filename is missing or ambiguous: {name}")
        ref = str(PurePosixPath(relative).parent / siblings[0])
        target = relative_path(root, ref)
        if target.parent == root and target.name.casefold() in ("boot.iso", "menu-demo.iso"):
            raise ValueError("CUE references a root menu boot image; keep it separate")
        if not target.is_file():
            raise ValueError(f"CUE data file is missing: {ref}")
        # The drive's firmware reads any track file as raw sectors.
        if target.suffix.lower() == ".cue":
            raise ValueError(f"A CUE track cannot be another CUE: {ref}")
        if ref not in refs:
            refs.append(ref)

    if not refs:
        raise ValueError("No FILE references in CUE")
    # A FILE line FILE_LINE can't read would otherwise be silently skipped.
    if len(re.findall(r"^\s*FILE\b", text, re.I | re.M)) != len(list(FILE_LINE.finditer(text))):
        raise ValueError("Cannot parse every CUE FILE directive")
    return refs, text, encoding, raw


def disc_image_paths(root):
    """Every .iso and .cue on the drive, relative to its root, skipping links,
    hidden and system folders, and the menu's own images."""
    paths = []

    def walk(directory):
        for path in sorted(directory.iterdir()):
            if path.is_symlink():
                continue
            if path.is_dir():
                if path.name.casefold() not in SKIP and not path.name.startswith("."):
                    walk(path)
            elif path.suffix.lower() in (".iso", ".cue"):
                if path.name.lower() == "oh my 3do - folder navigation helper (keep).iso":
                    continue
                if path.parent == root and path.name.lower() in ("boot.iso", "menu-demo.iso"):
                    continue
                paths.append(path.relative_to(root).as_posix())

    walk(root)
    return paths


def files_named_by_broken_cue(root, source):
    """The existing sibling files a CUE sheet that failed to parse still names.

    Even a broken CUE may reference an existing ISO. Keep that ISO with the
    unresolved set rather than moving it separately.
    """
    path = relative_path(root, source)
    if path.stat().st_size > MAX_CUE_BYTES:
        return []
    text = path.read_bytes().decode("utf-8-sig", errors="replace")
    found = []
    for match in FILE_LINE.finditer(text):
        name = match[2] or match[3]
        if "/" in name or "\\" in name:
            continue
        for sibling in path.parent.iterdir():
            if (
                sibling.name.casefold() == name.casefold()
                and sibling.is_file()
                and not sibling.is_symlink()
            ):
                found.append(sibling.relative_to(root).as_posix())
    return found


def cue_unit(root, source, consumed):
    """The disc entry for a CUE sheet, adding its track files to `consumed`.

    A CUE that only wraps a plain .iso becomes an entry for the .iso, with
    the sheet kept as an alias to be archived when organized.
    """
    unit = dict(source=source, files=[source])
    try:
        refs, text, encoding, raw = cue_files(root, source)
        unit.update(files=[source] + refs, cue_text=text, cue_encoding=encoding)
        consumed.update(refs)
        if len(refs) == 1 and index_tools.redundant_iso_name(text):
            unit.update(
                source=refs[0],
                name_source=source,
                files=[refs[0], source],
                ignored_cues=[source],
                aliases=[source],
            )
    except (ValueError, OSError) as error:
        unit["error"] = str(error)
        for ref in files_named_by_broken_cue(root, source):
            if ref not in unit["files"]:
                unit["files"].append(ref)
                consumed.add(ref)
    return unit


def scan_units(root):
    """Group the drive's disc images into disc entries ("units").

    Each unit is a dict with "source" (the .iso or .cue that names the disc)
    and "files" (the source plus any track files), and may have:
      cue_text, cue_encoding   the sheet's text, for CUE entries
      name_source, aliases, ignored_cues
                               for an .iso wrapped by a redundant CUE
      error                    why the entry can't be organized
      invalid_data             the data isn't a disc image (empty or archive)
    """
    units = []
    consumed = set()  # track files already claimed by a CUE sheet
    # CUE sheets first, so their tracks aren't also treated as discs.
    paths = sorted(disc_image_paths(root), key=lambda p: (Path(p).suffix.lower() != ".cue", p))
    for source in paths:
        if source in consumed:
            continue
        if Path(source).suffix.lower() == ".cue":
            units.append(cue_unit(root, source, consumed))
        else:
            units.append(dict(source=source, files=[source]))

    # Files claimed by more than one entry can't be moved safely.
    owners = collections.Counter(file for unit in units for file in unit["files"])
    for unit in units:
        if any(owners[file] > 1 for file in unit["files"]):
            if unit.get("ignored_cues"):
                # Treat the wrapping CUE as the entry again.
                cue = unit["ignored_cues"][0]
                unit.update(source=cue, files=[cue] + [f for f in unit["files"] if f != cue])
                unit.pop("ignored_cues")
                unit.pop("aliases")
            unit["error"] = "Data files are shared between disc entries; organize this set manually"
        if not unit.get("error"):
            for file in unit["files"]:
                if Path(file).suffix.lower() == ".cue":
                    continue
                issue = index_tools.disc_data_issue(relative_path(root, file))
                if issue:
                    unit.update(error=issue, invalid_data=True)
                    break
    return units


# identify() and its steps return (game or None, confidence, reason,
# suggestions). The confidence says how the disc was identified:
#   manual              chosen in an overrides file
#   checksum            every data file's SHA-1 and size match a release
#   converted-checksum  an .iso matches once rebuilt as full 2352-byte sectors
#   data-track          only the first data track matches (others missing)
#   filename            the file name names one release (not checksum-verified)
#   unverified-release  the title matches one release whose labels differ
#   ambiguous           more than one release fits; an override is needed
#   unmatched           nothing safe matches; the file stays in place


def identify_override(games, override):
    """The release an overrides file names, by exact release or catalog name."""
    selected = [g for g in games if g["_release_name"] == override]
    if not selected:
        selected = [g for g in games if g["name"] == override]
    names = sorted({g["_release_name"] for g in selected})
    if not selected or len(names) != 1:
        return None, "ambiguous", "Override needs an exact release/variant name", names
    return (
        selected[0],
        "manual",
        "Explicit overrides file; catalog dump checksum not verified",
        [],
    )


def identify_by_checksum(games, data):
    """A release whose tracks match the data files' sizes and SHA-1s exactly.
    Returns None when nothing matches."""
    checksums = sorted((f["size"], f["sha1"].lower()) for f in data)
    matches = [
        g
        for g in games
        if sorted((int(r["size"]), r.get("sha1", "").lower()) for r in g["roms"]) == checksums
    ]
    if matches and len({g["_release_name"].casefold() for g in matches}) == 1:
        return matches[0], "checksum", "Complete data-file SHA-1 and size match", []
    if len(matches) > 1:
        return (
            None,
            "ambiguous",
            "Checksums identify multiple catalog releases",
            [g["_release_name"] for g in matches],
        )
    return None


def is_first_track(rom):
    return re.search(r"(?:\(Track\s+0*1\)|\btrack0*1)\.bin$", rom.get("name", ""), re.I)


def identify_by_reconstruction(games, raw):
    """Match an .iso's rebuilt 2352-byte-sector checksum (see sector-codec.py)
    against single-track releases, or against the first track of
    multi-track releases. Returns None when nothing matches."""
    key = (raw["size"], raw["sha1"].lower())
    complete = []
    partial = []
    for game in games:
        roms = game["roms"]
        if len(roms) == 1:
            if (int(roms[0]["size"]), roms[0].get("sha1", "").lower()) == key:
                complete.append(game)
        else:
            # DAT ordering is not a track number. Only a named first
            # track can identify data when the other tracks are absent.
            first = [r for r in roms if is_first_track(r)]
            if len(first) == 1 and (int(first[0]["size"]), first[0].get("sha1", "").lower()) == key:
                partial.append(game)

    names = sorted({g["_release_name"] for g in complete + partial})
    if len(names) > 1:
        return (
            None,
            "ambiguous",
            "Reconstructed data checksum identifies multiple releases; exact disc unresolved",
            names,
        )
    if complete:
        return (
            complete[0],
            "converted-checksum",
            "Complete single-track SHA-1 and size match after MODE1/2048 reconstruction (LBA 0); "
            "source ISO unchanged",
            [],
        )
    if partial:
        return (
            partial[0],
            "data-track",
            "Reconstructed first data-track SHA-1 and size match; remaining catalog tracks are "
            "absent and unverified; review required",
            [],
        )
    return None


def exact_name_matches(games, name):
    """Releases whose name (or file name) is exactly `name`, ignoring case;
    otherwise the catalog games of that name if they are one release."""
    folded = name.casefold()
    exact = [
        g
        for g in games
        if folded in (g["_release_name"].casefold(), disc_stem(g["_release_name"]).casefold())
    ]
    if not exact:
        aliases = [g for g in games if g["name"].casefold() == folded]
        if len({g["_release_name"].casefold() for g in aliases}) == 1:
            exact = aliases
    return exact


def special_release_tags(name):
    """Bracketed tags marking a revision, beta, prototype, demo, sample,
    unlicensed or alternate release."""
    return {
        tag.casefold()
        for tag in re.findall(r"\(([^)]*)\)", name)
        if re.search(r"\b(rev|beta|proto|demo|sample|unl|alt)\b", tag, re.I)
    }


def game_level_entry(candidates, name, codes, regions, hint):
    """A filename code absent from Redump can still identify a game-level
    entry. Preserve it without assigning an unsupported Redump edition.

    Returns a match built from the candidates' shared LaunchBox metadata,
    or None when they don't share one.
    """
    metadata = {json.dumps(g.get("metadata", {}), sort_keys=True) for g in candidates}
    if len(metadata) != 1 or not candidates[0].get("metadata"):
        return None
    row = dict(candidates[0])
    title = (
        row["metadata"].get("title")
        or re.split(r"[\[(]", without_disc(name), maxsplit=1)[0].strip()
    )
    region = next(iter(regions))
    base = f"{title} ({region})"
    label = base + " " + " ".join("[" + code + "]" for code in codes)
    number = disc_of(name)
    disc = f" (Disc {number})" if number is not None else ""
    row.update(
        name=base + disc,
        region=region,
        _release_name=label + disc,
        _variant="; ".join(codes),
        _edition_unresolved=True,
        roms=[],
    )
    return (
        row,
        "filename",
        "Game-level title/region match"
        + hint
        + "; source code preserved; exact edition unresolved; dump checksum not verified",
        [],
    )


def identify_by_name(unit, games):
    """Identify a disc from its file name (and folder), never guessing.

    In order: an exact release name; then the same title and disc number,
    narrowed by region (from the name, or a single region folder) and by
    product codes in the name. A unique result that doesn't silently pick a
    revision, demo or other special release is accepted; otherwise the
    candidates are returned as suggestions.
    """
    name = Path(unit.get("name_source", unit["source"])).stem
    exact = exact_name_matches(games, name)
    if exact:
        return exact[0], "filename", "Exact catalog filename; dump checksum not verified", []

    title = identification_title(name)
    candidates = [
        g
        for g in games
        if identification_title(g["name"]) == title and disc_of(g["name"]) == disc_of(name)
    ]
    title_candidates = candidates

    regions = region_of(name)
    hint = ""
    if not regions:
        folders = folder_regions(unit["source"])
        if len(folders) == 1:
            regions = folders
            hint = " using folder region " + next(iter(folders))
    if regions:
        candidates = [g for g in candidates if regions <= region_of(g["name"])]

    codes = product_codes(name)
    if codes:
        code_keys = {serial_key(code) for code in codes}
        known_serials = [g for g in title_candidates if code_keys <= catalog_serials(g)]
        serial_matches = [g for g in candidates if code_keys <= catalog_serials(g)]
        if known_serials and not serial_matches:
            return (
                None,
                "ambiguous",
                "Catalog serial conflicts with the filename/folder region; explicit override needed",
                sorted({g["_release_name"] for g in known_serials}),
            )
        if len({g["_release_name"] for g in serial_matches}) == 1:
            return (
                serial_matches[0],
                "filename",
                "Exact title/disc/catalog serial match" + hint + "; dump checksum not verified",
                [],
            )

    # One candidate per release name.
    candidates = list({g["_release_name"].casefold(): g for g in candidates}.values())
    # Do not silently choose a revision, prototype, demo or alternate language.
    if len(candidates) == 1 and not codes:
        if special_release_tags(name) == special_release_tags(candidates[0]["_release_name"]):
            return (
                candidates[0],
                "filename",
                "Unique title/region/disc match" + hint + "; dump checksum not verified",
                [],
            )
    if codes and candidates and len(regions) == 1:
        match = game_level_entry(candidates, name, codes, regions, hint)
        if match:
            return match
    if len(candidates) == 1:
        return (
            None,
            "unverified-release",
            "Title matches one catalog entry, but its release/variant labels differ; "
            "exact edition and checksum remain unverified",
            [candidates[0]["_release_name"]],
        )
    if candidates:
        return (
            None,
            "ambiguous",
            "More than one release or variant; explicit override needed",
            [g["_release_name"] for g in candidates][:20],
        )

    # Nothing fits: suggest releases with similar titles and the same disc.
    similar = difflib.get_close_matches(
        title, {identification_title(g["name"]) for g in games}, n=3, cutoff=0.65
    )
    suggestions = [
        g["_release_name"]
        for g in games
        if identification_title(g["name"]) in similar
        and (disc_of(name) is None or disc_of(g["name"]) == disc_of(name))
    ]
    return None, "unmatched", "No safe match; file stays in place", suggestions[:12]


def identify(unit, games, stats, override=None, quick=False):
    """Identify one disc entry against the catalog's release records.

    `stats` holds each file's fingerprint (with "mode1_raw" for an .iso whose
    rebuilt sectors were hashed). `override` is a release name from an
    overrides file. With `quick`, no checksums were read, so only names are
    used. Returns (game or None, confidence, reason, suggestions); see the
    confidence list above.
    """
    if override:
        return identify_override(games, override)

    data = [stats[f] for f in unit["files"] if Path(f).suffix.lower() != ".cue"]
    if not quick:
        result = identify_by_checksum(games, data)
        if result:
            return result
        if len(data) == 1 and data[0].get("mode1_raw"):
            result = identify_by_reconstruction(games, data[0]["mode1_raw"])
            if result:
                return result
    return identify_by_name(unit, games)


def support_path(root, name):
    """A menu support file's path relative to the drive: names not already
    under .ohmy3do/ are placed in it."""
    if name.startswith(".ohmy3do/"):
        return name
    return (index_tools.support_base(root) / name).relative_to(root).as_posix()


def asset_associations(root, folder):
    """The media a folder ("covers" or "previews") already has for discs.

    Returns {} when the folder doesn't exist, otherwise {"previous": {folded
    disc path: file} from its index (files that still exist), "assets":
    {folded file stem: file} for every media file in it}.
    """
    directory = relative_path(root, support_path(root, folder))
    if not directory.exists():
        return {}
    previous = index_tools.existing_index(directory)
    assets = {}
    for path in directory.iterdir():
        if (
            path.is_file()
            and not path.is_symlink()
            and path.suffix.lower() in (".png", ".3dc", ".3dv")
            and not path.name.lower().endswith("-large.3dc")
        ):
            key = index_tools.fold(path.stem)
            if key in assets:
                raise ValueError(f"Ambiguous media basename: {path}")
            assets[key] = path.name
    previous = {
        key: name
        for key, name in previous.items()
        if relative_path(root, support_path(root, folder + "/" + name)).is_file()
    }
    return dict(previous=previous, assets=assets)


def existing_asset(associations, source):
    """The media file for a disc: one named like the disc file, or else the
    one its index entry names."""
    by_name = associations.get("assets", {})
    by_index = associations.get("previous", {})
    return by_name.get(
        index_tools.fold(Path(source).stem),
        by_index.get(index_tools.fold(source)),
    )


def row_asset(associations, row):
    """The existing media for a plan row, checking its aliases too."""
    for source in [row["source"]] + row.get("aliases", []):
        name = existing_asset(associations, source)
        if name:
            return name
    return None


# Redump category names that map to a different built-in category.
REDUMP_CATEGORY_ALIASES = {
    "video": "Multimedia",
    "audio": "Multimedia",
    "coverdiscs": "Samplers",
    "add-ons": "Games",
}

# LaunchBox genres that mark a game.
GAME_GENRES = {
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
}

# Title words and genres that suggest each non-game category.
DEMO_TITLE = r"\((?:demo|demonstration|trial)(?:[^)]*)\)|\b(?:demo|demonstration|trial\s+version)\b"
APPLICATION_TITLE = r"\b(?:storage\s+manager|system\s+utility|utilities|operating\s+system)\b"
APPLICATION_GENRES = {"utility", "utilities", "application", "applications", "operating system"}
MULTIMEDIA_TITLE = (
    r"\b(?:atlas|almanac|encyclop(?:e|ae)dia|dictionary|museum|art\s+gallery|documentary"
    r"|photo\s*(?:cd|shoot)|slide\s*show|interactive\s+(?:book|magazine)|music\s+video|karaoke)\b"
)
MULTIMEDIA_GENRES = {
    "multimedia",
    "reference",
    "documentary",
    "encyclopedia",
    "music video",
    "karaoke",
}
NON_GAME_GENRES = {
    "education",
    "educational",
    "multimedia",
    "reference",
    "documentary",
    "encyclopedia",
}


def category_evidence(title, genres, release):
    """{category: reason} for every non-game category the title, genres or
    LaunchBox release type point to."""
    evidence = {}
    if re.search(r"\bbonus\s+(?:disc|cd)\b", title, re.I) or release in ("bonus disc", "bonus cd"):
        evidence["Bonus Discs"] = "Catalog title/release type identifies a bonus disc"
    if re.search(r"\b(?:sampler|sample\s+(?:disc|cd))\b", title, re.I) or release == "sampler":
        evidence["Samplers"] = "Catalog title/release type identifies a sampler"
    if re.search(DEMO_TITLE, title, re.I) or release in ("demo", "demonstration", "trial"):
        evidence["Demos"] = "Catalog title/release type identifies a demo"
    if re.search(APPLICATION_TITLE, title, re.I) or genres & APPLICATION_GENRES:
        evidence["Applications"] = "Catalog title/genre identifies an application"
    if re.search(MULTIMEDIA_TITLE, title, re.I) or genres & MULTIMEDIA_GENRES:
        evidence["Multimedia"] = "Catalog title/genre identifies multimedia/reference content"
    is_educational = genres & {"education", "educational"} or re.search(
        r"\beducational\b", title, re.I
    )
    if is_educational and not evidence.keys() & {"Demos", "Samplers", "Multimedia", "Bonus Discs"}:
        evidence["Educational"] = "Catalog title/genre identifies educational software"
    return evidence


def classify(game, override=None, allowed=None):
    """Choose a category for an identified release.

    Returns (category or None, reason). An explicit `override` must be one
    of `allowed` (the drive's category names; the built-in ones by default).
    Otherwise Redump's own category is used when it has one, then evidence
    in the title, genres and release type. None means the evidence is
    missing or conflicting, so the disc waits for a person to choose.
    """
    if override is not None:
        choices = {name.casefold(): name for name in (allowed or CATEGORIES)}
        if not isinstance(override, str) or override.casefold() not in choices:
            raise ValueError("Category must be one of: " + ", ".join(allowed or CATEGORIES))
        return choices[override.casefold()], "Explicit category override"

    category = game.get("category", "").casefold()
    categories = {name.casefold(): name for name in CATEGORIES}
    categories.update(REDUMP_CATEGORY_ALIASES)
    if category in categories:
        return categories[category], "Redump category: " + game["category"]

    title = game["_release_name"]
    metadata = game.get("metadata", {})
    genres = {
        s.strip().casefold() for s in re.split(r"[;,]", metadata.get("genre") or "") if s.strip()
    }
    release = (metadata.get("release_type") or "").strip().casefold()
    evidence = category_evidence(title, genres, release)

    if "Multimedia" in evidence and genres & GAME_GENRES and not genres & NON_GAME_GENRES:
        return None, "Multimedia title/genre conflicts with a game genre; choose a category"
    if len(evidence) == 1:
        return next(iter(evidence.items()))
    if evidence:
        return None, "Mixed category evidence: " + ", ".join(evidence) + "; choose a category"
    if re.search(r"\((?:sample|preview)(?:[^)]*)\)", title, re.I) or genres & {
        "music",
        "life simulation",
    }:
        return (
            None,
            "Music/life-simulation/sample labels alone do not establish a category; "
            "choose a category",
        )
    if genres & GAME_GENRES:
        return "Games", "Catalog genre identifies a game"
    return (
        None,
        "Insufficient evidence to distinguish a game from other software; choose a category",
    )


def category_destinations(config):
    """{built-in category: the drive's folder name for it}."""
    return {
        role: next(row["name"] for row in config["categories"] if row["id"] == identifier)
        for role, identifier in config["defaults"].items()
    }


def choice_for(mapping, unit):
    """The value an overrides or categories file gives a disc entry (under
    its source or an alias), or None. Raises ValueError if they disagree."""
    keys = [unit["source"]] + unit.get("aliases", [])
    values = {mapping[key] for key in keys if key in mapping}
    if len(values) > 1:
        raise ValueError("Conflicting ISO/CUE choices for " + unit["source"])
    return next(iter(values), None)


def check_category_overrides(categories, units, allowed):
    """A categories file must map scanned disc paths to known category names."""
    if not isinstance(categories, dict):
        raise ValueError(
            "Category overrides must be a JSON object mapping relative paths to categories"
        )
    scanned = {key for unit in units for key in [unit["source"]] + unit.get("aliases", [])}
    for source, category in categories.items():
        if source not in scanned:
            raise ValueError(f"Category override does not name a scanned disc entry: {source}")
        if not isinstance(category, str):
            raise ValueError("Category overrides must contain category names")
        classify({}, category, allowed)


def previous_choices(root, allowed):
    """What an earlier library.json remembers.

    Returns (records with a category someone chose, by path; registered
    discs, by path).
    """
    database = relative_path(root, ".ohmy3do/library.json")
    if not database.exists():
        return {}, {}
    if not database.is_file() or database.stat().st_size > 16 * MIB:
        raise ValueError("Invalid existing library.json")
    records = json.loads(database.read_text(encoding="utf-8")).get("games", [])
    registrations = {
        r["path"]: r
        for r in records
        if r.get("identification") == "unverified" and r.get("registration")
    }
    saved = {
        row["path"]: row
        for row in records
        if row.get("category") in allowed
        and row.get("category_reason") == "Explicit category override"
    }
    return saved, registrations


def registration_still_valid(root, certificate, unit, quick):
    """Whether a registered disc's files are exactly those recorded (same
    names and sizes and times, and SHA-1s unless `quick`)."""
    if set(certificate) != set(unit["files"]):
        return False
    keys = ("size", "mtime_ns") + (() if quick else ("sha1",))
    for name, expected in certificate.items():
        actual = fingerprint(relative_path(root, name), not quick)
        if not all(actual.get(k) == expected.get(k) for k in keys):
            return False
    return True


def registered_row(row, registered, quick):
    """Fill in a plan row for a disc registered earlier, left in place."""
    row.update(
        status="registered",
        confidence="unverified",
        canonical=registered["title"],
        target=row["source"],
        region=registered.get("region"),
        category=registered["category"],
        category_reason=registered.get("category_reason"),
        category_review=False,
        registration=registered["registration"],
        reason="Previously registered disc; catalog identity remains unverified; files unchanged"
        + (" (size/time only in quick scan)" if quick else " (SHA-1 checked)"),
        metadata=registered.get("metadata", {}),
        cover=registered.get("cover"),
        preview=registered.get("preview"),
    )


def is_audio_only(unit):
    """Whether a CUE entry's tracks are all audio (a music CD)."""
    track_modes = re.findall(r"^\s*TRACK\s+\d+\s+(\S+)", unit.get("cue_text", ""), re.I | re.M)
    return bool(track_modes) and all(mode.upper() == "AUDIO" for mode in track_modes)


class Mode1Encoder:
    """The sector encoder used to rebuild .iso files as full sectors (see
    sector-codec.py), loaded on first use with a note saying which one."""

    def __init__(self):
        self.encoder = None
        self.announced = False

    def get(self):
        if not self.announced:
            self.encoder = sector_tools.native_encoder() or sector_tools.encode_python
            if self.encoder != sector_tools.encode_python:
                kind = "native accelerator"
            else:
                kind = "portable Python (slower; see build-sector-codec.py)"
            print("Converted ISO verification: " + kind, flush=True)
            self.announced = True
        return self.encoder


def file_stats(root, unit, quick, raw_sizes, encoder):
    """Fingerprint each of a disc entry's files ({path: fingerprint}).

    A lone .iso whose size, rebuilt as 2352-byte sectors, matches some
    catalog track is also hashed that way ("mode1_raw"), since Redump lists
    full-sector checksums. With `quick` only CUE sheets are hashed.
    """
    stats = {}
    data_files = [f for f in unit["files"] if Path(f).suffix.lower() != ".cue"]
    for file in unit["files"]:
        path = relative_path(root, file)
        size = path.stat().st_size
        sectors = size // 2048
        reconstruct = (
            not quick
            and len(data_files) == 1
            and file == unit["source"]
            and path.suffix.lower() == ".iso"
            and size > 0
            and size % 2048 == 0
            and sectors <= sector_tools.MAX_SECTORS
            and sectors * 2352 in raw_sizes
        )
        if reconstruct:
            stats[file] = sector_tools.digests(path, encoder.get())
        else:
            stats[file] = fingerprint(path, not quick or path.suffix.lower() == ".cue")
    return stats


def primary_region(canonical):
    """The first region named in a release name's brackets, or "Unknown"."""
    for tag in re.findall(r"\(([^)]*)\)", canonical):
        first = tag.split(",")[0].strip().casefold()
        if first in REGIONS:
            return REGIONS[first]
    return "Unknown"


def in_bonus_folder(source):
    """Whether a disc sits in a "Bonus Discs" folder (with or without a
    leading sort number)."""
    return any(
        re.sub(r"^\d+[\s._-]+", "", part).casefold() == "bonus discs"
        for part in PurePosixPath(source).parts[:-1]
    )


def planned_moves(unit, directory, main, output_title, stats):
    """Where each of an entry's files goes: the source to `main`, a redundant
    CUE to .ohmy3do/ignored-cues/, and track files beside the main file
    named after the title (numbered when there are several)."""
    moves = []
    for track, file in enumerate(unit["files"]):
        if file == unit["source"]:
            target = main
        elif file in unit.get("ignored_cues", []):
            target = (
                ".ohmy3do/ignored-cues/" + hashlib.sha256(file.encode()).hexdigest()[:24] + ".cue"
            )
        else:
            # Disc tags keep common track01.bin names from colliding across discs.
            track_tag = f" (Track {track:02d})" if len(unit["files"]) > 2 else ""
            name = disc_stem(output_title, 95) + track_tag + Path(file).suffix.lower()
            target = str(directory / name)
        moves.append(dict(source=file, target=target, fingerprint=stats[file]))
    return moves


def plan_identified(row, unit, game, confidence, reason, context):
    """Fill in the row for an identified disc: its category, its target
    Category/Region/Game/ path, and the moves to get there."""
    canonical = game["_release_name"]
    region = game.get("region") or primary_region(canonical)
    # Opaque dump codes do not imply an edition shared by other discs.
    # Keep them in filenames; retain the common game folder.
    group = re.sub(r"\s*\[Variant [0-9A-F]{8}\]", "", without_disc(canonical)).strip()
    if game.get("_edition_unresolved"):
        group = without_disc(game["name"])

    # The category: chosen in a categories file, remembered from an earlier
    # choice for the same release, from a "Bonus Discs" folder, or classified.
    override = choice_for(context["categories"], unit)
    saved = context["saved"]
    previous = next(
        (saved[k] for k in [unit["source"]] + unit.get("aliases", []) if k in saved), {}
    )
    if override is None and previous.get("title") == canonical:
        override = previous["category"]
    category, category_reason = classify(game, override, context["allowed"])
    if override is None and in_bonus_folder(unit["source"]):
        category, category_reason = "Bonus Discs", "Source folder identifies a bonus disc"
    bonus_category = category == "Bonus Discs"
    if override is None and category:
        category = context["destinations"].get(category, category)

    directory = None
    if category:
        directory = PurePosixPath(category) / safe_name(region) / safe_name(group)
    # An extra disc can share a base game's title but have its own audio or
    # pack-in labels. Keep those source labels instead of renaming it as the game.
    if bonus_category:
        output_title = Path(unit.get("name_source", unit["source"])).stem
    else:
        output_title = canonical
    main = None
    if directory:
        main = str(directory / (disc_stem(output_title) + Path(unit["source"]).suffix.lower()))

    verified = confidence in ("checksum", "converted-checksum", "manual")
    associations = context["associations"]
    row.update(
        status="ready" if verified or context["accept_names"] else "review",
        canonical=canonical,
        region=region,
        disc=disc_of(canonical),
        target=main,
        catalog_name=None if game.get("_edition_unresolved") else game["name"],
        variant=game["_variant"],
        edition_unresolved=bool(game.get("_edition_unresolved")),
        category=category,
        category_reason=category_reason,
        category_review=category is None,
        metadata=game.get("metadata", {}),
        group=group,
        cover=row_asset(associations["covers"], row),
        preview=row_asset(associations["previews"], row),
    )
    if category is None:
        row.update(status="category-review", reason=reason + "; " + category_reason)
        return
    row["moves"] = planned_moves(unit, directory, main, output_title, context["stats"])
    if "cue_text" in unit:
        row["cue_encoding"] = unit["cue_encoding"]


def plan_unit(root, unit, games, context, overrides, quick):
    """The plan row for one disc entry."""
    row = dict(
        source=unit["source"],
        status="unmatched",
        moves=[],
        metadata={},
        ignored_cues=unit.get("ignored_cues", []),
        aliases=unit.get("aliases", []),
    )
    if unit.get("error"):
        row["reason"] = unit["error"]
        if unit.get("invalid_data"):
            row["status"] = "blocked"
        return row

    registered = context["registrations"].get(unit["source"])
    if registered and registration_still_valid(root, registered["registration"], unit, quick):
        registered_row(row, registered, quick)
        return row

    if is_audio_only(unit):
        row.update(
            confidence="audio-only",
            reason="Audio-only CD; not a game disc. WAV/CUE sets can be prepared with "
            "prepare-audio-disc.py; preserve originals until hardware playback is tested.",
            category="Bonus Discs",
            category_review=False,
        )
        return row

    stats = file_stats(root, unit, quick, context["raw_sizes"], context["encoder"])
    game, confidence, reason, suggestions = identify(
        unit, games, stats, choice_for(overrides or {}, unit), quick
    )
    if any(f.get("mode1_raw") for f in stats.values()):
        if confidence in ("filename", "unmatched"):
            reason += "; reconstructed Mode 1 checksum did not match the catalog"
        elif confidence == "manual":
            reason += "; explicit choice takes precedence over checksum identification"
    if row["ignored_cues"]:
        reason += "; redundant ISO CUE will be archived if this entry is applied"
    row.update(confidence=confidence, reason=reason, suggestions=suggestions)

    if game is None:
        if confidence == "unverified-release" and not quick:
            row["reason"] += "; no complete disc checksum matched"
        return row
    plan_identified(row, unit, game, confidence, reason, dict(context, stats=stats))
    return row


def mark_collisions(root, entries):
    """Block entries whose move would land on an existing file or on another
    entry's destination (ignoring case)."""
    targets = collections.defaultdict(list)
    for row in entries:
        for move in row["moves"]:
            targets[move["target"].casefold()].append(row)
    for row in entries:
        for move in row["moves"]:
            try:
                target = relative_path(root, move["target"])
                shared = len(targets[move["target"].casefold()]) > 1
                occupied = target.exists() and move["source"] != move["target"]
                if shared or occupied:
                    raise ValueError(f'Destination collision: {move["target"]}')
            except ValueError as error:
                row.update(status="blocked", reason=str(error))


def note_duplicates(entries):
    """Point out entries whose data files are identical to another's.
    Nothing is deleted; the note only helps the reader tidy up."""
    duplicates = collections.defaultdict(list)
    for row in entries:
        data = [m for m in row["moves"] if Path(m["source"]).suffix.lower() != ".cue"]
        if data and all(m["fingerprint"].get("sha1") for m in data):
            key = tuple(sorted((m["fingerprint"]["size"], m["fingerprint"]["sha1"]) for m in data))
            duplicates[key].append(row)
    for group in duplicates.values():
        if len(group) <= 1:
            continue
        for row in group:
            row["duplicate_sources"] = [other["source"] for other in group if other is not row]
            row["reason"] += (
                "; identical data-file checksums also found at: "
                + ", ".join(row["duplicate_sources"])
                + "; CUE layouts are not compared; no duplicate is deleted automatically"
            )


def build_plan(root, catalog, quick=False, overrides=None, accept_names=False, categories=None):
    """Scan the drive and decide what to do with every disc entry.

    `catalog` comes from library-catalog.py's load_catalog. `overrides` maps
    disc paths to release names and `categories` maps them to category
    names. With `quick`, no checksums are read. With `accept_names`,
    filename matches are marked ready instead of needing review.

    Returns the plan: {"schema": 2, "root", "created", "quick", "sources",
    "entries": one row per disc entry, "categories": the drive's category
    names}. Each row has a "status": ready, review, registered,
    category-review, unmatched or blocked.
    """
    config = category_config(root)
    allowed = tuple(row["name"] for row in config["categories"])
    units = scan_units(root)
    categories = {} if categories is None else categories
    check_category_overrides(categories, units, allowed)
    saved, registrations = previous_choices(root, allowed)

    games = release_records(catalog["games"])
    # The catalog's full-sector track sizes, to spot .iso files worth rebuilding.
    raw_sizes = {
        int(r["size"])
        for g in games
        for r in g["roms"]
        if re.fullmatch(r"[0-9a-fA-F]{40}", r.get("sha1", ""))
    }
    context = dict(
        allowed=allowed,
        destinations=category_destinations(config),
        categories=categories,
        saved=saved,
        registrations=registrations,
        raw_sizes=raw_sizes,
        encoder=Mode1Encoder(),
        accept_names=accept_names,
        associations={
            folder: asset_associations(root, folder) for folder in ("covers", "previews")
        },
    )

    entries = []
    for number, unit in enumerate(units, 1):
        print(f'[{number}/{len(units)}] {unit["source"]}', flush=True)
        entries.append(plan_unit(root, unit, games, context, overrides, quick))

    mark_collisions(root, entries)
    if not quick:
        note_duplicates(entries)
    return dict(
        schema=2,
        root=str(root),
        created=time.time(),
        quick=quick,
        sources=catalog["sources"],
        entries=entries,
        categories=allowed,
    )


def json_bytes(value):
    """Pretty-printed UTF-8 JSON with a final newline."""
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def review_bucket(row):
    """Which tab of the report a plan row belongs to:
    matched, problems, decisions (needs a choice) or unidentified."""
    if row["status"] in ("ready", "review", "registered"):
        return "matched"
    if row["status"] == "blocked" or (row["status"] == "unmatched" and not row.get("confidence")):
        return "problems"
    if row["status"] == "category-review" or row.get("confidence") == "ambiguous":
        return "decisions"
    return "unidentified"


REPORT_TABS = (
    ("decisions", "Needs a choice"),
    ("unidentified", "Unidentified"),
    ("problems", "File problems"),
    ("matched", "Matched"),
)

REPORT_STYLE = """
body { font: 16px system-ui, sans-serif; background: #101b2d; color: #e1e9f3; margin: 2rem }
h1 { color: #3dd9ba }
input, select, button {
  padding: .6rem; margin: .3rem; font: inherit; background: #243c55; color: #e1e9f3;
  border: 1px solid #52718d; border-radius: .3rem
}
input { width: min(85%, 36rem) }
select { max-width: 30rem; width: 100%; margin: .5rem 0 }
button { cursor: pointer }
button[aria-pressed=true] { border-color: #3dd9ba; color: #3dd9ba }
table { border-collapse: collapse; width: 100%; background: #17283e }
th, td {
  padding: .8rem; text-align: left; border-bottom: 1px solid #344960; vertical-align: top;
  overflow-wrap: anywhere
}
th { background: #243c55 }
small, summary { color: #bacbdb }
.blocked td:first-child, .unmatched td:first-child { color: #ffb6a5 }
.ready td:first-child { color: #59e0bf }
.review td:first-child, .category-review td:first-child { color: #ffda85 }
strong { font-weight: 500; color: #9be7d9 }
@media (max-width: 700px) {
  body { margin: .6rem }
  table { font-size: 13px }
  th, td { padding: .4rem }
}
"""

REPORT_INTRODUCTION = """
<p><strong>Matched entries can be accepted as a batch.</strong> Filename matches are unverified,
but already identify a single game/release or an explicitly marked game-level entry.
Use <code>--accept-name-matches</code> on apply after checking a sample of their proposed paths.
Unidentified entries, unresolved choices and file problems stay in place; they do not block the matched batch.</p>
<p>No USB files changed. Suggestions are never selected automatically. Category choices and release
choices can be downloaded as JSON files below. Scan again with <code>--quick --overrides overrides.json --categories categories.json</code>
to build a new plan from those choices. Only include a flag if its file was downloaded.
Download choices before closing this page; changing this report does not edit the plan or drive.</p>
"""

# Shows one tab's rows (filtered by the search box), counts the choices made
# in the drop-downs, and downloads them as overrides.json / categories.json.
REPORT_SCRIPT = """
let view = 'decisions';
const rows = Array.from(document.querySelectorAll('tbody tr'));

function filter() {
  const query = document.getElementById('filter').value.toLowerCase();
  let count = 0;
  rows.forEach((row) => {
    row.hidden =
      (view !== 'all' && row.dataset.bucket !== view) || !row.textContent.toLowerCase().includes(query);
    if (!row.hidden) count++;
  });
  document
    .querySelectorAll('[data-view]')
    .forEach((button) => button.setAttribute('aria-pressed', String(button.dataset.view === view)));
  document.getElementById('visible').textContent =
    count + ' entries shown. Unresolved entries may be left in place.';
}

// {disc path: chosen value} for the "release" or "category" drop-downs.
function choices(kind) {
  return Object.fromEntries(
    rows.flatMap((row) => {
      const select = row.querySelector('select.' + kind);
      return select && select.value ? [[row.dataset.source, select.value]] : [];
    })
  );
}

function update() {
  document.getElementById('choices').textContent =
    ' ' + Object.keys(choices('release')).length + ' releases, ' +
    Object.keys(choices('category')).length + ' categories selected';
}

function download(kind, name) {
  const text = JSON.stringify(choices(kind), null, 2) + String.fromCharCode(10);
  const blob = new Blob([text], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = name;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

document.querySelectorAll('[data-view]').forEach((button) =>
  button.addEventListener('click', () => {
    view = button.dataset.view;
    filter();
  })
);
document.getElementById('filter').addEventListener('input', filter);
document.querySelectorAll('select').forEach((select) => select.addEventListener('change', update));
document.getElementById('export-releases').addEventListener('click', () => download('release', 'overrides.json'));
document.getElementById('export-categories').addEventListener('click', () => download('category', 'categories.json'));
filter();
update();
"""


def escape(value):
    return html.escape(str(value) if value is not None else "")


def choice_select(css_class, label, options):
    """A labeled drop-down whose first option leaves the disc in place."""
    items = "".join("<option>" + escape(option) + "</option>" for option in options)
    return (
        f'<label>{label} <select class="{css_class}"><option value="">Leave in place</option>'
        + items
        + "</select></label>"
    )


def report_row(row, categories):
    """One <tr> of the report: status, paths, category, metadata, and notes
    with a drop-down for any choice the person can make."""
    meta = row.get("metadata", {})
    suggestions = list(dict.fromkeys(row.get("suggestions", [])))
    notes = escape(row["reason"])
    if row.get("category_reason"):
        notes += "<br><small>" + escape(row["category_reason"]) + "</small>"

    controls = ""
    if row["status"] == "category-review":
        controls = choice_select("category", "Category", categories)
    elif suggestions and row["status"] == "unmatched" and row.get("confidence"):
        controls = choice_select("release", "Release", suggestions)
    if suggestions:
        notes += (
            "<details><summary>Catalog suggestions (not selected)</summary><ul>"
            + "".join("<li>" + escape(name) + "</li>" for name in suggestions)
            + "</ul></details>"
        )

    if row.get("category"):
        category = row["category"]
    elif row["status"] == "category-review":
        category = "Needs classification"
    else:
        category = "Unknown"
    cells = [
        escape(row["status"]) + "<br><small>" + escape(row.get("confidence", "")) + "</small>",
        escape(row["source"])
        + "<br><strong>"
        + escape(row.get("target") or "Stays in place")
        + "</strong>",
        escape(category),
        "<br>".join(escape(meta.get(field)) for field in ("genre", "publisher", "year")),
        notes + controls,
    ]
    return (
        f'<tr hidden data-bucket="{review_bucket(row)}" data-source="{escape(row["source"])}"'
        f' class="{escape(row["status"])}">'
        + "".join("<td>" + cell + "</td>" for cell in cells)
        + "</tr>\n"
    )


def report_html(plan):
    """The plan's interactive HTML report: tabs grouping the entries, a
    search box, and drop-downs to choose releases and categories, which
    download as files to feed back into a new scan."""
    counts = collections.Counter(review_bucket(r) for r in plan["entries"])
    tabs = "".join(
        f'<button type="button" data-view="{key}">{label} ({counts[key]})</button>'
        for key, label in REPORT_TABS
    )
    categories = plan.get("categories", CATEGORIES)
    rows = "".join(report_row(row, categories) for row in plan["entries"])
    page = (
        '<!doctype html>\n<html lang="en">\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width">\n'
        "<title>Oh My 3DO — Library preview</title>\n"
        "<style>" + REPORT_STYLE + "</style>\n"
        "<h1>Library preview</h1>\n"
        "<p>USB root: "
        + escape(plan["root"])
        + "</p>\n"
        + REPORT_INTRODUCTION
        + '<nav aria-label="Review groups">'
        + tabs
        + '<button type="button" data-view="all">All</button></nav>\n'
        '<input id="filter" type="search" placeholder="Filter this group by title, category, '
        'region or publisher" aria-label="Filter library">\n'
        '<p id="visible" aria-live="polite"></p>\n'
        '<button id="export-releases" type="button">Download release choices</button>\n'
        '<button id="export-categories" type="button">Download category choices</button>\n'
        '<span id="choices" aria-live="polite"></span>\n'
        "<noscript><p>Enable JavaScript for filtering and choice downloads.</p>"
        "<style>tr[hidden]{display:table-row}</style></noscript>\n"
        "<table>\n<thead><tr><th>Status / match</th><th>Original → proposed path</th>"
        "<th>Category</th><th>Genre / publisher / year</th><th>Notes / choice</th></tr></thead>\n"
        "<tbody>\n" + rows + "</tbody>\n</table>\n"
        "<script>" + REPORT_SCRIPT + "</script>\n</html>\n"
    )
    return page.encode("utf-8")


def atomic_write(path, data):
    """Write `data` to `path` through a temporary file flushed to disk, so the
    file is always either the old or the new contents."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".oh3do-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)


def sha256_of_file(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(MIB), b""):
            checksum.update(block)
    return checksum.hexdigest()


class Transaction:
    """An undo journal for changes to a drive, kept as JSON under
    .ohmy3do/transactions/.

    Every change is recorded (and saved) before it is made, so even an
    interrupted run can be undone:
      moves         files moved, with their fingerprints
      writes        support files written, with their previous contents
      directories   folders created
      folder_moves  whole folders renamed (library-maintenance.py)
      copies        files copied onto the drive (library-maintenance.py)
    The journal's "state" is applying, complete or undone.
    """

    def __init__(self, root, plan=None, path=None):
        """Start a new journal for `root`, or with `path` open an existing one."""
        self.root = root
        if path is not None:
            self.path = relative_path(root, path)
            self.data = json.loads(self.path.read_text(encoding="utf-8"))
            if self.data.get("root") != str(root) or self.data.get("schema") != 1:
                raise ValueError("Transaction belongs to a different USB root or version")
        else:
            self.path = relative_path(root, ".ohmy3do/transactions/" + uuid.uuid4().hex + ".json")
            self.data = dict(
                schema=1,
                root=str(root),
                state="applying",
                moves=[],
                writes=[],
                directories=[],
                plan=plan,
            )
            self.save()

    def save(self):
        atomic_write(self.path, json_bytes(self.data))

    def parents(self, path):
        """Create the missing folders above `path`, recording each first."""
        missing = []
        parent = path.parent
        while parent != self.root and not parent.exists():
            missing.append(parent)
            parent = parent.parent
        for parent in reversed(missing):
            relative = parent.relative_to(self.root).as_posix()
            self.data["directories"].append(relative)
            self.save()
            parent.mkdir(exist_ok=True)

    def move(self, operation):
        """Move a file as a plan's move describes ({source, target, fingerprint})."""
        if operation["source"] == operation["target"]:
            return
        source = relative_path(self.root, operation["source"])
        target = relative_path(self.root, operation["target"])
        if target.exists():
            raise ValueError(f"Refusing to overwrite: {target}")
        self.parents(target)
        entry = dict(operation, state="moving")
        self.data["moves"].append(entry)
        self.save()
        # Reserve the destination exclusively, then replace only our empty file.
        # This works on FAT, NTFS and POSIX without copying a whole disc image.
        with target.open("xb"):
            pass
        os.replace(source, target)
        entry["state"] = "done"
        self.save()

    def move_folder(self, source_name, target_name):
        source = relative_path(self.root, source_name)
        target = relative_path(self.root, target_name)
        if not source.is_dir() or target.exists():
            raise ValueError("Missing source or occupied folder: " + target_name)
        self.parents(target)
        entry = dict(source=source_name, target=target_name)
        self.data.setdefault("folder_moves", []).append(entry)
        self.save()
        source.rename(target)

    def write(self, relative, content):
        """Replace a support file, journaling its previous contents (and the
        new contents' hash) first. Unchanged contents are not rewritten."""
        path = relative_path(self.root, relative)
        before = path.read_bytes() if path.exists() else None
        if before == content:
            return
        if before is not None and len(before) > 16 * MIB:
            raise ValueError(f"Refusing to replace oversized support file: {relative}")
        entry = dict(
            path=relative,
            before=base64.b64encode(before).decode() if before is not None else None,
            after=hashlib.sha256(content).hexdigest(),
            state="writing",
        )
        self.data["writes"].append(entry)
        self.save()
        self.parents(path)
        atomic_write(path, content)
        entry["state"] = "done"
        self.save()

    # Undo. Every check runs before anything is restored, so an undo either
    # happens completely or not at all.

    def _check_folder_moves(self):
        for entry in self.data.get("folder_moves", []):
            source = relative_path(self.root, entry["source"])
            target = relative_path(self.root, entry["target"])
            if source.exists() and target.exists():
                raise ValueError("Cannot undo: original folder is occupied: " + entry["source"])
            if not source.exists() and not target.is_dir():
                raise ValueError("Cannot undo: moved folder is missing: " + entry["target"])

    def _check_copies(self):
        for entry in self.data.get("copies", []):
            path = relative_path(self.root, entry["path"])
            if path.exists() and sha256_of_file(path) != entry["sha256"]:
                raise ValueError("Cannot undo: copied file changed: " + entry["path"])

    def _check_removed_directories(self):
        for name in self.data.get("removed_directories", []):
            path = relative_path(self.root, name)
            if path.exists() and not path.is_dir():
                raise ValueError("Cannot undo: original directory is occupied by a file: " + name)

    def _check_writes(self):
        """Every written file must still hold what was written (or already be
        back to what it was). Modified files are never overwritten, including
        covers or favorites edited after apply."""
        restored = {}
        for entry in reversed(self.data["writes"]):
            path = relative_path(self.root, entry["path"])
            before = base64.b64decode(entry["before"]) if entry["before"] is not None else None
            if path not in restored:
                restored[path] = path.read_bytes() if path.exists() else None
            current = restored[path]
            if current != before and (
                current is None or hashlib.sha256(current).hexdigest() != entry["after"]
            ):
                raise ValueError(
                    f'Cannot undo: support file changed after organizing: {entry["path"]}'
                )
            # A support index can be regenerated more than once during apply.
            # Validate each earlier write against the bytes the later undo will
            # restore, without changing any files during preflight.
            restored[path] = before

    def _check_moves(self):
        """Every moved file must be in one place, unchanged since it moved."""
        first_writes = {}
        for entry in self.data["writes"]:
            first_writes.setdefault(entry["path"], entry)

        for entry in reversed(self.data["moves"]):
            source = relative_path(self.root, entry["source"])
            target = relative_path(self.root, entry["target"])
            if source.exists():
                # An interrupted move may have left an empty placeholder at the target.
                if target.exists() and (entry["state"] == "done" or target.stat().st_size):
                    raise ValueError(f"Cannot undo: original path is occupied: {source}")
                actual = source
            elif not target.is_file():
                raise ValueError(f"Cannot undo: moved file is missing: {target}")
            else:
                actual = target

            # A renamed CUE may also have updated FILE directives. Its original
            # bytes are in the write journal and are restored before moving it.
            write = first_writes.get(entry["target"]) if actual == target else None
            if write and write["before"] is not None:
                original = base64.b64decode(write["before"])
            else:
                original = None
            size = len(original) if original is not None else actual.stat().st_size
            sha = hashlib.sha1(original).hexdigest() if original is not None else None

            if size != entry["fingerprint"]["size"]:
                raise ValueError(f"Cannot undo: moved file size changed: {actual}")
            if entry["fingerprint"].get("sha1"):
                if (sha or digest(actual)) != entry["fingerprint"]["sha1"]:
                    raise ValueError(f"Cannot undo: moved file contents changed: {actual}")
            elif actual.stat().st_mtime_ns != entry["fingerprint"]["mtime_ns"]:
                raise ValueError(f"Cannot undo: moved file changed: {actual}")

    def _restore(self):
        """Reverse every change, newest first."""
        for entry in reversed(self.data.get("copies", [])):
            relative_path(self.root, entry["path"]).unlink(missing_ok=True)

        for entry in reversed(self.data["writes"]):
            path = relative_path(self.root, entry["path"])
            if entry["before"] is None:
                path.unlink(missing_ok=True)
            else:
                atomic_write(path, base64.b64decode(entry["before"]))

        for entry in reversed(self.data["moves"]):
            source = relative_path(self.root, entry["source"])
            target = relative_path(self.root, entry["target"])
            if not source.exists():
                source.parent.mkdir(parents=True, exist_ok=True)
                os.replace(target, source)
            elif target.exists() and entry["state"] == "moving" and not target.stat().st_size:
                target.unlink()  # the empty placeholder of an interrupted move

        for entry in reversed(self.data.get("folder_moves", [])):
            source = relative_path(self.root, entry["source"])
            target = relative_path(self.root, entry["target"])
            if not source.exists():
                source.parent.mkdir(parents=True, exist_ok=True)
                target.rename(source)

        for name in self.data.get("removed_directories", []):
            relative_path(self.root, name).mkdir(parents=True, exist_ok=True)

        # Remove the folders this journal created, if they are empty now.
        for directory in reversed(self.data["directories"]):
            try:
                relative_path(self.root, directory).rmdir()
            except OSError:
                pass

    def undo(self):
        """Restore the drive to how it was before this journal's changes.

        Raises ValueError, changing nothing, if anything the journal touched
        has changed since.
        """
        if self.data["state"] == "undone":
            raise ValueError("Transaction is already undone")
        self._check_folder_moves()
        self._check_copies()
        self._check_removed_directories()
        self._check_writes()
        self._check_moves()
        self._restore()
        self.data["state"] = "undone"
        self.save()


MAX_MENU_LIST_BYTES = 8192
MAX_POSITION_BYTES = 9216
POSITION_HEADER = "OH3DO_POSITION1"
POSITION_AT_ROOT = b"OH3DO_POSITION1\n/Storage\n\n"


def remap_list(path, folded):
    """favorites.txt or recent.txt with each line naming a moved disc
    rewritten to its new path. Comments, unknown paths and the line's
    indentation and line ending survive."""
    raw = path.read_bytes()
    text = raw.split(b"\0", 1)[0].decode("utf-8-sig")
    lines = []
    for line in text.splitlines(keepends=True):
        # Favorites are volume-relative; comments and unknown paths survive.
        key = line.strip()
        replacement = folded.get(index_tools.fold(key))
        if replacement:
            begin = len(line) - len(line.lstrip())
            line = line[:begin] + replacement + line[begin + len(key) :]
        lines.append(line)
    encoding = "utf-8-sig" if raw.startswith(b"\xef\xbb\xbf") else "utf-8"
    return "".join(lines).encode(encoding)


def check_position(raw, lines):
    """Raise ValueError unless last-position.txt is: the header, a folder
    under /Storage, and the highlighted entry's name, all within the menu's
    path limits."""
    if len(raw) >= MAX_POSITION_BYTES or len(lines) != 3 or lines[0] != POSITION_HEADER:
        raise ValueError("Invalid saved-position header or length")
    if lines[1] != "/Storage" and not lines[1].startswith("/Storage/"):
        raise ValueError("Invalid saved-position root")
    parts = lines[1][9:].split("/") if lines[1] != "/Storage" else []
    if lines[2]:
        parts.append(lines[2])
    if len(lines[1].encode()) > 8192 or any(
        not part
        or len(part.encode()) >= 1024
        or part in (".", "..")
        or any(ord(c) < 32 or ord(c) == 127 or c in "/\\:" for c in part)
        for part in parts
    ):
        raise ValueError("Invalid saved-position path")


def remap_position(raw, mapping, folded):
    """New last-position.txt contents for moved discs, or None to keep it.

    The menu remembers a folder and the entry highlighted in it. If that
    entry moved, the position follows it; if it was inside a moved folder,
    the position resets to the drive's root.
    """
    lines = raw.decode("utf-8-sig").splitlines()
    folder = lines[1][9:] if lines[1].startswith("/Storage/") else ""
    current = (folder.rstrip("/") + "/" if folder else "") + lines[2]
    target = folded.get(index_tools.fold(current))
    if target:
        return (
            POSITION_HEADER
            + "\n/Storage/"
            + str(PurePosixPath(target).parent)
            + "\n"
            + Path(target).name
            + "\n"
        ).encode()
    if any(
        old.startswith(current.rstrip("/") + "/") or (folder and old.startswith(folder + "/"))
        for old in mapping
    ):
        return POSITION_AT_ROOT
    return None


def prepare_text_remaps(root, mapping):
    """Parse menu text before moving discs; firmware text ends at its first NUL.

    `mapping` is {old disc path: new path}. Returns {support file: new
    contents} for favorites.txt, recent.txt and last-position.txt.
    """
    updates = {}
    folded = {index_tools.fold(k): v for k, v in mapping.items()}
    for name in ("favorites.txt", "recent.txt"):
        path = relative_path(root, support_path(root, name))
        if not path.exists():
            continue
        updated = remap_list(path, folded)
        if len(updated) >= MAX_MENU_LIST_BYTES:
            raise ValueError(f"Renamed paths exceed the menu limit for {name}")
        updates[support_path(root, name)] = updated

    position_name = support_path(root, "last-position.txt")
    position = relative_path(root, position_name)
    if not position.exists():
        return updates
    raw = position.read_bytes().split(b"\0", 1)[0]
    try:
        check_position(raw, raw.decode("utf-8-sig").splitlines())
    except (UnicodeError, ValueError):
        print(
            "Warning: invalid last-position.txt; resetting the saved menu position to the USB "
            "root. Undo restores the original file."
        )
        updates[position_name] = POSITION_AT_ROOT
        return updates
    new_position = remap_position(raw, mapping, folded)
    if new_position is not None:
        updates[position_name] = new_position
    return updates


def media_index(mapping):
    """A covers or previews index.3di (see index-media.py) from {disc path:
    media file name}, sorted by folded path."""
    rows = []
    for key, name in sorted(mapping.items(), key=lambda pair: index_tools.fold(pair[0])):
        key = key.encode() + b"\0"
        name = name.encode() + b"\0"
        rows.append(struct.pack(">HH", len(key), len(name)) + key + name)
    data = b"".join(rows)
    if len(rows) > 2048 or len(data) > 128 * 1024:
        raise ValueError("Media index exceeds menu capacity")
    header = struct.pack(">IIII", len(rows), len(data), binascii.crc32(data), 0)
    return b"OM3DIDX1" + header + data


VIDEO_EXTENSIONS = (".mp4", ".avi", ".mkv", ".mov", ".webm")


def artwork_key(value):
    """A box-art name for comparison: no space before "[" and case folded."""
    return re.sub(r"\s+(?=\[)", "", value).casefold()


def matching_box_art(names, row):
    """libretro box-art names for a plan row: by exact release name, then
    catalog name, then title within the same region."""
    exact = [n for n in names if artwork_key(Path(n).stem) == artwork_key(row["canonical"])]
    if not exact and row.get("catalog_name"):
        exact = [n for n in names if Path(n).stem.casefold() == row["catalog_name"].casefold()]
    if not exact:
        exact = [
            n
            for n in names
            if catalog_tools.title_key(Path(n).stem) == catalog_tools.title_key(row["group"])
            and row["region"] in region_of(Path(n).stem)
        ]
    return exact


def prepare_covers(rows, cache, offline=False):
    """Download and convert libretro box art for rows without a cover.

    Returns ({cover file name: contents}, warnings); each cover comes with
    its full-screen "-large" version. Fills in the rows' "cover" and
    "cover_source". Conversions are cached in `cache`.
    """
    if not rows:
        return {}, []
    try:
        names = catalog_tools.cover_names(cache, offline)
        resizer = module("resize-covers")
        from PIL import Image  # noqa: F401
    except (OSError, ValueError, ImportError) as error:
        return {}, [f"Covers skipped: {error}. Install Pillow with: py -m pip install Pillow"]

    prepared = {}
    warnings = []
    for row in rows:
        if row.get("cover"):
            continue
        canonical = row["canonical"]
        exact = matching_box_art(names, row)
        if not exact:
            warnings.append(f"No cover found: {canonical}")
            continue
        # Disc-specific scans may differ; only select a unique image or disc 1.
        first = [n for n in exact if disc_of(n) == 1]
        if len(exact) == 1:
            chosen = exact[0]
        elif len(first) == 1:
            chosen = first[0]
        else:
            warnings.append(f"Ambiguous cover: {canonical}")
            continue

        key = hashlib.sha1(chosen.encode()).hexdigest()
        output = cache / "prepared" / (key + "-fast96.3dc")
        large = cache / "prepared" / (key + "-fast192.3dc")
        try:
            original = catalog_tools.download(
                catalog_tools.cover_url(chosen),
                cache / "artwork" / (key + ".png"),
                16 * MIB,
                offline,
            )
            if not output.exists():
                resizer.fast_convert(original, output)
            if not large.exists():
                resizer.fast_convert(original, large, resizer.LARGE_SIZE)
            basename = safe_name(row["group"], 65) + "-" + key[:8] + ".3dc"
            row["cover"] = basename
            row["cover_source"] = catalog_tools.cover_url(chosen)
            prepared[basename] = output.read_bytes()
            prepared[resizer.large_name(basename)] = large.read_bytes()
        except (ValueError, OSError) as error:
            warnings.append(f"Cover failed for {canonical}: {error}")
    return prepared, warnings


def local_media_files(directory, extensions):
    """Every file with one of `extensions` in a folder and its subfolders,
    sorted, never following links."""
    if not directory.is_dir() or directory.is_symlink():
        raise ValueError("Media directory must be a real, existing directory")
    directory = directory.resolve()
    paths = []
    for folder, folders, files in os.walk(directory, followlinks=False):
        folders[:] = sorted(d for d in folders if not (Path(folder) / d).is_symlink())
        for name in sorted(files):
            path = Path(folder) / name
            if path.suffix.lower() in extensions and not path.is_symlink() and path.is_file():
                paths.append(path)
    return paths


def media_kind(name):
    """demo, demonstration, trial, sampler, or game."""
    match = re.search(r"\b(demo|demonstration|trial|sampler)\b", name, re.I)
    return match[1].casefold() if match else "game"


def local_media_matches(row, paths, *, other_regions=False):
    """Prefer exact/compatible names; previews may fall back across regions.

    A file named exactly like the release, catalog game or disc file wins.
    Otherwise files with the same title, kind and disc number match, limited
    to compatible regions unless `other_regions` allows any when none fit.
    """
    names = (row["canonical"], row.get("catalog_name"), Path(row["source"]).stem)
    for name in filter(None, names):
        exact = [p for p in paths if p.stem.casefold() == name.casefold()]
        if exact:
            return exact
    # Demo/sampler media must not become full-game art via title_key.
    candidates = [
        p
        for p in paths
        if identification_title(p.stem) == identification_title(row["canonical"])
        and media_kind(p.stem) == media_kind(row["canonical"])
        and (disc_of(p.stem) is None or disc_of(p.stem) == row.get("disc"))
    ]
    compatible = [
        p
        for p in candidates
        if not region_of(p.stem) or bool(region_of(p.stem) & region_of(row["canonical"]))
    ]
    return candidates if other_regions and not compatible else compatible


def prepare_local_covers(rows, directory):
    """Prepare local front-box images, with no network requests or replacements.

    Returns ({cover file name: contents}, warnings), like prepare_covers.
    """
    resizer = module("resize-covers")
    paths = local_media_files(directory, resizer.EXTENSIONS)
    directory = directory.resolve()
    prepared = {}
    warnings = []
    converted = {}  # image -> (name, cover, large cover), so each converts once
    for row in rows:
        if row.get("cover"):
            continue
        candidates = local_media_matches(row, paths)
        if len(candidates) != 1:
            found = "Ambiguous" if candidates else "No"
            warnings.append(f'{found} local cover found: {row["canonical"]}')
            continue
        image = candidates[0]
        try:
            if image not in converted:
                with tempfile.TemporaryDirectory(prefix="oh3do-cover-") as temp:
                    target = Path(temp) / "cover.3dc"
                    large = Path(temp) / "large.3dc"
                    resizer.fast_convert(image, target)
                    resizer.fast_convert(image, large, resizer.LARGE_SIZE)
                    content = target.read_bytes()
                    large_content = large.read_bytes()
                key = hashlib.sha256(content).hexdigest()[:16]
                converted[image] = ("cover-" + key + ".3dc", content, large_content)
            basename, content, large_content = converted[image]
            prepared[basename] = content
            prepared[resizer.large_name(basename)] = large_content
            row["cover"] = basename
            row["cover_source"] = image.relative_to(directory).as_posix()
        except (ValueError, OSError) as error:
            warnings.append(f'Local cover failed for {row["canonical"]}: {error}')
    return prepared, warnings


def prepare_previews(rows, directory, start=0, duration=None, sound=True, replace=False):
    """Import local, already downloaded clips; ambiguous matches stay untouched.

    Converts each matching clip to the menu's 160x120 preview format.
    Returns ({preview file name: contents}, warnings). With `replace`, rows
    that already have a preview get a new one (the old file is kept).
    """
    clips = local_media_files(directory, VIDEO_EXTENSIONS)
    if not 0 <= start <= 86400:
        raise ValueError("Preview start must be 0-86400 seconds")
    directory = directory.resolve()
    converter = module("prepare-preview")
    prepared = {}
    warnings = []
    converted = {}  # clip -> (key, contents)
    for row in rows:
        if row.get("preview") and not replace:
            continue
        candidates = local_media_matches(row, clips, other_regions=True)
        if len(candidates) != 1:
            found = "Ambiguous" if candidates else "No"
            warnings.append(f'{found} preview found: {row["canonical"]}')
            continue
        clip = candidates[0]
        try:
            if clip not in converted:
                # Conversion happens on the PC before any USB changes. Include
                # content and settings in the name so reruns never replace art.
                settings = (
                    f':{start}:15:{duration or "15"}:{int(sound)}:5:160x120x30:'
                    f"{converter.V5_BYTES//1024}k:fixed-fps:cap15"
                )
                key = hashlib.sha256((digest(clip) + settings).encode()).hexdigest()[:16]
                with tempfile.TemporaryDirectory(prefix="oh3do-preview-") as temp:
                    target = Path(temp) / "clip.3dv"
                    converter.convert(clip, target, start=start, duration=duration, sound=sound)
                    converted[clip] = (key, target.read_bytes())
            key, content = converted[clip]
            basename = "clip-" + key + ".3dv"
            prepared[basename] = content
            row["preview"] = basename
            row["preview_source"] = clip.relative_to(directory).as_posix()
        except (ValueError, OSError, subprocess.CalledProcessError) as error:
            warnings.append(f'Preview failed for {row["canonical"]}: {error}')
    return prepared, warnings


GAMES_TABLE = (
    "CREATE TABLE games (path TEXT PRIMARY KEY, title TEXT, region TEXT, disc INTEGER, "
    "genre TEXT, publisher TEXT, year INTEGER, developer TEXT, identification TEXT, "
    "cover TEXT, metadata_source TEXT, variant TEXT, catalog_name TEXT, "
    "edition_unresolved INTEGER, category TEXT, category_review INTEGER, category_reason TEXT)"
)


def database_record(row, path, organized):
    """The library.json record for a plan row whose disc is now at `path`."""
    record = dict(
        path=path,
        title=row.get("canonical"),
        region=row.get("region"),
        category=row.get("category"),
        category_review=row.get("category_review", True),
        category_reason=row.get("category_reason"),
        catalog_name=row.get("catalog_name"),
        variant=row.get("variant"),
        edition_unresolved=row.get("edition_unresolved", False),
        disc=row.get("disc"),
        identification=row.get("confidence", "unmatched"),
        status="organized" if organized else row["status"],
        metadata=row.get("metadata", {}),
        cover=row.get("cover"),
        cover_source=row.get("cover_source"),
        preview=row.get("preview"),
        preview_source=row.get("preview_source"),
        suggestions=row.get("suggestions", []),
    )
    if row.get("registration"):
        record["registration"] = row["registration"]
    return record


def sqlite_database(records, sources):
    """library.sqlite's bytes: a games table (one row per record) and a
    sources table (the catalog downloads used), for other tools to query."""
    with tempfile.TemporaryDirectory(prefix="oh3do-db-") as directory:
        path = Path(directory) / "library.sqlite"
        # SQLite's transaction context commits/rolls back, but does not close
        # the connection. Close it before copying/removing the file on Windows.
        with closing(sqlite3.connect(path)) as db, db:
            db.execute(GAMES_TABLE)
            db.execute("CREATE TABLE sources (url TEXT, sha256 TEXT)")
            db.executemany(
                "INSERT INTO sources VALUES (?, ?)",
                [(s["url"], s.get("sha256")) for s in sources],
            )
            for record in records:
                meta = record["metadata"]
                db.execute(
                    "INSERT INTO games VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (
                        record["path"],
                        record["title"],
                        record["region"],
                        record["disc"],
                        meta.get("genre"),
                        meta.get("publisher"),
                        meta.get("year"),
                        meta.get("developer"),
                        record["identification"],
                        record["cover"],
                        meta.get("source"),
                        record.get("variant"),
                        record.get("catalog_name"),
                        int(bool(record.get("edition_unresolved", False))),
                        record.get("category"),
                        int(bool(record.get("category_review", True))),
                        record.get("category_reason"),
                    ),
                )
        return path.read_bytes()


def write_database(root, tx, plan, selected):
    """Update the library database and the files derived from it.

    Records for the plan's entries replace earlier ones (under each disc's
    new path when it is in `selected`, the rows being organized); records
    for discs no longer on the drive are dropped. Writes, through the
    journal `tx`: library.json, save-names.txt and its cache, metadata.3di
    and library.sqlite.
    """
    previous = relative_path(root, ".ohmy3do/library.json")
    if previous.exists():
        existing = json.loads(previous.read_text(encoding="utf-8")).get("games", [])
    else:
        existing = []
    mapping = {r["source"]: r["target"] for r in selected}
    records = {r["path"]: r for r in existing}
    for row in plan["entries"]:
        path = mapping.get(row["source"], row["source"])
        records.pop(row["source"], None)
        records[path] = database_record(row, path, row in selected)
    # Do not retain stale records for discs no longer present.
    records = [r for r in records.values() if relative_path(root, r["path"]).is_file()]

    data = dict(
        schema=1,
        generated=time.time(),
        sources=plan["sources"],
        games=sorted(records, key=lambda r: r["path"]),
    )
    tx.write(".ohmy3do/library.json", json_bytes(data))

    games = [(index_tools.fold(r["path"]), r["path"], "") for r in records]
    titles = {index_tools.fold(r["path"]): r.get("title") or "" for r in records}
    names, count, cache = index_tools.save_names_text(
        root, index_tools.support_base(root), games, titles, write_cache=False
    )
    tx.write(".ohmy3do/save-names-cache.json", cache)
    tx.write(".ohmy3do/save-names.txt", names.encode("ascii"))
    print(f"Indexed {count} save names for the Save manager")

    disc_paths = [r["path"] for r in records if Path(r["path"]).suffix.lower() in (".iso", ".cue")]
    tx.write(".ohmy3do/metadata.3di", index_tools.metadata_index(records, disc_paths))
    tx.write(".ohmy3do/library.sqlite", sqlite_database(records, plan["sources"]))


def build_mirror(root, mirror, media_outputs):
    """A tiny stand-in for the drive in `mirror`, holding just what
    index-media.py reads: each disc's first 16 bytes (or an empty CUE), the
    first 24 bytes of each media file, and the new media indexes."""
    for unit in scan_units(root):
        if unit.get("invalid_data"):
            continue
        for file in unit["files"][:1]:
            path = mirror / file
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.suffix.lower() == ".iso":
                # Preserve the validation prefix without copying images.
                with relative_path(root, file).open("rb") as stream:
                    path.write_bytes(stream.read(16))
            else:
                path.touch()
    # CUE sets were validated by scan_units; no CUE parsing is needed here.
    for folder in ("covers", "previews"):
        usb = index_tools.support_base(root) / folder
        if not usb.exists():
            continue
        target = index_tools.support_base(mirror) / folder
        target.mkdir(parents=True, exist_ok=True)
        for file in usb.iterdir():
            if (
                file.is_file()
                and not file.is_symlink()
                and file.suffix.lower() in (".png", ".3dc", ".3dv", ".3di")
                and file.name != "index.3di"
            ):
                # Indexing reads PNG headers and index bytes only.
                with file.open("rb") as stream:
                    content = stream.read(24)
                (target / file.name).write_bytes(content)
        (target / "index.3di").write_bytes(media_outputs[folder])


def write_indexes(root, tx, media_outputs=None):
    """Build in a PC mirror; journal index writes without altering media files.

    Runs index-media.py on a mirror of the drive (see build_mirror) and
    writes the resulting library.3di and media indexes through `tx`.
    `media_outputs` are new covers/previews index.3di contents; by default
    the drive's current ones are kept.
    """
    if media_outputs is None:
        media_outputs = {
            folder: media_index(index_tools.existing_index(index_tools.support_base(root) / folder))
            for folder in ("covers", "previews")
        }
    with tempfile.TemporaryDirectory(prefix="oh3do-index-") as directory:
        mirror = Path(directory)
        build_mirror(root, mirror, media_outputs)
        index_tools.index(mirror, include_save_names=False)
        for name in ("library.3di", "covers/index.3di", "previews/index.3di"):
            located = support_path(root, name)
            if (mirror / located).exists():
                tx.write(located, (mirror / located).read_bytes())


def check_register_title(title):
    if (
        not title.strip()
        or len(title.encode("utf-8")) >= 1024
        or any(ord(c) < 32 or ord(c) == 127 for c in title)
    ):
        raise ValueError(
            "Use a nonempty title shorter than 1024 UTF-8 bytes, without control characters"
        )


def register_disc(root, source, title, category, region="Unknown", apply=False):
    """Register an existing uncatalogued disc without moving it or inventing a release.

    The disc keeps its place and gets a library record with the given
    title, category and region, marked "unverified". Its files' fingerprints
    are recorded so a later scan can tell whether it changed. Without
    `apply`, only describes what would happen. Returns the plan row.
    """
    relative_path(root, source)
    category, category_reason = classify({}, category, category_names(root))
    check_register_title(title)
    if region != "Unknown":
        region = REGIONS.get(region.casefold())
        if not region:
            raise ValueError("Use a known region name or Unknown")

    units = [u for u in scan_units(root) if source in [u["source"]] + u.get("aliases", [])]
    if len(units) != 1 or units[0].get("error"):
        raise ValueError(
            "Choose one existing, valid ISO/CUE disc entry; "
            + (units[0].get("error", "") if units else "entry not found")
        )
    unit = units[0]
    if Path(unit["source"]).suffix.lower() == ".iso":
        path = relative_path(root, unit["source"])
        with path.open("rb") as stream:
            header = stream.read(7)
        if path.stat().st_size % 2048 or header != b"\x01ZZZZZ\x01":
            raise ValueError("ISO must have a 3DO volume header and whole 2048-byte sectors")

    previous = root / ".ohmy3do/library.json"
    if previous.exists():
        database = json.loads(previous.read_text(encoding="utf-8"))
    else:
        database = {"sources": [], "games": []}
    existing = next((r for r in database["games"] if r["path"] == unit["source"]), {})
    if existing.get("identification") in ("checksum", "converted-checksum"):
        raise ValueError(
            "Disc already has a verified catalog identity; use index-media.py to refresh Find"
        )

    certificate = {name: fingerprint(relative_path(root, name)) for name in unit["files"]}
    row = dict(
        source=unit["source"],
        target=unit["source"],
        canonical=title.strip(),
        region=region,
        category=category,
        category_reason=category_reason,
        category_review=False,
        metadata={},
        status="registered",
        confidence="unverified",
        registration=certificate,
        moves=[],
        reason="Explicit registration; catalog identity and bootability are not certified",
    )
    for folder, key in (("covers", "cover"), ("previews", "preview")):
        row[key] = row_asset(asset_associations(root, folder), row)
    print(
        f'{"Register" if apply else "Would register"}: {unit["source"]}\n'
        f"Title: {title.strip()}\n"
        f"Category: {category}\n"
        "Catalog identity: unverified; ISO/CUE and data files stay in place."
    )
    if not apply:
        print("No changes made. Add --apply to update the database and Find.")
        return row

    tx = Transaction(root)
    print(f"Undo journal: {tx.path}", flush=True)
    try:
        write_indexes(root, tx)
        for name, expected in certificate.items():
            if fingerprint(relative_path(root, name)) != expected:
                raise ValueError("Disc changed during registration: " + name)
        write_database(root, tx, dict(entries=[row], sources=database["sources"]), [])
        tx.data["state"] = "complete"
        tx.save()
    except BaseException:
        tx.undo()
        raise
    print("Registered in the library and Find. No disc files moved or modified.")
    return row


VERIFIED_CONFIDENCE = ("checksum", "converted-checksum", "manual")


def check_registrations(root, plan):
    """Every registered disc in the plan must be unchanged since the scan."""
    for row in plan["entries"]:
        if row.get("registration"):
            for name, expected in row["registration"].items():
                if fingerprint(relative_path(root, name)) != expected:
                    raise ValueError("Registered disc changed since scan: " + name)


def check_row(root, row, accept_names, sources, destinations):
    """A row to organize must have a settled category matching its target,
    a verified identity (or `accept_names`), a well-formed move group, and
    files unchanged since the scan whose destinations are free. `sources`
    and `destinations` collect folded paths to catch duplicates."""
    if (
        row.get("category_review")
        or row.get("category") not in category_names(root)
        or not isinstance(row.get("target"), str)
        or PurePosixPath(row["target"]).parts[0] != row["category"]
    ):
        raise ValueError(
            "Category is unresolved or does not match the destination; "
            "scan again with a category override"
        )
    if row["confidence"] not in VERIFIED_CONFIDENCE and not accept_names:
        raise ValueError(
            "Filename and partial data-track matches need --accept-name-matches "
            "after reviewing the plan"
        )
    relative_path(root, row["source"])
    if (
        not row.get("moves")
        or row["moves"][0]["source"] != row["source"]
        or row["moves"][0]["target"] != row["target"]
    ):
        raise ValueError("Invalid move group in plan")

    for move in row["moves"]:
        source = relative_path(root, move["source"])
        target = relative_path(root, move["target"])
        if move["source"].casefold() in sources or move["target"].casefold() in destinations:
            raise ValueError("Duplicate source or destination in plan")
        sources.add(move["source"].casefold())
        destinations.add(move["target"].casefold())
        actual = fingerprint(source, False)
        if source.suffix.lower() != ".cue":
            issue = index_tools.disc_data_issue(source)
            if issue:
                raise ValueError(issue + "; no files were reorganized")
        if any(actual[k] != move["fingerprint"][k] for k in actual):
            raise ValueError(f"Source changed since scan: {source}; scan again")
        if target.exists() and move["source"] != move["target"]:
            raise ValueError(f"Destination already exists: {target}")


def rewritten_cue(root, row):
    """A moving CUE sheet's text with its FILE lines naming the renamed
    track files, in the sheet's original encoding."""
    refs, text, encoding, raw = cue_files(root, row["source"])
    renames = {Path(m["source"]).name.casefold(): Path(m["target"]).name for m in row["moves"][1:]}
    if set(renames) != {Path(ref).name.casefold() for ref in refs}:
        raise ValueError("CUE move group does not match its FILE references")

    def directive(match):
        new_name = renames[(match[2] or match[3]).casefold()]
        return match[1] + '"' + new_name + '"' + match[4] + match[5]

    return FILE_LINE.sub(directive, text).encode(encoding)


def check_support_files(root):
    """Preflight support paths too, before creating a transaction or moving discs."""
    for name in STATE_FILES:
        path = relative_path(root, support_path(root, name))
        if path.exists() and (not path.is_file() or path.stat().st_size > 16 * MIB):
            raise ValueError(f"Invalid support file: {name}")


def gather_media(root, rows, options, cache):
    """Covers and previews to add for the rows being organized.

    Returns ({cover file: contents}, {preview file: contents}) after
    printing any warnings and checking no existing file would be replaced
    by different contents.
    """
    if options["preview_dir"] is not None:
        preview_files, preview_warnings = prepare_previews(
            rows,
            options["preview_dir"],
            options["preview_start"],
            options["preview_duration"],
            options["preview_sound"],
            options["replace_previews"],
        )
    else:
        preview_files, preview_warnings = {}, []
    if options["cover_dir"] is not None:
        covers, warnings = prepare_local_covers(rows, options["cover_dir"])
    elif options["covers"]:
        covers, warnings = prepare_covers(rows, cache, options["offline"])
    else:
        covers, warnings = {}, []
    for warning in warnings + preview_warnings:
        print(warning)

    for name, content in covers.items():
        path = relative_path(root, support_path(root, "covers/" + name))
        if path.exists() and path.read_bytes() != content:
            raise ValueError(f"Cover destination is occupied: {path}")
    for name, content in preview_files.items():
        path = relative_path(root, support_path(root, "previews/" + name))
        if path.exists() and path.read_bytes() != content:
            raise ValueError(f"Preview destination is occupied: {path}")
    return covers, preview_files


def new_media_indexes(root, plan, mapping, associations):
    """The covers and previews index.3di contents after the moves: earlier
    associations for discs that didn't move, plus each plan entry's media
    under its new path."""
    moved = {index_tools.fold(p) for p in mapping}
    outputs = {}
    for folder in ("covers", "previews"):
        if not relative_path(root, support_path(root, folder)).exists():
            continue
        old = associations[folder]
        output = {key: value for key, value in old.get("previous", {}).items() if key not in moved}
        for row in plan["entries"]:
            key = mapping.get(row["source"], row["source"])
            name = row.get("cover" if folder == "covers" else "preview") or existing_asset(
                old, row["source"]
            )
            if name and relative_path(root, support_path(root, folder + "/" + name)).is_file():
                output[index_tools.fold(key)] = name
        outputs[folder] = media_index(output)
    return outputs


def apply_plan(
    root,
    plan,
    cache,
    covers=True,
    offline=False,
    accept_names=False,
    preview_dir=None,
    preview_start=0,
    cover_dir=None,
    preview_duration=None,
    preview_sound=True,
    replace_previews=False,
):
    """Carry out a scanned plan on the drive at `root`.

    Organizes the "ready" rows (and "review" rows with `accept_names`):
    moves their files, rewrites their CUE sheets, updates favorites and the
    saved menu position, adds covers (downloaded, or from `cover_dir`) and
    previews (from `preview_dir`), and rebuilds the indexes and database.
    Everything is checked first and done under one undo journal, which is
    undone automatically if anything fails. Returns the Transaction.
    """
    if plan.get("schema") != 2 or plan.get("root") != str(root):
        raise ValueError("Plan version or USB root does not match; scan this drive again")
    if cover_dir is not None and not covers:
        raise ValueError("--cover-dir cannot be combined with --no-covers")
    rows = [
        r
        for r in plan["entries"]
        if r["status"] == "ready" or (accept_names and r["status"] == "review")
    ]
    check_registrations(root, plan)

    # Check every row and prepare rewritten CUE sheets, changing nothing yet.
    sources = set()
    destinations = set()
    cues = {}
    for row in rows:
        check_row(root, row, accept_names, sources, destinations)
        if Path(row["source"]).suffix.lower() == ".cue":
            cues[row["target"]] = rewritten_cue(root, row)
    check_support_files(root)

    mapping = {source: r["target"] for r in rows for source in [r["source"]] + r.get("aliases", [])}
    text_updates = prepare_text_remaps(root, mapping)
    associations = {folder: asset_associations(root, folder) for folder in ("covers", "previews")}
    # Media added after scan also takes precedence over imports and downloads.
    for row in rows:
        row["cover"] = row_asset(associations["covers"], row)
        row["preview"] = row_asset(associations["previews"], row)
    options = dict(
        covers=covers,
        offline=offline,
        cover_dir=cover_dir,
        preview_dir=preview_dir,
        preview_start=preview_start,
        preview_duration=preview_duration,
        preview_sound=preview_sound,
        replace_previews=replace_previews,
    )
    prepared, preview_files = gather_media(root, rows, options, cache)

    tx = Transaction(root, plan)
    print(f"Undo journal: {tx.path}", flush=True)
    try:
        for row in rows:
            for move in row["moves"]:
                tx.move(move)
        for name, content in cues.items():
            tx.write(name, content)
        for name, content in text_updates.items():
            tx.write(name, content)
        for name, content in prepared.items():
            tx.write(support_path(root, "covers/" + name), content)
        for name, content in preview_files.items():
            tx.write(support_path(root, "previews/" + name), content)
        write_indexes(root, tx, new_media_indexes(root, plan, mapping, associations))
        write_database(root, tx, plan, rows)
        tx.data["state"] = "complete"
        tx.save()
    except BaseException:
        print("Apply interrupted; attempting to undo this transaction.", file=sys.stderr)
        try:
            tx.undo()
        except (OSError, ValueError) as error:
            print(
                f"Automatic undo could not finish: {error}\nKeep the journal: {tx.path}",
                file=sys.stderr,
            )
        raise
    print(f'Organized {len(rows)} disc entries. Database: {root / ".ohmy3do/library.sqlite"}')
    return tx


def default_cache():
    """Where catalog downloads and converted media are cached: under
    %LOCALAPPDATA% on Windows, else $XDG_CACHE_HOME or ~/.cache."""
    base = os.environ.get("LOCALAPPDATA") if os.name == "nt" else os.environ.get("XDG_CACHE_HOME")
    return (Path(base) if base else Path.home() / ".cache") / "oh-my-3do"


def directory_link(path):
    # Windows junctions are reparse points but aren't always reported as
    # symlinks, especially on Python versions before Path.is_junction().
    return path.is_symlink() or is_reparse_point(path)


# Windows hidden (0x2) and system (0x4) attributes.
HIDDEN_OR_SYSTEM = 0x6


def empty_directories(root):
    """Empty folders under `root`, deepest first. Folders holding only empty
    folders count as empty; protected, hidden and system folders and links
    are never candidates and keep their parents."""
    protected = SKIP | {"system volume information", "$recycle.bin"}
    candidates = []

    def visit(directory):
        empty = True
        for path in sorted(directory.iterdir()):
            if directory_link(path) or not path.is_dir():
                empty = False
            elif (
                path.name.casefold() in protected
                or path.name.startswith(".")
                or getattr(path.lstat(), "st_file_attributes", 0) & HIDDEN_OR_SYSTEM
            ):
                empty = False
            elif not visit(path):
                empty = False
        if empty and directory != root:
            candidates.append(directory.relative_to(root).as_posix())
        return empty

    visit(root)
    return candidates


def cleanup_empty(root, apply=False):
    """Remove only empty directories, deepest first; never delete files.

    Without `apply`, lists what would be removed. Returns the folders
    removed (with `apply`) or the candidates.
    """
    if directory_link(root) or not root.is_dir():
        raise ValueError("USB root must be a real directory, not a link or junction")
    candidates = empty_directories(root)
    removed = []
    for name in candidates:
        if not apply:
            print(f"Would remove: {name}")
            continue
        path = root / name
        # Recheck every component in case the drive changed after enumeration.
        if any(directory_link(p) for p in (path, *path.parents) if p == root or root in p.parents):
            print(f"Kept link/junction: {name}")
            continue
        try:
            path.rmdir()
            removed.append(name)
            print(f"Removed: {name}")
        except OSError as error:
            if error.errno not in (errno.ENOTEMPTY, errno.EEXIST):
                raise
            print(f"Kept folder that is no longer empty: {name}")
    if apply:
        print(f"Removed {len(removed)} empty directories. No files deleted.")
    else:
        print(
            f"{len(candidates)} empty directories could be removed. No changes made. "
            "Add --apply to remove them."
        )
    return removed if apply else candidates


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--version", action="version", version="Oh My 3DO library organizer " + SCANNER_VERSION
    )
    parser.add_argument("--cache", type=Path, default=default_cache())
    commands = parser.add_subparsers(dest="command", required=True)

    report_command = commands.add_parser(
        "report", help="rebuild the interactive HTML report from an existing plan without scanning"
    )
    report_command.add_argument("plan", type=Path)
    report_command.add_argument(
        "--output", type=Path, help="HTML output (default: beside the plan)"
    )

    register = commands.add_parser(
        "register",
        help="preview adding an uncatalogued disc to the database and Find; never move disc files",
    )
    register.add_argument("usb_root", type=Path)
    register.add_argument("disc", help="volume-relative ISO/CUE path")
    register.add_argument("--title", required=True)
    register.add_argument("--category", required=True)
    register.add_argument("--region", default="Unknown")
    register.add_argument("--apply", action="store_true")

    cleanup = commands.add_parser(
        "clean-empty", help="preview empty directories; add --apply to remove them"
    )
    cleanup.add_argument("usb_root", type=Path)
    cleanup.add_argument(
        "--apply", action="store_true", help="remove empty directories; never delete files"
    )

    scan = commands.add_parser(
        "scan", help="read the drive and write a preview plan; USB files stay unchanged"
    )
    scan.add_argument("usb_root", type=Path)
    scan.add_argument("--plan", type=Path, default=Path("library-plan.json"))
    scan.add_argument(
        "--quick", action="store_true", help="skip checksum reads; filename matches require review"
    )
    scan.add_argument(
        "--overrides", type=Path, help="JSON object mapping relative paths to exact catalog names"
    )
    scan.add_argument(
        "--categories",
        type=Path,
        help="JSON object mapping relative disc paths to " + ", ".join(CATEGORIES),
    )
    scan.add_argument("--offline", action="store_true")
    scan.add_argument("--refresh-catalog", action="store_true")
    scan.add_argument(
        "--dat", type=Path, help="use a local XML/Logiqx or clrmamepro DAT (plain text or ZIP)"
    )
    scan.add_argument("--launchbox", type=Path, help="use a local LaunchBox Metadata.zip")

    apply = commands.add_parser(
        "apply", help="apply a reviewed plan and build covers, database and menu indices"
    )
    apply.add_argument("usb_root", type=Path)
    apply.add_argument("--plan", type=Path, default=Path("library-plan.json"))
    apply.add_argument(
        "--accept-name-matches",
        action="store_true",
        help="also accept reviewed filename and partial data-track matches in this plan",
    )
    apply.add_argument(
        "--no-covers", action="store_true", help="skip all cover imports and downloads"
    )
    apply.add_argument(
        "--cover-dir",
        type=Path,
        help="import local front-box images instead of downloading covers; subfolders included",
    )
    apply.add_argument(
        "--preview-dir",
        type=Path,
        help="import matching clips downloaded with EmuMovies Sync or another source",
    )
    apply.add_argument(
        "--preview-start",
        type=float,
        default=0,
        help="clip offset in seconds (default: 0); requires --preview-dir",
    )
    apply.add_argument(
        "--preview-duration",
        default=None,
        help="maximum seconds (default and overall cap: 15); clip shortens further to preserve "
        "15 fps; requires --preview-dir",
    )
    apply.add_argument(
        "--preview-silent", action="store_true", help="omit preview audio; requires --preview-dir"
    )
    apply.add_argument(
        "--replace-previews",
        action="store_true",
        help="import new previews even when an association exists; keep old media files",
    )
    apply.add_argument("--offline", action="store_true")

    undo = commands.add_parser(
        "undo", help="restore a transaction; refuses to overwrite later edits"
    )
    undo.add_argument("usb_root", type=Path)
    undo.add_argument("journal", help="journal path relative to the USB root, printed by apply")
    return parser, parser.parse_args()


def run_report(args):
    """Rebuild a plan's HTML report without scanning."""
    plan = json.loads(args.plan.read_text(encoding="utf-8"))
    if plan.get("schema") != 2:
        raise ValueError("Unsupported plan version; scan again")
    output = args.output or args.plan.with_suffix(".html")
    if output.suffix.lower() != ".html":
        raise ValueError("Report output must end in .html")
    root = Path(plan["root"]).resolve()
    if output.resolve() == root or root in output.resolve().parents:
        raise ValueError("Keep the report on the PC, outside the USB root")
    atomic_write(output, report_html(plan))
    print(f"Open report: {output.resolve()}\nNo scan or USB changes performed.")


def load_json_option(path):
    return json.loads(path.read_text(encoding="utf-8")) if path else None


def run_scan(args, root, cache):
    """Scan the drive, write the plan and its report, and summarize."""
    if args.plan.suffix.lower() != ".json":
        raise ValueError("Use a .json filename for the plan")
    # Even report/cache files must stay off the drive in preview mode.
    for target in (args.plan.resolve(), cache):
        if target == root or root in target.parents:
            raise ValueError(
                "Keep the preview plan and download cache on the PC, outside the USB root"
            )
    catalog = catalog_tools.load_catalog(
        cache, args.offline, args.refresh_catalog, args.dat, args.launchbox
    )
    overrides = load_json_option(args.overrides)
    categories = load_json_option(args.categories)
    plan = build_plan(root, catalog, args.quick, overrides, categories=categories)
    atomic_write(args.plan, json_bytes(plan))
    report = args.plan.with_suffix(".html")
    atomic_write(report, report_html(plan))

    counts = collections.Counter(row["status"] for row in plan["entries"])
    groups = collections.Counter(review_bucket(row) for row in plan["entries"])
    for row in plan["entries"]:
        print(
            f'{row["status"].upper():9} {row["source"]}\n          {row.get("target") or row["reason"]}'
        )
    print(
        f"Plan: {args.plan.resolve()}\n"
        f"Open preview: {report.resolve()}\n"
        f"Counts: {dict(counts)}\n"
        "No USB files changed. Filename matches need review and --accept-name-matches."
    )
    print(
        f'Batch matches: {groups["matched"]}; choices: {groups["decisions"]}; '
        f'unidentified: {groups["unidentified"]}; file problems: {groups["problems"]}. '
        "Unresolved entries stay in place and do not block batch apply."
    )


def run_apply(args, root, cache):
    plan = json.loads(args.plan.read_text(encoding="utf-8"))
    uses_preview_options = (
        args.preview_start or args.preview_duration or args.preview_silent or args.replace_previews
    )
    if uses_preview_options and args.preview_dir is None:
        raise ValueError("Preview options require --preview-dir")
    apply_plan(
        root,
        plan,
        cache,
        not args.no_covers,
        args.offline,
        args.accept_name_matches,
        args.preview_dir,
        args.preview_start,
        args.cover_dir,
        args.preview_duration,
        not args.preview_silent,
        args.replace_previews,
    )


def run_command(args):
    if args.command == "report":
        run_report(args)
        return
    if args.usb_root.is_symlink() or not args.usb_root.is_dir():
        raise ValueError("USB root must be a real, existing directory")
    if args.command == "clean-empty":
        if directory_link(args.usb_root):
            raise ValueError("USB root must not be a link or junction")
        cleanup_empty(args.usb_root.resolve(), args.apply)
        return
    root = args.usb_root.resolve()
    if directory_link(root):
        raise ValueError("USB root must not be a link or junction")
    if args.command == "register":
        register_disc(root, args.disc, args.title, args.category, args.region, args.apply)
        return

    cache = args.cache.resolve()
    if cache == root or root in cache.parents:
        raise ValueError("Keep the download cache on the PC, outside the USB root")
    if args.command == "scan":
        run_scan(args, root, cache)
    elif args.command == "apply":
        run_apply(args, root, cache)
    else:
        tx = Transaction(root, path=args.journal)
        tx.undo()
        print("Original file paths and support files restored.")


def main():
    parser, args = parse_arguments()
    try:
        run_command(args)
    except (ValueError, OSError, sqlite3.Error, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
