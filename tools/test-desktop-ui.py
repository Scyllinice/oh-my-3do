#!/usr/bin/env python3
"""Native Tauri smoke test. Run under Xvfb/D-Bus with the desktop-tauri Docker image.
Creates disposable fixtures under build/desktop-ui-smoke. --library captures a
read-only real-library screenshot instead of exercising fixture transfers.
"""
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary',type=__import__('pathlib').Path)
parser.add_argument('--library',type=__import__('pathlib').Path)
parser.add_argument('--gallery',action='store_true')
parser.add_argument('--features',action='store_true',help='Exercise save comparison and the read-only drive check')
parser.add_argument('--fixture',type=__import__('pathlib').Path)
args=parser.parse_args()
import os,time,json,subprocess,shutil,struct,zlib
from pathlib import Path
R=Path(__file__).resolve().parents[1];fixture=args.fixture or R/'build/desktop-ui-smoke';source=fixture/'library';dest=fixture/'usb';cache=fixture/'cache';shots=fixture/'shots'
for p in [source,dest,cache/'oh-my-3do',shots]:p.mkdir(parents=True,exist_ok=True)
paths=['Games/USA/Example/Disc 1.iso','Games/USA/Example/Disc 2.iso','Games/USA/Other/Other.iso','Games/Japan/Third/Third.iso']
for name in paths:
 p=source/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(name.encode()*128)
shutil.copyfile(R/'iso/boot.iso',source/'boot.iso');support=source/'.ohmy3do';support.mkdir(exist_ok=True);shutil.copyfile(R/'ohmy3do.ini',support/'ohmy3do.ini')
def st(s):b=s.encode()+b'\0';return struct.pack('>H',len(b))+b
def ix(m,e,p,n):return m+struct.pack('>IIII',n,len(p),zlib.crc32(p),0)+p+e
paths.sort(key=str.lower);(support/'library.3di').write_bytes(ix(b'OM3DLIB1',b'END3DL01',b''.join(st(p) for p in paths),len(paths)));(support/'metadata.3di').write_bytes(ix(b'OM3DMET1',b'END3DM01',b''.join(st(p)+st('')*3 for p in paths),len(paths)))
records=[dict(path=p,title='Example (Disc 1)' if 'Disc 1' in p else 'Example (Disc 2)' if 'Disc 2' in p else Path(p).stem,category='Games',region='Japan' if 'Japan' in p else 'USA',metadata={}) for p in paths]
(support/'library.json').write_text(json.dumps(dict(schema=1,sources=[],games=records)))
(cache/'oh-my-3do/native-settings.json').write_text(json.dumps(dict(library=str(source),destination=str(dest),offline=True,disable_update_checks=True)))
os.environ.update(XDG_CACHE_HOME=str(cache),XDG_CONFIG_HOME=str(fixture/'config'),XDG_DATA_HOME=str(fixture/'data'),WEBKIT_DISABLE_DMABUF_RENDERER='1',LIBGL_ALWAYS_SOFTWARE='1')
if args.library:
 prefs=json.loads((cache/'oh-my-3do/native-settings.json').read_text());prefs['library']=str(args.library);(cache/'oh-my-3do/native-settings.json').write_text(json.dumps(prefs))

import ctypes
from PIL import ImageGrab
os.environ.pop('TAURI_WEBVIEW_AUTOMATION',None)
x=ctypes.CDLL('libX11.so.6');t=ctypes.CDLL('libXtst.so.6');x.XOpenDisplay.restype=ctypes.c_void_p;display=x.XOpenDisplay(None)
t.XTestFakeMotionEvent.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_ulong];t.XTestFakeButtonEvent.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_int,ctypes.c_ulong];x.XFlush.argtypes=[ctypes.c_void_p]
def click(px,py):
 t.XTestFakeMotionEvent(display,-1,px,py,0);t.XTestFakeButtonEvent(display,1,1,0);t.XTestFakeButtonEvent(display,1,0,0);x.XFlush(display);time.sleep(.35)
def shot(name):
 image=ImageGrab.grab(xdisplay=os.environ['DISPLAY']);image.save(str(shots/name));return image
def primary():
 image=ImageGrab.grab(xdisplay=os.environ['DISPLAY']);rows=[]
 for y in range(80,900):
  points=[xx for xx in range(185,1280) if image.getpixel((xx,y))[:3]==(46,102,63)]
  if len(points)>50:rows.append((y,min(points),max(points)))
 assert rows,'Primary action not visible'
 bottom=rows[-1][0];last=[r for r in rows if bottom-r[0]<12];y,left,right=last[len(last)//2];click((left+right)//2,y)
x.XStringToKeysym.argtypes=[ctypes.c_char_p];x.XStringToKeysym.restype=ctypes.c_ulong;x.XKeysymToKeycode.argtypes=[ctypes.c_void_p,ctypes.c_ulong];x.XKeysymToKeycode.restype=ctypes.c_uint
t.XTestFakeKeyEvent.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_int,ctypes.c_ulong]
def key(name,down=True):
 code=x.XKeysymToKeycode(display,x.XStringToKeysym(name.encode()));assert code,name;t.XTestFakeKeyEvent(display,code,1 if down else 0,0);x.XFlush(display)
def press(name):key(name);key(name,False);time.sleep(.1)
def chord(mod,name):key(mod);press(name);key(mod,False);time.sleep(.1)
def type_text(text):
 mapping={'/':'slash','.':'period','-':'minus','_':'underscore',':':'colon',' ':'space'}
 for ch in text:
  shifted=ch.isupper() or ch in '_:'
  if shifted:key('Shift_L')
  key(mapping.get(ch,ch.lower()));key(mapping.get(ch,ch.lower()),False)
  if shifted:key('Shift_L',False)
