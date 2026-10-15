#!/usr/bin/env python3
"""Export prepared images and a checked binary index into a USB /covers folder."""
import argparse
import binascii
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("build_covers_export", ROOT / "tools/build-covers.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


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
        files[name] = b"OM3DCOV1" + struct.pack(">HHIII", 64, 80, len(pixels), binascii.crc32(pixels), 0) + pixels
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
