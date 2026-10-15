#!/usr/bin/env python3
"""Identify and organize a 3DO USB library; preview first, apply explicitly."""
import argparse
import base64
import collections
import difflib
import hashlib
import html
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import sqlite3
import struct
import binascii
import sys
import tempfile
import time
import uuid
import zipfile


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), Path(__file__).with_name(name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


catalog_tools = module('library-catalog')
index_tools = module('index-media')
SKIP = {'covers', 'previews', 'music', 'saves', '.ohmy3do', '$boot', 'system'}
STATE_FILES = ['favorites.txt', 'recent.txt', 'last-position.txt', 'library.3di',
               'covers/index.3di', 'previews/index.3di', '.ohmy3do/library.json', '.ohmy3do/library.sqlite']
REGIONS = {'us': 'USA', 'usa': 'USA', 'na': 'USA', 'europe': 'Europe', 'eu': 'Europe',
           'pal': 'Europe', 'uk': 'Europe', 'jp': 'Japan', 'japan': 'Japan', 'j': 'Japan',
           'korea': 'Korea', 'kr': 'Korea', 'germany': 'Germany', 'france': 'France',
           'world': 'World', 'australia': 'Australia', 'canada': 'Canada'}
DISC = re.compile(r'\(\s*(?:disc|disk|cd)\s*0*(\d+)\s*\)', re.I)
DISC_SUFFIX = re.compile(r'(?:\s*[-_.]\s*|\s+)(?:disc|disk|cd)[\s_.-]*0*(\d+)(?:\s+of\s+\d+)?$', re.I)
FILE_LINE = re.compile(r'^([ \t]*FILE[ \t]+)(?:"([^"\r\n]+)"|([^\s]+))([ \t]+\S+[^\r\n]*)(\r?)$', re.I | re.M)


def digest(path):
    sha = hashlib.sha1()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(chunk)
    return sha.hexdigest()


def fingerprint(path, hashes=True):
    before = path.stat()
    result = dict(size=before.st_size, mtime_ns=before.st_mtime_ns)
    if hashes:
        result['sha1'] = digest(path)
    after = path.stat()
    if before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns:
        raise ValueError(f'File changed while reading: {path}')
    return result


def relative_path(root, relative):
    """Reject traversal, symlinks and names Windows/FAT cannot distinguish."""
    if not isinstance(relative, str) or not relative or '\\' in relative:
        raise ValueError(f'Invalid relative path: {relative!r}')
    parts = PurePosixPath(relative).parts
    if PurePosixPath(relative).is_absolute() or '/'.join(parts) != relative:
        raise ValueError(f'Noncanonical relative path: {relative!r}')
    for part in parts:
        if (part in ('.', '..') or part[-1:] in (' ', '.') or
                any(ord(c) < 32 or ord(c) == 127 or c in ':*?"<>|' for c in part)):
            raise ValueError(f'Unsafe USB path: {relative}')
    path = root
    for part in parts:
        if path.exists() and not path.is_dir():
            raise ValueError(f'Parent is not a directory: {path}')
        if path.is_dir():
            matches = [p.name for p in path.iterdir() if p.name.casefold() == part.casefold()]
            if any(name != part for name in matches):
                raise ValueError(f'Path differs only in case from an existing entry: {relative}')
        path = path / part
        if path.is_symlink():
            raise ValueError(f'Symlinks are not supported: {relative}')
    return path


def safe_name(name, maximum=110):
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f\x7f]', ' - ', name)
    name = re.sub(r'\s+', ' ', name).strip(' .')
    if not name:
        name = 'Untitled'
    if re.match(r'^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)', name, re.I):
        name = '_' + name
    raw = name.encode('utf-8')
    if len(raw) > maximum:
        tail = '-' + hashlib.sha1(raw).hexdigest()[:8]
        name = raw[:maximum-len(tail)].decode('utf-8', errors='ignore').rstrip(' .') + tail
    return name


def region_of(name):
    result = set()
    for group in re.findall(r'\(([^)]*)\)', name):
        for part in group.split(','):
            if part.strip().casefold() in REGIONS:
                result.add(REGIONS[part.strip().casefold()])
    return result


def disc_of(name):
    match = DISC.search(name) or DISC_SUFFIX.search(name)
    return int(match[1]) if match else None


def identification_title(name):
    return catalog_tools.title_key(DISC_SUFFIX.sub('', name).strip())


def without_disc(name):
    return DISC_SUFFIX.sub('', DISC.sub('', name)).strip()


def folder_regions(source):
    hints = set()
    for part in PurePosixPath(source).parts[:-1]:
        token = re.sub(r'^\d+[\s._-]+', '', part).strip().casefold()
        if len(token) > 1 and token in REGIONS:
            hints.add(REGIONS[token])
    return hints


def product_codes(name):
    return [tag.strip() for tag in re.findall(r'\[([^]]+)\]', name)
            if not tag.casefold().startswith('variant ') and len(tag.strip()) >= 4
            and re.fullmatch(r'[A-Za-z0-9]+(?:[ ._-][A-Za-z0-9]+)*', tag.strip())
            and re.search(r'[A-Za-z]', tag) and re.search(r'\d', tag)]


def serial_key(serial):
    return re.sub(r'[^a-z0-9]', '', serial.casefold())


def catalog_serials(game):
    result = set()
    for serial in [game.get('serial', '')] + [r.get('serial', '') for r in game['roms']]:
        for part in re.split(r'[,;/]', serial):
            if part.strip():
                result.add(serial_key(part))
    return result


