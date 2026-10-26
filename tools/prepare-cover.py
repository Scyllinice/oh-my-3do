#!/usr/bin/env python3
"""Associate a local image with a USB-relative game path; run via dev-container.

Usage: prepare-cover.py GAME IMAGE

GAME is the game's path on the USB drive, relative to its root and including
the real .iso or .cue extension (for example "Games/Road Rash.iso"). IMAGE is
any picture ImageMagick can read, stored inside the project so the container
can see it.

The image is letterboxed to 64x80 and converted to a 3DO cel with the SDK's
3it tool, then saved in assets/covers/ under a name derived from the game
path. assets/covers/index.json records which cel belongs to which game.
Finally the whole set is exported to iso/covers/ in the menu's USB format
(see export-covers.py). Nothing here rebuilds boot.iso.
"""
import argparse
import hashlib
from importlib.util import module_from_spec, spec_from_file_location
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

COVER_SIZE = "64x80"

# The menu's dark blue, used for the bars around art that isn't 64x80 in shape.
LETTERBOX_COLOR = "#081021"

# The longest USB path the menu accepts, in bytes of UTF-8.
MAX_PATH_BYTES = 8192

# The cover file name is this many hex digits of the game path's SHA-256.
HASH_DIGITS = 24


def normalize(path):
    """Return a USB-relative game path in the form the menu uses.

    Backslashes become forward slashes and leading or trailing slashes are
    dropped. Raises ValueError for paths that are empty, climb out of the
    drive ("." or ".." parts), contain control characters, are too long, or
    don't end in .iso or .cue.
    """
    path = path.replace("\\", "/").strip("/")
    parts = path.split("/")
    if not path or any(part in ("", ".", "..") for part in parts):
        raise ValueError("Use a path relative to the USB root, without . or .. components")

    has_control_character = any(ord(c) < 32 or ord(c) == 127 for c in path)
    if has_control_character or len(path.encode()) > MAX_PATH_BYTES:
        raise ValueError("Invalid or oversized USB path")

    if not path.lower().endswith((".iso", ".cue")):
        raise ValueError("The USB game path must end in .iso or .cue")
    return path


# Only A-Z are folded, matching the menu's own case-insensitive comparison.
# (str.lower() would also change accented and other non-ASCII letters.)
ASCII_LOWERCASE_TABLE = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def fold(path):
    """Return `path` with ASCII capitals lowered, for case-insensitive matching."""
    return path.translate(ASCII_LOWERCASE_TABLE)


def cover_file_name(game):
    """The cel file name for a game: a hash of its case-folded path, so the
    same game always gets the same file however its path is capitalized."""
    digest = hashlib.sha256(fold(game).encode()).hexdigest()
    return digest[:HASH_DIGITS] + ".cel"


def load_index(index, parser):
    """Read assets/covers/index.json ({game path: cel file name}), or return
    an empty mapping when it doesn't exist yet."""
    if not index.exists():
        return {}
    entries = json.loads(index.read_text())
    if not isinstance(entries, dict):
        parser.error(
            "assets/covers/index.json must be an object mapping USB paths to CEL filenames"
        )
    return entries


def convert_to_cel(converter, image, cel):
    """Turn `image` into a 64x80, 16-bit, unpacked 3DO cel at `cel`.

    ImageMagick letterboxes the picture rather than cropping it, so portrait
    and landscape art keep their titles, and flattens any transparency. 3it
    then writes the cel in the only layout the menu's cover code draws.
    """
    with tempfile.TemporaryDirectory(prefix="oh3do-cover-") as temporary:
        png = Path(temporary) / "cover.png"

        resize_command = [
            converter,
            str(image.resolve()) + "[0]",  # [0]: the first frame of animated images
            "-auto-orient",
            "-thumbnail",
            COVER_SIZE,
            "-background",
            LETTERBOX_COLOR,
            "-gravity",
            "center",
            "-extent",
            COVER_SIZE,
            "-alpha",
            "remove",
            "-alpha",
            "off",
            str(png),
        ]
        subprocess.run(resize_command, check=True)

        cel_command = [
            "3it",
            "to-cel",
            "--bpp",
            "16",
            "--packed",
            "false",
            "--ccb-last",
            "set",
            "--ccb-bgnd",
            "set",
            "-o",
            str(cel),
            str(png),
        ]
        subprocess.run(cel_command, check=True)


def export_to_usb_folder():
    """Run export-covers.py's export, writing iso/covers/ for the USB drive."""
    spec_path = Path(__file__).with_name("export-covers.py")
    spec = spec_from_file_location("export_covers", spec_path)
    export = module_from_spec(spec)
    spec.loader.exec_module(export)
    export.export(ROOT, ROOT / "iso/covers")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", help="USB-relative path including the real .iso/.cue extension")
    parser.add_argument("image", type=Path, help="Image file inside the mounted project")
    args = parser.parse_args()

    # Check the inputs and that the container's tools are available.
    game = normalize(args.game)
    if not args.image.is_file():
        parser.error("Image does not exist; put it inside the project before running the container")
    converter = shutil.which("magick") or shutil.which("convert")
    if not converter or not shutil.which("3it"):
        parser.error("Run ./tools/dev-container build, then ./tools/dev-container cover GAME IMAGE")

    directory = ROOT / "assets/covers"
    directory.mkdir(exist_ok=True)
    index = directory / "index.json"
    entries = load_index(index, parser)

    # Convert in a temporary folder so a failed conversion leaves no
    # half-written cel behind.
    name = cover_file_name(game)
    with tempfile.TemporaryDirectory(prefix="oh3do-cover-") as temporary:
        cel = Path(temporary) / name
        convert_to_cel(converter, args.image, cel)
        shutil.copyfile(cel, directory / name)

    # Replace any entry for the same game, even one spelled with different
    # capitals, so each game has one cover.
    entries = {key: value for key, value in entries.items() if fold(key) != fold(game)}
    entries[game] = name
    index.write_text(json.dumps(entries, indent=2, ensure_ascii=False) + "\n")

    export_to_usb_folder()
    print(f"Added cover for {game}. Copy iso/covers into USB .ohmy3do/; no ISO rebuild needed.")


if __name__ == "__main__":
    main()
