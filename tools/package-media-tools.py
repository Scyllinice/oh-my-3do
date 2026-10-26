#!/usr/bin/env python3
"""Package the portable Python media tools without SDK or FFmpeg binaries.

Usage: package-media-tools.py [OUTPUT] [--native-dir FOLDER] [--require-native]

Writes a zip (iso/media-tools.zip by default) holding the PC-side tools for
preparing a USB library: the Python scripts, the C sources of their native
helpers, any prebuilt helper libraries found in --native-dir, the README and
license, and demo-preview.3dv, a short original test clip. The zip is
reproducible: every entry has a fixed date and permissions.
"""
import argparse
import importlib.util
import math
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parent.parent

# Each file in the package: {name in the zip: path in the checkout}.
SOURCE_FILES = {
    "index-media.py": "tools/index-media.py",
    "ohmy3do.ini": "ohmy3do.ini",
    "categories.example.json": "categories.example.json",
    "organize-library.py": "tools/organize-library.py",
    "library-maintenance.py": "tools/library-maintenance.py",
    "library-catalog.py": "tools/library-catalog.py",
    "sector-codec.py": "tools/sector-codec.py",
    "sector-codec.c": "tools/sector-codec.c",
    "build-sector-codec.py": "tools/build-sector-codec.py",
    "prepare-preview.py": "tools/prepare-preview.py",
    "preview-encoder.c": "tools/preview-encoder.c",
    "preview7-encoder.c": "tools/preview7-encoder.c",
    "preview_v7.c": "src/preview_v7.c",
    "preview_v7.h": "src/preview_v7.h",
    "build-preview-encoder.py": "tools/build-preview-encoder.py",
    "resize-covers.py": "tools/resize-covers.py",
    "prepare-audio-disc.py": "tools/prepare-audio-disc.py",
    "README.txt": "assets/media-tools-README.txt",
    "LICENSE": "LICENSE",
}

# Prebuilt native helpers, included when present in --native-dir.
#
# The preview encoders have no Python fallback; without them, prepare-preview.py
# builds them from preview-encoder.c and preview7-encoder.c (with the menu's
# preview_v7.c beside it) when a C compiler is available. Release packages
# must carry the Windows and Linux builds; macOS builds are optional.
NATIVE_LIBRARIES = (
    "sector-codec-win64.dll",
    "sector-codec.so",
    "sector-codec.dylib",
    "preview-encoder-win64.dll",
    "preview-encoder.so",
    "preview-encoder.dylib",
    "preview7-encoder-win64.dll",
    "preview7-encoder.so",
    "preview7-encoder.dylib",
)

DEMO_PREVIEW_NAME = "demo-preview.3dv"

# The demo clip: 120 frames of 160x120 video (four seconds at 30 fps).
DEMO_FRAMES = 120
DEMO_WIDTH = 160
DEMO_HEIGHT = 120

# A teal bar moving across a dark blue checkerboard.
BAR_COLOR = bytes((56, 208, 176))
BAR_WIDTH = 12
CHECKER_SQUARE = 8
CHECKER_LIGHT = bytes((24, 40, 72))
CHECKER_DARK = bytes((8, 16, 32))

# The demo audio: a quiet 440 Hz tone, 8-bit signed at 11025 Hz.
TONE_HZ = 440
TONE_SAMPLE_RATE = 11025
TONE_SAMPLES = 44100
TONE_AMPLITUDE = 10
TONE_FADE_SAMPLES = 220

# Every zip entry gets the earliest date zip allows and plain rw-r--r--
# permissions, so the same inputs always give the same zip.
ZIP_DATE = (1980, 1, 1, 0, 0, 0)
ZIP_FILE_ATTRIBUTES = 0o100644 << 16


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?", default=ROOT / "iso/media-tools.zip")
    parser.add_argument("--native-dir", type=Path, default=ROOT / "build/media-native")
    parser.add_argument(
        "--require-native",
        action="store_true",
        help="require Windows x64 and Linux accelerators for release packaging",
    )
    return parser, parser.parse_args()


