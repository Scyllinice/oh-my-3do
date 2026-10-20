#!/usr/bin/env python3
"""Incremental media updates, visual artwork review, and verified USB preparation."""
import argparse
import base64
import binascii
import copy
import hashlib
import html
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import uuid
import zlib

# Reuse the organizer's path guards, media matching, database and undo journal.
import importlib.util
_spec = importlib.util.spec_from_file_location('organizer', Path(__file__).with_name('organize-library.py'))
o = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(o)
FOLD = o.index_tools.fold
KINDS = {'cover': ('covers', '.3dc'), 'preview': ('previews', '.3dv')}
PERSONAL = {'ohmy3do.ini', 'favorites.txt', 'recent.txt', 'last-position.txt', 'categories.json'}


def real_root(path):
    if o.directory_link(path) or not path.is_dir():
        raise ValueError('Library root must be a real, existing directory')
    return path.resolve()


def outside(path, *roots):
    path = path.resolve()
    if any(path == root or root in path.parents for root in roots):
        raise ValueError('Keep reports, caches and backups outside the library and USB roots')
    return path


def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def checked_path(root, name):
    """Also reject junctions and FAT-ambiguous components on either platform."""
    path = o.relative_path(root, name)
    parent = root
    for part in Path(name).parts:
        if (part.rstrip(' .') != part or re.search(r'[<>"|?*]', part) or
                re.fullmatch(r'(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?', part, re.I)):
            raise ValueError('Unsupported FAT filename: ' + name)
        if parent.is_dir():
            matches = [p.name for p in parent.iterdir() if FOLD(p.name) == FOLD(part)]
            if matches and matches != [part]:
                raise ValueError('Case-colliding path: ' + name)
        parent = parent / part
        if parent.is_symlink() or (parent.exists() and o.directory_link(parent)):
            raise ValueError('Links and junctions are unsupported: ' + name)
    return path


def database(root):
    data = json.loads(checked_path(root, '.ohmy3do/library.json').read_text(encoding='utf-8'))
    if data.get('schema') != 1 or not isinstance(data.get('games'), list):
        raise ValueError('Unsupported library database; organize or register discs first')
    seen = set()
    for record in data['games']:
        name = record['path']
        if FOLD(name) in seen or not checked_path(root, name).is_file():
            raise ValueError('Missing or duplicate database disc: ' + name)
        seen.add(FOLD(name))
    return data


def associations(root):
    result = {}
    for kind, (folder, _) in KINDS.items():
        directory = checked_path(root, '.ohmy3do/' + folder)
        result[kind] = o.index_tools.existing_index(directory)
        for name in result[kind].values():
            checked_path(root, '.ohmy3do/' + folder + '/' + name)
    return result


def row_for(record):
    row = copy.deepcopy(record)
    row.update(source=record['path'], target=record['path'], canonical=record['title'],
               confidence=record['identification'])
    return row


def media_sources(emu_movies=None, cover_dir=None, preview_dir=None):
    if emu_movies:
        emu_movies = real_root(emu_movies)
        if (emu_movies / 'Panasonic_3DO').is_dir():
            emu_movies /= 'Panasonic_3DO'
        if cover_dir is None:
            cover_dir = emu_movies / 'Box'
        if preview_dir is None:
            # One file per exact title, preferring the better quality download.
            preferred = {}
            for folder in ('Video_MP4', 'Video_MP4_HI_QUAL', 'Video_MP4_HD'):
                directory = emu_movies / folder
                if directory.exists():
                    group = {}
                    for p in o.local_media_files(directory, ('.mp4', '.avi', '.mkv', '.mov', '.webm')):
                        group.setdefault(p.stem.casefold(), []).append(p)
                    preferred.update(group)
            previews = [p for group in preferred.values() for p in group]
        else:
            previews = o.local_media_files(preview_dir, ('.mp4', '.avi', '.mkv', '.mov', '.webm'))
    else:
        previews = o.local_media_files(preview_dir, ('.mp4', '.avi', '.mkv', '.mov', '.webm')) if preview_dir else []
    covers = o.local_media_files(cover_dir, o.module('resize-covers').EXTENSIONS) if cover_dir else []
    return {'cover': covers, 'preview': previews}


