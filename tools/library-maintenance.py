#!/usr/bin/env python3
"""Incremental media updates, visual artwork review, and verified USB preparation.

Usage:
  library-maintenance.py update-media LIBRARY (--emu-movies DIR | --cover-dir DIR
                         | --preview-dir DIR) [--cache DIR] [--report FILE.html] [--apply]
  library-maintenance.py review LIBRARY [--report FILE.html]
  library-maintenance.py prepare-usb LIBRARY USB [--report FILE.json] [--apply]
                         [--verify-discs]

Works on a library that organize-library.py has already organized (it has
.ohmy3do/library.json):

  update-media  finds covers and preview clips for games that have none,
                converts them, and adds them with an undo journal. Games
                that already have media keep it. Writes an HTML contact
                sheet of the results.
  review        writes the same contact sheet without changing anything.
  prepare-usb   checks the library is ready for the console, then copies
                only the files the menu needs to a USB drive. Files already
                there are kept; changed support files are backed up first.

Without --apply, the commands only report what they would do.
"""
import argparse
import base64
import binascii
import copy
import hashlib
import html
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import uuid
import zlib

# Reuse the organizer's path guards, media matching, database and undo journal.
import importlib.util

_spec = importlib.util.spec_from_file_location(
    "organizer", Path(__file__).with_name("organize-library.py")
)
o = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(o)
FOLD = o.index_tools.fold

# Each kind of media: its folder under .ohmy3do/ and its prepared file type.
KINDS = {"cover": ("covers", ".3dc"), "preview": ("previews", ".3dv")}

# Files under .ohmy3do/ that hold the player's own settings and history.
# They are copied to a drive only when missing there, never overwritten.
PERSONAL = {"ohmy3do.ini", "favorites.txt", "recent.txt", "last-position.txt", "categories.json"}

VIDEO_EXTENSIONS = (".mp4", ".avi", ".mkv", ".mov", ".webm")

# EmuMovies folders holding preview videos; later ones are better quality
# and win when a title is in several.
EMU_MOVIES_VIDEO_FOLDERS = ("Video_MP4", "Video_MP4_HI_QUAL", "Video_MP4_HD")

MIB = 1024 * 1024
FAT32_MAX_FILE = 4 * 1024**3
MAX_FIND_INDEX_PATHS = 2048
MAX_SAVE_NAMES_BYTES = 32768
MAX_METADATA_INDEX_BYTES = 192 * 1024 + 32

# Names Windows reserves, which can't be used as file or folder names on FAT.
WINDOWS_RESERVED_NAME = r"(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?"


def real_root(path):
    if o.directory_link(path) or not path.is_dir():
        raise ValueError("Library root must be a real, existing directory")
    return path.resolve()


def outside(path, *roots):
    """`path`, resolved, after checking it isn't inside any of `roots`."""
    path = path.resolve()
    if any(path == root or root in path.parents for root in roots):
        raise ValueError("Keep reports, caches and backups outside the library and USB roots")
    return path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(MIB), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_path(root, name):
    """Also reject junctions and FAT-ambiguous components on either platform.

    Returns root/name after checking each part of `name`: no trailing spaces
    or dots, no characters or names FAT and Windows forbid, no existing
    sibling that differs only in case, and no links.
    """
    path = o.relative_path(root, name)
    parent = root
    for part in Path(name).parts:
        if (
            part.rstrip(" .") != part
            or re.search(r'[<>"|?*]', part)
            or re.fullmatch(WINDOWS_RESERVED_NAME, part, re.I)
        ):
            raise ValueError("Unsupported FAT filename: " + name)
        if parent.is_dir():
            matches = [p.name for p in parent.iterdir() if FOLD(p.name) == FOLD(part)]
            if matches and matches != [part]:
                raise ValueError("Case-colliding path: " + name)
        parent = parent / part
        if parent.is_symlink() or (parent.exists() and o.directory_link(parent)):
            raise ValueError("Links and junctions are unsupported: " + name)
    return path


def database(root):
    """The library's library.json, after checking every listed disc exists
    once."""
    data = json.loads(checked_path(root, ".ohmy3do/library.json").read_text(encoding="utf-8"))
    if data.get("schema") != 1 or not isinstance(data.get("games"), list):
        raise ValueError("Unsupported library database; organize or register discs first")
    seen = set()
    for record in data["games"]:
        name = record["path"]
        if FOLD(name) in seen or not checked_path(root, name).is_file():
            raise ValueError("Missing or duplicate database disc: " + name)
        seen.add(FOLD(name))
    return data


def associations(root):
    """{kind: {folded game path: media file name}} from the media indexes."""
    result = {}
    for kind, (folder, _) in KINDS.items():
        directory = checked_path(root, ".ohmy3do/" + folder)
        result[kind] = o.index_tools.existing_index(directory)
        for name in result[kind].values():
            checked_path(root, ".ohmy3do/" + folder + "/" + name)
    return result


def row_for(record):
    """A library.json record in the row form organize-library.py's media
    matching and database writing expect."""
    row = copy.deepcopy(record)
    row.update(
        source=record["path"],
        target=record["path"],
        canonical=record["title"],
        confidence=record["identification"],
    )
    return row


# Finding and importing media


def emu_movies_previews(emu_movies):
    """One file per exact title, preferring the better quality download."""
    preferred = {}
    for folder in EMU_MOVIES_VIDEO_FOLDERS:
        directory = emu_movies / folder
        if directory.exists():
            group = {}
            for path in o.local_media_files(directory, VIDEO_EXTENSIONS):
                group.setdefault(path.stem.casefold(), []).append(path)
            preferred.update(group)
    return [path for group in preferred.values() for path in group]


def media_sources(emu_movies=None, cover_dir=None, preview_dir=None):
    """{"cover": image files, "preview": video files} to match against games.

    An EmuMovies folder supplies its Box art and videos unless --cover-dir or
    --preview-dir name other folders.
    """
    if emu_movies:
        emu_movies = real_root(emu_movies)
        if (emu_movies / "Panasonic_3DO").is_dir():
            emu_movies /= "Panasonic_3DO"
        if cover_dir is None:
            cover_dir = emu_movies / "Box"
        if preview_dir is None:
            previews = emu_movies_previews(emu_movies)
        else:
            previews = o.local_media_files(preview_dir, VIDEO_EXTENSIONS)
    elif preview_dir:
        previews = o.local_media_files(preview_dir, VIDEO_EXTENSIONS)
    else:
        previews = []

    if cover_dir:
        covers = o.local_media_files(cover_dir, o.module("resize-covers").EXTENSIONS)
    else:
        covers = []
    return {"cover": covers, "preview": previews}


