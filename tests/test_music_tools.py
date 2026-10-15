#!/usr/bin/env python3
"""Round-trip prepared music and exercise actual USB loading without streaming."""
import binascii
import importlib.util
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import wave
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('music_tool',ROOT/'tools/prepare-music.py')
prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
with tempfile.TemporaryDirectory(prefix='oh3do-track-') as directory:
    root=Path(directory);source=root/'piano.wav';target=root/'music/menu.3dm';preview=root/'listen.wav'
    with wave.open(str(source),'wb') as stream:
        stream.setnchannels(1);stream.setsampwidth(2);stream.setframerate(11025)
        stream.writeframes(b''.join(struct.pack('<h',round(9000*math.sin(i*2*math.pi*440/11025))) for i in range(22056)))
    original=source.read_bytes();prepare.convert(source,target,11025,preview)
    assert source.read_bytes()==original
    data=target.read_bytes();rate,frames,size,crc=struct.unpack_from('>IIII',data,8)
    assert data[:8]==b'OM3DMUS2' and data[-8:]==b'END3DM02'
    assert rate==11025 and frames%8==0 and size==frames//2 and len(data)==size+32
    assert binascii.crc32(data[24:-8])==crc
    with wave.open(str(preview)) as stream:
        assert stream.getparams()[:4]==(1,2,11025,frames)
        decoded=struct.unpack('<'+'h'*frames,stream.readframes(frames))
        assert max(abs(x) for x in decoded)<15000
    subprocess.run([str(ROOT/'build/test_usb_music'),str(target)],check=True)
    before=target.read_bytes()
    for args in [(source,source),(source,target,44100),(source,target,11025,source)]:
        try:prepare.convert(*args)
        except ValueError:pass
        else:raise AssertionError('Accepted invalid music conversion')
    assert source.read_bytes()==original and target.read_bytes()==before
    # An independent long-track fixture proves the full 1:49 duration survives.
    long_source=root/'long.wav'
    subprocess.run(['ffmpeg','-v','error','-nostdin','-stream_loop','-1','-i',str(source),'-t','109',str(long_source)],check=True)
    prepare.convert(long_source,target)
    long_data=target.read_bytes();rate,frames,size,crc=struct.unpack_from('>IIII',long_data,8)
    assert abs(frames/rate-109)<0.001 and 580*1024<size<600*1024
    subprocess.run([str(ROOT/'build/test_usb_music'),str(target),'quick'],check=True)
    before=target.read_bytes()
    try:prepare.convert(long_source,target,22050)
    except ValueError:pass
    else:raise AssertionError('Oversize high-rate track should be rejected without truncation')
    assert target.read_bytes()==before
print('Music tools: ADP4 round-trip, complete 109-second track, limits, source protection and USB decoder passed')
