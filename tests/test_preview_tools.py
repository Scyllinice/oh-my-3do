#!/usr/bin/env python3
"""New preview converter and actual C loader: duration, PCM, bounds and corrupt frames."""
import binascii
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import zlib

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('preview',ROOT/'tools/prepare-preview.py')
converter=importlib.util.module_from_spec(spec);spec.loader.exec_module(converter)
BINARY=ROOT/'build/test_usb_media'


def container(frames,fps=6,audio=True):
    raw=b'\x7c\0'*(64*80)
    blocks=[zlib.compress(raw)]*frames
    offset=(frames+1)*4;table=[offset]
    for block in blocks:offset+=len(block);table.append(offset)
    video=b''.join(struct.pack('>I',x) for x in table)+b''.join(blocks)
    pcm=bytes((((frames*11025+fps-1)//fps+3)//4)*4) if audio else b''
    payload=video+pcm
    return b'OM3DVID2'+struct.pack('>HHIIHHIIII',64,80,len(payload),binascii.crc32(payload),frames,fps,len(video),len(pcm),11025 if pcm else 0,len(pcm))+payload+b'END3DV02'


class PreviewToolsTest(unittest.TestCase):
    def run_loader(self,mode,path,expected=None):
        subprocess.run([str(BINARY),mode,str(path)]+([str(expected)] if expected else []),check=True,stdout=subprocess.PIPE)

    def test_c_player_sound_silent_and_long_clips(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);expected=root/'red.rgb555';expected.write_bytes(b'\x7c\0'*(64*80))
            for frames,fps,sound in ((2,6,True),(2,6,False),(180,6,True),(720,6,False)):
                path=root/'clip.3dv';path.write_bytes(container(frames,fps,sound))
                self.run_loader('video2' if frames==2 else 'video2large',path,expected)

    def test_c_player_rejects_bad_metadata_indexes_zlib_and_checksums(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'bad.3dv';valid=container(2)
            bads=[]
            for pos,value in ((12,2*1024*1024),(24,4),(28,1),(32,22050),(36,4),(40,0),(44,0xffffffff)):
                bad=bytearray(valid);struct.pack_into('>I',bad,pos,value);bads.append(bad)
            # A valid CRC cannot make a broken compressed frame safe.
            bad=bytearray(valid);start=struct.unpack_from('>I',bad,40)[0];bad[40+start]=0
            struct.pack_into('>I',bad,16,binascii.crc32(bad[40:-8]));bads.append(bad)
            # A valid zlib stream with the wrong decoded frame size is rejected.
            small=zlib.compress(b'bad frame')
            video=struct.pack('>II',8,8+len(small))+small
            bad=b'OM3DVID2'+struct.pack('>HHIIHHIIII',64,80,len(video),binascii.crc32(video),1,6,len(video),0,0,0)+video+b'END3DV02'
            bads.append(bad)
            for data in bads:
                path.write_bytes(data);self.run_loader('badvideo2',path)

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'),'FFmpeg is required')
    def test_full_source_audio_and_legacy_conversion(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.mkv';out=root/'preview.3dv'
            subprocess.run(['ffmpeg','-v','error','-nostdin','-f','lavfi','-i','color=c=red:s=64x80:r=6:d=1.5',
                '-f','lavfi','-i','sine=frequency=440:sample_rate=44100:duration=1.5','-c:v','ffv1','-c:a','pcm_s16le','-shortest',str(source)],check=True)
            before=source.read_bytes();result=converter.convert(source,out)
            self.assertEqual(result['frames'],9)
            self.assertAlmostEqual(result['seconds'],1.5)
            self.assertGreater(result['audio_bytes'],0)
            data=out.read_bytes();video_bytes=struct.unpack_from('>I',data,24)[0]
            self.assertTrue(any(data[40+video_bytes:-8]))
            self.run_loader('video2',out)
            converter.convert(source,out,sound=False);self.run_loader('video2',out)
            self.assertEqual(struct.unpack_from('>I',out.read_bytes(),28)[0],0)
            converter.convert(source,out,frames=6);self.run_loader('video',out)
            self.assertEqual(source.read_bytes(),before)

    def test_full_clip_falls_back_to_30_seconds_under_ram_budget(self):
        # Synthetic decoder output is deliberately incompressible, so the
        # converter must shorten and reduce frame rate, rather than overallocate.
        import random
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.mkv';source.write_bytes(b'placeholder')
            random_frame=random.Random(42).randbytes(64*80*3)
            durations=[]
            def run(args,**kwargs):
                if '-show_format' in args:
                    return subprocess.CompletedProcess(args,0,json.dumps({'format':{'duration':'60'},'streams':[{'codec_type':'video'},{'codec_type':'audio'}]}).encode())
                if '-frames:v' in args:
                    durations.append(float(args[args.index('-t')+1]));n=int(args[args.index('-frames:v')+1])
                    return subprocess.CompletedProcess(args,0,random_frame*n)
                return subprocess.CompletedProcess(args,0,b'\x11'*330752)
            with mock.patch.object(converter.shutil,'which',return_value='ffmpeg'),mock.patch.object(converter.subprocess,'run',side_effect=run):
                result=converter.convert(source,root/'preview.3dv')
            self.assertIn(60,durations);self.assertIn(30,durations)
            self.assertLessEqual(result['seconds'],30)
            self.assertLessEqual(result['bytes'],converter.MAX_BYTES)
            self.run_loader('video2large',root/'preview.3dv')


if __name__=='__main__':unittest.main()
