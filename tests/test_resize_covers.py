#!/usr/bin/env python3
"""Check cover preparation and feed its output through the real menu PNG decoder."""
import contextlib
import io
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
from PIL import Image
ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('resize_covers', ROOT/'tools/resize-covers.py')
resize = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resize)


def rejects(*args, **kwargs):
    try:
        resize.plan(*args, **kwargs)
    except ValueError:
        return
    raise AssertionError('Expected rejected conversion plan')


with tempfile.TemporaryDirectory(prefix='oh3do-resize-') as temporary:
    root = Path(temporary)
    source, output = root/'originals', root/'covers'
    source.mkdir()
    Image.new('RGB', (160, 80), (255, 0, 0)).save(source/'Wide.bmp')
    Image.new('RGBA', (40, 100), (0, 255, 0, 128)).save(source/'Alpha.png')
    exif = Image.Exif(); exif[274] = 6
    Image.new('RGB', (160, 80), (0, 0, 255)).save(source/'Rotate.jpg', exif=exif)
    Image.new('RGB', (80, 100), 'yellow').save(source/'Photo.tiff')
    Image.new('RGB', (64, 80), 'magenta').save(source/'Still.gif', save_all=True,
                                            append_images=[Image.new('RGB', (64, 80), 'cyan')])
    before = {p: p.read_bytes() for p in source.iterdir()}
    audit=subprocess.run(['python3',str(ROOT/'tools/resize-covers.py'),str(source),'--check'],capture_output=True,text=True)
    assert audit.returncode==1 and '160x80' in audit.stdout and 'No files changed' in audit.stdout
    subprocess.run(['python3', str(ROOT/'tools/resize-covers.py'), str(source), str(output)], check=True)
    assert set(p.name for p in output.iterdir()) == {'Wide.png','Alpha.png','Rotate.png','Photo.png','Still.png'}
    assert all(p.read_bytes() == data for p, data in before.items())
    audit=subprocess.run(['python3',str(ROOT/'tools/resize-covers.py'),str(output),'--check'],capture_output=True,text=True)
    assert audit.returncode==0 and '0 need preparation' in audit.stdout
    for path in output.iterdir():
        with Image.open(path) as image:
            assert image.format == 'PNG' and image.mode == 'RGB' and image.size == (96, 120)
            assert path.stat().st_size < 512*1024
            expected = root/'pixels.bin'
            expected.write_bytes(b''.join(struct.pack('>H',((r>>3)<<10)|((g>>3)<<5)|(b>>3))
                                          for r,g,b in image.getdata()))
            subprocess.run([str(ROOT/'build/test_usb_media'), 'cover', str(path), str(expected)], check=True)
    with Image.open(output/'Wide.png') as image:
        assert image.getpixel((48, 0)) == (8,16,33)
        assert image.getpixel((48, 36)) == (255,0,0)
        assert image.getpixel((48, 83)) == (255,0,0)
        assert image.getpixel((48, 84)) == (8,16,33)
    with Image.open(output/'Alpha.png') as image:
        assert image.getpixel((0, 60)) == (8,16,33)
        assert image.getpixel((48, 60)) == (4,136,16)
    with Image.open(output/'Rotate.png') as image:
        assert image.getpixel((0, 60)) == (8,16,33)  # Portrait after EXIF rotation.
        assert image.getpixel((48, 0))[2] > 240
    with Image.open(output/'Still.png') as image:
        assert image.getpixel((48, 60)) == (255,0,255)
    # Prepared RGB555 covers must render exactly like the PNG console path.
    import binascii, zlib
    for name in ('Wide','Alpha','Rotate','Photo','Still'):
        png=output/(name+'.png');fast=output/(name+'.3dc')
        resize.fast_convert(png,fast)
        with Image.open(png) as image:
            raw=b''.join(struct.pack('>H',((r>>3)<<10)|((g>>3)<<5)|(b>>3)) for r,g,b in image.convert('RGB').getdata())
        expected=root/'expected.rgb555';expected.write_bytes(raw)
        subprocess.run([str(ROOT/'build/test_usb_media'),'fastcover',str(fast),str(expected)],check=True)
        assert resize.read_cover(fast).size==(96,120)
        valid=fast.read_bytes()
        for pos in (8,12,16,20,24,len(valid)-1):
            bad=bytearray(valid);bad[pos]^=1;fast.write_bytes(bad)
            subprocess.run([str(ROOT/'build/test_usb_media'),'badfastcover',str(fast)],check=True)
        fast.write_bytes(valid)
    # The new format also participates in basename associations.
    fastusb=root/'fast-usb';(fastusb/'covers').mkdir(parents=True)
    (fastusb/'Game.iso').write_bytes(b'synthetic disc')
    resize.fast_convert(output/'Wide.png',fastusb/'covers/Game.3dc')
    subprocess.run(['python3',str(ROOT/'tools/index-media.py'),str(fastusb)],check=True)
    subprocess.run([str(ROOT/'build/test_usb_media'),'index',str(fastusb/'covers/index.3di'),'Game.3dc'],check=True)
    # Indexing reports oversized PNGs without rewriting artwork.
    index_spec=importlib.util.spec_from_file_location('index_media',ROOT/'tools/index-media.py')
    indexer=importlib.util.module_from_spec(index_spec); index_spec.loader.exec_module(indexer)
    usb=root/'audit-usb'; (usb/'covers').mkdir(parents=True); (usb/'Large.iso').write_bytes(b'synthetic disc')
    large=usb/'covers/Large.png'; Image.new('RGB',(256,320),'red').save(large)
    original=large.read_bytes(); messages=io.StringIO()
    with contextlib.redirect_stdout(messages): indexer.index(usb)
    assert '256x320' in messages.getvalue() and 'resize-covers.py' in messages.getvalue()
    assert large.read_bytes()==original
    rejects(source, output)
    rejects(source/'Alpha.png', source/'Alpha.png', overwrite=True)
    rejects(source, source/'resized')
    rejects(source, output/'file.png')
    Image.new('RGB',(2,2)).save(source/'Wide.png')
    rejects(source, root/'new-covers')
    assert not (root/'new-covers').exists()
    (source/'Wide.png').unlink()
    nested=source/'nested';nested.mkdir()
    Image.new('RGB',(8,8)).save(nested/'Another.png')
    assert len(resize.plan(source, root/'new-covers')) == 5
    assert len(resize.plan(source, root/'new-covers', recursive=True)) == 6
    assert len(resize.plan(source, output, overwrite=True)) == 5
    rejects(source/'Wide.bmp', output/'WIDE.png', overwrite=True)
    rejects(source/'Wide.bmp', root/'wrong.jpg')
    renamed=root/'custom'/'Game Title.png'
    subprocess.run(['python3', str(ROOT/'tools/resize-covers.py'), str(source/'Wide.bmp'), str(renamed)], check=True)
    assert renamed.exists()
    usb=root/'usb';(usb/'Games').mkdir(parents=True)
    (usb/'Games'/'Game.iso').write_bytes(b'synthetic disc')
    subprocess.run(['python3', str(ROOT/'tools/resize-covers.py'), str(source/'Wide.bmp'), str(usb/'covers'/'Game.png')], check=True)
    subprocess.run(['python3', str(ROOT/'tools/index-media.py'), str(usb)], check=True)
    subprocess.run([str(ROOT/'build/test_usb_media'), 'index', str(usb/'covers'/'index.3di')], check=True)
print('Cover resizer: formats, aspect ratio, alpha, EXIF, batch names, source protection and actual C decoding passed')