words=[0x4c57,0,1,8,8,0,1,0,0];doom=struct.pack('>10I',*words,(12345+sum(words))&0xffffffff)
def archive(entries):
 payload=b''
 for name,data in entries:payload+=name.encode().ljust(32,b'\0')+struct.pack('>IIII',0,len(data),zlib.crc32(data),0)+data
 return b'OM3DSAV1'+struct.pack('>IIII',len(payload)+24,len(entries),zlib.crc32(payload),0)+payload
save_path=dest/'.ohmy3do/saves/slot01.3ds';save_path.parent.mkdir(parents=True,exist_ok=True);save_path.write_bytes(archive([('DoomPrefs',doom),('OtherSave',b'keep me')]))
second=save_path.with_name('slot02.3ds');second.write_bytes(archive([('DoomPrefs',doom)]))
p=subprocess.Popen([str(args.binary or R/'build/tauri-linux-target/release/oh-my-3do-library')],stdout=open(fixture/'native-app.log','w'),stderr=subprocess.STDOUT)
try:
 time.sleep(3)
 if args.library:
  time.sleep(4);shot('library-real.png');print('Read-only library screenshot captured.',flush=True)
 else:
  shot('native-library.png');click(280,390);shot('native-selected.png');primary();shot('native-copy-dialog.png');primary();time.sleep(1.5);shot('native-copy-review.png');primary();time.sleep(2)
  assert (dest/'Games/USA/Example/Disc 1.iso').is_file();assert (dest/'Games/USA/Example/Disc 2.iso').is_file();assert not (dest/'Games/USA/Other').exists();assert b'Other' not in (dest/'.ohmy3do/library.3di').read_bytes();assert (dest/'boot.iso').read_bytes()==(source/'boot.iso').read_bytes();assert (dest/'.ohmy3do/saves/slot-catalog.3di').is_file();shot('native-copy-complete.png');print('Actual Tauri mouse test: selected one multi-disc title, reviewed and copied it; filtered index, boot and save support verified.',flush=True)
  click(1200,124);shot('native-import.png');print('Actual Library Add discs action exercised.',flush=True)
  click(85,205);shot('native-saves.png');click(1053,124);time.sleep(1);shot('native-file-dialog.png');chord('Control_L','l');chord('Control_L','a');type_text(str(save_path));press('Return');time.sleep(.5);press('Return');time.sleep(2);shot('native-save-open.png')
  click(1020,647);chord('Control_L','a');type_text('5');press('Tab');time.sleep(.5);shot('native-save-edited.png');click(763,354);time.sleep(.3);shot('native-save-confirm.png');primary();time.sleep(1.5)
  data=save_path.read_bytes();assert struct.unpack_from('>I',data,80)[0]==5;assert b'keep me' in data;assert zlib.crc32(data[24:])==struct.unpack_from('>I',data,16)[0];assert list(save_path.parent.glob('slot01.3ds.*.bak'));assert struct.unpack_from('>I',data,108)[0]==(12345+sum(struct.unpack_from('>9I',data,72)))&0xffffffff
  shot('native-save-saved.png');print('Actual Save Manager: edited DOOM starting map, saved over slot01 with recovery copy; archive/game checksums and unrelated save verified.',flush=True)
  click(1053,124);time.sleep(.5);chord('Control_L','l');chord('Control_L','a');type_text(str(second));press('Return');time.sleep(.3);press('Return');time.sleep(1);shot('native-second-save.png');assert second.read_bytes()==archive([('DoomPrefs',doom)])
  if args.features:
   click(1090,354);time.sleep(.5);chord('Control_L','l');chord('Control_L','a');type_text(str(save_path));press('Return');time.sleep(.3);press('Return');time.sleep(1);shot('native-save-comparison.png');press('Tab');press('Return');shot('native-save-byte-differences.png');press('Tab');press('Return');time.sleep(.3);shot('native-comparison-hex-editor.png')
   click(80,205);t.XTestFakeMotionEvent(display,-1,1100,300,0)
   for _ in range(25):t.XTestFakeButtonEvent(display,4,1,0);t.XTestFakeButtonEvent(display,4,0,0)
   x.XFlush(display);time.sleep(.3)
  click(874,220);time.sleep(1);shot('native-slot-search.png');assert save_path.with_name('slot02.3si').is_file();print('Opened another backup after saving and refreshed occupied slots from the actual USB fixture.',flush=True)
  if args.features:
   before={str(p.relative_to(dest)):p.read_bytes() for p in dest.rglob('*') if p.is_file()}
   click(80,320);shot('native-readiness-before.png');click(280,720);time.sleep(2)
   shot('native-readiness-summary.png')
   t.XTestFakeMotionEvent(display,-1,1100,720,0)
   for _ in range(5):t.XTestFakeButtonEvent(display,5,1,0);t.XTestFakeButtonEvent(display,5,0,0)
   x.XFlush(display);time.sleep(.5);shot('native-readiness-results.png')
   assert {str(p.relative_to(dest)):p.read_bytes() for p in dest.rglob('*') if p.is_file()}==before
   print('Native save comparison and readiness controls exercised; drive check preserved every file.',flush=True)
  if args.gallery:
   click(80,320);shot('drive-setup.png');click(85,260);shot('media.png');click(85,814);shot('settings.png')
finally:p.terminate();p.wait(timeout=10)
