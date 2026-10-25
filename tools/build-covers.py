#!/usr/bin/env python3
"""Validate CEL assets and stage only the offline demo's embedded artwork."""
from pathlib import Path
import shutil
import struct
import sys

from importlib.util import module_from_spec, spec_from_file_location

ROOT = Path(__file__).resolve().parent.parent
spec = spec_from_file_location("prepare_cover", ROOT / "tools/prepare-cover.py")
prepare = module_from_spec(spec)
spec.loader.exec_module(prepare)


def c_string(value):
    # Octal escapes avoid source-encoding assumptions and greedy hex escapes.
    return '"' + "".join(f"\\{byte:03o}" for byte in value.encode()) + '"'


def validate_cel(path):
    data = path.read_bytes()
    # Covers prepared here are one unpacked, uncoded, 16-bit 64x80 cel.
    if len(data) > 16384:
        raise ValueError(f"Cover is too large: {path}")
    offset, found, pixels = 0, False, False
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError(f"Truncated CEL chunk: {path}")
        kind, size = struct.unpack_from(">4sI", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError(f"Invalid CEL chunk: {path}")
        if kind == b"CCB ":
            if found or size != 80:
                raise ValueError(f"Expected one standard CCB: {path}")
            flags = struct.unpack_from(">I", data, offset + 12)[0]
            pre0, pre1 = struct.unpack_from(">II", data, offset + 64)
            width, height = struct.unpack_from(">II", data, offset + 72)
            if (
                (width, height) != (64, 80)
                or not flags & 0x40000000
                or not flags & 0x00400000
                or flags & 0x00000200
                or pre0 & 0x17 != 0x16
                or ((pre0 >> 6) & 0x3FF) + 1 != 80
                or pre1 & 0x800
                or (pre1 & 0x7FF) + 1 != 64
                or (((pre1 >> 16) & 0x3FF) + 2) * 4 != 128
            ):
                raise ValueError(f"Cover must be a single unpacked 16-bit 64x80 cel: {path}")
            found = True
        elif kind == b"PDAT":
            if pixels or size != 8 + 64 * 80 * 2:
                raise ValueError(f"Invalid cover pixel data: {path}")
            pixels = True
        else:
            raise ValueError(f"Unexpected cover chunk: {path}")
        offset += size
    if not found or not pixels:
        raise ValueError(f"Missing CCB or pixel data in cover: {path}")


def main():
    stage, mode = Path(sys.argv[1]), sys.argv[2]
    # Normal browser artwork is entirely external on USB. Only the offline
    # demo has embedded project-branding thumbnails.
    if mode not in ("menu", "demo"):
        raise ValueError("Expected menu or demo build mode")
    rows = []
    destination = stage / "takeme/covers"
    if mode == "demo":
        # Project branding is demonstration art, not a game's box cover.
        source = ROOT / "assets/demo-cover.cel"
        validate_cel(source)
        destination.mkdir(exist_ok=True)
        shutil.copyfile(source, destination / "demo.cel")
        for game in ("Alone in the Dark.iso", "Road Rash.iso", "Disc 1.iso"):
            rows.append((prepare.fold(game).encode(), game, "$boot/covers/demo.cel"))
    rows.sort()
    if sum(len(game.encode()) + len(file.encode()) + 10 for _, game, file in rows) > 128 * 1024:
        raise ValueError("Cover catalogue exceeds the 128 KiB memory budget")
    code = [
        "/* Offline demo catalogue. Normal artwork is loaded from USB. */",
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
