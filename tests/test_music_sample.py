#!/usr/bin/env python3
"""Validate the saved sample and deterministic, loop-safe generation."""
from pathlib import Path
import struct
import subprocess
import tempfile
import wave
ROOT=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='oh3do-music-') as folder:
    folder=Path(folder);sample=folder/'loop.aiff';wav=folder/'loop.wav'
    subprocess.run(['python3',str(ROOT/'tools/generate-menu-music.py'),'--output',str(sample),'--wav',str(wav)],check=True)
    data=sample.read_bytes()
    assert data==(ROOT/'assets/menu-music.aiff').read_bytes()
    assert data[:4]==b'FORM' and data[8:12]==b'AIFF' and struct.unpack_from('>I',data,4)[0]==len(data)-8
    pos=12;chunks={}
    while pos<len(data):
        kind=data[pos:pos+4];size=struct.unpack_from('>I',data,pos+4)[0]
        chunks[kind]=data[pos+8:pos+8+size];pos+=8+size+(size&1)
    comm=chunks[b'COMM'];channels,frames,bits=struct.unpack_from('>hIh',comm)
    assert (channels,frames,bits)==(1,441000,8) and not frames%4
    assert comm[8:]==bytes.fromhex('400dac44000000000000')
    assert chunks[b'SSND'][:8]==b'\0'*8
    pcm=chunks[b'SSND'][8:];assert len(pcm)==frames
    signed=[value if value<128 else value-256 for value in pcm]
    assert max(abs(v) for v in signed)<100  # Headroom, no hard clipping.
    assert abs(sum(signed)/len(signed))<1
    assert abs(signed[0]-signed[-1])<4  # No large step at the wrap point.
    assert sum(v*v for v in signed)/len(signed)>30
    with wave.open(str(wav)) as stream:
        assert stream.getparams()[:4]==(1,2,22050,frames)
        assert stream.readframes(frames)==b''.join(struct.pack('<h',v*256) for v in signed)
print('Music sample: deterministic AIFF, supported format, headroom, loop seam and matching WAV passed')
