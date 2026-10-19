#!/usr/bin/env python3
"""Preview converter and the menu's C loader: encoding, duration, PCM, bounds and damage."""
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

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('preview',ROOT/'tools/prepare-preview.py')
converter=importlib.util.module_from_spec(spec);spec.loader.exec_module(converter)
spec=importlib.util.spec_from_file_location('build_preview_encoder',ROOT/'tools/build-preview-encoder.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)


def v5_frames(count):
    """A drifting gradient with a noisy square: motion, smooth areas and detail."""
    import random
    noise=random.Random(7);frames=bytearray()
    for frame in range(count):
        for y in range(120):
            row=bytearray()
            for x in range(160):
                if 40<=x<104 and 30<=y<94:row+=bytes(noise.randrange(256) for _ in range(3))
                else:row+=bytes(((x+frame*3)&255,y&255,((x+y)//2+frame*3)&255))
            frames+=row
    return bytes(frames)


def v5_container(video,frames,audio=True):
    pcm=bytes(converter.audio_bytes(frames)) if audio else b''
    return converter.v5_container(video,frames,pcm)


class PreviewV5Test(unittest.TestCase):
    def test_word_sum_matches_menu_and_desktop(self):
        # a ends 0x80000003, b ends 0x80000007; a ^ rotl(b,16) = 0x80078003. Same vector in desktop/tests/engine.rs.
        self.assertEqual(converter.word_sum(bytes([0,0,0,1,0,0,0,2,0x80,0,0,0])),0x80078003)
        data=converter.v5_container(bytes(41),3,b'')
        self.assertEqual(len(data)-48,44)  # 41 video bytes padded to a word boundary

    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name)
        builder.build(cls.root)
        cls.encode=converter.native_encoder(cls.root)
        cls.frames=100;decoded=__import__('ctypes').create_string_buffer(cls.frames*38400)
        cls.video=converter.encode_v5(v5_frames(cls.frames),cls.frames,1024*1024,cls.encode,decoded)
        cls.expected=cls.root/'expected.rgb555';cls.expected.write_bytes(decoded.raw)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def run_loader(self,binary,mode,data,expected=True):
        path=self.root/'clip.3dv';path.write_bytes(data)
        if expected is True:expected=self.expected
        subprocess.run([str(ROOT/'build'/binary),mode,str(path)]+([str(expected)] if expected else []),check=True)

    def test_encoder_keeps_every_frame_within_the_console_decode_budget(self):
        import ctypes
        library=ctypes.CDLL(str(self.root/builder.library_name()))
        library.oh3do_preview_max_cycles.restype=ctypes.c_double
        frames=v5_frames(12)
        out=ctypes.create_string_buffer(13*4+12*converter.V5_PACKET)
        # Pure noise at the finest lambda wants GRAD everywhere, far past the cap.
        self.assertGreater(self.encode(frames,12,0.01,0,0.01,out,len(out),None),0)
        self.assertLessEqual(library.oh3do_preview_max_cycles(),250000)
        first=out.raw
        self.encode(frames,12,0.01,0,0.01,out,len(out),None)
        self.assertEqual(out.raw,first)  # the same input always gives the same clip

    def test_encoder_reference_matches_menu_decoder_in_every_storage_layout(self):
        self.assertGreater(len(self.video),300*1024)  # more than one host chunk or the tight SDK layout holds
        for audio in (True,False):
            data=v5_container(self.video,self.frames,audio)
            self.run_loader('test_usb_media','video',data)
            self.run_loader('test_usb_media_sdk','video',data)
            self.run_loader('test_usb_media_sdk','budgetvideo',data)

    def test_rejects_bad_headers_tables_and_packets(self):
        valid=v5_container(self.video,self.frames)
        def resealed(data):
            data=bytearray(data);struct.pack_into('>I',data,16,converter.word_sum(bytes(data[40:-8])));return bytes(data)
        # Only version 5 plays; menu 0.16's version 4 clips are rejected.
        bads=[valid[:-8]+b'END3DV04',valid[:7]+b'4'+valid[8:-8]+b'END3DV04',valid[:7]+b'6'+valid[8:-8]+b'END3DV06']
        for offset,fmt,value in ((8,'>H',320),(10,'>H',240),(20,'>H',451),(20,'>H',0),(22,'>H',15),(24,'>I',4),(28,'>I',8),(40,'>I',8),(44,'>I',0)):
            bad=bytearray(valid);struct.pack_into(fmt,bad,offset,value);bads.append(resealed(bad))
        first=struct.unpack_from('>I',valid,40)[0]
        audio=struct.unpack_from('>I',valid,28)[0]  # packets follow the table and the audio
        bad=bytearray(valid);bad[40+audio+first]=0xff;bads.append(resealed(bad))  # first op runs past the frame
        bad=bytearray(valid);bad[50]^=1;bads.append(bytes(bad))  # checksum
        for data in bads:self.run_loader('test_usb_media','badvideo',data,False)
        # A later damaged packet still loads; playback stops at that frame.
        late=struct.unpack_from('>I',valid,40+4*10)[0]
        bad=bytearray(valid);bad[40+audio+late]=0xff;self.run_loader('test_usb_media','latebadvideo',resealed(bad),False)

    def test_converter_reports_a_missing_encoder(self):
        with tempfile.TemporaryDirectory() as temp:
            source=Path(temp)/'source.mkv';source.write_bytes(b'placeholder')
            with mock.patch.object(converter,'native_encoder',return_value=None):
                with self.assertRaisesRegex(ValueError,'encoder library is missing'):
                    converter.convert(source,Path(temp)/'preview.3dv')

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
                    result=converter.convert(source,root/'preview.3dv',duration=duration,encoder=self.encode)
                self.assertEqual(durations,[expected])
                self.assertEqual(result['fps'],30)
                self.assertEqual(result['seconds'],expected)
                self.assertEqual(result['frames'],expected*30)
                self.assertEqual((root/'preview.3dv').read_bytes()[:8],b'OM3DVID5')

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'),'FFmpeg is required')
    def test_converter_fits_budget_with_sound(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.mkv';out=root/'preview.3dv'
            subprocess.run(['ffmpeg','-v','error','-nostdin','-f','lavfi','-i','testsrc=s=320x240:r=30:d=2',
                '-f','lavfi','-i','sine=frequency=440:sample_rate=44100:duration=2','-c:v','ffv1','-c:a','pcm_s16le','-shortest',str(source)],check=True)
            budget=36*1024  # below what best quality needs, so rate control must work
            result=converter.convert(source,out,budget=budget,encoder=self.encode)
            data=out.read_bytes()
            self.assertEqual(data[:8],b'OM3DVID5');self.assertEqual(data[-8:],b'END3DV05')
            self.assertEqual(result['frames'],60);self.assertGreater(result['audio_bytes'],0)
            self.assertLessEqual(result['bytes'],budget)
            self.assertGreater(result['bytes'],budget*0.9)  # the budget is used, not wasted
            self.run_loader('test_usb_media','video',data,False)
            before=source.read_bytes()
            result=converter.convert(source,out,sound=False,encoder=self.encode)
            self.assertEqual(result['audio_bytes'],0);self.assertEqual(struct.unpack_from('>I',out.read_bytes(),28)[0],0)
            self.run_loader('test_usb_media','video',out.read_bytes(),False)
            self.assertEqual(source.read_bytes(),before)


if __name__=='__main__':unittest.main()