def disc_stem(name, maximum=110):
    number = disc_of(name)
    base = without_disc(name)
    variant = re.search(r'\s*((?:\[[^][]+\]\s*)+)$', base)
    if variant and len(variant[1].encode()) > maximum - 20:
        variant = None
    suffix = (' ' + variant[1].strip()) if variant else ''
    if variant:
        base = base[:variant.start()].strip()
    suffix += f' (Disc {number})' if number is not None else ''
    return safe_name(base, maximum - len(suffix)) + suffix


def release_records(games):
    """Recover dump labels the DAT game title omits; disambiguate unnamed dumps.

    Serial-only duplicates with identical data describe the same dump. The
    fallback code identifies a disc dump, not an inferred multi-disc edition.
    """
    result, signatures = [], collections.defaultdict(set)
    for game in games:
        row = dict(game)
        stems = {re.sub(r'\s*\(Track\s+\d+\)\s*$', '', Path(rom['name']).stem, flags=re.I).strip()
                 for rom in game['roms'] if rom.get('name')}
        name = next(iter(stems)) if len(stems) == 1 else game['name']
        if catalog_tools.title_key(name) != catalog_tools.title_key(game['name']) or disc_of(name) != disc_of(game['name']):
            name = game['name']
        identity = sorted((str(rom['size']), next((key + ':' + rom[key].lower() for key in ('sha1', 'md5', 'crc')
                                                 if rom.get(key)), 'unknown')) for rom in game['roms'])
        row['_dump_signature'] = json.dumps(identity, separators=(',', ':'))
        row['_release_name'] = name
        signatures[name.casefold()].add(row['_dump_signature'])
        result.append(row)
    for row in result:
        row['_variant'] = None
        if len(signatures[row['_release_name'].casefold()]) > 1:
            code = hashlib.sha1(row['_dump_signature'].encode()).hexdigest()[:8].upper()
            row['_variant'] = code
            number = disc_of(row['_release_name'])
            name = without_disc(row['_release_name'])
            row['_release_name'] = name + f' [Variant {code}]' + (f' (Disc {number})' if number is not None else '')
    return result


def cue_files(root, relative):
    path = relative_path(root, relative)
    if path.stat().st_size > 256 * 1024:
        raise ValueError('CUE exceeds 256 KiB')
    raw = path.read_bytes()
    if len(raw) > 256 * 1024:
        raise ValueError('CUE exceeds 256 KiB')
    # Preserve the original encoding and all non-FILE lines verbatim.
    try:
        text, encoding = raw.decode('utf-8-sig'), 'utf-8-sig' if raw.startswith(b'\xef\xbb\xbf') else 'utf-8'
    except UnicodeDecodeError:
        text, encoding = raw.decode('cp1252'), 'cp1252'
    refs = []
    for match in FILE_LINE.finditer(text):
        name = match[2] or match[3]
        if '/' in name or '\\' in name or name in ('.', '..'):
            raise ValueError('CUE FILE references must name sibling files (no paths)')
        siblings = [p.name for p in path.parent.iterdir() if p.name.casefold() == name.casefold()]
        if len(siblings) != 1:
            raise ValueError(f'CUE data filename is missing or ambiguous: {name}')
        ref = str(PurePosixPath(relative).parent / siblings[0])
        target = relative_path(root, ref)
        if not target.is_file():
            raise ValueError(f'CUE data file is missing: {ref}')
        if target.suffix.lower() not in ('.bin', '.iso', '.wav', '.aiff', '.aif'):
            raise ValueError(f'Unsupported CUE data file: {ref}')
        if ref not in refs:
            refs.append(ref)
    if not refs:
        raise ValueError('No FILE references in CUE')
    if len(re.findall(r'^\s*FILE\b', text, re.I | re.M)) != len(list(FILE_LINE.finditer(text))):
        raise ValueError('Cannot parse every CUE FILE directive')
    return refs, text, encoding, raw


def scan_units(root):
    paths = []
    def walk(directory):
        for path in sorted(directory.iterdir()):
            if path.is_symlink():
                continue
            if path.is_dir():
                if path.name.casefold() not in SKIP and not path.name.startswith('.'):
                    walk(path)
            elif path.suffix.lower() in ('.iso', '.cue'):
                rel = path.relative_to(root).as_posix()
                if path.parent == root and path.name.lower() in ('boot.iso', 'menu-demo.iso'):
                    continue
                paths.append(rel)
    walk(root)
    units, consumed = [], set()
    for source in sorted(paths, key=lambda p: (Path(p).suffix.lower() != '.cue', p)):
        if source in consumed:
            continue
        unit = dict(source=source, files=[source])
        if Path(source).suffix.lower() == '.cue':
            try:
                refs, text, encoding, raw = cue_files(root, source)
                unit.update(files=[source] + refs, cue_text=text, cue_encoding=encoding)
                consumed.update(refs)
            except (ValueError, OSError) as error:
                unit['error'] = str(error)
                # Even a broken CUE may reference an existing ISO. Keep that
                # ISO with the unresolved set rather than moving it separately.
                path = relative_path(root, source)
                if path.stat().st_size <= 256 * 1024:
                    text = path.read_bytes().decode('utf-8-sig', errors='replace')
                    for match in FILE_LINE.finditer(text):
                        name = match[2] or match[3]
                        if '/' in name or '\\' in name:
                            continue
                        for sibling in path.parent.iterdir():
                            if sibling.name.casefold() == name.casefold() and sibling.is_file() and not sibling.is_symlink():
                                ref = sibling.relative_to(root).as_posix()
                                if ref not in unit['files']:
                                    unit['files'].append(ref)
                                    consumed.add(ref)
        units.append(unit)
    owners = collections.Counter(file for unit in units for file in unit['files'])
    for unit in units:
        if any(owners[file] > 1 for file in unit['files']):
            unit['error'] = 'Data files are shared between disc entries; organize this set manually'
    return units


