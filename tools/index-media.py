#!/usr/bin/env python3
"""Index games for Find and associate PNG covers/prepared previews on USB."""
import argparse
import binascii
from pathlib import Path
import re
import struct
import json
import unicodedata
import datetime


def metadata_text(value):
    text = unicodedata.normalize('NFKD', str(value or '')).encode('ascii', 'ignore').decode()
    return re.sub(r'\s+', ' ', ''.join(c if 32 <= ord(c) <= 126 else ' ' for c in text)).strip()[:96]


def metadata_date(meta):
    date = str(meta.get('release_date') or '')[:10]
    if re.fullmatch(r'\d{4}-\d{2}-\d{2}', date):
        try:
            return datetime.date.fromisoformat(date).isoformat()
        except ValueError:
            pass
    year = str(meta.get('year') or '')
    return year if re.fullmatch(r'[12]\d{3}', year) else ''


def metadata_index(records, games):
    """Index every disc for game-folder recognition; catalog fields are optional."""
    allowed = {fold(path): path for path in games}
    rows = {key: [path, '', '', ''] for key, path in allowed.items()}
    seen = set()
    for record in records:
        path = allowed.get(fold(record.get('path') or ''))
        if path is None:
            continue
        meta = record.get('metadata') or {}
        fields = [metadata_text(meta.get('genre')), metadata_text(meta.get('publisher')), metadata_date(meta)]
        key = fold(path)
        if key in seen:
            raise ValueError('Duplicate metadata path: ' + path)
        seen.add(key)
        rows[key] = [path] + fields
    payload = bytearray()
    for key in sorted(rows):
        for field in rows[key]:
            raw = field.encode('utf-8') + b'\0'
            if len(raw) > 8193:
                raise ValueError('Oversized metadata field')
            payload.extend(struct.pack('>H', len(raw)) + raw)
    if len(rows) > 2048 or len(payload) + 16 * len(rows) > 192 * 1024:
        raise ValueError('Console metadata index exceeds capacity')
    return b'OM3DMET1' + struct.pack('>IIII', len(rows), len(payload), binascii.crc32(payload), 0) + payload + b'END3DM01'


def metadata_paths(data):
    if len(data) < 32 or data[:8] != b'OM3DMET1' or data[-8:] != b'END3DM01':
        raise ValueError('Invalid metadata header/footer')
    count, size, crc, reserved = struct.unpack_from('>IIII', data, 8)
    payload = data[24:-8]
    if reserved or count > 2048 or size + 16 * count > 192 * 1024 or size != len(payload) or binascii.crc32(payload) != crc:
        raise ValueError('Invalid metadata bounds/checksum')
    paths = []; pos = 0
    for _ in range(count):
        for field in range(4):
            if pos + 2 > size:
                raise ValueError('Truncated metadata')
            n = struct.unpack_from('>H', payload, pos)[0]; pos += 2
            raw = payload[pos:pos+n]; pos += n
            if not n or len(raw) != n or raw[-1:] != b'\0' or b'\0' in raw[:-1]:
                raise ValueError('Invalid metadata string')
            if field:
                if n > 97 or any(c < 32 or c > 126 for c in raw[:-1]):
                    raise ValueError('Invalid metadata text')
            else:
                path = raw[:-1].decode('utf-8')
                if len(raw) > 8193 or len(path.split('/')) > 33 or any(p in ('', '.', '..') or len(p.encode()) >= 1024 for p in path.split('/')) or any(ord(c) < 32 or ord(c) == 127 or c in '\\:' for c in path) or not path.lower().endswith(('.iso', '.cue')):
                    raise ValueError('Invalid metadata path')
                if paths and fold(paths[-1]) >= fold(path):
                    raise ValueError('Metadata paths must be unique and sorted')
                paths.append(path)
    if pos != size:
        raise ValueError('Trailing metadata')
    return paths