def media_plan(root, sources):
    db = database(root)
    maps = associations(root)
    rows = []
    for record in db['games']:
        row = row_for(record)
        item = {'path': record['path'], 'title': record['title']}
        for kind, (folder, _) in KINDS.items():
            choices = (maps[kind].get(FOLD(record['path'])), record.get(kind))
            name = next((n for n in choices if n and checked_path(root, '.ohmy3do/' + folder + '/' + n).is_file()), None)
            if name:
                item[kind] = {'status': 'existing', 'file': name,
                              'repair_index': maps[kind].get(FOLD(record['path'])) != name}
                continue
            candidates = o.local_media_matches(row, sources[kind], other_regions=kind == 'preview') if sources[kind] else []
            item[kind] = {'status': 'matched' if len(candidates) == 1 else 'ambiguous' if candidates else 'missing',
                          'candidates': [str(p) for p in candidates]}
        rows.append(item)
    return db, maps, rows


def cached_conversion(kind, source, cache):
    before = source.stat()
    policy = 'cover-fast96x120-v3' if kind == 'cover' else 'preview-v2-160x120-512k-v3'
    converter_file = Path(__file__).with_name('resize-covers.py' if kind == 'cover' else 'prepare-preview.py')
    key = hashlib.sha256((sha256(source) + policy + sha256(converter_file)).encode()).hexdigest()
    target = cache / (key + KINDS[kind][1])
    receipt = cache / (key + '.json')
    valid = False
    if target.is_file() and receipt.is_file():
        try:
            valid = json.loads(receipt.read_text(encoding='utf-8'))['sha256'] == sha256(target)
        except (ValueError, KeyError):
            pass
    if not valid:
        cache.mkdir(parents=True, exist_ok=True)
        if kind == 'cover':
            o.module('resize-covers').fast_convert(source, target)
        else:
            o.module('prepare-preview').convert(source, target, duration=15, sound=True)
        o.atomic_write(receipt, o.json_bytes({'sha256': sha256(target)}))
    after = source.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError('Media source changed during conversion: ' + str(source))
    content = target.read_bytes()
    return kind + '-' + hashlib.sha256(content).hexdigest()[:24] + KINDS[kind][1], content


def review_page(root, rows):
    body = []
    for row in rows:
        cells = []
        missing = False
        for kind, (folder, _) in KINDS.items():
            item = row[kind]
            status = item['status']
            missing |= status not in ('existing', 'added')
            text = html.escape(status)
            source = item.get('source') or next(iter(item.get('candidates', [])), '')
            if source:
                text += '<br>' + html.escape(source)
            if item.get('error'):
                text += '<br>' + html.escape(item['error'])
            image = None
            if kind == 'cover':
                if item.get('file'):
                    image = checked_path(root, '.ohmy3do/' + folder + '/' + item['file'])
                elif status == 'matched':
                    image = Path(source)
                if image:
                    try:
                        from PIL import Image, ImageOps
                        with o.module('resize-covers').read_cover(image) as art:
                            thumb = ImageOps.exif_transpose(art).convert('RGB')
                            thumb.thumbnail((168, 210), Image.Resampling.LANCZOS)
                            stream = io.BytesIO(); thumb.save(stream, format='PNG')
                        text = '<img alt="Cover" src="data:image/png;base64,' + base64.b64encode(stream.getvalue()).decode() + '"><br>' + text
                    except (ImportError, OSError, ValueError):
                        text += '<br>Thumbnail unavailable'
            cells.append('<td>' + text + '</td>')
        body.append('<tr data-missing="' + str(int(missing)) + '"><td>' + html.escape(row['title']) + '<br><small>' + html.escape(row['path']) + '</small></td>' + ''.join(cells) + '</tr>')
    page = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>3DO artwork review</title>