def identify(unit, games, stats, override=None, quick=False):
    if override:
        selected = [g for g in games if g['_release_name'] == override]
        if not selected:
            selected = [g for g in games if g['name'] == override]
        if not selected or len({g['_release_name'] for g in selected}) != 1:
            return None, 'ambiguous', 'Override needs an exact release/variant name', sorted({g['_release_name'] for g in selected})
        return selected[0], 'manual', 'Explicit overrides file', []
    data = [stats[f] for f in unit['files'] if Path(f).suffix.lower() != '.cue']
    matches = []
    if not quick:
        checksums = sorted((f['size'], f['sha1'].lower()) for f in data)
        matches = [g for g in games if sorted((int(r['size']), r.get('sha1', '').lower()) for r in g['roms']) == checksums]
        if matches and len({g['_release_name'].casefold() for g in matches}) == 1:
            return matches[0], 'checksum', 'Complete data-file SHA-1 and size match', []
        if len(matches) > 1:
            return None, 'ambiguous', 'Checksums identify multiple catalog releases', [g['_release_name'] for g in matches]
    name = Path(unit['source']).stem
    exact = [g for g in games if name.casefold() in (g['_release_name'].casefold(), disc_stem(g['_release_name']).casefold())]
    if not exact:
        aliases = [g for g in games if g['name'].casefold() == name.casefold()]
        if len({g['_release_name'].casefold() for g in aliases}) == 1:
            exact = aliases
    if exact:
        return exact[0], 'filename', 'Exact catalog filename; dump checksum not verified', []
    title = identification_title(name)
    candidates = [g for g in games if identification_title(g['name']) == title and disc_of(g['name']) == disc_of(name)]
    title_candidates = candidates
    regions = region_of(name)
    hint = ''
    if not regions:
        folders = folder_regions(unit['source'])
        if len(folders) == 1:
            regions = folders
            hint = ' using folder region ' + next(iter(folders))
    if regions:
        candidates = [g for g in candidates if regions <= region_of(g['name'])]
    codes = product_codes(name)
    if codes:
        known_serials = [g for g in title_candidates if {serial_key(code) for code in codes} <= catalog_serials(g)]
        serial_matches = [g for g in candidates if {serial_key(code) for code in codes} <= catalog_serials(g)]
        if known_serials and not serial_matches:
            return None, 'ambiguous', 'Catalog serial conflicts with the filename/folder region; explicit override needed', sorted({g['_release_name'] for g in known_serials})
        serial_names = {g['_release_name'] for g in serial_matches}
        if len(serial_names) == 1:
            return serial_matches[0], 'filename', 'Exact title/disc/catalog serial match' + hint + '; dump checksum not verified', []
    candidates = list({g['_release_name'].casefold(): g for g in candidates}.values())
    # Do not silently choose a revision, prototype, demo or alternate language.
    if len(candidates) == 1 and not codes:
        tags = lambda s: {x.casefold() for x in re.findall(r'\(([^)]*)\)', s)
                          if re.search(r'\b(rev|beta|proto|demo|sample|unl|alt)\b', x, re.I)}
        if tags(name) == tags(candidates[0]['_release_name']):
            return candidates[0], 'filename', 'Unique title/region/disc match' + hint + '; dump checksum not verified', []
    if codes and candidates and len(regions) == 1:
        # A filename code absent from Redump can still identify a game-level
        # entry. Preserve it without assigning an unsupported Redump edition.
        metadata = {json.dumps(g.get('metadata', {}), sort_keys=True) for g in candidates}
        if len(metadata) == 1 and candidates[0].get('metadata'):
            row = dict(candidates[0])
            title = row['metadata'].get('title') or re.split(r'[\[(]', without_disc(name), maxsplit=1)[0].strip()
            region = next(iter(regions))
            base = f'{title} ({region})'
            label = base + ' ' + ' '.join('[' + code + ']' for code in codes)
            number = disc_of(name)
            row.update(name=base + (f' (Disc {number})' if number is not None else ''), region=region,
                       _release_name=label + (f' (Disc {number})' if number is not None else ''),
                       _variant='; '.join(codes), _edition_unresolved=True, roms=[])
            return row, 'filename', 'Game-level title/region match' + hint + '; source code preserved; exact edition unresolved; dump checksum not verified', []
    if candidates:
        return None, 'ambiguous', 'More than one release or variant; explicit override needed', [g['_release_name'] for g in candidates][:20]
    suggestions = difflib.get_close_matches(title, {identification_title(g['name']) for g in games}, n=3, cutoff=.65)
    return None, 'unmatched', 'No safe match; file stays in place', [g['_release_name'] for g in games if identification_title(g['name']) in suggestions and (disc_of(name) is None or disc_of(g['name']) == disc_of(name))][:12]


def asset_associations(root, folder):
    directory = relative_path(root, folder)
    if not directory.exists():
        return {}
    previous = index_tools.existing_index(directory)
    assets = {}
    for path in directory.iterdir():
        if path.is_file() and not path.is_symlink() and path.suffix.lower() in ('.png', '.3dc', '.3dv'):
            key = index_tools.fold(path.stem)
            if key in assets:
                raise ValueError(f'Ambiguous media basename: {path}')
            assets[key] = path.name
    previous = {key: name for key, name in previous.items()
                if relative_path(root, folder + '/' + name).is_file()}
    return dict(previous=previous, assets=assets)


