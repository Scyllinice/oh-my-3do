#!/usr/bin/env python3
"""Validate CEL assets and stage only the offline demo's embedded artwork.

Usage: build-covers.py STAGE menu|demo

Writes the menu's built-in cover catalog (STAGE/src/cover_catalog.c). The
normal menu has none: its artwork is read from the USB drive. The offline
demo disc embeds one cover (assets/demo-cover.cel, checked to be a cover
the menu can draw) for its sample games.
"""
from pathlib import Path
import shutil
import struct
import sys

from importlib.util import module_from_spec, spec_from_file_location

ROOT = Path(__file__).resolve().parent.parent
spec = spec_from_file_location("prepare_cover", ROOT / "tools/prepare-cover.py")
prepare = module_from_spec(spec)
spec.loader.exec_module(prepare)

COVER_WIDTH = 64
COVER_HEIGHT = 80
MAX_COVER_BYTES = 16384

# The memory the menu allows for its cover catalog.
MAX_CATALOG_BYTES = 128 * 1024

# The demo's sample games, which all show the demo cover.
DEMO_GAMES = ("Alone in the Dark.iso", "Road Rash.iso", "Disc 1.iso")


def c_string(value):
    # Octal escapes avoid source-encoding assumptions and greedy hex escapes.
    return '"' + "".join(f"\\{byte:03o}" for byte in value.encode()) + '"'


def is_plain_cover_ccb(flags, pre0, pre1, width, height):
    """Whether a CCB describes an unpacked, uncoded 16-bit 64x80 cel (the
    only kind the menu's cover code draws). The checks are on the 3DO cel
    control block's flags and preamble words."""
    return (
        (width, height) == (COVER_WIDTH, COVER_HEIGHT)
        and flags & 0x40000000  # CCB_LAST: the only cel in its list
        and flags & 0x00400000  # CCB_CCBPRE: the preamble is in the CCB
        and not flags & 0x00000200  # not CCB_PACKED
        and pre0 & 0x17 == 0x16  # PRE0_LINEAR (uncoded), 16 bits per pixel
        and ((pre0 >> 6) & 0x3FF) + 1 == COVER_HEIGHT  # PRE0_VCNT: rows - 1
        and not pre1 & 0x800  # not PRE1_LRFORM
        and (pre1 & 0x7FF) + 1 == COVER_WIDTH  # PRE1_TLHPCNT: pixels - 1
        and (((pre1 >> 16) & 0x3FF) + 2) * 4 == COVER_WIDTH * 2  # PRE1_WOFFSET: words - 2
    )


def validate_cel(path):
    """Raises ValueError unless `path` is one standard CCB chunk and its
    pixel data, describing a 64x80 16-bit cover."""
    data = path.read_bytes()
    # Covers prepared here are one unpacked, uncoded, 16-bit 64x80 cel.
    if len(data) > MAX_COVER_BYTES:
        raise ValueError(f"Cover is too large: {path}")

    found_ccb = False
    found_pixels = False
    offset = 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError(f"Truncated CEL chunk: {path}")
        kind, size = struct.unpack_from(">4sI", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError(f"Invalid CEL chunk: {path}")

        if kind == b"CCB ":
            if found_ccb or size != 80:
                raise ValueError(f"Expected one standard CCB: {path}")
            flags = struct.unpack_from(">I", data, offset + 12)[0]
            pre0, pre1 = struct.unpack_from(">II", data, offset + 64)
            width, height = struct.unpack_from(">II", data, offset + 72)
            if not is_plain_cover_ccb(flags, pre0, pre1, width, height):
                raise ValueError(f"Cover must be a single unpacked 16-bit 64x80 cel: {path}")
            found_ccb = True
        elif kind == b"PDAT":
            if found_pixels or size != 8 + COVER_WIDTH * COVER_HEIGHT * 2:
                raise ValueError(f"Invalid cover pixel data: {path}")
            found_pixels = True
        else:
            raise ValueError(f"Unexpected cover chunk: {path}")
        offset += size

    if not found_ccb or not found_pixels:
        raise ValueError(f"Missing CCB or pixel data in cover: {path}")


def main():
    stage, mode = Path(sys.argv[1]), sys.argv[2]
    # Normal browser artwork is entirely external on USB. Only the offline
    # demo has embedded project-branding thumbnails.
    if mode not in ("menu", "demo"):
        raise ValueError("Expected menu or demo build mode")

    # (folded name, game, cover file) for each built-in cover.
    rows = []
    if mode == "demo":
        # Project branding is demonstration art, not a game's box cover.
        source = ROOT / "assets/demo-cover.cel"
        validate_cel(source)
        destination = stage / "takeme/covers"
        destination.mkdir(exist_ok=True)
        shutil.copyfile(source, destination / "demo.cel")
        for game in DEMO_GAMES:
            rows.append((prepare.fold(game).encode(), game, "$boot/covers/demo.cel"))
    rows.sort()

    catalog_bytes = sum(len(game.encode()) + len(file.encode()) + 10 for _, game, file in rows)
    if catalog_bytes > MAX_CATALOG_BYTES:
        raise ValueError("Cover catalog exceeds the 128 KiB memory budget")

    code = [
        "/* Offline demo catalog. Normal artwork is loaded from USB. */",
        '#include "menu_ui.h"',
        "const MenuCover menu_covers[] = {",
    ]
    code.extend(f"    {{{c_string(game)}, {c_string(file)}}}," for _, game, file in rows)
    if not rows:
        code.append("    {NULL, NULL},")
    code.extend(["};", f"const size_t menu_cover_count = {len(rows)};", ""])
    (stage / "src/cover_catalog.c").write_text("\n".join(code))
    print(f"Staged {len(rows)} cover associations")


if __name__ == "__main__":
    main()
