#!/usr/bin/env python3
"""Build the PC preview encoder libraries; Windows builds need Clang + LLD.

Usage: build-preview-encoder.py [--output FOLDER] [--windows]

Compiles the two shared libraries prepare-preview.py loads to encode game
preview clips: preview-encoder (the older full-load .3dv format) and
om3dvid7 (the streamed format: the om3dvid/ library's encoder, decoder and
PC reader).
Output goes to build/media-native/ by default.

--windows cross-compiles x64 DLLs that need no C runtime, using clang and
lld-link, so they load on any Windows PC without installing anything.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent

# Each library: its name, its source files, the C standard, the checkout
# folders holding the sources and the headers, and the macro that marks a
# Windows DLL build. In a media-tools package every file sits beside this script.
LIBRARIES = (
    ("preview-encoder", ("preview-encoder.c",), "c89", ("tools",), (), "OH3DO_PREVIEW_DLL"),
    (
        "om3dvid7",
        ("om3dvid7_encoder.c", "om3dvid7_decoder.c", "om3dvid7_reader.c"),
        "c99",
        ("om3dvid/src",),
        ("om3dvid/include",),
        "OM3DVID7_DLL",
    ),
)


def library_name(windows=False, stem="preview-encoder"):
    """The file name of a built library on this platform (or Windows)."""
    if windows:
        return stem + "-win64.dll"
    if sys.platform == "darwin":
        return stem + ".dylib"
    return stem + ".so"


def source_path(name, folders):
    """Beside this script in a media-tools package; in `folders` in a checkout."""
    here = Path(__file__).resolve().parent
    if (here / name).is_file():
        return here / name
    for folder in folders:
        if (ROOT / folder / name).is_file():
            return ROOT / folder / name
    raise ValueError("Missing encoder source: " + name)


def include_flags(extra):
    """-I flags for this script's folder and each `extra` checkout folder
    that exists (in a media-tools package everything sits beside the script)."""
    here = Path(__file__).resolve().parent
    flags = ["-I" + str(here)]
    for folder in extra:
        if (ROOT / folder).is_dir():
            flags.append("-I" + str(ROOT / folder))
    return flags


def build_windows(output, stem, sources, target, standard, includes, dll_macro):
    """Cross-compile `sources` to a Windows x64 DLL at `target`.

    The DLL is freestanding: no C runtime, no entry point and no default
    libraries, so it depends on nothing. The timestamp is zeroed so the same
    source always gives the same file.
    """
    clang = shutil.which("clang") or shutil.which("clang-21")
    linker = shutil.which("lld-link") or shutil.which("lld-link-21")
    if not clang or not linker:
        raise ValueError("Windows build requires clang and lld-link")

    objects = []
    for index, source in enumerate(sources):
        # One object per source; the first keeps the old single-file name.
        suffix = "" if index == 0 else "-" + source.stem
        obj = output / (stem + suffix + "-win64.obj")
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
            "-D" + dll_macro,
            *includes,
            "-c",
            str(source),
            "-o",
            str(obj),
        ]
        subprocess.run(compile_command, check=True)
        objects.append(str(obj))

    link_command = [
        linker,
        "/dll",
        "/noentry",
        "/nodefaultlib",
        "/timestamp:0",
        "/out:" + str(target),
        *objects,
    ]
    subprocess.run(link_command, check=True)


def build_native(sources, target, standard, includes):
    """Compile `sources` to a shared library for this PC with the system cc."""
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
        *[str(source) for source in sources],
        "-o",
        str(target),
    ]
    subprocess.run(command, check=True)


def build_one(output, library, windows=False):
    """Build one library (an entry of LIBRARIES) into `output` and return its path."""
    stem, names, standard, source_folders, include_folders, dll_macro = library
    sources = [source_path(name, source_folders) for name in names]
    target = output / library_name(windows, stem)
    includes = include_flags(include_folders)
    if windows:
        build_windows(output, stem, sources, target, standard, includes, dll_macro)
    else:
        build_native(sources, target, standard, includes)
    print(target)
    return target


def build(output, windows=False):
    """Build every library into `output`; returns the preview-encoder path."""
    output.mkdir(parents=True, exist_ok=True)
    targets = []
    for library in LIBRARIES:
        targets.append(build_one(output, library, windows))
    return targets[0]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/media-native")
    parser.add_argument(
        "--windows", action="store_true", help="cross-build a CRT-free Windows x64 DLL"
    )
    args = parser.parse_args()
    build(args.output, args.windows)