def existing_media(root, maps, kind, record):
    """The game's current media file for `kind`: from the media index, or
    else from library.json. None when neither names a file that exists."""
    folder = KINDS[kind][0]
    choices = (maps[kind].get(FOLD(record["path"])), record.get(kind))
    for name in choices:
        if name and checked_path(root, ".ohmy3do/" + folder + "/" + name).is_file():
            return name
    return None


def media_plan(root, sources):
    """Decide, for every game and kind of media, what update-media would do.

    Returns (library.json, media indexes, rows). Each row has the game's
    path and title, and per kind either {"status": "existing", "file",
    "repair_index"} or {"status": "matched" | "ambiguous" | "missing",
    "candidates": [source files]}.
    """
    db = database(root)
    maps = associations(root)
    rows = []
    for record in db["games"]:
        row = row_for(record)
        item = {"path": record["path"], "title": record["title"]}
        for kind in KINDS:
            name = existing_media(root, maps, kind, record)
            if name:
                # repair_index: the file is known from library.json only.
                item[kind] = {
                    "status": "existing",
                    "file": name,
                    "repair_index": maps[kind].get(FOLD(record["path"])) != name,
                }
                continue

            candidates = []
            if sources[kind]:
                # A preview from another region's release is still useful.
                candidates = o.local_media_matches(
                    row, sources[kind], other_regions=kind == "preview"
                )
            if len(candidates) == 1:
                status = "matched"
            elif candidates:
                status = "ambiguous"
            else:
                status = "missing"
            item[kind] = {"status": status, "candidates": [str(p) for p in candidates]}
        rows.append(item)
    return db, maps, rows


def cached_conversion(kind, source, cache):
    """Convert a source image or video, reusing an earlier conversion.

    Conversions are cached under a key made from the source's contents, the
    conversion settings and the converter script itself, so a changed
    converter converts again. Returns (file name, converted bytes); the name
    is derived from the converted contents.
    """
    before = source.stat()
    policy = "cover-fast96x120-v3" if kind == "cover" else "preview-v2-160x120-512k-v3"
    converter_file = Path(__file__).with_name(
        "resize-covers.py" if kind == "cover" else "prepare-preview.py"
    )
    key = hashlib.sha256((sha256(source) + policy + sha256(converter_file)).encode()).hexdigest()
    target = cache / (key + KINDS[kind][1])
    receipt = cache / (key + ".json")

    # The receipt records the converted file's hash, so a damaged cache
    # entry is converted again.
    valid = False
    if target.is_file() and receipt.is_file():
        try:
            valid = json.loads(receipt.read_text(encoding="utf-8"))["sha256"] == sha256(target)
        except (ValueError, KeyError):
            pass
    if not valid:
        cache.mkdir(parents=True, exist_ok=True)
        if kind == "cover":
            o.module("resize-covers").fast_convert(source, target)
        else:
            o.module("prepare-preview").convert(source, target, duration=15, sound=True)
        o.atomic_write(receipt, o.json_bytes({"sha256": sha256(target)}))

    after = source.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("Media source changed during conversion: " + str(source))
    content = target.read_bytes()
    return kind + "-" + hashlib.sha256(content).hexdigest()[:24] + KINDS[kind][1], content


# The artwork review page

REVIEW_PAGE_START = """<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width">
<title>3DO artwork review</title>
<style>
  body { font: 16px system-ui; background: #101c28; color: #e2eaf0; margin: 2rem }
  table { border-collapse: collapse; width: 100% }
  td, th { border: 1px solid #486273; padding: .7rem; text-align: left; overflow-wrap: anywhere }
  img { max-width: 168px; max-height: 210px }
  input, button { font: inherit; padding: .5rem; margin: .3rem }
  small { color: #bacbd7 }
</style>
<h1>3DO artwork review</h1>
<p>Existing media stays in place. Missing or ambiguous matches are left for review.
No game identities are changed.</p>
<input id="search" type="search" placeholder="Find a title or path" aria-label="Find a title or path">
<button id="missing">Show missing only</button>
<p id="count"></p>
<table>
<thead><tr><th>Game / disc</th><th>Cover</th><th>Preview</th></tr></thead>
<tbody>
"""

# Filters the rows by the search text and the "missing only" toggle.
REVIEW_PAGE_END = """</tbody>
</table>
<script>
  let missing = false;
  const rows = [...document.querySelectorAll('tbody tr')];
  function filter() {
    const query = document.getElementById('search').value.toLowerCase();
    let shown = 0;
    rows.forEach((row) => {
      row.hidden =
        (missing && row.dataset.missing !== '1') || !row.textContent.toLowerCase().includes(query);
      if (!row.hidden) shown++;
    });
    document.getElementById('count').textContent = shown + ' disc entries shown';
  }
  document.getElementById('search').oninput = filter;
  document.getElementById('missing').onclick = function () {
    missing = !missing;
    this.textContent = missing ? 'Show all' : 'Show missing only';
    filter();
  };
  filter();
</script>
</html>
"""

THUMBNAIL_SIZE = (168, 210)


def cover_thumbnail(image):
    """An <img> tag holding a small PNG of a cover (prepared or source art)."""
    from PIL import Image, ImageOps

    with o.module("resize-covers").read_cover(image) as art:
        thumbnail = ImageOps.exif_transpose(art).convert("RGB")
        thumbnail.thumbnail(THUMBNAIL_SIZE, Image.Resampling.LANCZOS)
        stream = io.BytesIO()
        thumbnail.save(stream, format="PNG")
    encoded = base64.b64encode(stream.getvalue()).decode()
    return '<img alt="Cover" src="data:image/png;base64,' + encoded + '">'


def review_cell(root, kind, item):
    """One table cell: the status, the source file, any error, and for covers
    a thumbnail of the art in use or about to be added."""
    folder = KINDS[kind][0]
    status = item["status"]
    text = html.escape(status)
    source = item.get("source") or next(iter(item.get("candidates", [])), "")
    if source:
        text += "<br>" + html.escape(source)
    if item.get("error"):
        text += "<br>" + html.escape(item["error"])

    if kind == "cover":
        image = None
        if item.get("file"):
            image = checked_path(root, ".ohmy3do/" + folder + "/" + item["file"])
        elif status == "matched":
            image = Path(source)
        if image:
            try:
                text = cover_thumbnail(image) + "<br>" + text
            except (ImportError, OSError, ValueError):
                text += "<br>Thumbnail unavailable"
    return "<td>" + text + "</td>"


