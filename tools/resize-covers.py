#!/usr/bin/env python3
"""Resize local artwork into 96x120 PNG covers for Oh My 3DO.

Usage:
  resize-covers.py IMAGE OUTPUT.png
  resize-covers.py FOLDER OUTPUT_FOLDER [--recursive] [--overwrite]
  resize-covers.py IMAGE_OR_FOLDER --check

Each image is scaled to fit 96x120 without cropping, centered on the menu's
dark blue, and saved as an RGB PNG named after the source. --check only
reports which images still need preparing.

Other tools also use this module: fast_convert writes the menu's own .3dc
cover format directly, and read_cover opens .3dc files on a PC.
"""
import argparse
import binascii
import os
from pathlib import Path
import struct
import tempfile
import zlib

EXTENSIONS = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".tif", ".tiff"}
SIZE = (96, 120)
LARGE_SIZE = (192, 240)  # the menu's full-screen cover
BACKGROUND = (8, 16, 33, 255)

# Output file names: no leading dot, at most this many UTF-8 bytes, and none
# of these characters (FAT drives and the menu can't handle them).
MAX_NAME_BYTES = 96
UNSAFE_NAME_CHARACTERS = "\\:|"

# A .3dc file: a 24-byte header, zlib-compressed RGB555 pixels, an 8-byte footer.
COVER_MAGIC = b"OM3DCOV2"
COVER_FOOTER = b"END3DC02"
COVER_HEADER_SIZE = 24
COVER_FOOTER_SIZE = 8
MAX_COVER_PAYLOAD = 32768
SMALL_COVER_PIXEL_BYTES = SIZE[0] * SIZE[1] * 2  # 23040

ASCII_LOWERCASE_TABLE = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def import_pillow():
    """Return Pillow's Image module, with a helpful error when it's missing."""
    try:
        from PIL import Image
    except ImportError as error:
        raise ValueError("Install Pillow first: python -m pip install Pillow") from error
    return Image


def find_images(source, recursive):
    """The supported images in a folder (and its subfolders if `recursive`),
    sorted, skipping symbolic links."""
    candidates = source.rglob("*") if recursive else source.iterdir()
    return sorted(
        path
        for path in candidates
        if path.is_file() and not path.is_symlink() and path.suffix.lower() in EXTENSIONS
    )


def check_output_name(name):
    has_unsafe_character = any(
        ord(c) < 32 or ord(c) == 127 or c in UNSAFE_NAME_CHARACTERS for c in name
    )
    if name.startswith(".") or len(name.encode("utf-8")) > MAX_NAME_BYTES or has_unsafe_character:
        raise ValueError(f"Unsupported cover filename: {name}")


def check_no_case_twin(target, name):
    """Detect existing names with different case before copying onto FAT
    storage, where "Game.png" and "game.png" are the same file."""
    matches = []
    if target.parent.is_dir():
        matches = [p for p in target.parent.iterdir() if p.name.lower() == name.lower()]
    if any(p.name != name for p in matches):
        raise ValueError(f"Existing output differs only in case: {target}")


def plan(source, output, recursive=False, overwrite=False):
    """Work out every (source image, output PNG) pair, checking all of them
    before anything is written.

    `source` is one image or a folder of them. `output` is a .png file (for
    one image) or a folder. Raises ValueError when two images would produce
    the same cover, an output would replace an input or an existing file
    (without `overwrite`), or a name isn't safe on the USB drive.
    """
    if source.is_symlink():
        raise ValueError("Input must not be a symlink")
    if source.is_file():
        sources = [source]
    elif source.is_dir():
        sources = find_images(source, recursive)
        if output.suffix.lower() == ".png" and not output.is_dir():
            raise ValueError("Folder input needs an output folder")
    else:
        raise ValueError(f"Input does not exist: {source}")
    if not sources:
        raise ValueError("No supported images found")

    output_is_other_file = output.suffix and output.suffix.lower() != ".png" and not output.is_dir()
    if source.is_file() and output_is_other_file:
        raise ValueError("Output must be a .png file or an existing folder")
    single_file = source.is_file() and output.suffix.lower() == ".png" and not output.is_dir()
    if source.is_dir() and output.resolve().is_relative_to(source.resolve()):
        raise ValueError("Use an output folder outside the input folder")

    jobs = []
    seen = set()
    inputs = {path.resolve() for path in sources}
    for image in sources:
        target = output if single_file else output / (image.stem + ".png")
        name = target.name
        check_output_name(name)

        # Two sources mapping to one name (ignoring case) would overwrite each other.
        key = str(target.resolve()).translate(ASCII_LOWERCASE_TABLE)
        if key in seen:
            raise ValueError(f"Multiple images would produce the same cover: {name}")
        seen.add(key)

        if target.resolve() in inputs:
            raise ValueError(
                "Output would replace an input image; use a separate folder or filename"
            )
        if target.is_symlink() or (target.exists() and not target.is_file()):
            raise ValueError(f"Output must be a regular file: {target}")
        check_no_case_twin(target, name)
        if target.exists() and not overwrite:
            raise ValueError(f"Output already exists: {target}; use --overwrite to replace it")
        jobs.append((image, target))
    return jobs