def existing_asset(associations, source):
    return associations.get('assets', {}).get(index_tools.fold(Path(source).stem),
        associations.get('previous', {}).get(index_tools.fold(source)))


def build_plan(root, catalog, quick=False, overrides=None, accept_names=False):
    units = scan_units(root)
    games = release_records(catalog['games'])
    associations = {folder: asset_associations(root, folder) for folder in ('covers', 'previews')}
    entries = []
    for number, unit in enumerate(units, 1):
        print(f'[{number}/{len(units)}] {unit["source"]}', flush=True)
        row = dict(source=unit['source'], status='unmatched', moves=[], metadata={})
        if unit.get('error'):
            row['reason'] = unit['error']
            entries.append(row)
            continue
        stats = {file: fingerprint(relative_path(root, file), not quick or Path(file).suffix.lower() == '.cue') for file in unit['files']}
        game, confidence, reason, suggestions = identify(unit, games, stats,
            (overrides or {}).get(unit['source']), quick)
        row.update(confidence=confidence, reason=reason, suggestions=suggestions)
        if game is None:
            entries.append(row)
            continue
        canonical = game['_release_name']
        region = game.get('region') or ', '.join(sorted(region_of(canonical))) or 'Unknown'
        # Opaque dump codes do not imply an edition shared by other discs.
        # Keep them in filenames; retain the common game folder.
        group = re.sub(r'\s*\[Variant [0-9A-F]{8}\]', '', without_disc(canonical)).strip()
        if game.get('_edition_unresolved'):
            group = without_disc(game['name'])
        directory = PurePosixPath('Games') / safe_name(region) / safe_name(group)
        main = str(directory / (disc_stem(canonical) + Path(unit['source']).suffix.lower()))
        row.update(status='ready' if confidence in ('checksum', 'manual') or accept_names else 'review',
                   canonical=canonical, region=region, disc=disc_of(canonical), target=main,
                   catalog_name=None if game.get('_edition_unresolved') else game['name'], variant=game['_variant'],
                   edition_unresolved=bool(game.get('_edition_unresolved')),
                   metadata=game.get('metadata', {}), group=group,
                   cover=existing_asset(associations['covers'], unit['source']),
                   preview=existing_asset(associations['previews'], unit['source']))
        for track, file in enumerate(unit['files']):
            if file == unit['source']:
                target = main
            else:
                # Disc tags keep common track01.bin names from colliding across discs.
                track_tag = f' (Track {track:02d})' if len(unit['files']) > 2 else ''
                target = str(directory / (disc_stem(canonical, 95) + track_tag + Path(file).suffix.lower()))
            row['moves'].append(dict(source=file, target=target, fingerprint=stats[file]))
        if 'cue_text' in unit:
            row['cue_encoding'] = unit['cue_encoding']
        entries.append(row)
    targets = collections.defaultdict(list)
    for row in entries:
        for move in row['moves']:
            targets[move['target'].casefold()].append(row)
    for row in entries:
        for move in row['moves']:
            try:
                target = relative_path(root, move['target'])
                if (len(targets[move['target'].casefold()]) > 1 or
                        (target.exists() and move['source'] != move['target'])):
                    raise ValueError(f'Destination collision: {move["target"]}')
            except ValueError as error:
                row.update(status='blocked', reason=str(error))
    return dict(schema=1, root=str(root), created=time.time(), quick=quick,
                sources=catalog['sources'], entries=entries)


