#!/usr/bin/env python3
"""Prepare a whole track for RAM playback; no USB streaming or ISO rebuild."""
import argparse
import binascii
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import wave
MAX_BYTES = 768*1024
ROOT=Path(__file__).resolve().parent.parent


def convert(source, output, rate=11025, preview=None):
    if not source.is_file() or rate not in (11025,22050):
        raise ValueError('Use an existing audio file and rate 11025 or 22050')
    if output.suffix.lower()!='.3dm' or source.resolve()==output.resolve():
        raise ValueError('Choose a separate .3dm output file')
    if preview and (preview.suffix.lower()!='.wav' or preview.resolve() in (source.resolve(),output.resolve())):
        raise ValueError('Choose a separate .wav listening-copy file')
    ffmpeg=shutil.which('ffmpeg')
    encoder=os.environ.get('OH3DO_3AT') or shutil.which('3at')
    if not ffmpeg or not encoder:
        raise ValueError('FFmpeg and 3at are required; rebuild the Docker image or put both tools on PATH')
    with tempfile.TemporaryDirectory(prefix='oh3do-music-') as directory:
        directory=Path(directory);raw=directory/'music.raw'
        # Read at most one frame beyond capacity, so oversized tracks are rejected
        # instead of silently truncated. Tiny fades soften the whole-song wrap.
        subprocess.run([ffmpeg,'-v','error','-nostdin','-t',str((MAX_BYTES*2+16)/rate),'-i',str(source.resolve()),'-vn','-sn','-ac','1','-af',
            f'aresample={rate},afade=t=in:d=0.02,areverse,afade=t=in:d=0.02,areverse',
            '-f','s16le',str(raw)],check=True)
        pcm=raw.read_bytes()
        if not pcm or len(pcm)%2: raise ValueError('Input produced no complete audio samples')
        frames=(len(pcm)//2+7)//8*8
        if frames//2>MAX_BYTES:
            raise ValueError(f'Track exceeds {MAX_BYTES*2/rate:.1f} seconds at {rate} Hz; choose a shorter track or the 11025 Hz preset')
        raw.write_bytes(pcm+b'\0'*(frames*2-len(pcm)))
        subprocess.run([encoder,'to-adp4','--input-type','raw','--output-type','raw',str(raw)],check=True,stdout=subprocess.DEVNULL)
        encoded=raw.with_name(raw.name+'.adp4.1ch.22050hz.raw')
        payload=encoded.read_bytes()
        if len(payload)!=frames//2: raise ValueError('Unexpected ADP4 output length')
        data=b'OM3DMUS2'+struct.pack('>IIII',rate,frames,len(payload),binascii.crc32(payload))+payload+b'END3DM02'
        preview_data=None
        if preview:
            subprocess.run([encoder,'from-adp4','--output-type','raw',str(encoded)],check=True,stdout=subprocess.DEVNULL)
            preview_data=encoded.with_name(encoded.name+'.raw').read_bytes()
            if len(preview_data)!=frames*2: raise ValueError('Unexpected ADP4 preview length')
        output.parent.mkdir(parents=True,exist_ok=True)
        temporary=output.with_name(output.name+'.tmp')
        try:
            temporary.write_bytes(data);temporary.replace(output)
        finally:
            temporary.unlink(missing_ok=True)
        if preview:
            preview.parent.mkdir(parents=True,exist_ok=True)
            with wave.open(str(preview),'wb') as stream:
                stream.setnchannels(1);stream.setsampwidth(2);stream.setframerate(rate);stream.writeframes(preview_data)
    print(f'Created {output}: {frames/rate:.2f} seconds, {rate} Hz mono ADP4, {len(payload)/1024:.1f} KiB RAM')
    print('Copy to USB .ohmy3do/music/menu.3dm, then cold boot or toggle music off and on. No media index needed.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path,nargs='?',default=ROOT/'iso/.ohmy3do/music/menu.3dm')
    parser.add_argument('--rate',type=int,choices=(11025,22050),default=11025)
    parser.add_argument('--preview',type=Path,help='Optional decoded WAV for checking the converted sound')
    args=parser.parse_args()
    try: convert(args.source,args.output,args.rate,args.preview)
    except (ValueError,OSError,subprocess.CalledProcessError) as error: parser.error(str(error))


if __name__=='__main__': main()
