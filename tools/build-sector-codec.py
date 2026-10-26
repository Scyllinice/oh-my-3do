#!/usr/bin/env python3
"""Build the optional PC sector accelerator; Windows builds need Clang + LLD.

sector-codec.c rebuilds raw CD sectors (see sector-codec.py) much faster
than Python can. It is built as a small shared library: a .so (Linux), a
.dylib (macOS), or with --windows a Windows DLL that needs no C runtime.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent

# Strict warnings: the code must build cleanly with every compiler used.
WARNINGS = ["-Wall", "-Wextra", "-Werror"]


def build_windows(source, output):
    """Cross-builds a Windows x64 DLL with Clang and LLD: freestanding, with
    no C runtime and no entry point, and a fixed timestamp so the same
    source always gives the same file."""
    clang = shutil.which("clang") or shutil.which("clang-21")
    linker = shutil.which("lld-link") or shutil.which("lld-link-21")
    if not clang or not linker:
        raise ValueError("Windows build requires clang and lld-link")
    obj = output / "sector-codec-win64.obj"
    target = output / "sector-codec-win64.dll"
    compile_flags = ["--target=x86_64-pc-windows-msvc", "-O2", "-ffreestanding", "-fno-builtin"]
    subprocess.run(
        [clang, *compile_flags, "-fno-stack-protector", *WARNINGS, "-c", str(source)]
        + ["-o", str(obj)],
        check=True,
    )
    subprocess.run(
        [linker, "/dll", "/noentry", "/nodefaultlib", "/timestamp:0", "/out:" + str(target)]
        + [str(obj)],
        check=True,
    )
    return target


def build_native(source, output):
    """Builds a shared library for this system with its C compiler (as C89)."""
    on_mac = sys.platform == "darwin"
    target = output / ("sector-codec.dylib" if on_mac else "sector-codec.so")
    subprocess.run(
        ["cc", "-O2", "-std=c89", "-pedantic", *WARNINGS]
        + ["-dynamiclib" if on_mac else "-shared", "-fPIC", str(source), "-o", str(target)],
        check=True,
    )
    return target


def build(output, windows=False):
    """Builds the library into `output` and returns its path."""
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name("sector-codec.c")
    target = build_windows(source, output) if windows else build_native(source, output)
    print(target)
    return target


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/media-native")
    parser.add_argument(
        "--windows", action="store_true", help="cross-build a CRT-free Windows x64 DLL"
    )
    args = parser.parse_args()
    build(args.output, args.windows)