def review_page(root, rows):
    """The artwork contact sheet: one table row per disc, searchable, with a
    toggle to show only discs still missing media."""
    body = []
    for row in rows:
        missing = any(row[kind]["status"] not in ("existing", "added") for kind in KINDS)
        cells = "".join(review_cell(root, kind, row[kind]) for kind in KINDS)
        body.append(
            f'<tr data-missing="{int(missing)}">'
            f"<td>{html.escape(row['title'])}<br><small>{html.escape(row['path'])}</small></td>"
            f"{cells}</tr>\n"
        )
    page = REVIEW_PAGE_START + "".join(body) + REVIEW_PAGE_END
    return page.encode("utf-8")


def import_matched_media(root, db, maps, rows, cache):
    """Convert each game's single matched source and plan its addition.

    Updates `maps` and `rows` in place. A source that fails to convert is
    marked "failed" and skipped. Returns (changed library.json records,
    {path under the library: file contents} to write).
    """
    records = {record["path"]: record for record in db["games"]}
    changed = []
    outputs = {}
    converted = {}  # (kind, source) -> (name, content), so a source converts once
    for item in rows:
        record = row_for(records[item["path"]])
        dirty = False
        for kind, (folder, _) in KINDS.items():
            value = item[kind]
            if value["status"] == "existing" and value.get("repair_index"):
                maps[kind][FOLD(item["path"])] = value["file"]
                dirty = True
            if value["status"] != "matched":
                continue

            source = Path(value["candidates"][0])
            try:
                key = (kind, source)
                if key not in converted:
                    print("Preparing " + kind + ": " + source.name, flush=True)
                    converted[key] = cached_conversion(kind, source, cache)
                name, content = converted[key]
                path = checked_path(root, ".ohmy3do/" + folder + "/" + name)
                if path.exists() and path.read_bytes() != content:
                    raise ValueError("Refusing to overwrite existing media: " + name)
                outputs[path.relative_to(root).as_posix()] = content
                maps[kind][FOLD(item["path"])] = name
                record[kind] = name
                record[kind + "_source"] = str(source)
                value.update(status="added", file=name, source=str(source))
                dirty = True
            except (OSError, ValueError, subprocess.CalledProcessError) as error:
                value.update(status="failed", source=str(source), error=str(error))
                print("Skipped " + str(source) + ": " + str(error), flush=True)
        if dirty:
            changed.append(record)
    return changed, outputs


def commit_media(root, db, maps, changed, outputs):
    """Write the new media, both media indexes and library.json under an
    undo journal, undoing everything if any step fails."""
    # No disc reads, renames, catalog lookup, Find rebuild or deletion.
    transaction = o.Transaction(root)
    print("Undo journal: " + str(transaction.path), flush=True)
    try:
        for path, content in outputs.items():
            transaction.write(path, content)
        for kind, (folder, _) in KINDS.items():
            transaction.write(".ohmy3do/" + folder + "/index.3di", o.media_index(maps[kind]))
        o.write_database(root, transaction, {"entries": changed, "sources": db["sources"]}, [])
        transaction.data["state"] = "complete"
        transaction.save()
    except BaseException:
        transaction.undo()
        raise


def update_media(root, sources, cache, report, apply=False):
    """The update-media and review commands; returns the plan's rows.

    Always writes the HTML contact sheet to `report` and the plan beside it
    as JSON. With `apply`, imports the matched media first.
    """
    cache = outside(cache, root)
    report = outside(report, root)
    if report.suffix.lower() != ".html":
        raise ValueError("Artwork report must end in .html")

    db, maps, rows = media_plan(root, sources)
    if apply:
        changed, outputs = import_matched_media(root, db, maps, rows, cache)
        if changed:
            commit_media(root, db, maps, changed, outputs)

    o.atomic_write(report, review_page(root, rows))
    o.atomic_write(
        report.with_suffix(".json"),
        o.json_bytes({"root": str(root), "applied": apply, "rows": rows}),
    )
    for kind in KINDS:
        counts = o.collections.Counter(row[kind]["status"] for row in rows)
        print(kind.capitalize() + ": " + str(dict(counts)))
    print("Artwork review: " + str(report))
    if not apply:
        print("Preview only; no library files changed. Add --apply to import matched media.")
    return rows


# Checking media files the way the menu will


def inflate_exact(data, size):
    """Decompress zlib data that must expand to exactly `size` bytes and end
    exactly where the data ends."""
    decoder = zlib.decompressobj()
    raw = decoder.decompress(data, size + 1)
    if len(raw) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError("Invalid compressed media data")
    return raw


def media_size_limit(path):
    suffix = path.suffix.lower()
    if suffix == ".png":
        return 512 * 1024
    if suffix == ".3dv":
        return 16384 * 1024 + 48
    return 1024 * 1024 + 48