def json_bytes(value):
    return (json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def report_html(plan):
    escape = lambda value: html.escape(str(value) if value is not None else '')
    rows = []
    for row in plan['entries']:
        meta = row.get('metadata', {})
        suggestions = ''.join('<li>' + escape(name) + '</li>' for name in row.get('suggestions', []))
        notes = escape(row['reason'])
        if suggestions:
            notes += '<details><summary>Catalog suggestions (not selected)</summary><ul>' + suggestions + '</ul></details>'
        cells = [escape(row['status']) + '<br><small>' + escape(row.get('confidence', '')) + '</small>',
                 escape(row['source']) + '<br><strong>' + escape(row.get('target', 'Stays in place')) + '</strong>',
                 '<br>'.join(escape(meta.get(field)) for field in ('genre', 'publisher', 'year')), notes]
        rows.append('<tr class="' + escape(row['status']) + '">' + ''.join('<td>' + cell + '</td>' for cell in cells) + '</tr>')
    return ('''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Oh My 3DO — Library preview</title><style>
body{font:16px system-ui,sans-serif;background:#101b2d;color:#e1e9f3;margin:2rem}h1{color:#3dd9ba}
input{padding:.7rem;width:min(90%,36rem);margin:1rem 0;font:inherit}table{border-collapse:collapse;width:100%;background:#17283e}
th,td{padding:.8rem;text-align:left;border-bottom:1px solid #344960;vertical-align:top;overflow-wrap:anywhere}
th{background:#243c55}small,summary{color:#bacbdb}.blocked td:first-child,.unmatched td:first-child{color:#ffb6a5}
.ready td:first-child{color:#59e0bf}.review td:first-child{color:#ffda85}strong{font-weight:500;color:#9be7d9}
@media(max-width:700px){body{margin:.6rem}table{font-size:13px}th,td{padding:.4rem}}
</style><h1>Library preview</h1><p>USB root: ''' + escape(plan['root']) + '''</p>
<p>No USB files changed during scanning. Ready entries have checksum or manual matches.
Review entries use filenames and need <code>--accept-name-matches</code>. Other entries stay in place.
Metadata is community supplied; empty fields are unknown. No fuzzy suggestions are applied automatically.</p>
<input id="filter" type="search" placeholder="Filter by title, region, status, genre or publisher" aria-label="Filter games">
<table><thead><tr><th>Status / match</th><th>Original → proposed path</th><th>Genre / publisher / year</th><th>Notes</th></tr></thead>
<tbody>''' + ''.join(rows) + '''</tbody></table>
<script>document.getElementById('filter').addEventListener('input',function(){const q=this.value.toLowerCase();
document.querySelectorAll('tbody tr').forEach(row=>{row.hidden=!row.textContent.toLowerCase().includes(q);});});</script></html>''').encode('utf-8')


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.oh3do-', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)


class Transaction:
    def __init__(self, root, plan=None, path=None):
        self.root = root
        if path is not None:
            self.path = relative_path(root, path)
            self.data = json.loads(self.path.read_text(encoding='utf-8'))
            if self.data.get('root') != str(root) or self.data.get('schema') != 1:
                raise ValueError('Transaction belongs to a different USB root or version')
        else:
            self.path = relative_path(root, '.ohmy3do/transactions/' + uuid.uuid4().hex + '.json')
            self.data = dict(schema=1, root=str(root), state='applying', moves=[], writes=[], directories=[], plan=plan)
            self.save()

    def save(self):
        atomic_write(self.path, json_bytes(self.data))

    def parents(self, path):
        missing = []
        parent = path.parent
        while parent != self.root and not parent.exists():
            missing.append(parent)
            parent = parent.parent
        for parent in reversed(missing):
            relative = parent.relative_to(self.root).as_posix()
            self.data['directories'].append(relative)
            self.save()
            parent.mkdir(exist_ok=True)

    def move(self, operation):
        if operation['source'] == operation['target']:
            return
        source = relative_path(self.root, operation['source'])
        target = relative_path(self.root, operation['target'])
        if target.exists():
            raise ValueError(f'Refusing to overwrite: {target}')
        self.parents(target)
        entry = dict(operation, state='moving')
        self.data['moves'].append(entry)
        self.save()
        # Reserve the destination exclusively, then replace only our empty file.
        # This works on FAT, NTFS and POSIX without copying a whole disc image.
        with target.open('xb'):
            pass
        os.replace(source, target)
        entry['state'] = 'done'
        self.save()

    def write(self, relative, content):
        path = relative_path(self.root, relative)
        before = path.read_bytes() if path.exists() else None
        if before == content:
            return
        if before is not None and len(before) > 16 * 1024 * 1024:
            raise ValueError(f'Refusing to replace oversized support file: {relative}')
        entry = dict(path=relative, before=base64.b64encode(before).decode() if before is not None else None,
                     after=hashlib.sha256(content).hexdigest(), state='writing')
        self.data['writes'].append(entry)
        self.save()
        self.parents(path)
        atomic_write(path, content)
        entry['state'] = 'done'
        self.save()

    def undo(self):
        if self.data['state'] == 'undone':
            raise ValueError('Transaction is already undone')
        # Preflight the entire undo before changing anything. Modified files are
        # never overwritten, including covers or favorites edited after apply.
        for entry in reversed(self.data['writes']):
            path = relative_path(self.root, entry['path'])
            before = base64.b64decode(entry['before']) if entry['before'] is not None else None
            current = path.read_bytes() if path.exists() else None
            if current != before and (current is None or hashlib.sha256(current).hexdigest() != entry['after']):
                raise ValueError(f'Cannot undo: support file changed after organizing: {entry["path"]}')
        written = {entry['path']: entry for entry in self.data['writes']}
        for entry in reversed(self.data['moves']):
            source = relative_path(self.root, entry['source'])
            target = relative_path(self.root, entry['target'])
            if source.exists():
                if target.exists() and (entry['state'] == 'done' or target.stat().st_size):
                    raise ValueError(f'Cannot undo: original path is occupied: {source}')
                actual = source
            elif not target.is_file():
                raise ValueError(f'Cannot undo: moved file is missing: {target}')
            else:
                actual = target
            # A renamed CUE may also have updated FILE directives. Its original
            # bytes are in the write journal and are restored before moving it.
            write = written.get(entry['target']) if actual == target else None
            original = base64.b64decode(write['before']) if write and write['before'] is not None else None
            size = len(original) if original is not None else actual.stat().st_size
            sha = hashlib.sha1(original).hexdigest() if original is not None else None
            if size != entry['fingerprint']['size']:
                raise ValueError(f'Cannot undo: moved file size changed: {actual}')
            if entry['fingerprint'].get('sha1'):
                if (sha or digest(actual)) != entry['fingerprint']['sha1']:
                    raise ValueError(f'Cannot undo: moved file contents changed: {actual}')
            elif actual.stat().st_mtime_ns != entry['fingerprint']['mtime_ns']:
                raise ValueError(f'Cannot undo: moved file changed: {actual}')
        for entry in reversed(self.data['writes']):
            path = relative_path(self.root, entry['path'])
            if entry['before'] is None:
                path.unlink(missing_ok=True)
            else:
                atomic_write(path, base64.b64decode(entry['before']))
        for entry in reversed(self.data['moves']):
            source = relative_path(self.root, entry['source'])
            target = relative_path(self.root, entry['target'])
            if not source.exists():
                source.parent.mkdir(parents=True, exist_ok=True)
                os.replace(target, source)
            elif target.exists() and entry['state'] == 'moving' and not target.stat().st_size:
                target.unlink()
        for directory in reversed(self.data['directories']):
            try:
                relative_path(self.root, directory).rmdir()
            except OSError:
                pass
        self.data['state'] = 'undone'
        self.save()


def remap_text(root, tx, mapping):
    folded = {index_tools.fold(k): v for k, v in mapping.items()}
    for name in ('favorites.txt', 'recent.txt'):
        path = relative_path(root, name)
        if not path.exists():
            continue
        raw = path.read_bytes()
        text = raw.decode('utf-8-sig')
        lines = []
        for line in text.splitlines(keepends=True):
            # Favorites are volume-relative; comments and unknown paths survive.
            key = line.strip()
            replacement = folded.get(index_tools.fold(key))
            if replacement:
                begin = len(line) - len(line.lstrip())
                line = line[:begin] + replacement + line[begin + len(key):]
            lines.append(line)
        updated = ''.join(lines).encode('utf-8-sig' if raw.startswith(b'\xef\xbb\xbf') else 'utf-8')
        if len(updated) >= 8192:
            raise ValueError(f'Renamed paths exceed the menu limit for {name}')
        tx.write(name, updated)
    position = relative_path(root, 'last-position.txt')
    if position.exists():
        lines = position.read_text(encoding='utf-8').splitlines()
        if len(lines) != 3 or lines[0] != 'OH3DO_POSITION1':
            raise ValueError('Invalid saved position; no files were reorganized')
        folder = lines[1][9:] if lines[1].startswith('/Storage/') else ''
        current = (folder.rstrip('/') + '/' if folder else '') + lines[2]
        target = folded.get(index_tools.fold(current))
        if target:
            tx.write('last-position.txt', ('OH3DO_POSITION1\n/Storage/' + str(PurePosixPath(target).parent) + '\n' + Path(target).name + '\n').encode())
        elif any(old.startswith(current.rstrip('/') + '/') or (folder and old.startswith(folder + '/')) for old in mapping):
            tx.write('last-position.txt', b'OH3DO_POSITION1\n/Storage\n\n')


def media_index(mapping):
    rows = []
    for key, name in sorted(mapping.items(), key=lambda p: index_tools.fold(p[0])):
        key, name = key.encode() + b'\0', name.encode() + b'\0'
        rows.append(struct.pack('>HH', len(key), len(name)) + key + name)
    data = b''.join(rows)
    if len(rows) > 2048 or len(data) > 128 * 1024:
        raise ValueError('Media index exceeds menu capacity')
    return b'OM3DIDX1' + struct.pack('>IIII', len(rows), len(data), binascii.crc32(data), 0) + data


def prepare_covers(rows, cache, offline=False):
    if not rows:
        return {}, []
    try:
        names = catalog_tools.cover_names(cache, offline)
        resizer = module('resize-covers')
        from PIL import Image  # noqa: F401
    except (OSError, ValueError, ImportError) as error:
        return {}, [f'Covers skipped: {error}. Install Pillow with: py -m pip install Pillow']
    prepared, warnings = {}, []
    for row in rows:
        if row.get('cover'):
            continue
        canonical, group = row['canonical'], row['group']
        artwork_key = lambda value: re.sub(r'\s+(?=\[)', '', value).casefold()
        exact = [n for n in names if artwork_key(Path(n).stem) == artwork_key(canonical)]
        if not exact and row.get('catalog_name'):
            exact = [n for n in names if Path(n).stem.casefold() == row['catalog_name'].casefold()]
        if not exact:
            exact = [n for n in names if catalog_tools.title_key(Path(n).stem) == catalog_tools.title_key(group)
                     and row['region'] in region_of(Path(n).stem)]
        if not exact:
            warnings.append(f'No cover found: {canonical}')
            continue
        # Disc-specific scans may differ; only select a unique image or disc 1.
        first = [n for n in exact if disc_of(n) == 1]
        chosen = exact[0] if len(exact) == 1 else first[0] if len(first) == 1 else None
        if not chosen:
            warnings.append(f'Ambiguous cover: {canonical}')
            continue
        key = hashlib.sha1(chosen.encode()).hexdigest()
        output = cache / 'prepared' / (key + '.png')
        try:
            original = catalog_tools.download(catalog_tools.cover_url(chosen), cache / 'artwork' / (key + '.png'), 16 * 1024 * 1024, offline)
            if not output.exists():
                resizer.convert(original, output)
            basename = safe_name(row['group'], 65) + '-' + key[:8] + '.png'
            row['cover'] = basename
            row['cover_source'] = catalog_tools.cover_url(chosen)
            prepared[basename] = output.read_bytes()
        except (ValueError, OSError) as error:
            warnings.append(f'Cover failed for {canonical}: {error}')
    return prepared, warnings


def write_database(root, tx, plan, selected):
    previous = relative_path(root, '.ohmy3do/library.json')
    existing = json.loads(previous.read_text(encoding='utf-8')).get('games', []) if previous.exists() else []
    mapping = {r['source']: r['target'] for r in selected}
    records = {r['path']: r for r in existing}
    for row in plan['entries']:
        path = mapping.get(row['source'], row['source'])
        records.pop(row['source'], None)
        records[path] = dict(path=path, title=row.get('canonical'), region=row.get('region'),
            catalog_name=row.get('catalog_name'), variant=row.get('variant'),
            edition_unresolved=row.get('edition_unresolved',False),
            disc=row.get('disc'), identification=row.get('confidence', 'unmatched'),
            status='organized' if row in selected else row['status'], metadata=row.get('metadata', {}),
            cover=row.get('cover'), cover_source=row.get('cover_source'), suggestions=row.get('suggestions', []))
    # Do not retain stale records for discs no longer present.
    records = [r for r in records.values() if relative_path(root, r['path']).is_file()]
    data = dict(schema=1, generated=time.time(), sources=plan['sources'], games=sorted(records, key=lambda r: r['path']))
    tx.write('.ohmy3do/library.json', json_bytes(data))
    with tempfile.TemporaryDirectory(prefix='oh3do-db-') as directory:
        path = Path(directory) / 'library.sqlite'
        with sqlite3.connect(path) as db:
            db.execute('CREATE TABLE games (path TEXT PRIMARY KEY, title TEXT, region TEXT, disc INTEGER, genre TEXT, publisher TEXT, year INTEGER, developer TEXT, identification TEXT, cover TEXT, metadata_source TEXT, variant TEXT, catalog_name TEXT, edition_unresolved INTEGER)')
            db.execute('CREATE TABLE sources (url TEXT, sha256 TEXT)')
            db.executemany('INSERT INTO sources VALUES (?, ?)', [(s['url'], s.get('sha256')) for s in plan['sources']])
            for record in records:
                meta = record['metadata']
                db.execute('INSERT INTO games VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)', (record['path'], record['title'], record['region'], record['disc'], meta.get('genre'), meta.get('publisher'), meta.get('year'), meta.get('developer'), record['identification'], record['cover'], meta.get('source'), record.get('variant'), record.get('catalog_name'), int(record.get('edition_unresolved',False))))
        tx.write('.ohmy3do/library.sqlite', path.read_bytes())


def apply_plan(root, plan, cache, covers=True, offline=False, accept_names=False):
    if plan.get('schema') != 1 or plan.get('root') != str(root):
        raise ValueError('Plan version or USB root does not match; scan this drive again')
    rows = [r for r in plan['entries'] if r['status'] == 'ready' or (accept_names and r['status'] == 'review')]
    destinations, sources, cues = set(), set(), {}
    for row in rows:
        if row['confidence'] not in ('checksum', 'manual') and not accept_names:
            raise ValueError('Filename matches need --accept-name-matches after reviewing the plan')
        relative_path(root, row['source'])
        if not row.get('moves') or row['moves'][0]['source'] != row['source'] or row['moves'][0]['target'] != row['target']:
            raise ValueError('Invalid move group in plan')
        for move in row['moves']:
            source = relative_path(root, move['source'])
            target = relative_path(root, move['target'])
            if move['source'].casefold() in sources or move['target'].casefold() in destinations:
                raise ValueError('Duplicate source or destination in plan')
            sources.add(move['source'].casefold())
            destinations.add(move['target'].casefold())
            actual = fingerprint(source, False)
            if any(actual[k] != move['fingerprint'][k] for k in actual):
                raise ValueError(f'Source changed since scan: {source}; scan again')
            if target.exists() and move['source'] != move['target']:
                raise ValueError(f'Destination already exists: {target}')
        if Path(row['source']).suffix.lower() == '.cue':
            refs, text, encoding, raw = cue_files(root, row['source'])
            renames = {Path(m['source']).name.casefold(): Path(m['target']).name for m in row['moves'][1:]}
            if set(renames) != {Path(ref).name.casefold() for ref in refs}:
                raise ValueError('CUE move group does not match its FILE references')
            def directive(match):
                return match[1] + '"' + renames[(match[2] or match[3]).casefold()] + '"' + match[4] + match[5]
            cues[row['target']] = FILE_LINE.sub(directive, text).encode(encoding)
    # Preflight support paths too, before creating a transaction or moving discs.
    for name in STATE_FILES:
        path = relative_path(root, name)
        if path.exists() and (not path.is_file() or path.stat().st_size > 16 * 1024 * 1024):
            raise ValueError(f'Invalid support file: {name}')
    associations = {folder: asset_associations(root, folder) for folder in ('covers', 'previews')}
    prepared, warnings = prepare_covers(rows, cache, offline) if covers else ({}, [])
    for warning in warnings:
        print(warning)
    for name, content in prepared.items():
        path = relative_path(root, 'covers/' + name)
        if path.exists() and path.read_bytes() != content:
            raise ValueError(f'Cover destination is occupied: {path}')
    tx = Transaction(root, plan)
    print(f'Undo journal: {tx.path}', flush=True)
    try:
        for row in rows:
            for move in row['moves']:
                tx.move(move)
        for name, content in cues.items():
            tx.write(name, content)
        mapping = {r['source']: r['target'] for r in rows}
        remap_text(root, tx, mapping)
        for name, content in prepared.items():
            tx.write('covers/' + name, content)
        media_outputs = {}
        for folder in ('covers', 'previews'):
            path = relative_path(root, folder)
            if not path.exists():
                continue
            old = associations[folder]
            output = {key: value for key, value in old.get('previous', {}).items() if key not in {index_tools.fold(p) for p in mapping}}
            for row in plan['entries']:
                key = mapping.get(row['source'], row['source'])
                name = row.get('cover' if folder == 'covers' else 'preview') or existing_asset(old, row['source'])
                if name and relative_path(root, folder + '/' + name).is_file():
                    output[index_tools.fold(key)] = name
            media_outputs[folder] = media_index(output)
        # Let the existing, tested indexer validate and build search + media indices
        # in a temporary mirror. Only its generated files are committed to USB.
        with tempfile.TemporaryDirectory(prefix='oh3do-index-') as directory:
            mirror = Path(directory)
            for unit in scan_units(root):
                for file in unit['files'][:1]:
                    path = mirror / file
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()
            # CUE FILE parsing is unnecessary here: index() enumerates paths only.
            for folder in ('covers', 'previews'):
                usb = root / folder
                if usb.exists():
                    target = mirror / folder
                    target.mkdir(exist_ok=True)
                    for file in usb.iterdir():
                        if file.is_file() and not file.is_symlink() and file.suffix.lower() in ('.png', '.3dc', '.3dv', '.3di'):
                            if file.name == 'index.3di':
                                continue
                            # Indexing reads PNG headers and index bytes only.
                            with file.open('rb') as stream:
                                content = stream.read(24)
                            (target / file.name).write_bytes(content)
                    (target / 'index.3di').write_bytes(media_outputs[folder])
            index_tools.index(mirror)
            for name in ('library.3di', 'covers/index.3di', 'previews/index.3di'):
                if (mirror / name).exists():
                    tx.write(name, (mirror / name).read_bytes())
        write_database(root, tx, plan, rows)
        tx.data['state'] = 'complete'
        tx.save()
    except BaseException:
        print('Apply interrupted; attempting to undo this transaction.', file=sys.stderr)
        try:
            tx.undo()
        except (OSError, ValueError) as error:
            print(f'Automatic undo could not finish: {error}\nKeep the journal: {tx.path}', file=sys.stderr)
        raise
    print(f'Organized {len(rows)} disc entries. Database: {root / ".ohmy3do/library.sqlite"}')
    return tx


def default_cache():
    base = os.environ.get('LOCALAPPDATA') if os.name == 'nt' else os.environ.get('XDG_CACHE_HOME')
    return (Path(base) if base else Path.home() / '.cache') / 'oh-my-3do'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=default_cache())
    commands = parser.add_subparsers(dest='command', required=True)
    scan = commands.add_parser('scan', help='read the drive and write a preview plan; USB files stay unchanged')
    scan.add_argument('usb_root', type=Path)
    scan.add_argument('--plan', type=Path, default=Path('library-plan.json'))
    scan.add_argument('--quick', action='store_true', help='skip checksum reads; filename matches require review')
    scan.add_argument('--overrides', type=Path, help='JSON object mapping relative paths to exact catalog names')
    scan.add_argument('--offline', action='store_true')
    scan.add_argument('--refresh-catalog', action='store_true')
    scan.add_argument('--dat', type=Path, help='use a local Redump-format DAT')
    scan.add_argument('--launchbox', type=Path, help='use a local LaunchBox Metadata.zip')
    apply = commands.add_parser('apply', help='apply a reviewed plan and build covers, database and menu indices')
    apply.add_argument('usb_root', type=Path)
    apply.add_argument('--plan', type=Path, default=Path('library-plan.json'))
    apply.add_argument('--accept-name-matches', action='store_true', help='also accept the unverified filename matches in this plan')
    apply.add_argument('--no-covers', action='store_true')
    apply.add_argument('--offline', action='store_true')
    undo = commands.add_parser('undo', help='restore a transaction; refuses to overwrite later edits')
    undo.add_argument('usb_root', type=Path)
    undo.add_argument('journal', help='journal path relative to the USB root, printed by apply')
    args = parser.parse_args()
    try:
        if args.usb_root.is_symlink() or not args.usb_root.is_dir():
            raise ValueError('USB root must be a real, existing directory')
        root = args.usb_root.resolve()
        cache = args.cache.resolve()
        if cache == root or root in cache.parents:
            raise ValueError('Keep the download cache on the PC, outside the USB root')
        if args.command == 'scan':
            if args.plan.suffix.lower() != '.json':
                raise ValueError('Use a .json filename for the plan')
            # Even report/cache files must stay off the drive in preview mode.
            for target in (args.plan.resolve(), cache):
                if target == root or root in target.parents:
                    raise ValueError('Keep the preview plan and download cache on the PC, outside the USB root')
            catalog = catalog_tools.load_catalog(cache, args.offline, args.refresh_catalog, args.dat, args.launchbox)
            overrides = json.loads(args.overrides.read_text(encoding='utf-8')) if args.overrides else None
            plan = build_plan(root, catalog, args.quick, overrides)
            atomic_write(args.plan, json_bytes(plan))
            report = args.plan.with_suffix('.html')
            atomic_write(report, report_html(plan))
            counts = collections.Counter(row['status'] for row in plan['entries'])
            for row in plan['entries']:
                print(f'{row["status"].upper():9} {row["source"]}\n          {row.get("target", row["reason"])}')
            print(f'Plan: {args.plan.resolve()}\nOpen preview: {report.resolve()}\nCounts: {dict(counts)}\nNo USB files changed. Filename matches need review and --accept-name-matches.')
        elif args.command == 'apply':
            plan = json.loads(args.plan.read_text(encoding='utf-8'))
            apply_plan(root, plan, cache, not args.no_covers, args.offline, args.accept_name_matches)
        else:
            tx = Transaction(root, path=args.journal)
            tx.undo()
            print('Original file paths and support files restored.')
    except (ValueError, OSError, sqlite3.Error, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f'Error: {error}\n')


if __name__ == '__main__':
    main()
