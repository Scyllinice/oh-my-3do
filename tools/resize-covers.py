#!/usr/bin/env python3
"""Resize local artwork into 96x120 PNG covers for Oh My 3DO."""
import argparse
from pathlib import Path
import os
import tempfile

EXTENSIONS = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".tif", ".tiff"}
SIZE = (96, 120)
LARGE_SIZE = (192, 240)
BACKGROUND = (8, 16, 33, 255)


def plan(source, output, recursive=False, overwrite=False):
    if source.is_symlink():
        raise ValueError("Input must not be a symlink")
    if source.is_file():
        sources = [source]
    elif source.is_dir():
        sources = sorted(
            p
            for p in (source.rglob("*") if recursive else source.iterdir())
            if p.is_file() and not p.is_symlink() and p.suffix.lower() in EXTENSIONS
        )
        if output.suffix.lower() == ".png" and not output.is_dir():
            raise ValueError("Folder input needs an output folder")
    else:
        raise ValueError(f"Input does not exist: {source}")
    if not sources:
        raise ValueError("No supported images found")
    if (
        source.is_file()
        and output.suffix
        and output.suffix.lower() != ".png"
        and not output.is_dir()
    ):
        raise ValueError("Output must be a .png file or an existing folder")
    single_file = source.is_file() and output.suffix.lower() == ".png" and not output.is_dir()
    if source.is_dir() and output.resolve().is_relative_to(source.resolve()):
        raise ValueError("Use an output folder outside the input folder")
    jobs, seen = [], set()
    inputs = {p.resolve() for p in sources}
    for image in sources:
        target = output if single_file else output / (image.stem + ".png")
        name = target.name
        if (
            name.startswith(".")
            or len(name.encode("utf-8")) > 96
            or any(ord(c) < 32 or ord(c) == 127 or c in "\\:|" for c in name)
        ):
            raise ValueError(f"Unsupported cover filename: {name}")
        key = str(target.resolve()).translate(
            str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")
        )
        if key in seen:
            raise ValueError(f"Multiple images would produce the same cover: {name}")
        seen.add(key)
        if target.resolve() in inputs:
            raise ValueError(
                "Output would replace an input image; use a separate folder or filename"
            )
        if target.is_symlink() or (target.exists() and not target.is_file()):
            raise ValueError(f"Output must be a regular file: {target}")
        # Detect existing names with different case before copying onto FAT storage.
        matches = []
        if target.parent.is_dir():
            matches = [p for p in target.parent.iterdir() if p.name.lower() == name.lower()]
        if any(p.name != name for p in matches):
            raise ValueError(f"Existing output differs only in case: {target}")
        if target.exists() and not overwrite:
            raise ValueError(f"Output already exists: {target}; use --overwrite to replace it")
        jobs.append((image, target))
    return jobs


