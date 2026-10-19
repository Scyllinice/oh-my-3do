#!/usr/bin/env python3
"""Build the PC preview encoder library; Windows builds need Clang + LLD."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def library_name(windows=False):
    if windows:
        return 'preview-encoder-win64.dll'
    return 'preview-encoder.dylib' if sys.platform == 'darwin' else 'preview-encoder.so'


def build(output, windows=False):
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name('preview-encoder.c')
    target = output/library_name(windows)
    if windows:
        clang = shutil.which('clang') or shutil.which('clang-21')
        linker = shutil.which('lld-link') or shutil.which('lld-link-21')
        if not clang or not linker:
            raise ValueError('Windows build requires clang and lld-link')
        obj = output/'preview-encoder-win64.obj'
        subprocess.run([clang, '--target=x86_64-pc-windows-msvc', '-O2', '-ffreestanding',
                        '-fno-builtin', '-fno-stack-protector', '-std=c89', '-Wall', '-Wextra', '-Werror', '-DOH3DO_PREVIEW_DLL',
                        '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run([linker, '/dll', '/noentry', '/nodefaultlib', '/timestamp:0',
                        '/out:'+str(target), str(obj)], check=True)
    else:
        subprocess.run(['cc', '-O2', '-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror',
                        '-dynamiclib' if sys.platform == 'darwin' else '-shared', '-fPIC',
                        str(source), '-o', str(target)], check=True)
    print(target)
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/media-native')
    parser.add_argument('--windows', action='store_true', help='cross-build a CRT-free Windows x64 DLL')
    args = parser.parse_args()
    build(args.output, args.windows)