def convert(source, target, size=None):
    """Fit `source` inside `size` (default 96x120) on the background color and
    save it as an RGB PNG at `target`, replacing it in one step."""
    Image = import_pillow()
    from PIL import ImageOps

    size = size or SIZE
    with Image.open(source) as original:
        # Animated inputs use their first frame; EXIF orientation is applied before fitting.
        original.seek(0)
        image = ImageOps.exif_transpose(original).convert("RGBA")
        image = ImageOps.contain(image, size, Image.Resampling.LANCZOS)

        canvas = Image.new("RGBA", size, BACKGROUND)
        left = (size[0] - image.width) // 2
        top = (size[1] - image.height) // 2
        canvas.alpha_composite(image, (left, top))
        result = canvas.convert("RGB")

    # Write a hidden temporary file beside the target, then rename it over the
    # target, so a failed save never leaves a half-written cover.
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


def rgb_to_rgb555(r, g, b):
    """Pack 8-bit red, green and blue into the menu's 16-bit 0RRRRRGGGGGBBBBB."""
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def fast_convert(source, target, size=None):
    """Prepare a native RGB555 cover with bounded zlib payload and EOF marker.
    LARGE_SIZE makes the menu's full-screen cover.

    Layout: "OM3DCOV2", then big-endian width, height (16 bits each),
    compressed size, CRC-32 of the compressed data and uncompressed size
    (32 bits each), then the zlib data and the footer "END3DC02".
    """
    Image = import_pillow()
    size = size or SIZE

    # Resize through convert, so both formats look the same.
    with tempfile.TemporaryDirectory(prefix="oh3do-fast-cover-") as temp:
        png = Path(temp) / "cover.png"
        convert(source, png, size)
        with Image.open(png) as image:
            raw = b"".join(struct.pack(">H", rgb_to_rgb555(r, g, b)) for r, g, b in image.getdata())

    payload = zlib.compress(raw, 9)
    header = struct.pack(
        ">HHIII", size[0], size[1], len(payload), binascii.crc32(payload), len(raw)
    )
    data = COVER_MAGIC + header + payload + COVER_FOOTER

    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".tmp")
    try:
        temporary.write_bytes(data)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def decode_small_cover(data):
    """Check a 96x120 .3dc file's header, CRC and footer and return its RGB555
    pixel bytes. Raises ValueError for anything the menu would reject."""
    if len(data) < COVER_HEADER_SIZE:
        raise ValueError("Truncated cover")
    width, height, payload_size, crc, decoded_size = struct.unpack_from(">HHIII", data, 8)
    if data[:8] != COVER_MAGIC:
        raise ValueError("Invalid prepared cover")

    expected_length = payload_size + COVER_HEADER_SIZE + COVER_FOOTER_SIZE
    if (
        (width, height) != SIZE
        or not 0 < payload_size <= MAX_COVER_PAYLOAD
        or decoded_size != SMALL_COVER_PIXEL_BYTES
        or len(data) != expected_length
        or data[-COVER_FOOTER_SIZE:] != COVER_FOOTER
    ):
        raise ValueError("Invalid native cover")

    payload = data[COVER_HEADER_SIZE:-COVER_FOOTER_SIZE]
    if binascii.crc32(payload) != crc:
        raise ValueError("Invalid native cover CRC")

    # Ask for one byte more than expected, to notice data that is too long,
    # and require the zlib stream to end exactly at the payload's end.
    decoder = zlib.decompressobj()
    raw = decoder.decompress(payload, SMALL_COVER_PIXEL_BYTES + 1)
    if (
        len(raw) != SMALL_COVER_PIXEL_BYTES
        or not decoder.eof
        or decoder.unused_data
        or decoder.unconsumed_tail
    ):
        raise ValueError("Invalid native cover pixels")
    return raw


def read_cover(path):
    """Open prepared covers for PC contact sheets without losing PNG support.

    A .3dc file is decoded to an RGB image; anything else is opened by Pillow.
    """
    Image = import_pillow()
    if path.suffix.lower() != ".3dc":
        return Image.open(path)

    raw = decode_small_cover(path.read_bytes())

    # Expand each 5-bit channel to the full 0-255 range.
    rgb = bytearray()
    for (pixel,) in struct.iter_unpack(">H", raw):
        red = (pixel >> 10) & 31
        green = (pixel >> 5) & 31
        blue = pixel & 31
        rgb.extend((red * 255 // 31, green * 255 // 31, blue * 255 // 31))
    return Image.frombytes("RGB", SIZE, bytes(rgb))


def is_ready_cover(image):
    """Whether an image can be used as it is: a non-interlaced 96x120 RGB PNG."""
    return (
        image.format == "PNG"
        and image.size == SIZE
        and image.mode == "RGB"
        and not image.info.get("interlace")
    )


def check(source, recursive=False):
    """Report each image's size and format without changing anything, and
    return how many still need preparing."""
    Image = import_pillow()
    if source.is_symlink():
        raise ValueError("Input must not be a symlink")
    if source.is_file():
        paths = [source]
    elif source.is_dir():
        paths = find_images(source, recursive)
    else:
        raise ValueError(f"Input does not exist: {source}")
    if not paths:
        raise ValueError("No supported images found")

    needs_resize = 0
    for path in paths:
        with Image.open(path) as image:
            ready = is_ready_cover(image)
            if not ready:
                needs_resize += 1
            status = "ready" if ready else "prepare as 96x120 RGB PNG"
            print(
                f"{path}: {image.width}x{image.height} {image.mode}, "
                f"{path.stat().st_size:,} bytes - {status}"
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
        f"Prepared {len(jobs)} cover(s). Match game basenames, "
        "then run index-media.py against the USB root."
    )


if __name__ == "__main__":
    raise SystemExit(main())