def convert(source, target, size=None):
    try:
        from PIL import Image, ImageOps
    except ImportError as error:
        raise ValueError("Install Pillow first: python -m pip install Pillow") from error
    with Image.open(source) as original:
        # Animated inputs use their first frame; EXIF orientation is applied before fitting.
        original.seek(0)
        image = ImageOps.exif_transpose(original).convert("RGBA")
        size = size or SIZE
        image = ImageOps.contain(image, size, Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", size, BACKGROUND)
        canvas.alpha_composite(image, ((size[0] - image.width) // 2, (size[1] - image.height) // 2))
        result = canvas.convert("RGB")
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=".cover-", suffix=".tmp", dir=target.parent, delete=False
        ) as stream:
            temporary = Path(stream.name)
            result.save(stream, format="PNG", optimize=True)
        os.replace(temporary, target)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def large_name(name):
    """The full-screen cover stored beside NAME.png or NAME.3dc."""
    return name[:-4] + "-large.3dc"


def fast_convert(source, target, size=None):
    """Prepare a native RGB555 cover with bounded zlib payload and EOF marker.
    LARGE_SIZE makes the menu's full-screen cover."""
    import binascii, struct, zlib
    from PIL import Image

    with tempfile.TemporaryDirectory(prefix="oh3do-fast-cover-") as temp:
        png = Path(temp) / "cover.png"
        size = size or SIZE
        convert(source, png, size)
        with Image.open(png) as image:
            raw = b"".join(
                struct.pack(">H", ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))
                for r, g, b in image.getdata()
            )
    payload = zlib.compress(raw, 9)
    data = (
        b"OM3DCOV2"
        + struct.pack(">HHIII", size[0], size[1], len(payload), binascii.crc32(payload), len(raw))
        + payload
        + b"END3DC02"
    )
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".tmp")
    try:
        temporary.write_bytes(data)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def read_cover(path):
    """Open prepared covers for PC contact sheets without losing PNG support."""
    import struct, zlib, binascii
    from PIL import Image

    if path.suffix.lower() != ".3dc":
        return Image.open(path)
    data = path.read_bytes()
    if len(data) < 24:
        raise ValueError("Truncated cover")
    w, h, n, crc, decoded = struct.unpack_from(">HHIII", data, 8)
    if data[:8] == b"OM3DCOV2":
        if (
            (w, h) != (96, 120)
            or not 0 < n <= 32768
            or decoded != 23040
            or len(data) != n + 32
            or data[-8:] != b"END3DC02"
        ):
            raise ValueError("Invalid native cover")
        payload = data[24:-8]
        if binascii.crc32(payload) != crc:
            raise ValueError("Invalid native cover CRC")
        decoder = zlib.decompressobj()
        raw = decoder.decompress(payload, 23041)
        if len(raw) != 23040 or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ValueError("Invalid native cover pixels")
    else:
        raise ValueError("Invalid prepared cover")
    rgb = bytearray()
    for (pixel,) in struct.iter_unpack(">H", raw):
        rgb.extend(
            (
                (pixel >> 10 & 31) * 255 // 31,
                (pixel >> 5 & 31) * 255 // 31,
                (pixel & 31) * 255 // 31,
            )
        )
    return Image.frombytes("RGB", (w, h), bytes(rgb))


def check(source, recursive=False):
    try:
        from PIL import Image
    except ImportError as error:
        raise ValueError("Install Pillow first: python -m pip install Pillow") from error
    if source.is_symlink():
        raise ValueError("Input must not be a symlink")
    if source.is_file():
        paths = [source]
    elif source.is_dir():
        paths = sorted(
            p
            for p in (source.rglob("*") if recursive else source.iterdir())
            if p.is_file() and not p.is_symlink() and p.suffix.lower() in EXTENSIONS
        )
    else:
        raise ValueError(f"Input does not exist: {source}")
    if not paths:
        raise ValueError("No supported images found")
    needs_resize = 0
    for path in paths:
        with Image.open(path) as image:
            ready = (
                image.format == "PNG"
                and image.size == SIZE
                and image.mode == "RGB"
                and not image.info.get("interlace")
            )
            if not ready:
                needs_resize += 1
            print(
                f"{path}: {image.width}x{image.height} {image.mode}, {path.stat().st_size:,} bytes - "
                + ("ready" if ready else "prepare as 96x120 RGB PNG")
            )
    print(f"{len(paths)} image(s) checked; {needs_resize} need preparation. No files changed.")
    return needs_resize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Image or folder of images")
    parser.add_argument("output", nargs="?", type=Path, help="Output .png file or covers folder")
    parser.add_argument(
        "--recursive", action="store_true", help="Include subfolders; output is flat"
    )
    parser.add_argument(
        "--overwrite", action="store_true", help="Replace existing output covers (never inputs)"
    )
    parser.add_argument(
        "--check", action="store_true", help="Report dimensions and format without changing files"
    )
    args = parser.parse_args()
    try:
        if args.check:
            if args.output is not None:
                parser.error("--check takes only the input image or folder")
            return 1 if check(args.source, args.recursive) else 0
        if args.output is None:
            parser.error("An output file or folder is required unless --check is used")
        jobs = plan(args.source, args.output, args.recursive, args.overwrite)
        for source, target in jobs:
            convert(source, target)
            print(f"Created {target}: 96x120 RGB PNG")
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(
        f"Prepared {len(jobs)} cover(s). Match game basenames, then run index-media.py against the USB root."
    )


if __name__ == "__main__":
    raise SystemExit(main())
