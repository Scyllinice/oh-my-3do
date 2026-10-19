#!/usr/bin/env python3
"""Export prepared images and a checked binary index into a USB /covers folder."""
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


def upscale(pixels):
    """Bilinear 64x80 -> 96x120 RGB555, the filter the menu used for small art."""
    words = struct.unpack(">5120H", pixels)
    out = bytearray()
    for y in range(120):
        sy = y * 79 * 256 // 119
        y0 = sy >> 8; y1 = min(y0 + 1, 79); fy = sy & 255
        for x in range(96):
            sx = x * 63 * 256 // 95
            x0 = sx >> 8; x1 = min(x0 + 1, 63); fx = sx & 255
            colors = (words[y0 * 64 + x0], words[y0 * 64 + x1], words[y1 * 64 + x0], words[y1 * 64 + x1])
            color = 0
            for shift in (0, 5, 10):
                c = [(value >> shift) & 31 for value in colors]
                top = c[0] * (256 - fx) + c[1] * fx
                bottom = c[2] * (256 - fx) + c[3] * fx
                color |= ((top * (256 - fy) + bottom * fy + 32768) >> 16) << shift
            out += struct.pack(">H", color)
    return bytes(out)


def native_cover(raw):
    """A prepared 96x120 .3dc cover: zlib-compressed RGB555 with a CRC and footer."""
    payload = zlib.compress(raw, 9)
    return b"OM3DCOV2" + struct.pack(">HHIII", 96, 120, len(payload), binascii.crc32(payload), len(raw)) + payload + b"END3DC02"


def export(root, destination):
    source = root / "assets/covers/index.json"
    entries = json.loads(source.read_text()) if source.exists() else {}
    if not isinstance(entries, dict) or len(entries) > 2048:
        raise ValueError("Cover index must map at most 2048 game paths to prepared CEL files")
    rows, files, seen = [], {}, set()
    for game, filename in entries.items():
        game = build.prepare.normalize(game)
        key = build.prepare.fold(game)
        if key in seen:
            raise ValueError(f"Duplicate cover path: {game}")
        seen.add(key)
        if not isinstance(filename, str) or Path(filename).name != filename or not filename.endswith(".cel"):
            raise ValueError("Prepared covers must have plain .cel basenames")
        cel = root / "assets/covers" / filename
        build.validate_cel(cel)
        data, pos = cel.read_bytes(), 0
        while data[pos:pos + 4] != b"PDAT":
            pos += struct.unpack_from(">I", data, pos + 4)[0]
        pixels = data[pos + 8:pos + 8 + 64 * 80 * 2]
        name = hashlib.sha256(key.encode()).hexdigest()[:24] + ".3dc"
        files[name] = native_cover(upscale(pixels))
        path_bytes, file_bytes = game.encode() + b"\0", name.encode() + b"\0"
        row = struct.pack(">HH", len(path_bytes), len(file_bytes)) + path_bytes + file_bytes
        rows.append((key.encode(), row))
    rows.sort()
    payload = b"".join(row for _, row in rows)
    if len(payload) > 128 * 1024:
        raise ValueError("USB cover index exceeds 128 KiB")
    index = b"OM3DIDX1" + struct.pack(">IIII", len(rows), len(payload), binascii.crc32(payload), 0) + payload
    destination.mkdir(parents=True, exist_ok=True)
    for name, contents in files.items():
        temporary = destination / (name + ".tmp")
        temporary.write_bytes(contents)
        temporary.replace(destination / name)
    temporary = destination / "index.3di.tmp"
    temporary.write_bytes(index)
    temporary.replace(destination / "index.3di")
    print(f"Exported {len(rows)} USB cover associations to {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "iso/covers")
    args = parser.parse_args()
    export(ROOT, args.output)


if __name__ == "__main__":
    main()
