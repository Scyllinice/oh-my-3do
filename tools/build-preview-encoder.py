#!/usr/bin/env python3
"""Build the PC preview encoder library; Windows builds need Clang + LLD."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def library_name(windows=False, stem='preview-encoder'):
    if windows:
        return stem + '-win64.dll'
    return stem + ('.dylib' if sys.platform == 'darwin' else '.so')


# Each library: its source, C standard, and where else its includes live.
LIBRARIES = (('preview-encoder', 'preview-encoder.c', 'c89', ()),
             # The streamed-preview encoder includes the menu's decoder.
             ('preview7-encoder', 'preview7-encoder.c', 'c99', ('src',)))


def source_dir(name):
    """Beside this script in a media-tools package; tools/ and src/ in a checkout."""
    here = Path(__file__).resolve().parent
    return here if (here/name).is_file() else ROOT/'tools'


def include_flags(extra):
    here = Path(__file__).resolve().parent
    return ['-I'+str(here)] + ['-I'+str(ROOT/folder) for folder in extra if (ROOT/folder).is_dir()]


def build_one(output, stem, source_name, standard, extra, windows=False):
    source = source_dir(source_name)/source_name
    target = output/library_name(windows, stem)
    includes = include_flags(extra)
    if windows:
        clang = shutil.which('clang') or shutil.which('clang-21')
        linker = shutil.which('lld-link') or shutil.which('lld-link-21')
        if not clang or not linker:
            raise ValueError('Windows build requires clang and lld-link')
        obj = output/(stem+'-win64.obj')
        subprocess.run([clang, '--target=x86_64-pc-windows-msvc', '-O2', '-ffreestanding',
                        '-fno-builtin', '-fno-stack-protector', '-std='+standard, '-Wall', '-Wextra', '-Werror', '-DOH3DO_PREVIEW_DLL',
                        *includes, '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run([linker, '/dll', '/noentry', '/nodefaultlib', '/timestamp:0',
                        '/out:'+str(target), str(obj)], check=True)
    else:
        subprocess.run(['cc', '-O2', '-std='+standard, '-pedantic', '-Wall', '-Wextra', '-Werror', *includes,
                        '-dynamiclib' if sys.platform == 'darwin' else '-shared', '-fPIC',
                        str(source), '-o', str(target)], check=True)
    print(target)
    return target


def build(output, windows=False):
    output.mkdir(parents=True, exist_ok=True)
    targets = [build_one(output, stem, name, standard, extra, windows) for stem, name, standard, extra in LIBRARIES]
    return targets[0]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/media-native')
    parser.add_argument('--windows', action='store_true', help='cross-build a CRT-free Windows x64 DLL')
    args = parser.parse_args()
    build(args.output, args.windows)
