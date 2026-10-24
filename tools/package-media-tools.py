#!/usr/bin/env python3
"""Package the portable Python media tools without SDK or FFmpeg binaries."""
from pathlib import Path
import argparse
import importlib.util
import math
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
       'library-maintenance.py':'tools/library-maintenance.py',
       'library-catalog.py':'tools/library-catalog.py',
       'sector-codec.py':'tools/sector-codec.py',
       'sector-codec.c':'tools/sector-codec.c',
       'build-sector-codec.py':'tools/build-sector-codec.py',
       'prepare-preview.py':'tools/prepare-preview.py',
       'preview-encoder.c':'tools/preview-encoder.c',
       'preview7-encoder.c':'tools/preview7-encoder.c',
       'preview_v7.c':'src/preview_v7.c',
       'preview_v7.h':'src/preview_v7.h',
       'build-preview-encoder.py':'tools/build-preview-encoder.py',
       'resize-covers.py':'tools/resize-covers.py',
       'prepare-audio-disc.py':'tools/prepare-audio-disc.py',
       'README.txt':'assets/media-tools-README.txt','LICENSE':'LICENSE'}
# The preview encoders have no Python fallback; without them, prepare-preview.py
# builds them from preview-encoder.c and preview7-encoder.c (with the menu's
# preview_v7.c beside it) when a C compiler is available.
for name in ('sector-codec-win64.dll','sector-codec.so','sector-codec.dylib',
             'preview-encoder-win64.dll','preview-encoder.so','preview-encoder.dylib',
             'preview7-encoder-win64.dll','preview7-encoder.so','preview7-encoder.dylib'):
    path=args.native_dir/name
    if path.is_file(): files[name]=path
    elif args.require_native and not name.endswith('.dylib'):
        parser.error('Missing release native library: '+str(path))
# Original moving test pattern lets the first console test skip FFmpeg setup.
# It is encoded exactly as prepare-preview.py encodes real clips.
spec=importlib.util.spec_from_file_location('prepare_preview',ROOT/'tools/prepare-preview.py')
converter=importlib.util.module_from_spec(spec); spec.loader.exec_module(converter)
encode=converter.native_encoder(args.native_dir if (args.native_dir/'preview-encoder.so').is_file() else None)
if not encode: parser.error('The preview encoder library is required to build demo-preview.3dv')
rgb=bytearray()
for frame in range(120):
    for y in range(120):
        for x in range(160):
            if (x-frame)%160 < 12: rgb+=bytes((56,208,176))
            elif ((x//8)+(y//8))%2: rgb+=bytes((24,40,72))
            else: rgb+=bytes((8,16,32))
# Original quiet test tone, not background music or downloaded audio.
audio=bytes(int(10*math.sin(2*math.pi*440*i/11025)*min(1,i/220,(44099-i)/220))&255 for i in range(44100))
video=converter.encode_v5(bytes(rgb),120,converter.V5_BYTES-len(audio),encode)
sample=converter.v5_container(video,120,audio)
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