def validate_png(path, data):
    """Native, non-interlaced RGB/RGBA covers; check every chunk and scanline."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Invalid PNG: " + str(path))
    position = 8
    compressed = bytearray()
    color = None
    ended = False
    while position + 12 <= len(data):
        # Each chunk: length, 4-letter tag, data, CRC of tag and data.
        length = struct.unpack_from(">I", data, position)[0]
        tag = data[position + 4 : position + 8]
        chunk = data[position + 8 : position + 8 + length]
        if (
            position + 12 + length > len(data)
            or binascii.crc32(tag + chunk)
            != struct.unpack_from(">I", data, position + 8 + length)[0]
        ):
            raise ValueError("PNG chunk checksum or length failed: " + str(path))

        if tag == b"IHDR":
            if position != 8 or len(chunk) != 13:
                raise ValueError("Invalid PNG header")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
            # Color type 2 is RGB and 6 is RGBA.
            if (
                (width, height) != (96, 120)
                or (depth, compression, filtering, interlace) != (8, 0, 0, 0)
                or color not in (2, 6)
            ):
                raise ValueError("Prepare covers as native 96x120 RGB/RGBA PNGs: " + str(path))
        elif tag == b"IDAT":
            compressed.extend(chunk)
        elif tag == b"IEND":
            if length or position + 12 != len(data):
                raise ValueError("Invalid PNG end")
            ended = True
            break
        elif tag[:1].isupper() and tag != b"PLTE":
            # An uppercase first letter marks a chunk readers must understand.
            raise ValueError("Unsupported critical PNG chunk")
        position += length + 12

    if color not in (2, 6) or not ended:
        raise ValueError("Incomplete PNG")
    # Each row is a filter byte (0-4) followed by the pixels.
    stride = width * (3 if color == 2 else 4) + 1
    raw = inflate_exact(bytes(compressed), stride * height)
    if any(raw[i] > 4 for i in range(0, len(raw), stride)):
        raise ValueError("Invalid PNG filter")


def validate_native_cover(path, data):
    """A .3dc cover (see resize-covers.py): 96x120, or 192x240 for a
    "-large" full-screen cover."""
    if len(data) < 32:
        raise ValueError("Truncated native cover")
    width, height, payload_size, crc, decoded_size = struct.unpack_from(">HHIII", data, 8)
    if path.name.lower().endswith("-large.3dc"):
        size, payload_limit = (192, 240), 192 * 240 * 2 + 1024
    else:
        size, payload_limit = (96, 120), 32768
    if (
        (width, height) != size
        or not 0 < payload_size <= payload_limit
        or decoded_size != width * height * 2
        or len(data) != payload_size + 32
        or data[-8:] != b"END3DC02"
        or binascii.crc32(data[24:-8]) != crc
    ):
        raise ValueError("Invalid native cover")
    inflate_exact(data[24:-8], width * height * 2)


def validate_v5_preview(data):
    """An OM3DVID5 preview (see prepare-preview.py's v5_container): header,
    checksum, sizes, sound and frame table. Packets are checked by the
    menu's decoder as they play."""
    if len(data) < 48:
        raise ValueError("Truncated preview")
    (
        width,
        height,
        payload_size,
        crc,
        frames,
        fps,
        video,
        audio,
        rate,
        samples,
    ) = struct.unpack_from(">HHIIHHIIII", data, 8)
    payload = data[40:-8]
    if len(data) != 40 + payload_size + 8 or word_sum(payload) != crc or data[-8:] != b"END3DV05":
        raise ValueError("Invalid preview checksum/length/end marker")
    if (width, height, fps) != (160, 120, 30) or not 1 <= frames <= 450:
        raise ValueError("Invalid preview size, rate or length")

    padding = -video % 4
    if (
        payload_size > 4096 * 1024
        or video + padding + audio != payload_size
        or video <= (frames + 1) * 4
        or any(payload[payload_size - padding : payload_size])
    ):
        raise ValueError("Invalid preview memory/frame bounds")

    # Sound, when present, is 11025 Hz for exactly the frames, padded to a word.
    expected = (((frames * 11025 + fps - 1) // fps) + 3) & ~3
    bad_sound = audio and (audio != samples or rate != 11025 or samples != expected)
    stray_sound_fields = not audio and (rate or samples)
    if bad_sound or stray_sound_fields:
        raise ValueError("Invalid preview audio")

    offsets = struct.unpack_from(">" + "I" * (frames + 1), payload)
    if offsets[0] != (frames + 1) * 4 or offsets[-1] != video:
        raise ValueError("Invalid preview frame table")
    # Packets are checked by the menu's decoder as they play.
    if any(begin >= end for begin, end in zip(offsets, offsets[1:])):
        raise ValueError("Invalid preview frame offset")


def validate_media(path):
    """Check a cover or preview the way the menu will before it is copied.

    Raises ValueError (or zlib.error) when the menu would reject the file.
    """
    if path.stat().st_size > media_size_limit(path):
        raise ValueError("Oversized media: " + str(path))
    data = path.read_bytes()
    suffix = path.suffix.lower()
    if suffix == ".png":
        validate_png(path, data)
        return
    if data[:8] == b"OM3DCOV2":
        validate_native_cover(path, data)
        return
    if suffix == ".3dv" and data[:8] == b"OM3DVID7":
        return validate_stream(data)
    if suffix != ".3dv" or data[:8] != b"OM3DVID5":
        raise ValueError("Invalid preview signature")
    validate_v5_preview(data)


STREAM_HEADER_BLOCK = 2048
STREAM_CHUNK_TABLE = 64
STREAM_CHUNK_HEADER = 24


def validate_stream(data):
    """OM3DVID7 (om3dvid/include/om3dvid7.h), as the menu checks it: the header block and
    its sum, the chunk table against the file, every chunk's sum, numbering,
    entries and frame table. Packets are checked by the decoder as they play."""
    if len(data) < STREAM_HEADER_BLOCK:
        raise ValueError("Truncated streamed preview")
    (
        width,
        height,
        fps,
        frames_per_chunk,
        frames,
        chunks,
        preroll_chunks,
        max_entries4,
        max_entries2,
        audio_rate,
        largest_chunk,
        _,
        header_sum,
        slots,
    ) = struct.unpack_from(">HHHHIHHHHIIIIH", data, 8)
    max_chunks = (STREAM_HEADER_BLOCK - STREAM_CHUNK_TABLE) // 2
    chunk_table_end = STREAM_CHUNK_TABLE + 2 * chunks
    if (
        (width, height, fps) != (320, 240, 30)
        or not 1 <= frames_per_chunk <= 30
        or not 1 <= frames <= 1800
        or chunks != -(-frames // frames_per_chunk)  # frames / per chunk, rounded up
        or chunks > max_chunks
        or not 1 <= preroll_chunks <= chunks
        or slots < preroll_chunks
        or max_entries4 > 8192
        or max_entries2 > 4096
        or audio_rate not in (0, 11025)
        or not largest_chunk
        or largest_chunk % 2048
        or any(data[46:STREAM_CHUNK_TABLE])
        or any(data[chunk_table_end:STREAM_HEADER_BLOCK])
    ):
        raise ValueError("Invalid streamed preview header")
    # The sum covers the header block with its own field (bytes 40-43) as zero.
    if word_sum(data[:40] + bytes(4) + data[44:STREAM_HEADER_BLOCK]) != header_sum:
        raise ValueError("Streamed preview header checksum mismatch")

    # Walk the chunks the table lists; each must agree with its position.
    position = STREAM_HEADER_BLOCK
    first4 = 0
    first2 = 0
    table = data[STREAM_CHUNK_TABLE:chunk_table_end]
    for number, (blocks,) in enumerate(struct.iter_unpack(">H", table)):
        size = blocks * 2048
        if not size or size > largest_chunk or position + size > len(data):
            raise ValueError("Invalid streamed preview chunk table")
        chunk = data[position : position + size]
        (
            chunk_sum,
            chunk_number,
            count,
            chunk_first4,
            new4,
            chunk_first2,
            new2,
            audio,
            video,
        ) = struct.unpack_from(">IHHHHHHII", chunk)

        first_frame = number * frames_per_chunk
        if audio_rate:
            # The samples for this chunk's frames, rounded as a running total.
            expected_audio = (first_frame + count) * 11025 // 30 - first_frame * 11025 // 30
        else:
            expected_audio = 0
        # The frame table follows the header, the sound (padded to a word) and
        # the new dictionary entries (40 bytes for 4x4, 12 for 2x2).
        frame_table = STREAM_CHUNK_HEADER + ((audio + 3) & ~3) + new4 * 40 + new2 * 12
        if (
            word_sum(chunk[4:]) != chunk_sum
            or chunk_number != number
            or count != min(frames_per_chunk, frames - first_frame)
            or (chunk_first4, chunk_first2) != (first4, first2)
            or first4 + new4 > max_entries4
            or first2 + new2 > max_entries2
            or audio != expected_audio
            or frame_table + video > size
            or video < (count + 1) * 4
            or struct.unpack_from(">I", chunk, frame_table)[0] != (count + 1) * 4
            or struct.unpack_from(">I", chunk, frame_table + count * 4)[0] != video
        ):
            raise ValueError("Invalid streamed preview chunk " + str(number))
        first4 += new4
        first2 += new2
        position += size

    if position != len(data):
        raise ValueError("Streamed preview has trailing data")


def word_sum(data):
    """Preview payload check, as in src/usb_media.c and prepare-preview.py.

    Returns None when `data` isn't a whole number of 32-bit words.
    """
    a = b = 0
    if len(data) % 4:
        return None
    for (word,) in struct.iter_unpack(">I", data):
        a = (a + word) & 0xFFFFFFFF
        b = (b + a) & 0xFFFFFFFF
    return a ^ (((b << 16) | (b >> 16)) & 0xFFFFFFFF)


# Checking the library is ready for the console

# The most each menu setting may be (all are whole numbers from 0).
SETTING_LIMITS = dict(
    folder_workaround=2,
    usb_covers=1,
    screen_margin=12,
    large_titles=1,
    remember_position=1,
    recent_games=1,
    video_previews=1,
    preview_sound=1,
    hide_adult=1,
)
SETTING_SECTIONS = ("[compatibility]", "[artwork]", "[display]", "[library]", "[audio]")
SETTINGS_END_MARKER = "; End of Oh My 3DO settings."
MAX_SETTINGS_BYTES = 512


def validate_config(path):
    """Check ohmy3do.ini as the menu reads it: at most 512 bytes, known
    settings within their limits, each once, and every setting present when
    the end marker is reached."""
    data = path.read_bytes()
    if len(data) > MAX_SETTINGS_BYTES or b"\0" in data:
        raise ValueError("Invalid settings length or NUL padding: " + str(path))
    values = {}
    ended = False
    for line in data.decode("utf-8-sig").split("\n"):
        line = line.strip(" \t\r")
        if line == SETTINGS_END_MARKER:
            # hide_adult is newer than the end marker and stays optional.
            if not set(SETTING_LIMITS) - {"hide_adult"} <= values.keys():
                raise ValueError("Incomplete generated settings")
            ended = True
            break
        if not line or line.startswith(("#", ";")) or line in SETTING_SECTIONS:
            continue
        match = re.fullmatch(r"([a-z_]+)[ \t]*=[ \t]*([0-9]{1,2})[ \t]*(?:[#;].*)?", line)
        if (
            not match
            or match[1] not in SETTING_LIMITS
            or match[1] in values
            or int(match[2]) > SETTING_LIMITS[match[1]]
        ):
            raise ValueError("Invalid menu setting: " + line)
        values[match[1]] = int(match[2])
    # The menu reads at most 512 bytes, so a full file must say where it ends.
    if len(data) == MAX_SETTINGS_BYTES and not ended:
        raise ValueError("Full settings buffer needs an end marker")


def find_paths(path):
    """Check the Find index (library.3di, see index-media.py) and return its
    game paths."""
    data = path.read_bytes()
    if len(data) < 32 or data[:8] != b"OM3DLIB1" or data[-8:] != b"END3DL01":
        raise ValueError("Invalid Find index")
    count, size, crc, reserved = struct.unpack_from(">IIII", data, 8)
    payload = data[24:-8]
    if (
        count > MAX_FIND_INDEX_PATHS
        or size + 9 * count > 128 * 1024
        or size != len(payload)
        or reserved
        or binascii.crc32(payload) != crc
    ):
        raise ValueError("Invalid Find index checksum or bounds")

    paths = []
    position = 0
    for _ in range(count):
        if position + 2 > len(payload):
            raise ValueError("Truncated Find index")
        length = struct.unpack_from(">H", payload, position)[0]
        position += 2
        raw = payload[position : position + length]
        position += length
        if length < 2 or len(raw) != length or raw[-1:] != b"\0" or b"\0" in raw[:-1]:
            raise ValueError("Invalid Find path")
        name = raw[:-1].decode("utf-8")
        parts = name.split("/")
        if len(parts) > 33 or any(len(part.encode()) >= 1024 for part in parts):
            raise ValueError("Find path exceeds navigation limits")
        paths.append(name)

    if position != len(payload) or len(set(map(FOLD, paths))) != len(paths):
        raise ValueError("Duplicate paths or trailing Find data")
    return paths


def check_boot_image(root):
    """boot.iso must be a whole 3DO (Opera) volume of 2048-byte blocks."""
    boot = checked_path(root, "boot.iso")
    with boot.open("rb") as stream:
        volume = stream.read(84)
    # The volume label: its start, block size (offset 76) and count (80).
    if (
        len(volume) < 84
        or volume[:7] != b"\x01ZZZZZ\x01"
        or struct.unpack_from(">I", volume, 76)[0] != 2048
        or struct.unpack_from(">I", volume, 80)[0] * 2048 != boot.stat().st_size
    ):
        raise ValueError("Invalid 3DO boot volume header")
    issue = o.index_tools.disc_data_issue(boot)
    if issue or boot.stat().st_size % 2048:
        raise ValueError("Invalid menu boot image: " + (issue or "not sector aligned"))


def disc_files(root):
    """Scan the library's discs. Returns (game paths, every file they use,
    such as a CUE sheet's tracks)."""
    games = set()
    files = set()
    for unit in o.scan_units(root):
        if unit.get("error") or unit.get("invalid_data"):
            problem = unit.get("error") or unit["invalid_data"]
            raise ValueError("Disc problem: " + unit["source"] + ": " + str(problem))
        games.add(unit["source"])
        files.update(unit["files"])
    return games, files


def media_inventory(root, games):
    """The indexes and indexed media files to copy, each checked as the menu
    would (plus a cover's full-screen "-large" version when present)."""
    files = set()
    maps = associations(root)
    folded_games = set(map(FOLD, games))
    checked = set()

    def add_media(path):
        if path not in checked:
            validate_media(checked_path(root, path))
            checked.add(path)
        files.add(path)

    for kind, (folder, _) in KINDS.items():
        index = ".ohmy3do/" + folder + "/index.3di"
        if checked_path(root, index).exists():
            files.add(index)
        for game, name in maps[kind].items():
            if game not in folded_games:
                raise ValueError("Media index refers to a missing disc: " + game)
            path = ".ohmy3do/" + folder + "/" + name
            allowed = (".png", ".3dc") if kind == "cover" else (".3dv",)
            if Path(name).suffix.lower() not in allowed:
                raise ValueError("Unsupported indexed media: " + path)
            add_media(path)
            large = ".ohmy3do/covers/" + name[:-4] + "-large.3dc"
            if kind == "cover" and checked_path(root, large).is_file():
                add_media(large)
    return files


def usb_inventory(root):
    """Check the staging library is ready for the console and list the files
    a USB drive needs: boot.iso, settings, indexes, discs, indexed media,
    personal files and saves. Returns (sorted file list, game paths)."""
    files = {"boot.iso", ".ohmy3do/ohmy3do.ini", ".ohmy3do/library.3di"}
    check_boot_image(root)
    validate_config(checked_path(root, ".ohmy3do/ohmy3do.ini"))

    games, used_files = disc_files(root)
    files.update(used_files)
    indexed = find_paths(checked_path(root, ".ohmy3do/library.3di"))
    if set(indexed) != games:
        raise ValueError("Find index is stale; run index-media.py against the staging root first")

    metadata = checked_path(root, ".ohmy3do/metadata.3di")
    if metadata.is_file():
        if metadata.stat().st_size > MAX_METADATA_INDEX_BYTES:
            raise ValueError("Oversized console metadata index")
        metadata_games = set(map(FOLD, o.index_tools.metadata_paths(metadata.read_bytes())))
        if not metadata_games <= set(map(FOLD, games)):
            raise ValueError("Metadata index refers to a missing disc; rebuild indexes")
        files.add(".ohmy3do/metadata.3di")

    names = checked_path(root, ".ohmy3do/save-names.txt")
    if names.is_file():
        if names.stat().st_size >= MAX_SAVE_NAMES_BYTES:
            raise ValueError("Oversized save names; rebuild indexes")
        files.add(".ohmy3do/save-names.txt")

    files.update(media_inventory(root, games))

    for name in PERSONAL - {"ohmy3do.ini"}:
        path = ".ohmy3do/" + name
        if checked_path(root, path).is_file():
            files.add(path)

    saves = checked_path(root, ".ohmy3do/saves")
    if saves.exists():
        for parent, folders, names in os.walk(saves, followlinks=False):
            for name in folders + names:
                checked_path(root, (Path(parent) / name).relative_to(root).as_posix())
            files.update((Path(parent) / name).relative_to(root).as_posix() for name in names)

    for name in files:
        path = checked_path(root, name)
        if not path.is_file() or path.stat().st_size >= FAT32_MAX_FILE:
            raise ValueError("Missing file or file exceeds FAT32 limit: " + name)
    return sorted(files), games


# Planning the copy


def category_renames(target, source_config):
    """(old, new) folder names for categories renamed in the library since the
    drive was last prepared (matched by category id)."""
    moves = []
    previous = o.category_config(target)
    for category in source_config["categories"]:
        old = next(
            (
                c
                for c in previous["categories"]
                if c["id"] == category["id"] and c["name"] != category["name"]
            ),
            None,
        )
        if old and checked_path(target, old["name"]).exists():
            if checked_path(target, category["name"]).exists():
                raise ValueError(
                    "Both old and new category folders exist; resolve them before copying"
                )
            moves.append((old["name"], category["name"]))
    return moves


def folder_moves(target, files, allowed=None, source_config=None):
    """Folder moves that bring the drive's layout in line with the library.

    Renamed categories move whole, and a game folder that changed category
    (Category/Region/Game) moves when exactly one drive folder matches it,
    so its discs aren't copied again. Returns [(old, new)] relative to the
    drive.
    """
    category_moves = category_renames(target, source_config) if source_config else []

    # The library's game folders: Category/Region/Game.
    wanted = {
        "/".join(Path(name).parts[:3])
        for name in files
        if len(Path(name).parts) >= 4 and Path(name).parts[0] in (allowed or o.CATEGORIES)
    }
    wanted_folded = {FOLD(p) for p in wanted}

    # The drive's game folders the library doesn't have at the same place,
    # keyed by Region/Game.
    found = {}
    for category in o.category_names(target):
        if any(category == old for old, _ in category_moves):
            continue
        parent = checked_path(target, category)
        if not parent.is_dir():
            continue
        for region in parent.iterdir():
            checked_path(target, region.relative_to(target).as_posix())
            if not region.is_dir():
                continue
            for game in region.iterdir():
                name = game.relative_to(target).as_posix()
                checked_path(target, name)
                if game.is_dir() and FOLD(name) not in wanted_folded:
                    found.setdefault(FOLD("/".join(name.split("/")[1:])), []).append(name)

    moves = list(category_moves)
    used = set()
    for new in sorted(wanted):
        if any(new.startswith(prefix + "/") for _, prefix in category_moves):
            continue
        candidates = found.get(FOLD("/".join(new.split("/")[1:])), [])
        if not checked_path(target, new).exists() and len(candidates) == 1:
            old = candidates[0]
            if old in used:
                raise ValueError("One USB folder matches multiple staging categories: " + old)
            used.add(old)
            moves.append((old, new))
    return moves


def move_remaps(target, moves):
    """CUE sheets and other text files that name moved paths, rewritten:
    {path on the drive: new contents}."""
    mapping = {}
    for old, new in moves:
        for path in checked_path(target, old).rglob("*"):
            name = path.relative_to(target).as_posix()
            checked_path(target, name)
            if path.is_file():
                mapping[name] = new + name[len(old) :]
    return o.prepare_text_remaps(target, mapping) if moves else {}


def is_personal(name):
    """Saves and the player's own settings, which are never overwritten."""
    if name.startswith(".ohmy3do/saves/"):
        return True
    return name.startswith(".ohmy3do/") and name.split("/", 1)[1] in PERSONAL


def receipt_matches(receipt, source_stat, dest_stat):
    """Whether a copy recorded in an earlier report still describes both
    files (same sizes and modification times)."""
    return (
        isinstance(receipt, dict)
        and (receipt.get("source_size"), receipt.get("source_mtime_ns"))
        == (source_stat.st_size, source_stat.st_mtime_ns)
        and (receipt.get("destination_size"), receipt.get("destination_mtime_ns"))
        == (dest_stat.st_size, dest_stat.st_mtime_ns)
    )


def existing_file_status(name, source, dest, source_stat, receipt, verify):
    """What to do with a file the drive already has: "unchanged" or
    "update". Returns (status, verified copy receipt or None).

    Discs that look the same (same size and time, or as recorded after an
    earlier verified copy) are trusted without reading them, unless
    `verify`. A disc that differs is never replaced.
    """
    dest_stat = dest.stat()
    disc = not name.startswith(".ohmy3do/") and name != "boot.iso"
    matches_receipt = receipt_matches(receipt, source_stat, dest_stat)
    same_size_and_time = (source_stat.st_size, source_stat.st_mtime_ns) == (
        dest_stat.st_size,
        dest_stat.st_mtime_ns,
    )
    if disc and not verify and (same_size_and_time or matches_receipt):
        return "unchanged", receipt if matches_receipt else None
    if source_stat.st_size == dest_stat.st_size and sha256(source) == sha256(dest):
        return "unchanged", None
    if disc:
        raise ValueError(
            "Different disc already exists on destination; resolve it before copying: " + name
        )
    return "update", None


def usb_plan(root, target, verify=False, receipts=None):
    """Plan copying the staging library at `root` to the drive at `target`.

    Returns (operations, game paths). Each operation is a dict with the
    file's path, size and time, and a status:
      move       a folder renamed or moved on the drive (source_path is the old path)
      remap      a text file on the drive rewritten for moved paths
      copy       a file the drive lacks
      update     a support file that changed
      preserve   a personal file the drive already has
      unchanged  a file the drive already has
    `receipts` are verified copies from an earlier report, which let
    unchanged discs be trusted without reading them.
    """
    if root == target or root in target.parents or target in root.parents:
        raise ValueError("Staging and USB directories must be separate and non-overlapping")
    print("Validating staging library and indexed media...", flush=True)
    files, games = usb_inventory(root)
    o.transfer_categories(root, target)
    moves = folder_moves(target, files, o.category_names(root), o.category_config(root))
    remaps = move_remaps(target, moves)
    operations = [
        dict(path=new, source_path=old, status="move", size=0, mtime_ns=0) for old, new in moves
    ]
    receipts = receipts or {}

    for name in files:
        source = checked_path(root, name)
        # Where the file is on the drive now, before any planned move.
        current = next(
            (old + name[len(new) :] for old, new in moves if name.startswith(new + "/")), name
        )
        dest = checked_path(target, current)
        stat = source.stat()
        verified_copy = None
        if dest.exists() and not dest.is_file():
            raise ValueError("Destination is occupied by a directory: " + name)

        if name in remaps and dest.is_file():
            status = "remap"
        elif is_personal(name) and dest.exists():
            if name.endswith("/ohmy3do.ini"):
                validate_config(dest)
            # Category settings are merged; everything else personal is kept.
            status = "update" if name.endswith("/categories.json") else "preserve"
        elif not dest.exists():
            status = "copy"
        else:
            status, verified_copy = existing_file_status(
                name, source, dest, stat, receipts.get(name), verify
            )

        item = dict(path=name, status=status, size=stat.st_size, mtime_ns=stat.st_mtime_ns)
        if verified_copy:
            item["verified_copy"] = verified_copy
        operations.append(item)

    # Rewritten text files that aren't part of the library's own files.
    planned = {operation["path"] for operation in operations}
    for name in remaps:
        if name not in planned:
            stat = checked_path(target, name).stat()
            operations.append(
                dict(path=name, status="remap", size=stat.st_size, mtime_ns=stat.st_mtime_ns)
            )

    required = sum(op["size"] for op in operations if op["status"] in ("copy", "update"))
    if shutil.disk_usage(target).free < required:
        raise ValueError("Insufficient free space for the planned copy")
    return operations, games


# Copying


def stream_copy(source, dest):
    """Copy to a sibling temporary file; verify readback before atomic replacement.

    The copy keeps the source's modification time. Returns a receipt of the
    copy: both files' sizes and times, and the SHA-256.
    """
    before = source.stat()
    temporary = None
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        digest = hashlib.sha256()
        with source.open("rb") as reader, tempfile.NamedTemporaryFile(
            dir=dest.parent, prefix=".oh3do-", delete=False
        ) as writer:
            temporary = Path(writer.name)
            for block in iter(lambda: reader.read(MIB), b""):
                digest.update(block)
                writer.write(block)
            writer.flush()
            os.fsync(writer.fileno())

        after = source.stat()
        source_changed = (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns)
        if source_changed or sha256(temporary) != digest.hexdigest():
            raise ValueError("File changed or copy verification failed: " + str(source))
        os.utime(temporary, ns=(before.st_atime_ns, before.st_mtime_ns))
        temporary.replace(dest)

        dest_stat = dest.stat()
        return dict(
            source_size=before.st_size,
            source_mtime_ns=before.st_mtime_ns,
            destination_size=dest_stat.st_size,
            destination_mtime_ns=dest_stat.st_mtime_ns,
            sha256=digest.hexdigest(),
        )
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)


def load_receipts(report, root, target):
    """Verified copies recorded by an earlier report for the same library and
    drive: {path: receipt}. Anything unreadable counts as no receipts."""
    if not report.is_file():
        return {}
    try:
        previous = json.loads(report.read_text(encoding="utf-8"))
        if previous.get("source") == str(root) and previous.get("destination") == str(target):
            return {
                item["path"]: item["verified_copy"]
                for item in previous.get("files", [])
                if item.get("verified_copy")
            }
    except (ValueError, KeyError, TypeError):
        pass
    return {}


def copy_order(item):
    """Write data first, then the indexes, then boot.iso last, so an
    interrupted copy never leaves a menu pointing at missing files."""
    if item["path"] == "boot.iso":
        rank = 2
    elif item["path"].endswith(".3di"):
        rank = 1
    else:
        rank = 0
    return rank, item["path"]


def copy_one(item, root, target, transaction, backup, category_config):
    """Copy or update one planned file, recording a verified-copy receipt in
    `item`. Updated files are backed up to `backup` first."""
    name = item["path"]
    source = checked_path(root, name)
    dest = checked_path(target, name)
    stat = source.stat()
    if (stat.st_size, stat.st_mtime_ns) != (item["size"], item["mtime_ns"]):
        raise ValueError("Staging file changed since validation: " + name)
    if item["status"] == "copy" and name.endswith("/categories.json"):
        transaction.write(name, o.json_bytes(category_config))
        return False
    if item["status"] == "copy" and dest.exists():
        raise ValueError("Destination appeared during copying: " + name)
    if item["status"] == "update":
        stream_copy(dest, backup / name)

    print("Copying: " + name, flush=True)
    transaction.parents(dest)
    if item["status"] == "copy":
        # Journal the copy before it starts, so undo can remove it.
        transaction.data.setdefault("copies", []).append(dict(path=name, sha256=sha256(source)))
        transaction.save()
        item["verified_copy"] = stream_copy(source, dest)
    else:
        content = source.read_bytes()
        if name.endswith("/categories.json"):
            content = o.json_bytes(category_config)
        transaction.write(name, content)
        if dest.read_bytes() != content:
            raise ValueError("USB update readback failed: " + name)
        dest_stat = dest.stat()
        item["verified_copy"] = dict(
            source_size=stat.st_size,
            source_mtime_ns=stat.st_mtime_ns,
            destination_size=dest_stat.st_size,
            destination_mtime_ns=dest_stat.st_mtime_ns,
            sha256=sha256(dest),
        )
    item["copied"] = True
    return True


def apply_copy(root, target, report, result, operations):
    """Carry out a planned copy under an undo journal on the drive.

    The report is rewritten after every file, so an interrupted copy shows
    how far it got. On any failure the journal is undone (the copy can then
    be rerun); it never deletes destination files or replaces game images.
    """
    backup = outside(report.parent / ("usb-copy-backup-" + uuid.uuid4().hex), root, target)
    backup.mkdir()
    result.update(state="copying", backup=str(backup))
    o.atomic_write(report, o.json_bytes(result))

    transaction = o.Transaction(target)
    result["journal"] = str(transaction.path)
    o.atomic_write(report, o.json_bytes(result))
    try:
        category_config = o.transfer_categories(root, target)
        moves = [(op["source_path"], op["path"]) for op in operations if op["status"] == "move"]
        remaps = move_remaps(target, moves)
        for old, new in moves:
            transaction.move_folder(old, new)
        for name, text in remaps.items():
            transaction.write(name, text)

        for item in sorted(operations, key=copy_order):
            if item["status"] not in ("copy", "update"):
                continue
            if copy_one(item, root, target, transaction, backup, category_config):
                o.atomic_write(report, o.json_bytes(result))

        checked_path(target, ".ohmy3do/saves").mkdir(parents=True, exist_ok=True)
        transaction.data["state"] = "complete"
        transaction.save()
        result["state"] = "complete"
    except BaseException:
        try:
            transaction.undo()
        except (ValueError, OSError) as error:
            result["rollback_error"] = str(error)
        result["state"] = "interrupted"
        o.atomic_write(report, o.json_bytes(result))
        raise
    o.atomic_write(report, o.json_bytes(result))


def prepare_usb(root, target, report, apply=False, verify=False):
    """The prepare-usb command: plan the copy, write the JSON report, and with
    `apply` carry it out. Returns the report's contents."""
    report = outside(report, root, target)
    if report.suffix.lower() != ".json":
        raise ValueError("USB copy report must end in .json")
    receipts = load_receipts(report, root, target)
    operations, games = usb_plan(root, target, verify, receipts)
    result = {
        "source": str(root),
        "destination": str(target),
        "state": "preview",
        "disc_entries": len(games),
        "files": operations,
    }
    o.atomic_write(report, o.json_bytes(result))
    if apply:
        apply_copy(root, target, report, result, operations)

    print("USB files: " + str(dict(o.collections.Counter(op["status"] for op in operations))))
    print("Disc entries: " + str(len(games)) + "; report: " + str(report))
    if not apply:
        print("Preview only; add --apply to copy. No destination files changed.")
    return result


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    commands = parser.add_subparsers(dest="command", required=True)

    update = commands.add_parser(
        "update-media",
        help="fill missing covers/previews for registered library discs; preview by default",
    )
    update.add_argument("root", type=Path)
    update.add_argument("--emu-movies", type=Path, help="EmuMovies root or Panasonic_3DO directory")
    update.add_argument("--cover-dir", type=Path)
    update.add_argument("--preview-dir", type=Path)
    update.add_argument("--cache", type=Path, default=o.default_cache() / "maintenance")
    update.add_argument("--report", type=Path, default=Path("artwork-review.html"))
    update.add_argument("--apply", action="store_true")

    review = commands.add_parser(
        "review", help="create a searchable artwork contact sheet without changing the library"
    )
    review.add_argument("root", type=Path)
    review.add_argument("--report", type=Path, default=Path("artwork-review.html"))

    usb = commands.add_parser(
        "prepare-usb", help="validate staging and preview a copy of runtime files only"
    )
    usb.add_argument("root", type=Path)
    usb.add_argument("destination", type=Path)
    usb.add_argument("--report", type=Path, default=Path("usb-copy-report.json"))
    usb.add_argument("--apply", action="store_true")
    usb.add_argument(
        "--verify-discs",
        action="store_true",
        help="hash existing discs even when size and timestamp agree; reads the entire library",
    )

    args = parser.parse_args()
    try:
        root = real_root(args.root)
        if args.command == "prepare-usb":
            prepare_usb(
                root, real_root(args.destination), args.report, args.apply, args.verify_discs
            )
        elif args.command == "review":
            no_sources = {"cover": [], "preview": []}
            update_media(root, no_sources, o.default_cache() / "maintenance", args.report)
        else:
            if not (args.emu_movies or args.cover_dir or args.preview_dir):
                raise ValueError("Supply --emu-movies, --cover-dir or --preview-dir")
            update_media(
                root,
                media_sources(args.emu_movies, args.cover_dir, args.preview_dir),
                args.cache,
                args.report,
                args.apply,
            )
    except (ValueError, OSError, KeyError, struct.error, zlib.error, o.sqlite3.Error) as error:
        parser.exit(1, "Error: " + str(error) + "\n")


if __name__ == "__main__":
    main()