<style>body{font:16px system-ui;background:#101c28;color:#e2eaf0;margin:2rem}table{border-collapse:collapse;width:100%}td,th{border:1px solid #486273;padding:.7rem;text-align:left;overflow-wrap:anywhere}img{max-width:168px;max-height:210px}input,button{font:inherit;padding:.5rem;margin:.3rem}small{color:#bacbd7}</style>
<h1>3DO artwork review</h1><p>Existing media stays in place. Missing or ambiguous matches are left for review. No game identities are changed.</p>
<input id="search" type="search" placeholder="Find a title or path" aria-label="Find a title or path"><button id="missing">Show missing only</button><p id="count"></p>
<table><thead><tr><th>Game / disc</th><th>Cover</th><th>Preview</th></tr></thead><tbody>''' + ''.join(body) + '''</tbody></table>
<script>let missing=false;const rows=[...document.querySelectorAll('tbody tr')];function filter(){const q=document.getElementById('search').value.toLowerCase();let n=0;rows.forEach(r=>{r.hidden=(missing&&r.dataset.missing!=='1')||!r.textContent.toLowerCase().includes(q);if(!r.hidden)n++});document.getElementById('count').textContent=n+' disc entries shown'}document.getElementById('search').oninput=filter;document.getElementById('missing').onclick=function(){missing=!missing;this.textContent=missing?'Show all':'Show missing only';filter()};filter();</script></html>'''
    return page.encode('utf-8')


def update_media(root, sources, cache, report, apply=False):
    cache = outside(cache, root)
    report = outside(report, root)
    if report.suffix.lower() != '.html':
        raise ValueError('Artwork report must end in .html')
    db, maps, rows = media_plan(root, sources)
    changed, outputs, converted = [], {}, {}
    if apply:
        records = {r['path']: r for r in db['games']}
        for item in rows:
            record = row_for(records[item['path']])
            dirty = False
            for kind, (folder, _) in KINDS.items():
                value = item[kind]
                if value['status'] == 'existing' and value.get('repair_index'):
                    maps[kind][FOLD(item['path'])] = value['file']
                    dirty = True
                if value['status'] != 'matched':
                    continue
                source = Path(value['candidates'][0])
                try:
                    key = (kind, source)
                    if key not in converted:
                        print('Preparing ' + kind + ': ' + source.name, flush=True)
                        converted[key] = cached_conversion(kind, source, cache)
                    name, content = converted[key]
                    path = checked_path(root, '.ohmy3do/' + folder + '/' + name)
                    if path.exists() and path.read_bytes() != content:
                        raise ValueError('Refusing to overwrite existing media: ' + name)
                    outputs[path.relative_to(root).as_posix()] = content
                    maps[kind][FOLD(item['path'])] = name
                    record[kind] = name
                    record[kind + '_source'] = str(source)
                    value.update(status='added', file=name, source=str(source))
                    dirty = True
                except (OSError, ValueError, subprocess.CalledProcessError) as error:
                    value.update(status='failed', source=str(source), error=str(error))
                    print('Skipped ' + str(source) + ': ' + str(error), flush=True)
            if dirty:
                changed.append(record)
        if changed:
            # No disc reads, renames, catalog lookup, Find rebuild or deletion.
            tx = o.Transaction(root)
            print('Undo journal: ' + str(tx.path), flush=True)
            try:
                for path, content in outputs.items():
                    tx.write(path, content)
                for kind, (folder, _) in KINDS.items():
                    tx.write('.ohmy3do/' + folder + '/index.3di', o.media_index(maps[kind]))
                o.write_database(root, tx, {'entries': changed, 'sources': db['sources']}, [])
                tx.data['state'] = 'complete'; tx.save()
            except BaseException:
                tx.undo()
                raise
    o.atomic_write(report, review_page(root, rows))
    o.atomic_write(report.with_suffix('.json'), o.json_bytes({'root': str(root), 'applied': apply, 'rows': rows}))
    for kind in KINDS:
        counts = o.collections.Counter(r[kind]['status'] for r in rows)
        print(kind.capitalize() + ': ' + str(dict(counts)))
    print('Artwork review: ' + str(report))
    if not apply:
        print('Preview only; no library files changed. Add --apply to import matched media.')
    return rows


def inflate_exact(data, size):
    decoder = zlib.decompressobj()
    raw = decoder.decompress(data, size + 1)
    if len(raw) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError('Invalid compressed media data')
    return raw


def validate_media(path):
    limit = 512 * 1024 if path.suffix.lower() == '.png' else (4096 if path.suffix.lower() == '.3dv' else 1024) * 1024 + 48
    if path.stat().st_size > limit:
        raise ValueError('Oversized media: ' + str(path))
    data = path.read_bytes()
    if path.suffix.lower() == '.png':
        # Native, non-interlaced RGB/RGBA covers; check every chunk and scanline.
        if data[:8] != b'\x89PNG\r\n\x1a\n':
            raise ValueError('Invalid PNG: ' + str(path))
        pos, compressed, color, ended = 8, bytearray(), None, False
        while pos + 12 <= len(data):
            n = struct.unpack_from('>I', data, pos)[0]
            tag = data[pos+4:pos+8]; chunk = data[pos+8:pos+8+n]
            if pos+12+n > len(data) or binascii.crc32(tag+chunk) != struct.unpack_from('>I', data, pos+8+n)[0]:
                raise ValueError('PNG chunk checksum or length failed: ' + str(path))
            if tag == b'IHDR':
                if pos != 8 or len(chunk) != 13:
                    raise ValueError('Invalid PNG header')
                w,h,depth,color,comp,filt,interlace = struct.unpack('>IIBBBBB', chunk)
                if (w,h) != (96,120) or (depth,comp,filt,interlace) != (8,0,0,0) or color not in (2,6):
                    raise ValueError('Prepare covers as native 96x120 RGB/RGBA PNGs: ' + str(path))
            elif tag == b'IDAT':
                compressed.extend(chunk)
            elif tag == b'IEND':
                if n or pos+12 != len(data):
                    raise ValueError('Invalid PNG end')
                ended = True
                break
            elif tag[:1].isupper() and tag != b'PLTE':
                raise ValueError('Unsupported critical PNG chunk')
            pos += n+12
        if color not in (2,6) or not ended:
            raise ValueError('Incomplete PNG')
        stride = w*(3 if color == 2 else 4)+1
        raw = inflate_exact(bytes(compressed), stride*h)
        if any(raw[i] > 4 for i in range(0,len(raw),stride)):
            raise ValueError('Invalid PNG filter')
        return
    if data[:8] == b'OM3DCOV2':
        if len(data)<32:raise ValueError('Truncated native cover')
        w,h,n,crc,decoded=struct.unpack_from('>HHIII',data,8)
        large=path.name.lower().endswith('-large.3dc')
        size,limit=((192,240),192*240*2+1024) if large else ((96,120),32768)
        if (w,h)!=size or not 0<n<=limit or decoded!=w*h*2 or len(data)!=n+32 or data[-8:]!=b'END3DC02' or binascii.crc32(data[24:-8])!=crc:
            raise ValueError('Invalid native cover')
        inflate_exact(data[24:-8],w*h*2)
        return
    if path.suffix.lower() != '.3dv' or data[:8] != b'OM3DVID5':
        raise ValueError('Invalid preview signature')
    if len(data) < 48:
        raise ValueError('Truncated preview')
    w,h,n,crc,frames,fps,video,audio,rate,samples = struct.unpack_from('>HHIIHHIIII',data,8)
    payload = data[40:-8]
    if len(data) != 40+n+8 or word_sum(payload) != crc or data[-8:] != b'END3DV05':
        raise ValueError('Invalid preview checksum/length/end marker')
    if (w,h,fps) != (160,120,30) or not 1 <= frames <= 450:
        raise ValueError('Invalid preview size, rate or length')
    pad = -video % 4
    if n > 4096*1024 or video+pad+audio != n or video <= (frames+1)*4 or any(payload[n-pad:n]):
        raise ValueError('Invalid preview memory/frame bounds')
    expected = (((frames*11025+fps-1)//fps)+3)&~3
    if (audio and (audio != samples or rate != 11025 or samples != expected)) or (not audio and (rate or samples)):
        raise ValueError('Invalid preview audio')
    offsets = struct.unpack_from('>'+'I'*(frames+1),payload)
    if offsets[0] != (frames+1)*4 or offsets[-1] != video:
        raise ValueError('Invalid preview frame table')
    # Packets are checked by the menu's decoder as they play.
    if any(begin >= end for begin,end in zip(offsets, offsets[1:])):
        raise ValueError('Invalid preview frame offset')


def word_sum(data):
    """Preview payload check, as in src/usb_media.c and prepare-preview.py."""
    a = b = 0
    if len(data) % 4:
        return None
    for (w,) in struct.iter_unpack('>I', data):
        a = (a + w) & 0xffffffff
        b = (b + a) & 0xffffffff
    return a ^ (((b << 16) | (b >> 16)) & 0xffffffff)


def validate_config(path):
    data = path.read_bytes()
    if len(data)>512 or b'\0' in data:
        raise ValueError('Invalid settings length or NUL padding: ' + str(path))
    values = {}
    limits = dict(folder_workaround=2,usb_covers=1,screen_margin=12,large_titles=1,
                  remember_position=1,recent_games=1,video_previews=1,preview_sound=1,hide_adult=1)
    ended = False
    for line in data.decode('utf-8-sig').split('\n'):
        line = line.strip(' \t\r')
        if line == '; End of Oh My 3DO settings.':
            # hide_adult is newer than the end marker and stays optional.
            if not set(limits) - {'hide_adult'} <= values.keys():
                raise ValueError('Incomplete generated settings')
            ended = True; break
        if not line or line.startswith(('#',';')):
            continue
        if line in ('[compatibility]','[artwork]','[display]','[library]','[audio]'):
            continue
        match = re.fullmatch(r'([a-z_]+)[ \t]*=[ \t]*([0-9]{1,2})[ \t]*(?:[#;].*)?',line)
        if not match or match[1] not in limits or match[1] in values or int(match[2])>limits[match[1]]:
            raise ValueError('Invalid menu setting: ' + line)
        values[match[1]] = int(match[2])
    if len(data)==512 and not ended:
        raise ValueError('Full settings buffer needs an end marker')


def find_paths(path):
    data = path.read_bytes()
    if len(data)<32 or data[:8]!=b'OM3DLIB1' or data[-8:]!=b'END3DL01':
        raise ValueError('Invalid Find index')
    count,size,crc,reserved = struct.unpack_from('>IIII',data,8)
    payload = data[24:-8]
    if count>2048 or size+9*count>128*1024 or size!=len(payload) or reserved or binascii.crc32(payload)!=crc:
        raise ValueError('Invalid Find index checksum or bounds')
    paths=[];pos=0
    for _ in range(count):
        if pos+2>len(payload):
            raise ValueError('Truncated Find index')
        n=struct.unpack_from('>H',payload,pos)[0];pos+=2
        raw=payload[pos:pos+n];pos+=n
        if n<2 or len(raw)!=n or raw[-1:]!=b'\0' or b'\0' in raw[:-1]:
            raise ValueError('Invalid Find path')
        name=raw[:-1].decode('utf-8')
        if len(name.split('/'))>33 or any(len(p.encode())>=1024 for p in name.split('/')):
            raise ValueError('Find path exceeds navigation limits')
        paths.append(name)
    if pos!=len(payload) or len(set(map(FOLD,paths)))!=len(paths):
        raise ValueError('Duplicate paths or trailing Find data')
    return paths


def usb_inventory(root):
    files={'boot.iso','.ohmy3do/ohmy3do.ini','.ohmy3do/library.3di'}
    boot=checked_path(root,'boot.iso')
    with boot.open('rb') as stream:
        volume=stream.read(84)
    if len(volume)<84 or volume[:7]!=b'\x01ZZZZZ\x01' or struct.unpack_from('>I',volume,76)[0]!=2048 or struct.unpack_from('>I',volume,80)[0]*2048!=boot.stat().st_size:
        raise ValueError('Invalid 3DO boot volume header')
    issue=o.index_tools.disc_data_issue(boot)
    if issue or boot.stat().st_size%2048:
        raise ValueError('Invalid menu boot image: ' + (issue or 'not sector aligned'))
    validate_config(checked_path(root,'.ohmy3do/ohmy3do.ini'))
    units=o.scan_units(root)
    games=set()
    for unit in units:
        if unit.get('error') or unit.get('invalid_data'):
            raise ValueError('Disc problem: ' + unit['source'] + ': ' + str(unit.get('error') or unit['invalid_data']))
        games.add(unit['source'])
        files.update(unit['files'])
    indexed=find_paths(checked_path(root,'.ohmy3do/library.3di'))
    if set(indexed)!=games:
        raise ValueError('Find index is stale; run index-media.py against the staging root first')
    metadata=checked_path(root,'.ohmy3do/metadata.3di')
    if metadata.is_file():
        if metadata.stat().st_size>192*1024+32:
            raise ValueError('Oversized console metadata index')
        if not set(map(FOLD,o.index_tools.metadata_paths(metadata.read_bytes())))<=set(map(FOLD,games)):
            raise ValueError('Metadata index refers to a missing disc; rebuild indexes')
        files.add('.ohmy3do/metadata.3di')
    names=checked_path(root,'.ohmy3do/save-names.txt')
    if names.is_file():
        if names.stat().st_size>=16384:
            raise ValueError('Oversized save names; rebuild indexes')
        files.add('.ohmy3do/save-names.txt')
    maps=associations(root)
    checked=set()
    for kind,(folder,_) in KINDS.items():
        index='.ohmy3do/'+folder+'/index.3di'
        if checked_path(root,index).exists():
            files.add(index)
        for game,name in maps[kind].items():
            if game not in set(map(FOLD,games)):
                raise ValueError('Media index refers to a missing disc: '+game)
            path='.ohmy3do/'+folder+'/'+name
            if Path(name).suffix.lower() not in (('.png','.3dc') if kind=='cover' else ('.3dv',)):
                raise ValueError('Unsupported indexed media: '+path)
            if path not in checked:
                validate_media(checked_path(root,path));checked.add(path)
            files.add(path)
            large='.ohmy3do/covers/'+name[:-4]+'-large.3dc'
            if kind=='cover' and checked_path(root,large).is_file():
                if large not in checked:
                    validate_media(checked_path(root,large));checked.add(large)
                files.add(large)
    for name in PERSONAL-{'ohmy3do.ini'}:
        path='.ohmy3do/'+name
        if checked_path(root,path).is_file():files.add(path)
    saves=checked_path(root,'.ohmy3do/saves')
    if saves.exists():
        for parent,dirs,names in os.walk(saves,followlinks=False):
            for name in dirs+names:
                checked_path(root,(Path(parent)/name).relative_to(root).as_posix())
            files.update((Path(parent)/name).relative_to(root).as_posix() for name in names)
    for name in files:
        path=checked_path(root,name)
        if not path.is_file() or path.stat().st_size>=4*1024**3:
            raise ValueError('Missing file or file exceeds FAT32 limit: '+name)
    return sorted(files),games


def folder_moves(target, files, allowed=None, source_config=None):
    category_moves=[]
    if source_config:
        previous=o.category_config(target)
        for category in source_config['categories']:
            old=next((c for c in previous['categories'] if c['id']==category['id'] and c['name']!=category['name']),None)
            if old and checked_path(target,old['name']).exists():
                if checked_path(target,category['name']).exists(): raise ValueError('Both old and new category folders exist; resolve them before copying')
                category_moves.append((old['name'],category['name']))
    wanted = {'/'.join(Path(name).parts[:3])
              for name in files if len(Path(name).parts) >= 4 and Path(name).parts[0] in (allowed or o.CATEGORIES)}
    found = {}
    for category in o.category_names(target):
        if any(category==old for old,_ in category_moves): continue
        parent = checked_path(target, category)
        if not parent.is_dir(): continue
        for region in parent.iterdir():
            checked_path(target, region.relative_to(target).as_posix())
            if not region.is_dir(): continue
            for game in region.iterdir():
                name = game.relative_to(target).as_posix()
                checked_path(target, name)
                if game.is_dir() and FOLD(name) not in {FOLD(p) for p in wanted}:
                    found.setdefault(FOLD('/'.join(name.split('/')[1:])), []).append(name)
    moves = list(category_moves)
    used = set()
    for new in sorted(wanted):
        if any(new.startswith(prefix+'/') for _,prefix in category_moves): continue
        candidates = found.get(FOLD('/'.join(new.split('/')[1:])), [])
        if not checked_path(target, new).exists() and len(candidates) == 1:
            old = candidates[0]
            if old in used:
                raise ValueError('One USB folder matches multiple staging categories: ' + old)
            used.add(old)
            moves.append((old, new))
    return moves


def move_remaps(target, moves):
    mapping = {}
    for old, new in moves:
        for path in checked_path(target, old).rglob('*'):
            name = path.relative_to(target).as_posix()
            checked_path(target, name)
            if path.is_file(): mapping[name] = new + name[len(old):]
    return o.prepare_text_remaps(target, mapping) if moves else {}


def usb_plan(root,target,verify=False,receipts=None):
    if root==target or root in target.parents or target in root.parents:
        raise ValueError('Staging and USB directories must be separate and non-overlapping')
    print('Validating staging library and indexed media...',flush=True)
    files,games=usb_inventory(root)
    o.transfer_categories(root,target)
    moves = folder_moves(target, files, o.category_names(root), o.category_config(root))
    remaps = move_remaps(target, moves)
    operations=[dict(path=new, source_path=old, status='move', size=0, mtime_ns=0) for old,new in moves]
    receipts=receipts or {}
    for name in files:
        source=checked_path(root,name)
        current = next((old + name[len(new):] for old,new in moves if name.startswith(new + '/')), name)
        dest=checked_path(target,current)
        stat=source.stat()
        receipt=receipts.get(name)
        verified_copy=None
        personal=name.startswith('.ohmy3do/saves/') or (name.startswith('.ohmy3do/') and name.split('/',1)[1] in PERSONAL)
        if dest.exists() and not dest.is_file():
            raise ValueError('Destination is occupied by a directory: '+name)
        if name in remaps and dest.is_file():
            status='remap'
        elif personal and dest.exists():
            if name.endswith('/ohmy3do.ini'):
                validate_config(dest)
            status='update' if name.endswith('/categories.json') else 'preserve'
        elif not dest.exists():
            status='copy'
        elif not dest.is_file():
            raise ValueError('Destination is occupied by a directory: '+name)
        else:
            ds=dest.stat()
            disc=not name.startswith('.ohmy3do/') and name!='boot.iso'
            receipt_matches = (isinstance(receipt,dict) and
                (receipt.get('source_size'),receipt.get('source_mtime_ns')) == (stat.st_size,stat.st_mtime_ns) and
                (receipt.get('destination_size'),receipt.get('destination_mtime_ns')) == (ds.st_size,ds.st_mtime_ns))
            if disc and not verify and ((stat.st_size,stat.st_mtime_ns)==(ds.st_size,ds.st_mtime_ns) or receipt_matches):
                status='unchanged'
                if receipt_matches:verified_copy=receipt
            elif stat.st_size==ds.st_size and sha256(source)==sha256(dest):
                status='unchanged'
            elif disc:
                raise ValueError('Different disc already exists on destination; resolve it before copying: '+name)
            else:
                status='update'
        item=dict(path=name,status=status,size=stat.st_size,mtime_ns=stat.st_mtime_ns)
        if verified_copy:item['verified_copy']=verified_copy
        operations.append(item)
    planned = {r['path'] for r in operations}
    for name in remaps:
        if name not in planned:
            stat = checked_path(target, name).stat()
            operations.append(dict(path=name, status='remap', size=stat.st_size, mtime_ns=stat.st_mtime_ns))
    required=sum(r['size'] for r in operations if r['status'] in ('copy','update'))
    if shutil.disk_usage(target).free<required:
        raise ValueError('Insufficient free space for the planned copy')
    return operations,games


def stream_copy(source,dest):
    """Copy to a sibling temporary file; verify readback before atomic replacement."""
    before=source.stat();temp=None
    dest.parent.mkdir(parents=True,exist_ok=True)
    try:
        h=hashlib.sha256()
        with source.open('rb') as src, tempfile.NamedTemporaryFile(dir=dest.parent,prefix='.oh3do-',delete=False) as out:
            temp=Path(out.name)
            for block in iter(lambda:src.read(1024*1024),b''):
                h.update(block);out.write(block)
            out.flush();os.fsync(out.fileno())
        after=source.stat()
        if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns) or sha256(temp)!=h.hexdigest():
            raise ValueError('File changed or copy verification failed: '+str(source))
        os.utime(temp,ns=(before.st_atime_ns,before.st_mtime_ns))
        temp.replace(dest)
        ds=dest.stat()
        return dict(source_size=before.st_size,source_mtime_ns=before.st_mtime_ns,
                    destination_size=ds.st_size,destination_mtime_ns=ds.st_mtime_ns,sha256=h.hexdigest())
    finally:
        if temp:temp.unlink(missing_ok=True)


def prepare_usb(root,target,report,apply=False,verify=False):
    report=outside(report,root,target)
    if report.suffix.lower() != '.json':
        raise ValueError('USB copy report must end in .json')
    receipts={}
    if report.is_file():
        try:
            previous=json.loads(report.read_text(encoding='utf-8'))
            if previous.get('source')==str(root) and previous.get('destination')==str(target):
                receipts={r['path']:r['verified_copy'] for r in previous.get('files',[]) if r.get('verified_copy')}
        except (ValueError,KeyError,TypeError):
            pass
    operations,games=usb_plan(root,target,verify,receipts)
    result={'source':str(root),'destination':str(target),'state':'preview','disc_entries':len(games),'files':operations}
    o.atomic_write(report,o.json_bytes(result))
    if apply:
        backup=outside(report.parent/('usb-copy-backup-'+uuid.uuid4().hex),root,target)
        backup.mkdir()
        result.update(state='copying',backup=str(backup))
        o.atomic_write(report,o.json_bytes(result))
        # Write data first, then associations/Find, then boot. A failed copy can
        # be rerun; it never deletes destination files or replaces game images.
        order=lambda r:(2 if r['path']=='boot.iso' else 1 if r['path'].endswith('.3di') else 0,r['path'])
        tx = o.Transaction(target)
        result['journal'] = str(tx.path)
        o.atomic_write(report,o.json_bytes(result))
        try:
            category_config=o.transfer_categories(root,target)
            moves = [(r['source_path'], r['path']) for r in operations if r['status'] == 'move']
            remaps = move_remaps(target, moves)
            for old, new in moves:
                tx.move_folder(old, new)
            for name, text in remaps.items():
                tx.write(name, text)
            for item in sorted(operations,key=order):
                if item['status'] not in ('copy','update'):continue
                name=item['path'];source=checked_path(root,name);dest=checked_path(target,name)
                stat=source.stat()
                if (stat.st_size,stat.st_mtime_ns)!=(item['size'],item['mtime_ns']):
                    raise ValueError('Staging file changed since validation: '+name)
                if item['status']=='copy' and name.endswith('/categories.json'):
                    tx.write(name,o.json_bytes(category_config));continue
                if item['status']=='copy' and dest.exists():
                    raise ValueError('Destination appeared during copying: '+name)
                if item['status']=='update':
                    stream_copy(dest,backup/name)
                print('Copying: '+name,flush=True)
                tx.parents(dest)
                if item['status'] == 'copy':
                    tx.data.setdefault('copies', []).append(dict(path=name, sha256=sha256(source)))
                    tx.save()
                    item['verified_copy']=stream_copy(source,dest)
                else:
                    content=source.read_bytes()
                    if name.endswith('/categories.json'):
                        content=o.json_bytes(category_config)
                    tx.write(name,content)
                    if dest.read_bytes()!=content:
                        raise ValueError('USB update readback failed: ' + name)
                    ds=dest.stat()
                    item['verified_copy']=dict(source_size=stat.st_size,source_mtime_ns=stat.st_mtime_ns,
                        destination_size=ds.st_size,destination_mtime_ns=ds.st_mtime_ns,sha256=sha256(dest))
                item['copied']=True
                o.atomic_write(report,o.json_bytes(result))
            checked_path(target,'.ohmy3do/saves').mkdir(parents=True,exist_ok=True)
            tx.data['state']='complete';tx.save()
            result['state']='complete'
        except BaseException:
            try:
                tx.undo()
            except (ValueError,OSError) as error:
                result['rollback_error']=str(error)
            result['state']='interrupted'
            o.atomic_write(report,o.json_bytes(result))
            raise
        o.atomic_write(report,o.json_bytes(result))
    print('USB files: '+str(dict(o.collections.Counter(r['status'] for r in operations))))
    print('Disc entries: '+str(len(games))+'; report: '+str(report))
    if not apply:print('Preview only; add --apply to copy. No destination files changed.')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_subparsers(dest='command',required=True)
    update=commands.add_parser('update-media',help='fill missing covers/previews for registered library discs; preview by default')
    update.add_argument('root',type=Path)
    update.add_argument('--emu-movies',type=Path,help='EmuMovies root or Panasonic_3DO directory')
    update.add_argument('--cover-dir',type=Path)
    update.add_argument('--preview-dir',type=Path)
    update.add_argument('--cache',type=Path,default=o.default_cache()/'maintenance')
    update.add_argument('--report',type=Path,default=Path('artwork-review.html'))
    update.add_argument('--apply',action='store_true')
    review=commands.add_parser('review',help='create a searchable artwork contact sheet without changing the library')
    review.add_argument('root',type=Path)
    review.add_argument('--report',type=Path,default=Path('artwork-review.html'))
    usb=commands.add_parser('prepare-usb',help='validate staging and preview a copy of runtime files only')
    usb.add_argument('root',type=Path)
    usb.add_argument('destination',type=Path)
    usb.add_argument('--report',type=Path,default=Path('usb-copy-report.json'))
    usb.add_argument('--apply',action='store_true')
    usb.add_argument('--verify-discs',action='store_true',help='hash existing discs even when size and timestamp agree; reads the entire library')
    args=parser.parse_args()
    try:
        root=real_root(args.root)
        if args.command=='prepare-usb':
            prepare_usb(root,real_root(args.destination),args.report,args.apply,args.verify_discs)
        elif args.command=='review':
            update_media(root,{'cover':[],'preview':[]},o.default_cache()/'maintenance',args.report)
        else:
            if not (args.emu_movies or args.cover_dir or args.preview_dir):
                raise ValueError('Supply --emu-movies, --cover-dir or --preview-dir')
            update_media(root,media_sources(args.emu_movies,args.cover_dir,args.preview_dir),args.cache,args.report,args.apply)
    except (ValueError,OSError,KeyError,struct.error,zlib.error,o.sqlite3.Error) as error:
        parser.exit(1,'Error: '+str(error)+'\n')


if __name__=='__main__':
    main()
