#!/usr/bin/env python3
"""Export prepared images and a checked binary index into a USB /covers folder.

Usage: export-covers.py [--output FOLDER]

Reads assets/covers/index.json (written by prepare-cover.py), which maps USB
game paths to 64x80 cels in assets/covers/. Each cel is scaled up to 96x120
and saved as a .3dc file, the compressed cover format the menu reads from the
USB drive. An index.3di file lists which .3dc belongs to which game. The
default output folder is iso/covers/, to be copied into .ohmy3do/ on the drive.
"""
import argparse
import binascii
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("build_covers_export", ROOT / "tools/build-covers.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

SOURCE_WIDTH = 64
SOURCE_HEIGHT = 80
OUTPUT_WIDTH = 96
OUTPUT_HEIGHT = 120

MAX_COVERS = 2048

# The memory the menu allows for the index's game rows.
MAX_INDEX_PAYLOAD_BYTES = 128 * 1024

# The .3dc file name is this many hex digits of the game path's SHA-256.
HASH_DIGITS = 24

# Interpolation weights are in 1/256ths (8-bit fixed point).
WEIGHT_ONE = 256

# RGB555 keeps each 5-bit channel at one of these bit positions.
CHANNEL_SHIFTS = (0, 5, 10)
CHANNEL_MASK = 31


def blend_pixels(top_left, top_right, bottom_left, bottom_right, x_weight, y_weight):
    """Mix four RGB555 pixels, one 5-bit channel at a time.

    `x_weight` and `y_weight` (0-255) say how far the sample point lies from
    the left column toward the right one and from the top row toward the
    bottom one. Each channel is rounded to the nearest value.
    """
    color = 0
    for shift in CHANNEL_SHIFTS:
        a = (top_left >> shift) & CHANNEL_MASK
        b = (top_right >> shift) & CHANNEL_MASK
        c = (bottom_left >> shift) & CHANNEL_MASK
        d = (bottom_right >> shift) & CHANNEL_MASK

        top = a * (WEIGHT_ONE - x_weight) + b * x_weight
        bottom = c * (WEIGHT_ONE - x_weight) + d * x_weight

        # The two weights multiply to 65536ths; add half of one to round.
        mixed = top * (WEIGHT_ONE - y_weight) + bottom * y_weight
        channel = (mixed + 32768) >> 16
        color |= channel << shift
    return color


def upscale(pixels):
    """Bilinear 64x80 -> 96x120 RGB555, the filter the menu used for small art.

    `pixels` is the cel's big-endian 16-bit pixel data; the result is the same
    format at the larger size. The first and last output rows and columns land
    exactly on the source's edges, so the picture isn't shifted.
    """
    words = struct.unpack(f">{SOURCE_WIDTH * SOURCE_HEIGHT}H", pixels)
    output = bytearray()

    for y in range(OUTPUT_HEIGHT):
        # The source row this output row falls on, in 1/256ths of a row.
        source_y = y * (SOURCE_HEIGHT - 1) * WEIGHT_ONE // (OUTPUT_HEIGHT - 1)
        row_above = source_y >> 8
        row_below = min(row_above + 1, SOURCE_HEIGHT - 1)
        y_weight = source_y & 255

        for x in range(OUTPUT_WIDTH):
            source_x = x * (SOURCE_WIDTH - 1) * WEIGHT_ONE // (OUTPUT_WIDTH - 1)
            column_left = source_x >> 8
            column_right = min(column_left + 1, SOURCE_WIDTH - 1)
            x_weight = source_x & 255

            color = blend_pixels(
                words[row_above * SOURCE_WIDTH + column_left],
                words[row_above * SOURCE_WIDTH + column_right],
                words[row_below * SOURCE_WIDTH + column_left],
                words[row_below * SOURCE_WIDTH + column_right],
                x_weight,
                y_weight,
            )
            output += struct.pack(">H", color)

    return bytes(output)


def native_cover(raw):
    """A prepared 96x120 .3dc cover: zlib-compressed RGB555 with a CRC and footer.

    Layout: "OM3DCOV2", then big-endian width, height (16 bits each),
    compressed size, CRC-32 of the compressed data and uncompressed size
    (32 bits each), then the zlib data and the footer "END3DC02".
    """
    payload = zlib.compress(raw, 9)
    header = struct.pack(
        ">HHIII",
        OUTPUT_WIDTH,
        OUTPUT_HEIGHT,
        len(payload),
        binascii.crc32(payload),
        len(raw),
    )
    return b"OM3DCOV2" + header + payload + b"END3DC02"


def cel_pixels(cel):
    """Return the 64x80 pixel data from a cel file's PDAT chunk.

    The cel must already have passed build.validate_cel, which guarantees the
    chunks are well formed and that a PDAT chunk exists.
    """
    data = cel.read_bytes()
    offset = 0
    while data[offset : offset + 4] != b"PDAT":
        chunk_size = struct.unpack_from(">I", data, offset + 4)[0]
        offset += chunk_size

    # Skip the chunk's 8-byte header (kind and size).
    start = offset + 8
    return data[start : start + SOURCE_WIDTH * SOURCE_HEIGHT * 2]


def index_row(game, file_name):
    """One index.3di row: the path and file name lengths (16 bits each,
    counting their terminators), then both as NUL-terminated strings."""
    path_bytes = game.encode() + b"\0"
    file_bytes = file_name.encode() + b"\0"
    return struct.pack(">HH", len(path_bytes), len(file_bytes)) + path_bytes + file_bytes


def write_replacing(path, contents):
    """Write a file through a temporary name, so a reader never sees half of it."""
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(contents)
    temporary.replace(path)


def export(root, destination):
    """Convert every cover listed in root/assets/covers/index.json and write
    the .3dc files and index.3di to `destination`.

    Raises ValueError for a malformed index, a duplicate game path (ignoring
    case), a cel name that isn't a plain .cel file name, or a bad cel.
    """
    source = root / "assets/covers/index.json"
    entries = json.loads(source.read_text()) if source.exists() else {}
    if not isinstance(entries, dict) or len(entries) > MAX_COVERS:
        raise ValueError("Cover index must map at most 2048 game paths to prepared CEL files")

    # (case-folded path, row bytes) for the index, and {file name: contents}.
    rows = []
    files = {}
    seen_keys = set()

    for game, cel_name in entries.items():
        game = build.prepare.normalize(game)
        key = build.prepare.fold(game)
        if key in seen_keys:
            raise ValueError(f"Duplicate cover path: {game}")
        seen_keys.add(key)

        # A plain file name, so the index can't point outside assets/covers.
        is_plain_cel_name = (
            isinstance(cel_name, str)
            and Path(cel_name).name == cel_name
            and cel_name.endswith(".cel")
        )
        if not is_plain_cel_name:
            raise ValueError("Prepared covers must have plain .cel basenames")

        cel = root / "assets/covers" / cel_name
        build.validate_cel(cel)

        name = hashlib.sha256(key.encode()).hexdigest()[:HASH_DIGITS] + ".3dc"
        files[name] = native_cover(upscale(cel_pixels(cel)))
        rows.append((key.encode(), index_row(game, name)))

    # The menu searches the index by case-folded path, so keep it sorted.
    rows.sort()
    payload = b"".join(row for _, row in rows)
    if len(payload) > MAX_INDEX_PAYLOAD_BYTES:
        raise ValueError("USB cover index exceeds 128 KiB")

    # Header: "OM3DIDX1", then the row count, payload size, payload CRC-32
    # and a reserved zero (big-endian, 32 bits each).
    header = struct.pack(">IIII", len(rows), len(payload), binascii.crc32(payload), 0)
    index = b"OM3DIDX1" + header + payload

    # Write the covers before the index, so the index never lists a missing file.
    destination.mkdir(parents=True, exist_ok=True)
    for name, contents in files.items():
        write_replacing(destination / name, contents)
    write_replacing(destination / "index.3di", index)
    print(f"Exported {len(rows)} USB cover associations to {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "iso/covers")
    args = parser.parse_args()
    export(ROOT, args.output)


if __name__ == "__main__":
    main()
