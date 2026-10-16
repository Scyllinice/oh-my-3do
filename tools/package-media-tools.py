#!/usr/bin/env python3
"""Package the portable Python media tools without SDK or FFmpeg binaries."""
from pathlib import Path
import argparse
import binascii
import struct
import zipfile
ROOT=Path(__file__).resolve().parent.parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path,nargs='?',default=ROOT/'iso/media-tools.zip')
parser.add_argument('--native-dir',type=Path,default=ROOT/'build/media-native')
parser.add_argument('--require-native',action='store_true',help='require Windows x64 and Linux accelerators for release packaging')
args=parser.parse_args()
output=args.output
output.parent.mkdir(parents=True,exist_ok=True)
files={'index-media.py':'tools/index-media.py',
       'ohmy3do.ini':'ohmy3do.ini',
       'categories.example.json':'categories.example.json',
       'organize-library.py':'tools/organize-library.py',
       'library-catalog.py':'tools/library-catalog.py',
       'sector-codec.py':'tools/sector-codec.py',
       'sector-codec.c':'tools/sector-codec.c',
       'build-sector-codec.py':'tools/build-sector-codec.py',
       'prepare-preview.py':'tools/prepare-preview.py',
       'resize-covers.py':'tools/resize-covers.py',
       'prepare-music.py':'tools/prepare-music.py',
       'prepare-audio-disc.py':'tools/prepare-audio-disc.py',
       'README.txt':'assets/media-tools-README.txt','LICENSE':'LICENSE'}
for name in ('sector-codec-win64.dll','sector-codec.so','sector-codec.dylib'):
    path=args.native_dir/name
    if path.is_file(): files[name]=path
    elif args.require_native and name != 'sector-codec.dylib':
        parser.error('Missing release accelerator: '+str(path))
# Original moving test pattern lets the first console test skip FFmpeg setup.
pixels=bytearray()
for frame in range(24):
    for y in range(80):
        for x in range(64):
            if (x-frame*3)%64 < 12: color=(7<<10)|(26<<5)|22
            elif ((x//8)+(y//8))%2: color=(3<<10)|(5<<5)|9
            else: color=(1<<10)|(2<<5)|4
            pixels+=struct.pack('>H',color)
sample=b'OM3DVID1'+struct.pack('>HHIIHH',64,80,len(pixels),binascii.crc32(pixels),24,6)+pixels+b'END3DV01'
with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED) as archive:
    for name,source in files.items():
        entry=zipfile.ZipInfo(name,date_time=(1980,1,1,0,0,0))
        entry.compress_type=zipfile.ZIP_DEFLATED; entry.external_attr=0o100644<<16
        archive.writestr(entry,(ROOT/source).read_bytes())
    entry=zipfile.ZipInfo('demo-preview.3dv',date_time=(1980,1,1,0,0,0))
    entry.compress_type=zipfile.ZIP_DEFLATED; entry.external_attr=0o100644<<16
    archive.writestr(entry,sample)
with zipfile.ZipFile(output) as archive:
    assert archive.testzip() is None
    assert set(archive.namelist())==set(files)|{'demo-preview.3dv'}
    assert archive.read('demo-preview.3dv')==sample
    for name,source in files.items(): assert archive.read(name)==(ROOT/source).read_bytes()
print(output)
