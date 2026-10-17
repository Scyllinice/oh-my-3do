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


def container(frames,fps=6,audio=True,width=64,height=80):
    raw=b'\x7c\0'*(width*height)
    blocks=[zlib.compress(raw)]*frames
    offset=(frames+1)*4;table=[offset]
    for block in blocks:offset+=len(block);table.append(offset)
    video=b''.join(struct.pack('>I',x) for x in table)+b''.join(blocks)
    pcm=bytes((((frames*11025+fps-1)//fps+3)//4)*4) if audio else b''
    payload=video+pcm
    return b'OM3DVID2'+struct.pack('>HHIIHHIIII',width,height,len(payload),binascii.crc32(payload),frames,fps,len(video),len(pcm),11025 if pcm else 0,len(pcm))+payload+b'END3DV02'


class PreviewToolsTest(unittest.TestCase):
    def run_loader(self,mode,path,expected=None):
        subprocess.run([str(BINARY),mode,str(path)]+([str(expected)] if expected else []),check=True,stdout=subprocess.PIPE)

    def test_c_player_sound_silent_and_long_clips(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);expected=root/'red.rgb555';expected.write_bytes(b'\x7c\0'*(64*80))
            for frames,fps,sound in ((2,6,True),(2,6,False),(180,6,True),(30,15,True),(720,15,False)):
                path=root/'clip.3dv';path.write_bytes(container(frames,fps,sound))
                self.run_loader('video2' if frames==2 else 'video2large',path,expected)

    def test_landscape_resolution_and_wrong_frame_sizes(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);path=root/'landscape.3dv';expected=root/'red.rgb555'
            path.write_bytes(container(2,15,True,160,120))
            expected.write_bytes(b'\x7c\0'*(160*120))
            self.run_loader('video2',path,expected)
            # A valid CRC cannot make an unsupported dimension or short frame safe.
            original=path.read_bytes()
            for width,height in ((0,120),(161,120),(160,121),(64,80),(65535,65535)):
                bad=bytearray(original);struct.pack_into('>HH',bad,8,width,height);path.write_bytes(bad)
                self.run_loader('badvideo2',path)
            old=bytearray(container(2));struct.pack_into('>HH',old,8,160,120);path.write_bytes(old)
            self.run_loader('badvideo2',path)

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
            bad=bytearray(valid);struct.pack_into('>H',bad,22,16);bads.append(bad)
            legacy_payload=b'\0'*10240
            legacy=b'OM3DVID1'+struct.pack('>HHIIHH',64,80,len(legacy_payload),binascii.crc32(legacy_payload),1,15)+legacy_payload+b'END3DV01'
            bads.append(legacy)
            for data in bads:
                path.write_bytes(data);self.run_loader('badvideo2',path)

    def test_later_frames_decode_on_playback_not_during_startup(self):
        with tempfile.TemporaryDirectory() as temp:
            data=bytearray(container(2))
            second=struct.unpack_from('>I',data,44)[0]
            data[40+second]=0
            struct.pack_into('>I',data,16,binascii.crc32(data[40:-8]))
            path=Path(temp)/'late-bad.3dv';path.write_bytes(data)
            self.run_loader('latebadvideo2',path)

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'),'FFmpeg is required')
    def test_full_source_audio_and_legacy_conversion(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.mkv';out=root/'preview.3dv'
            subprocess.run(['ffmpeg','-v','error','-nostdin','-f','lavfi','-i','color=c=red:s=64x80:r=30:d=2',
                '-f','lavfi','-i','sine=frequency=440:sample_rate=44100:duration=2','-c:v','ffv1','-c:a','pcm_s16le','-shortest',str(source)],check=True)
            before=source.read_bytes();result=converter.convert(source,out)
            self.assertEqual(result['frames'],30)
            self.assertEqual(result['fps'],15)
            self.assertAlmostEqual(result['seconds'],2)
            self.assertGreater(result['audio_bytes'],0)
            data=out.read_bytes();video_bytes=struct.unpack_from('>I',data,24)[0]
            self.assertTrue(any(data[40+video_bytes:-8]))
            self.run_loader('video2',out)
            converter.convert(source,out,sound=False);self.run_loader('video2',out)
            self.assertEqual(struct.unpack_from('>I',out.read_bytes(),28)[0],0)
            converter.convert(source,out,frames=6);self.run_loader('video',out)
            self.assertEqual(source.read_bytes(),before)

    def test_all_conversion_requests_respect_15_second_cap(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.mkv';source.write_bytes(b'placeholder')
            for duration,available,expected in ((None,'60',15),('full','60',15),('30','60',15),('5','60',5),(None,'8',8),(None,None,15)):
                durations=[]
                def run(args,**kwargs):
                    if '-show_format' in args:
                        return subprocess.CompletedProcess(args,0,json.dumps({'format':{'duration':available},'streams':[{'codec_type':'video'},{'codec_type':'audio'}]}).encode())
                    if '-frames:v' in args:
                        durations.append(float(args[args.index('-t')+1]));n=int(args[args.index('-frames:v')+1])
                        return subprocess.CompletedProcess(args,0,b'\0'*(n*160*120*3))
                    return subprocess.CompletedProcess(args,0,b'\0')
                with mock.patch.object(converter.shutil,'which',return_value='ffmpeg'),mock.patch.object(converter.subprocess,'run',side_effect=run):
                    result=converter.convert(source,root/'preview.3dv',duration=duration)
                self.assertEqual(durations,[expected])
                self.assertEqual(result['fps'],15)
                self.assertEqual(result['seconds'],expected)
                self.assertLessEqual(result['frames'],225)
                self.run_loader('video2large',root/'preview.3dv')

    def test_clip_shortens_without_reducing_fps_under_ram_budget(self):
        # Synthetic decoder output is deliberately incompressible, so the
        # converter must shorten at 15 fps, rather than overallocate or lower fps.
        import random
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.mkv';source.write_bytes(b'placeholder')
            random_frame=random.Random(42).randbytes(160*120*3)
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
            self.assertEqual(durations,[15])
            self.assertEqual(result['fps'],15)
            self.assertLess(result['frames'],720)
            self.assertLessEqual(result['seconds'],15)
            self.assertLessEqual(result['bytes'],converter.MAX_BYTES)
            next_count=result['frames']+1
            frame_size=len(zlib.compress(converter.rgb555(random_frame),9))
            next_audio=((next_count*735+3)//4)*4
            self.assertGreater((next_count+1)*4+next_count*frame_size+next_audio,converter.MAX_BYTES)

            self.run_loader('video2large',root/'preview.3dv')


if __name__=='__main__':unittest.main()