def fold(value):
    return value.translate(str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"))


def disc_data_issue(path):
    """Reject empty data and recognizable archives, without guessing a game."""
    if not path.stat().st_size:
        return 'Empty disc data file: ' + path.name
    with path.open('rb') as stream:
        header = stream.read(16)
    formats = ((b'PK\x03\x04','ZIP'),(b'PK\x05\x06','ZIP'),(b'PK\x07\x08','ZIP'),
               (b'7z\xbc\xaf\x27\x1c','7-Zip'),(b'Rar!\x1a\x07','RAR'),
               (b'\x1f\x8b','gzip'),(b'BZh','bzip2'),(b'\xfd7zXZ\0','xz'))
    for signature, name in formats:
        if header.startswith(signature):
            return name + ' archive disguised as disc data: ' + path.name + '; extract it before scanning'
    return None


OPERA_BLOCK = 2048
SAVE_NAMES_BYTES = 32768
SAVE_PATH = re.compile(rb'/nvram/([\x21-\x7e][\x20-\x7e]{0,30})', re.I)
SAVE_NAME = re.compile(r"[A-Za-z0-9_.\- \[\]'&+!()]+")


def opera_files(stream):
    """(byte offset, size) of every file on a 3DO (Opera) disc image."""
    header = stream.read(OPERA_BLOCK)
    if len(header) < OPERA_BLOCK or header[:7] != b'\x01ZZZZZ\x01':
        return
    pending, seen = [(struct.unpack_from('>I', header, 100)[0], struct.unpack_from('>I', header, 88)[0])], set()
    while pending:
        start, count = pending.pop()
        if start in seen or len(seen) > 4096:
            continue
        seen.add(start)
        for block in range(min(count, 64)):
            stream.seek((start + block) * OPERA_BLOCK)
            data = stream.read(OPERA_BLOCK)
            if len(data) < OPERA_BLOCK:
                break
            end = min(struct.unpack_from('>I', data, 12)[0], OPERA_BLOCK)
            pos = struct.unpack_from('>I', data, 16)[0]
            while 20 <= pos <= end - 72:
                flags, _, _, block_size, size, blocks = struct.unpack_from('>II4sIII', data, pos)
                last, avatar = struct.unpack_from('>II', data, pos + 64)
                if last > 15:
                    break
                if flags & 0xff == 7:
                    pending.append((avatar, blocks))
                else:
                    yield avatar * (block_size or OPERA_BLOCK), size
                pos += 72 + 4 * last
                if flags & 0xc0000000:
                    break


def disc_save_names(path):
    """NVRAM files a disc's programs name: '=FILE' exact, '*PREFIX' from FILE%d-style patterns."""
    found = set()
    with path.open('rb') as stream:
        for offset, size in list(opera_files(stream)):
            if size < 64 or size > 3 * 1024 * 1024:
                continue
            stream.seek(offset)
            data = stream.read(size)
            if b'/nvram/' not in data.lower():
                continue
            for match in SAVE_PATH.finditer(data):
                name = match[1].decode('ascii')
                if '%' in name:
                    prefix = name.split('%', 1)[0]
                    if len(prefix) >= 3 and SAVE_NAME.fullmatch(prefix):
                        found.add('*' + prefix)
                elif len(name) >= 2 and SAVE_NAME.fullmatch(name):
                    found.add('=' + name)
    return sorted(found)


def short_title(title):
    """A list title without trailing (Region) / (Disc 1) groups, as the menu shows it."""
    title = re.sub(r'\.(iso|cue)$', '', title, flags=re.I)
    while True:
        shorter = re.sub(r' \([^()]*\)$', '', title)
        if shorter == title or not shorter:
            return metadata_text(title)
        title = shorter


def save_names_text(root, support, games, titles, write_cache=True):
    """save-names.txt for the console's Save manager, reusing a per-disc cache."""
    cache_path = support / 'save-names-cache.json'
    try:
        cache = json.loads(cache_path.read_text(encoding='utf-8')) if cache_path.is_file() else {}
        cache = cache if isinstance(cache, dict) and cache.get('schema') == 2 else {}
    except (OSError, ValueError):
        cache = {}
    discs, rows, scanned = {}, {}, 0
    for _, game, _ in sorted(games):
        if not game.lower().endswith('.iso'):
            continue
        path = root / game
        stat = path.stat()
        stamp = '%s|%d|%d' % (path.name, stat.st_size, stat.st_mtime_ns)
        names = cache.get('discs', {}).get(stamp)
        if not isinstance(names, list):
            if not scanned:
                print('Reading save names from discs; later runs reuse the results...', flush=True)
            scanned += 1
            try:
                names = disc_save_names(path)
            except (OSError, struct.error):
                names = []
        discs[stamp] = names
        title = short_title(titles.get(fold(game)) or Path(game).name)
        for name in names:
            row = rows.setdefault(name, [])
            if title and title not in row:
                row.append(title)
    text = '# Save names read from the discs by index-media.py\n'
    dropped = 0
    for name in sorted(rows, key=lambda n: (n[1:].lower(), n)):
        line = name + '\t' + ' / '.join(rows[name])[:63] + '\n'
        if len(text) + len(line) >= SAVE_NAMES_BYTES:
            dropped += 1
            continue
        text += line
    if dropped:
        print(f'Save names: {dropped} names left out to stay within {SAVE_NAMES_BYTES} bytes')
    cache_data = json.dumps(dict(schema=2, discs=discs), sort_keys=True).encode('utf-8')
    if not write_cache:
        return text, len(rows), cache_data
    temporary = cache_path.with_name(cache_path.name + '.tmp')
    temporary.write_bytes(cache_data)
    temporary.replace(cache_path)
    return text, len(rows)


def support_base(root):
    directory = root/'.ohmy3do'
    marker = directory/'ohmy3do.ini'
    if (directory.is_symlink() or marker.is_symlink() or
            (directory.exists() and getattr(directory.lstat(),'st_file_attributes',0) & 0x400)):
        raise ValueError('Support directory/config must not be a symlink')
    return directory


def redundant_iso_name(text):
    """Recognize only a plain, single-track MODE1/2048 ISO wrapper."""
    directives = []
    name = None
    for line in text.splitlines():
        line = line.strip()
        if not line or re.match(r'^REM(?:\s|$)', line, re.I):
            continue
        match = re.fullmatch(r'FILE\s+"([^"\r\n]+)"\s+BINARY', line, re.I)
        if match:
            name = match[1]
            if (Path(name).suffix.lower() != '.iso' or name in ('.', '..') or
                    any(ord(c) < 32 or ord(c) == 127 or c in '/\\:' for c in name)):
                return None
            directives.append('FILE')
        elif re.fullmatch(r'TRACK\s+0*1\s+MODE1/2048', line, re.I):
            directives.append('TRACK')
        elif re.fullmatch(r'INDEX\s+0*1\s+00:00:00', line, re.I):
            directives.append('INDEX')
        elif re.fullmatch(r'CATALOG\s+\d{13}|(?:TITLE|PERFORMER|SONGWRITER)\s+"[^"\r\n]*"', line, re.I):
            continue
        else:
            return None
    return name if directives == ['FILE', 'TRACK', 'INDEX'] else None


def redundant_cue_iso(path):
    if path.stat().st_size > 256*1024:
        return None
    raw = path.read_bytes()
    if len(raw) > 256*1024:
        return None
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeError:
        text = raw.decode('cp1252', errors='replace')
    name = redundant_iso_name(text)
    if not name:
        return None
    matches = [p for p in path.parent.iterdir() if p.name.casefold() == name.casefold()]
    if len(matches) != 1 or not matches[0].is_file() or matches[0].is_symlink():
        return None
    return matches[0]


def existing_index(directory):
    path=directory/'index.3di'
    if path.is_symlink(): raise ValueError('Index must not be a symlink')
    if path.exists() and path.stat().st_size > 128*1024+24: raise ValueError('Existing index is oversized')
    if not path.exists() or path.stat().st_size == 0:
        return {}
    data=path.read_bytes()
    if len(data) < 24 or data[:8] != b'OM3DIDX1':
        raise ValueError(f'Invalid existing index: {path}; back it up before replacing it')
    count,size,crc,reserved=struct.unpack_from('>IIII',data,8)
    if count > 2048 or size > 128*1024 or len(data) != size+24 or reserved or binascii.crc32(data[24:]) != crc:
        raise ValueError(f'Invalid existing index: {path}')
    result={}; pos=24
    for _ in range(count):
        if pos+4 > len(data): raise ValueError('Truncated existing index')
        nk,nf=struct.unpack_from('>HH',data,pos); pos+=4
        if nk < 2 or nf < 6 or pos+nk+nf > len(data): raise ValueError('Invalid existing index entry')
        key,name=data[pos:pos+nk],data[pos+nk:pos+nk+nf]; pos+=nk+nf
        if key[-1:] != b'\0' or name[-1:] != b'\0' or b'\0' in key[:-1]+name[:-1]: raise ValueError('Invalid index strings')
        key,name=key[:-1].decode(),name[:-1].decode()
        if len(name.encode()) > 96 or name.startswith('.') or '/' in name or any(ord(c)<32 or ord(c)==127 or c in "\\:|" for c in name): raise ValueError('Invalid media filename in existing index')
        if fold(key) in result: raise ValueError('Duplicate existing association')
        result[fold(key)]=name
    if pos != len(data): raise ValueError('Trailing index data')
    return result


def index(root, include_save_names=True):
    if not root.is_dir():
        raise ValueError("USB root must be an existing directory")
    support = support_base(root)
    support.mkdir(exist_ok=True)
    games = []
    for path in root.rglob("*"):
        if path.is_symlink() or not path.is_file() or path.suffix.lower() not in (".iso", ".cue"):
            continue
        relative = path.relative_to(root)
        if path.name.lower() == "oh my 3do - folder navigation helper (keep).iso":
            continue
        if len(relative.parts) == 1 and path.name.lower() in ("boot.iso", "menu-demo.iso"):
            continue
        if (relative.parts[0].lower() in ("$boot", "system") or
                any(part.startswith('.') for part in relative.parts)):
            continue
        if path.suffix.lower() == '.cue' and redundant_cue_iso(path) is not None:
            continue
        if path.suffix.lower() == '.iso':
            issue = disc_data_issue(path)
            if issue:
                print('Skipped: ' + issue)
                continue
        game = relative.as_posix()
        if any(ord(c) < 32 or ord(c) == 127 or c in "\\:" for c in game) or len(game.encode()) > 8192:
            raise ValueError(f"Unsupported game path: {game}")
        games.append((fold(game), game, fold(path.stem)))
    if len({key for key, _, _ in games}) != len(games):
        raise ValueError("Game paths collide when ignoring ASCII case")
    library_rows=[]
    for _,game,_ in sorted(games):
        raw=game.encode()+b"\0"
        if any(len(part.encode())>=1024 for part in game.split('/')) or len(game.split('/'))>33:
            raise ValueError(f"Game path exceeds menu navigation limits: {game}")
        library_rows.append(struct.pack('>H',len(raw))+raw)
    library_payload=b''.join(library_rows)
    if len(games)>2048 or len(library_payload)+9*len(games)>128*1024:
        raise ValueError('Search index exceeds 2048 games or 128 KiB of paths')
    library_data=b'OM3DLIB1'+struct.pack('>IIII',len(games),len(library_payload),binascii.crc32(library_payload),0)+library_payload+b'END3DL01'
    library=support/'library.3di'
    database = support/'library.json'
    metadata = None
    titles = {}
    if database.is_file():
        if database.is_symlink() or database.stat().st_size > 16 * 1024 * 1024:
            raise ValueError('Invalid library metadata database')
        records = json.loads(database.read_text(encoding='utf-8'))
        if records.get('schema') != 1 or not isinstance(records.get('games'), list):
            raise ValueError('Invalid library metadata database')
        metadata = metadata_index(records['games'], [game for _, game, _ in games])
        titles = {fold(r.get('path') or ''): r.get('title') or '' for r in records['games'] if isinstance(r, dict)}
        target = support/'metadata.3di'
        if target.is_symlink() or (target.exists() and not target.is_file()) or (support/'metadata.3di.tmp').is_symlink():
            raise ValueError('Metadata index must be a regular file')
    if library.is_symlink() or (library.exists() and not library.is_file()):
        raise ValueError('Search index must be a regular file')
    oversized = []
    outputs = []
    for folder, suffix in (("covers", ".png"), ("previews", ".3dv")):
        directory = support/folder
        if not directory.exists():
            continue
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError(f"Media folder must be a real directory: {directory}")
        previous=existing_index(directory)
        assets = {}
        for path in directory.iterdir():
            if path.is_symlink() or not path.is_file() or path.suffix.lower() not in (('.png','.3dc') if folder=='covers' else (suffix,)):
                continue
            if path.name.lower().endswith('-large.3dc'):
                continue  # the full-screen version of another cover
            name = path.name
            if len(name.encode()) > 96 or name.startswith(".") or any(ord(c) < 32 or ord(c) == 127 or c in "\\:|" for c in name):
                raise ValueError(f"Unsupported media basename: {name}")
            if path.suffix.lower() == '.png':
                with path.open('rb') as stream:
                    header = stream.read(24)
                if len(header) == 24 and header[:8] == b'\x89PNG\r\n\x1a\n' and header[12:16] == b'IHDR':
                    w, h = struct.unpack_from('>II', header, 16)
                    if (w,h) != (96,120):
                        oversized.append((name,w,h))
            key = fold(path.stem)
            if key in assets:
                raise ValueError(f"Media filenames collide: {name}")
            if path.suffix.lower() == ".png" and path.stat().st_size > 512*1024:
                raise ValueError(f"PNG exceeds 512 KiB: {name}")
            assets[key] = name
        rows = []
        for folded, game, stem in sorted(games):
            name=assets.get(stem,previous.get(folded))
            # Explicit preview imports can supersede an older same-name clip.
            # Keep that association across indexing without deleting originals.
            associated=previous.get(folded)
            if folder == 'previews' and associated and (directory/associated).is_file():
                name=associated
            if not name or not (directory/name).is_file() or (directory/name).is_symlink():
                continue
            if Path(name).suffix.lower() not in ((".png",".3dc") if folder=="covers" else (".3dv",)):
                raise ValueError(f"Unsupported indexed media type: {name}")
            key, name = game.encode()+b"\0", name.encode()+b"\0"
            rows.append(struct.pack(">HH", len(key), len(name))+key+name)
        payload = b"".join(rows)
        if len(rows) > 2048 or len(payload) > 128*1024:
            raise ValueError(f"{folder} index exceeds 2048 entries or 128 KiB")
        data = b"OM3DIDX1"+struct.pack(">IIII",len(rows),len(payload),binascii.crc32(payload),0)+payload
        outputs.append((directory, data, len(rows)))
    if oversized:
        print(f'{len(oversized)} PNG cover(s) differ from the native 96x120 cover size:')
        for name,w,h in oversized[:5]:
            print(f'  {name}: {w}x{h}')
        print('Use resize-covers.py with a separate output folder for 96x120 PNGs; originals were not changed.')
    # Validate every index before replacing any existing output.
    temporary=support/'library.3di.tmp'
    if temporary.is_symlink(): raise ValueError('Temporary search index must not be a symlink')
    temporary.write_bytes(library_data); temporary.replace(library)
    if metadata is not None:
        temporary = support/'metadata.3di.tmp'
        temporary.write_bytes(metadata); temporary.replace(support/'metadata.3di')
    if include_save_names:
        names, count = save_names_text(root, support, games, titles)
        temporary = support/'save-names.txt.tmp'
        temporary.write_bytes(names.encode('ascii')); temporary.replace(support/'save-names.txt')
        print(f'Indexed {count} save names for the Save manager')
    print(f'Indexed {len(games)} games for Find in {library}')
    # Validate both indices before replacing either. Media files stay untouched.
    for directory, data, count in outputs:
        temporary = directory/"index.3di.tmp"
        temporary.write_bytes(data)
        temporary.replace(directory/"index.3di")
        print(f"Indexed {count} games in {directory}; media files unchanged")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("usb_root", type=Path)
    args = parser.parse_args()
    try:
        index(args.usb_root)
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
