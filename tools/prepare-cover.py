#!/usr/bin/env python3
"""Associate a local image with a USB-relative game path; run via dev-container."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def normalize(path):
    path = path.replace("\\", "/").strip("/")
    parts = path.split("/")
    if not path or any(part in ("", ".", "..") for part in parts):
        raise ValueError("Use a path relative to the USB root, without . or .. components")
    if any(ord(c) < 32 or ord(c) == 127 for c in path) or len(path.encode()) > 8192:
        raise ValueError("Invalid or oversized USB path")
    if not path.lower().endswith((".iso", ".cue")):
        raise ValueError("The USB game path must end in .iso or .cue")
    return path


def fold(path):
    return path.translate(str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", help="USB-relative path including the real .iso/.cue extension")
    parser.add_argument("image", type=Path, help="Image file inside the mounted project")
    args = parser.parse_args()
    game = normalize(args.game)
    if not args.image.is_file():
        parser.error("Image does not exist; put it inside the project before running the container")
    converter = shutil.which("magick") or shutil.which("convert")
    if not converter or not shutil.which("3it"):
        parser.error("Run ./tools/dev-container build, then ./tools/dev-container cover GAME IMAGE")
    directory = ROOT / "assets/covers"
    directory.mkdir(exist_ok=True)
    index = directory / "index.json"
    entries = json.loads(index.read_text()) if index.exists() else {}
    if not isinstance(entries, dict):
        parser.error("assets/covers/index.json must be an object mapping USB paths to CEL filenames")
    name = hashlib.sha256(fold(game).encode()).hexdigest()[:24] + ".cel"
    # Letterbox rather than crop, so portrait and landscape art retain titles.
    with tempfile.TemporaryDirectory(prefix="oh3do-cover-") as temporary:
        png = Path(temporary) / "cover.png"
        cel = Path(temporary) / name
        subprocess.run([converter, str(args.image.resolve()) + "[0]", "-auto-orient",
                        "-thumbnail", "64x80", "-background", "#081021", "-gravity", "center",
                        "-extent", "64x80", "-alpha", "remove", "-alpha", "off", str(png)], check=True)
        subprocess.run(["3it", "to-cel", "--bpp", "16", "--packed", "false",
                        "--ccb-last", "set", "--ccb-bgnd", "set", "-o", str(cel), str(png)], check=True)
        shutil.copyfile(cel, directory / name)
    entries = {key: value for key, value in entries.items() if fold(key) != fold(game)}
    entries[game] = name
    index.write_text(json.dumps(entries, indent=2, ensure_ascii=False) + "\n")
    # Conversion and USB export do not rebuild or alter boot.iso.
    spec_path = Path(__file__).with_name("export-covers.py")
    from importlib.util import module_from_spec, spec_from_file_location
    spec = spec_from_file_location("export_covers", spec_path)
    export = module_from_spec(spec)
    spec.loader.exec_module(export)
    export.export(ROOT, ROOT / "iso/covers")
    print(f"Added cover for {game}. Copy iso/covers into USB .ohmy3do/; no ISO rebuild needed.")


if __name__ == "__main__":
    main()
