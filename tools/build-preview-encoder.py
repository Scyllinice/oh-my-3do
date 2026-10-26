#!/usr/bin/env python3
"""Build the PC preview encoder libraries; Windows builds need Clang + LLD.

Usage: build-preview-encoder.py [--output FOLDER] [--windows]

Compiles the two shared libraries prepare-preview.py loads to encode game
preview clips: preview-encoder (the full-load .3dv format) and
preview7-encoder (the streamed format, which includes the menu's own
decoder from src/). Output goes to build/media-native/ by default.

--windows cross-compiles x64 DLLs that need no C runtime, using clang and
lld-link, so they load on any Windows PC without installing anything.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent

# Each library: its name, source file, C standard, and the checkout folders
# that hold its other includes.
LIBRARIES = (
    ("preview-encoder", "preview-encoder.c", "c89", ()),
    # The streamed-preview encoder includes the menu's decoder.
    ("preview7-encoder", "preview7-encoder.c", "c99", ("src",)),
)


def library_name(windows=False, stem="preview-encoder"):
    """The file name of a built library on this platform (or Windows)."""
    if windows:
        return stem + "-win64.dll"
    if sys.platform == "darwin":
        return stem + ".dylib"
    return stem + ".so"


def source_dir(name):
    """Beside this script in a media-tools package; tools/ and src/ in a checkout."""
    here = Path(__file__).resolve().parent
    if (here / name).is_file():
        return here
    return ROOT / "tools"


def include_flags(extra):
    """-I flags for this script's folder and each `extra` checkout folder
    that exists (in a media-tools package everything sits beside the script)."""
    here = Path(__file__).resolve().parent
    flags = ["-I" + str(here)]
    for folder in extra:
        if (ROOT / folder).is_dir():
            flags.append("-I" + str(ROOT / folder))
    return flags


def build_windows(output, stem, source, target, standard, includes):
    """Cross-compile `source` to a Windows x64 DLL at `target`.

    The DLL is freestanding: no C runtime, no entry point and no default
    libraries, so it depends on nothing. The timestamp is zeroed so the same
    source always gives the same file.
    """
    clang = shutil.which("clang") or shutil.which("clang-21")
    linker = shutil.which("lld-link") or shutil.which("lld-link-21")
    if not clang or not linker:
        raise ValueError("Windows build requires clang and lld-link")

    obj = output / (stem + "-win64.obj")
    compile_command = [
        clang,
        "--target=x86_64-pc-windows-msvc",
        "-O2",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-std=" + standard,
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DOH3DO_PREVIEW_DLL",
        *includes,
        "-c",
        str(source),
        "-o",
        str(obj),
    ]
    subprocess.run(compile_command, check=True)

    link_command = [
        linker,
        "/dll",
        "/noentry",
        "/nodefaultlib",
        "/timestamp:0",
        "/out:" + str(target),
        str(obj),
    ]
    subprocess.run(link_command, check=True)


def build_native(source, target, standard, includes):
    """Compile `source` to a shared library for this PC with the system cc."""
    shared_flag = "-dynamiclib" if sys.platform == "darwin" else "-shared"
    command = [
        "cc",
        "-O2",
        "-std=" + standard,
        "-pedantic",
        "-Wall",
        "-Wextra",
        "-Werror",
        *includes,
        shared_flag,
        "-fPIC",
        str(source),
        "-o",
        str(target),
    ]
    subprocess.run(command, check=True)


def build_one(output, stem, source_name, standard, extra, windows=False):
    """Build one library into `output` and return its path."""
    source = source_dir(source_name) / source_name
    target = output / library_name(windows, stem)
    includes = include_flags(extra)
    if windows:
        build_windows(output, stem, source, target, standard, includes)
    else:
        build_native(source, target, standard, includes)
    print(target)
    return target


def build(output, windows=False):
    """Build every library into `output`; returns the preview-encoder path."""
    output.mkdir(parents=True, exist_ok=True)
    targets = []
    for stem, source_name, standard, extra in LIBRARIES:
        targets.append(build_one(output, stem, source_name, standard, extra, windows))
    return targets[0]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/media-native")
    parser.add_argument(
        "--windows", action="store_true", help="cross-build a CRT-free Windows x64 DLL"
    )
    args = parser.parse_args()
    build(args.output, args.windows)