def package_files(parser, native_dir, require_native):
    """Return {name in the zip: source path} for everything except the demo
    clip, adding the native helpers found in `native_dir`."""
    files = dict(SOURCE_FILES)
    for name in NATIVE_LIBRARIES:
        path = native_dir / name
        if path.is_file():
            files[name] = path
        elif require_native and not name.endswith(".dylib"):
            parser.error("Missing release native library: " + str(path))
    return files


def demo_video_frames():
    """Return the demo clip's frames as one block of 8-bit RGB pixels."""
    rgb = bytearray()
    for frame in range(DEMO_FRAMES):
        for y in range(DEMO_HEIGHT):
            for x in range(DEMO_WIDTH):
                # The bar moves one pixel right per frame and wraps around.
                if (x - frame) % DEMO_WIDTH < BAR_WIDTH:
                    rgb += BAR_COLOR
                elif ((x // CHECKER_SQUARE) + (y // CHECKER_SQUARE)) % 2:
                    rgb += CHECKER_LIGHT
                else:
                    rgb += CHECKER_DARK
    return bytes(rgb)


def demo_audio():
    """Return the demo's test tone: an original sine wave, not music or
    downloaded audio, faded in and out to avoid clicks at the ends."""
    samples = bytearray()
    for i in range(TONE_SAMPLES):
        fade = min(1, i / TONE_FADE_SAMPLES, (TONE_SAMPLES - 1 - i) / TONE_FADE_SAMPLES)
        wave = math.sin(2 * math.pi * TONE_HZ * i / TONE_SAMPLE_RATE)
        value = int(TONE_AMPLITUDE * wave * fade)
        samples.append(value & 255)  # store as a signed byte
    return bytes(samples)


def build_demo_preview(parser, native_dir):
    """Encode demo-preview.3dv, a moving test pattern that lets the first
    console test skip FFmpeg setup. It is encoded exactly as
    prepare-preview.py encodes real clips."""
    spec = importlib.util.spec_from_file_location(
        "prepare_preview", ROOT / "tools/prepare-preview.py"
    )
    converter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(converter)

    # Use the libraries in native_dir when they're there; otherwise let
    # prepare-preview.py find or build them.
    has_local_encoder = (native_dir / "preview-encoder.so").is_file()
    encode = converter.native_encoder(native_dir if has_local_encoder else None)
    if not encode:
        parser.error("The preview encoder library is required to build demo-preview.3dv")

    audio = demo_audio()
    video_budget = converter.V5_BYTES - len(audio)
    video = converter.encode_v5(demo_video_frames(), DEMO_FRAMES, video_budget, encode)
    return converter.v5_container(video, DEMO_FRAMES, audio)


def add_entry(archive, name, contents):
    entry = zipfile.ZipInfo(name, date_time=ZIP_DATE)
    entry.compress_type = zipfile.ZIP_DEFLATED
    entry.external_attr = ZIP_FILE_ATTRIBUTES
    archive.writestr(entry, contents)


def check_archive(output, files, sample):
    """Read the finished zip back and check every entry matches its source."""
    with zipfile.ZipFile(output) as archive:
        assert archive.testzip() is None
        assert set(archive.namelist()) == set(files) | {DEMO_PREVIEW_NAME}
        assert archive.read(DEMO_PREVIEW_NAME) == sample
        for name, source in files.items():
            assert archive.read(name) == (ROOT / source).read_bytes()


def main():
    parser, args = parse_arguments()
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)

    files = package_files(parser, args.native_dir, args.require_native)
    sample = build_demo_preview(parser, args.native_dir)

    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, source in files.items():
            add_entry(archive, name, (ROOT / source).read_bytes())
        add_entry(archive, DEMO_PREVIEW_NAME, sample)

    check_archive(output, files, sample)
    print(output)


if __name__ == "__main__":
    main()
