#!/usr/bin/env python3
"""Exercise PNG variants through the actual USB decoder and video/index tools."""
import binascii
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib
ROOT=Path(__file__).resolve().parent.parent


def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/name)
    result=importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


def chunk(kind,data):
    return struct.pack('>I',len(data))+kind+data+struct.pack('>I',binascii.crc32(kind+data))


def png(w,h,color,depth,rows,extra=b'',interlace=0):
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,depth,color,0,0,interlace))+extra+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')


def rgb555(rgba):
    r,g,b,a=rgba
    rgb=[(v*a+bg*(255-a)+127)//255 for v,bg in zip((r,g,b),(8,16,33))]
    return struct.pack('>H',((rgb[0]>>3)<<10)|((rgb[1]>>3)<<5)|(rgb[2]>>3))


def run(binary,kind,path,expected=None):
    subprocess.run([str(binary),kind,str(path)]+([str(expected)] if expected else []),check=True)


def main():
    indexer,converter=module('index-media.py'),module('prepare-preview.py')
    binary=Path(os.environ.get('USB_MEDIA_TEST_BINARY',str(ROOT/'build/test_usb_media')))
    with tempfile.TemporaryDirectory(prefix='oh3do-media-') as folder:
        folder=Path(folder)
        cases=[]
        for color,depth,pixel,rgba,extra in [
            (2,8,b'\xff\x00\x00',(255,0,0,255),b''),
            (6,8,b'\xff\x00\x00\x80',(255,0,0,128),chunk(b'bKGD',b'\0'*6)),
            (0,8,b'\x80',(128,128,128,255),b''),
            (4,8,b'\x80\x80',(128,128,128,128),b''),
            (0,16,b'\x80\x80',(128,128,128,255),b''),
            (3,8,b'\0',(255,0,0,128),chunk(b'PLTE',b'\xff\0\0')+chunk(b'tRNS',b'\x80')),
        ]:
            data=png(64,80,color,depth,(b'\0'+pixel*64)*80,extra)
            cases.append((data,rgb555(rgba)*5120))
        cases.append((png(64,80,0,1,(b'\0'+b'\xff'*8)*80),rgb555((255,255,255,255))*5120))
        cases.append((png(1,1,2,8,b'\0\xff\0\0',interlace=1),
                      rgb555((8,16,33,255))*64*8+rgb555((255,0,0,255))*64*64+rgb555((8,16,33,255))*64*8))
        # All row filters, nonuniform pixels and Adam7 exercise scaling as well.
        w,h=73,91
        sample=lambda x,y: bytes(((x*9)%256,(y*7)%256,(x+y)%256))
        for mode in range(5):
            rows=bytearray(); previous=bytes(w*3)
            for y in range(h):
                row=b''.join(sample(x,y) for x in range(w)); encoded=bytearray()
                for i,v in enumerate(row):
                    a=row[i-3] if i>=3 else 0; b=previous[i]; c=previous[i-3] if i>=3 else 0
                    p=a+b-c; distances=(abs(p-a),abs(p-b),abs(p-c))
                    paeth=(a,b,c)[distances.index(min(distances))]
                    predictor=(0,a,b,(a+b)//2,paeth)[mode]
                    encoded.append((v-predictor)&255)
                rows+=bytes([mode])+encoded; previous=row
            expected=b''.join(rgb555((*sample(x*w//64,y*h//79),255)) for y in range(79) for x in range(64))
            expected+=rgb555((8,16,33,255))*64 # aspect-fit height is 79
            cases.append((png(w,h,2,8,rows),expected))
        rows=bytearray()
        for ox,oy,sx,sy in ((0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)):
            for y in range(oy,80,sy): rows+=b'\0'+b'\xff\0\0'*len(range(ox,64,sx))
        cases.append((png(64,80,2,8,rows,interlace=1),rgb555((255,0,0,255))*5120))
        for i,(data,expected) in enumerate(cases):
            image,output=folder/f'{i}.png',folder/f'{i}.rgb555'
            image.write_bytes(data); output.write_bytes(expected); run(binary,'png',image,output)
        for name,data in [('empty',b''),('oversize',png(1025,1,2,8,b'\0')),('shortpixels',png(64,80,2,8,b'\0\xff\0\0'))]:
            image=folder/(name+'.png'); image.write_bytes(data); run(binary,'badpng',image)
        # Covers preserve every native pixel; old 64x80 covers gain interpolated shades.
        native=folder/'native.png'; expected_native=folder/'native.rgb555'
        sample_native=lambda x,y: ((x*3)%256,(y*2)%256,(x+y)%256)
        native.write_bytes(png(96,120,2,8,b''.join(b'\0'+b''.join(bytes(sample_native(x,y)) for x in range(96)) for y in range(120))))
        expected_native.write_bytes(b''.join(rgb555((*sample_native(x,y),255)) for y in range(120) for x in range(96)))
        run(binary,'cover',native,expected_native)
        checker=folder/'checker.png'
        checker.write_bytes(png(64,80,2,8,b''.join(b'\0'+b''.join(b'\xff\xff\xff' if (x+y)%2==0 else b'\0\0\0' for x in range(64)) for y in range(80))))
        run(binary,'smooth',checker)
        legacy=folder/'legacy.3dc'; red=rgb555((255,0,0,255))*5120
        legacy.write_bytes(b'OM3DCOV1'+struct.pack('>HHIII',64,80,len(red),binascii.crc32(red),0)+red)
        run(binary,'legacycover',legacy)
        payload=rgb555((255,0,0,255))*5120+rgb555((0,0,255,255))*5120
        video=folder/'Game.3dv'; video.write_bytes(b'OM3DVID1'+struct.pack('>HHIIHH',64,80,len(payload),binascii.crc32(payload),2,6)+payload+b'END3DV01')
        run(binary,'video',video)
        black=bytes(32*64*80*2)
        maximal=folder/'maximum.3dv'; maximal.write_bytes(b'OM3DVID1'+struct.pack('>HHIIHH',64,80,len(black),binascii.crc32(black),32,12)+black+b'END3DV01')
        run(binary,'video',maximal)
        usb=folder/'usb'; (usb/'covers').mkdir(parents=True); (usb/'previews').mkdir()
        (usb/'Game.iso').write_bytes(b'synthetic disc'); (usb/'boot.iso').write_bytes(b'')
        bare=folder/'bare'; bare.mkdir(); (bare/'Game.iso').write_bytes(b'synthetic disc'); indexer.index(bare)
        run(binary,'library',bare/'library.3di'); assert not (bare/'covers').exists()
        empty=folder/'empty'; empty.mkdir(); indexer.index(empty); run(binary,'emptylibrary',empty/'library.3di')
        source=usb/'covers/Game.png'; source.write_bytes(cases[0][0]); before=source.read_bytes()
        shutil.copyfile(video,usb/'previews/Game.3dv'); indexer.index(usb)
        assert source.read_bytes()==before
        run(binary,'library',usb/'library.3di')
        run(binary,'index',usb/'covers/index.3di')
        damaged_library=bytearray((usb/'library.3di').read_bytes()); damaged_library[-9]^=1
        (folder/'bad-library.3di').write_bytes(damaged_library); run(binary,'badlibrary',folder/'bad-library.3di')
        for bad_path in [b'../Game.iso\0',b'/Game.iso\0',b'Games//Game.iso\0',b'Game.png\0']:
            body=struct.pack('>H',len(bad_path))+bad_path
            invalid=b'OM3DLIB1'+struct.pack('>IIII',1,len(body),binascii.crc32(body),0)+body+b'END3DL01'
            (folder/'bad-library.3di').write_bytes(invalid); run(binary,'badlibrary',folder/'bad-library.3di')
        # Re-indexing is deterministic and preserves valid converted associations.
        old=(usb/'covers/index.3di').read_bytes(); indexer.index(usb)
        assert (usb/'covers/index.3di').read_bytes()==old
        raw_name='a'*24+'.3dc'; (usb/'covers'/raw_name).write_bytes(b'legacy fixture')
        key=b'Game.iso\0'; name=raw_name.encode()+b'\0'; body=struct.pack('>HH',len(key),len(name))+key+name
        (usb/'covers/index.3di').write_bytes(b'OM3DIDX1'+struct.pack('>IIII',1,len(body),binascii.crc32(body),0)+body)
        source.rename(usb/'covers/Unmatched.png'); indexer.index(usb)
        assert indexer.existing_index(usb/'covers')['game.iso']==raw_name
        damaged=bytearray((usb/'covers/index.3di').read_bytes()); damaged[-1]^=1
        (usb/'covers/index.3di').write_bytes(damaged)
        try: indexer.index(usb)
        except ValueError: pass
        else: raise AssertionError('Corrupt existing index was overwritten')
        assert (usb/'covers/index.3di').read_bytes()==damaged
        source=usb/'covers/Unmatched.png'
        for output in (source,folder/'wrong.mp4'):
            try: converter.convert(source,output)
            except ValueError: pass
            else: raise AssertionError('Invalid preview output path was accepted')
        assert source.read_bytes() == before
        if shutil.which('ffmpeg'):
            clip=folder/'source.mkv'
            subprocess.run(['ffmpeg','-v','error','-nostdin','-f','lavfi','-i','color=c=red:s=64x80:r=6:d=1','-c:v','ffv1',str(clip)],check=True)
            output=folder/'converted.3dv'; converter.convert(clip,output,6,6)
            run(binary,'video',output)
        print('USB media: PNG formats/filters/interlace/scaling/CRC/EOF, preview cancellation/faults and conversion/index roundtrips passed')


if __name__=='__main__': main()
