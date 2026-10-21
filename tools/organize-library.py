#!/usr/bin/env python3
"""Identify and organize a 3DO USB library; preview first, apply explicitly."""
import argparse
import base64
import collections
from contextlib import closing
import difflib
import errno
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
import subprocess
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
sector_tools = module('sector-codec')
SCANNER_VERSION = '1.0.0'
SKIP = {'.ohmy3do', '$boot', 'system'}
STATE_FILES = ['favorites.txt', 'recent.txt', 'last-position.txt', 'library.3di', 'metadata.3di',
               'covers/index.3di', 'previews/index.3di', '.ohmy3do/library.json', '.ohmy3do/library.sqlite']
CATEGORIES = ('Games', 'Demos', 'Samplers', 'Applications', 'Multimedia', 'Educational', 'Bonus Discs', 'Adult')
def category_config(root):
    path = relative_path(root, '.ohmy3do/categories.json')
    if not path.exists():
        rows = [dict(id='category-'+str(i),name=name,hidden=False) for i,name in enumerate(CATEGORIES)]
        return dict(schema=1,categories=rows,defaults={name:row['id'] for name,row in zip(CATEGORIES,rows)})
    raw=path.read_bytes()
    if len(raw)>8192: raise ValueError('Category configuration exceeds 8 KiB')
    config=json.loads(raw.split(b'\0',1)[0]);rows=config.get('categories',[]);ids=set();names=set()
    if config.get('schema')!=1 or not isinstance(rows,list) or not 1<=len(rows)<=32: raise ValueError('Invalid category configuration')
    for row in rows:
        name=row.get('name','');identifier=row.get('id','')
        base=name.split('.')[0].upper()
        if not isinstance(row.get('hidden'),bool) or not re.fullmatch(r'[A-Za-z0-9-]{1,40}',identifier) or identifier in ids: raise ValueError('Invalid category identifier or visibility')
        if not name or len(name)>48 or name.strip()!=name or name.endswith('.') or any(ord(c)<32 or ord(c)>=127 or c in '<>:\"/\\|?*' for c in name) or name.casefold() in names or name.casefold()=='.ohmy3do' or base in ('CON','PRN','AUX','NUL') or re.fullmatch(r'(COM|LPT)[1-9]',base): raise ValueError('Invalid category name')
        ids.add(identifier);names.add(name.casefold())
    defaults=config.get('defaults',{})
    if set(defaults)!=set(CATEGORIES) or any(identifier not in ids for identifier in defaults.values()): raise ValueError('Invalid import category destinations')
    return config


def transfer_categories(source,target):
    config=category_config(source);previous=category_config(target);configured=relative_path(target,'.ohmy3do/categories.json').exists()
    hidden={c['id']:c['hidden'] for c in previous['categories']}
    for category in config['categories']:
        if configured or hidden.get(category['id'],False): category['hidden']=hidden.get(category['id'],category['hidden'])
    known={c['id'] for c in config['categories']}
    for category in previous['categories']:
        if category['id'] not in known and (configured or relative_path(target,category['name']).exists()): config['categories'].append(category)
    if len(config['categories'])>32 or len({c['name'].casefold() for c in config['categories']})!=len(config['categories']): raise ValueError('Library and drive categories conflict; resolve them in Manage categories before copying')
    return config


def category_names(root):
    return tuple(row['name'] for row in category_config(root)['categories'])


REGIONS = {'us': 'USA', 'usa': 'USA', 'na': 'USA', 'europe': 'Europe', 'eu': 'Europe',
           'pal': 'Europe', 'uk': 'Europe', 'gb': 'Europe', 'jp': 'Japan', 'japan': 'Japan', 'j': 'Japan',
           'korea': 'Korea', 'kr': 'Korea', 'germany': 'Germany', 'france': 'France',
           'world': 'World', 'australia': 'Australia', 'canada': 'Canada'}
DISC = re.compile(r'\(\s*(?:disc|disk|cd)\s*0*(\d+)(?:\s+of\s+\d+)?\s*\)', re.I)
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
        if path.is_symlink() or (path.exists() and getattr(path.lstat(),'st_file_attributes',0) & 0x400):
            raise ValueError(f'Symlinks/junctions are not supported: {relative}')
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
        for part in re.split(r'[,/-]', group):
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
        if target.parent == root and target.name.casefold() in ('boot.iso','menu-demo.iso'):
            raise ValueError('CUE references a root menu boot image; keep it separate')
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
                if path.name.lower() == 'oh my 3do - folder navigation helper (keep).iso':
                    continue
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
                if len(refs) == 1 and index_tools.redundant_iso_name(text):
                    unit.update(source=refs[0], name_source=source, files=[refs[0], source], ignored_cues=[source], aliases=[source])
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
            if unit.get('ignored_cues'):
                cue = unit['ignored_cues'][0]
                unit.update(source=cue, files=[cue] + [f for f in unit['files'] if f != cue])
                unit.pop('ignored_cues'); unit.pop('aliases')
            unit['error'] = 'Data files are shared between disc entries; organize this set manually'
        if not unit.get('error'):
            for file in unit['files']:
                if Path(file).suffix.lower() == '.cue':
                    continue
                issue = index_tools.disc_data_issue(relative_path(root,file))
                if issue:
                    unit.update(error=issue, invalid_data=True)
                    break
    return units


def identify(unit, games, stats, override=None, quick=False):
    if override:
        selected = [g for g in games if g['_release_name'] == override]
        if not selected:
            selected = [g for g in games if g['name'] == override]
        if not selected or len({g['_release_name'] for g in selected}) != 1:
            return None, 'ambiguous', 'Override needs an exact release/variant name', sorted({g['_release_name'] for g in selected})
        return selected[0], 'manual', 'Explicit overrides file; catalog dump checksum not verified', []
    data = [stats[f] for f in unit['files'] if Path(f).suffix.lower() != '.cue']
    matches = []
    if not quick:
        checksums = sorted((f['size'], f['sha1'].lower()) for f in data)
        matches = [g for g in games if sorted((int(r['size']), r.get('sha1', '').lower()) for r in g['roms']) == checksums]
        if matches and len({g['_release_name'].casefold() for g in matches}) == 1:
            return matches[0], 'checksum', 'Complete data-file SHA-1 and size match', []
        if len(matches) > 1:
            return None, 'ambiguous', 'Checksums identify multiple catalog releases', [g['_release_name'] for g in matches]
        if len(data) == 1 and data[0].get('mode1_raw'):
            raw = data[0]['mode1_raw']
            key = (raw['size'], raw['sha1'].lower())
            complete, partial = [], []
            for game in games:
                roms = game['roms']
                if len(roms) == 1:
                    if (int(roms[0]['size']), roms[0].get('sha1', '').lower()) == key:
                        complete.append(game)
                else:
                    # DAT ordering is not a track number. Only a named first
                    # track can identify data when the other tracks are absent.
                    first = [r for r in roms if re.search(r'(?:\(Track\s+0*1\)|\btrack0*1)\.bin$', r.get('name',''), re.I)]
                    if len(first) == 1 and (int(first[0]['size']),first[0].get('sha1','').lower()) == key:
                        partial.append(game)
            names = sorted({g['_release_name'] for g in complete + partial})
            if len(names) > 1:
                return None, 'ambiguous', 'Reconstructed data checksum identifies multiple releases; exact disc unresolved', names
            if complete:
                return complete[0], 'converted-checksum', 'Complete single-track SHA-1 and size match after MODE1/2048 reconstruction (LBA 0); source ISO unchanged', []
            if partial:
                return partial[0], 'data-track', 'Reconstructed first data-track SHA-1 and size match; remaining catalog tracks are absent and unverified; review required', []
    name = Path(unit.get('name_source', unit['source'])).stem
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
    if len(candidates) == 1:
        return None, 'unverified-release', 'Title matches one catalog entry, but its release/variant labels differ; exact edition and checksum remain unverified', [candidates[0]['_release_name']]
    if candidates:
        return None, 'ambiguous', 'More than one release or variant; explicit override needed', [g['_release_name'] for g in candidates][:20]
    suggestions = difflib.get_close_matches(title, {identification_title(g['name']) for g in games}, n=3, cutoff=.65)
    return None, 'unmatched', 'No safe match; file stays in place', [g['_release_name'] for g in games if identification_title(g['name']) in suggestions and (disc_of(name) is None or disc_of(g['name']) == disc_of(name))][:12]


def support_path(root, name):
    if name.startswith('.ohmy3do/'):
        return name
    return (index_tools.support_base(root)/name).relative_to(root).as_posix()


def asset_associations(root, folder):
    directory = relative_path(root, support_path(root, folder))
    if not directory.exists():
        return {}
    previous = index_tools.existing_index(directory)
    assets = {}
    for path in directory.iterdir():
        if path.is_file() and not path.is_symlink() and path.suffix.lower() in ('.png', '.3dc', '.3dv') and not path.name.lower().endswith('-large.3dc'):
            key = index_tools.fold(path.stem)
            if key in assets:
                raise ValueError(f'Ambiguous media basename: {path}')
            assets[key] = path.name
    previous = {key: name for key, name in previous.items()
                if relative_path(root, support_path(root, folder + '/' + name)).is_file()}
    return dict(previous=previous, assets=assets)


def existing_asset(associations, source):
    return associations.get('assets', {}).get(index_tools.fold(Path(source).stem),
        associations.get('previous', {}).get(index_tools.fold(source)))


def row_asset(associations, row):
    for source in [row['source']] + row.get('aliases', []):
        name = existing_asset(associations, source)
        if name:
            return name
    return None


def classify(game, override=None, allowed=None):
    if override is not None:
        choices = {name.casefold(): name for name in (allowed or CATEGORIES)}
        if not isinstance(override,str) or override.casefold() not in choices:
            raise ValueError('Category must be one of: ' + ', '.join(allowed or CATEGORIES))
        return choices[override.casefold()], 'Explicit category override'
    category = game.get('category', '').casefold()
    categories = {name.casefold(): name for name in CATEGORIES}
    categories.update(video='Multimedia', audio='Multimedia', coverdiscs='Samplers', **{'add-ons':'Games'})
    if category in categories:
        return categories[category], 'Redump category: ' + game['category']
    title = game['_release_name']
    metadata = game.get('metadata', {})
    genres = {s.strip().casefold() for s in re.split(r'[;,]',metadata.get('genre') or '') if s.strip()}
    release = (metadata.get('release_type') or '').strip().casefold()
    game_genres = {'action','adventure','arcade','board game','casino','compilation','fighting','flight simulator',
                   'horror','pinball','platform','puzzle','racing','role-playing','shooter',
                   'simulation','sports','strategy','visual novel','quiz','sandbox','party','construction and management simulation'}
    evidence = {}
    if re.search(r'\bbonus\s+(?:disc|cd)\b',title,re.I) or release in ('bonus disc','bonus cd'):
        evidence['Bonus Discs'] = 'Catalog title/release type identifies a bonus disc'
    if re.search(r'\b(?:sampler|sample\s+(?:disc|cd))\b', title, re.I) or release == 'sampler':
        evidence['Samplers'] = 'Catalog title/release type identifies a sampler'
    if re.search(r'\((?:demo|demonstration|trial)(?:[^)]*)\)|\b(?:demo|demonstration|trial\s+version)\b',title,re.I) or release in ('demo','demonstration','trial'):
        evidence['Demos'] = 'Catalog title/release type identifies a demo'
    if re.search(r'\b(?:storage\s+manager|system\s+utility|utilities|operating\s+system)\b',title,re.I) or genres & {'utility','utilities','application','applications','operating system'}:
        evidence['Applications'] = 'Catalog title/genre identifies an application'
    if re.search(r'\b(?:atlas|almanac|encyclop(?:e|ae)dia|dictionary|museum|art\s+gallery|documentary|photo\s*(?:cd|shoot)|slide\s*show|interactive\s+(?:book|magazine)|music\s+video|karaoke)\b',title,re.I) or genres & {'multimedia','reference','documentary','encyclopedia','music video','karaoke'}:
        evidence['Multimedia'] = 'Catalog title/genre identifies multimedia/reference content'
    if (genres & {'education','educational'} or re.search(r'\beducational\b',title,re.I)) and not evidence.keys() & {'Demos','Samplers','Multimedia','Bonus Discs'}:
        evidence['Educational'] = 'Catalog title/genre identifies educational software'
    if 'Multimedia' in evidence and genres & game_genres and not genres & {'education','educational','multimedia','reference','documentary','encyclopedia'}:
        return None, 'Multimedia title/genre conflicts with a game genre; choose a category'
    if len(evidence) == 1:
        return next(iter(evidence.items()))
    if evidence:
        return None, 'Mixed category evidence: ' + ', '.join(evidence) + '; choose a category'
    if re.search(r'\((?:sample|preview)(?:[^)]*)\)',title,re.I) or genres & {'music','life simulation'}:
        return None, 'Music/life-simulation/sample labels alone do not establish a category; choose a category'
    if genres & game_genres:
        return 'Games', 'Catalog genre identifies a game'
    return None, 'Insufficient evidence to distinguish a game from other software; choose a category'


def build_plan(root, catalog, quick=False, overrides=None, accept_names=False, categories=None):
    config=category_config(root);allowed=tuple(row['name'] for row in config['categories']);destinations={role:next(row['name'] for row in config['categories'] if row['id']==identifier) for role,identifier in config['defaults'].items()}
    units = scan_units(root)
    def choice(mapping, unit):
        values = {mapping[key] for key in [unit['source']] + unit.get('aliases', []) if key in mapping}
        if len(values) > 1:
            raise ValueError('Conflicting ISO/CUE choices for ' + unit['source'])
        return next(iter(values), None)
    categories = {} if categories is None else categories
    if not isinstance(categories,dict):
        raise ValueError('Category overrides must be a JSON object mapping relative paths to categories')
    for source, category in categories.items():
        if source not in {key for unit in units for key in [unit['source']] + unit.get('aliases', [])}:
            raise ValueError(f'Category override does not name a scanned disc entry: {source}')
        if not isinstance(category,str):
            raise ValueError('Category overrides must contain category names')
        classify({}, category, allowed)
    saved, registrations = {}, {}
    database = relative_path(root,'.ohmy3do/library.json')
    if database.exists():
        if not database.is_file() or database.stat().st_size > 16*1024*1024:
            raise ValueError('Invalid existing library.json')
        records = json.loads(database.read_text(encoding='utf-8')).get('games',[])
        registrations = {r['path']:r for r in records if r.get('identification') == 'unverified' and r.get('registration')}
        saved = {row['path']:row for row in records
                 if row.get('category') in allowed and row.get('category_reason') == 'Explicit category override'}
    games = release_records(catalog['games'])
    raw_sizes = {int(r['size']) for g in games for r in g['roms']
                 if re.fullmatch(r'[0-9a-fA-F]{40}',r.get('sha1',''))}
    encoder, announced = None, False
    associations = {folder: asset_associations(root, folder) for folder in ('covers', 'previews')}
    entries = []
    for number, unit in enumerate(units, 1):
        print(f'[{number}/{len(units)}] {unit["source"]}', flush=True)
        row = dict(source=unit['source'], status='unmatched', moves=[], metadata={},
                   ignored_cues=unit.get('ignored_cues', []), aliases=unit.get('aliases', []))
        if unit.get('error'):
            row['reason'] = unit['error']
            if unit.get('invalid_data'): row['status'] = 'blocked'
            entries.append(row)
            continue
        registered = registrations.get(unit['source'])
        if registered:
            certificate = registered['registration']
            same = set(certificate) == set(unit['files'])
            for name, expected in certificate.items():
                if not same:
                    break
                actual = fingerprint(relative_path(root,name), not quick)
                same = all(actual.get(k) == expected.get(k) for k in ('size','mtime_ns') + (() if quick else ('sha1',)))
            if same:
                row.update(status='registered', confidence='unverified', canonical=registered['title'],
                           target=unit['source'], region=registered.get('region'), category=registered['category'],
                           category_reason=registered.get('category_reason'), category_review=False,
                           registration=certificate, reason='Previously registered disc; catalog identity remains unverified; files unchanged' + (' (size/time only in quick scan)' if quick else ' (SHA-1 checked)'),
                           metadata=registered.get('metadata',{}), cover=registered.get('cover'), preview=registered.get('preview'))
                entries.append(row)
                continue
        track_modes = re.findall(r'^\s*TRACK\s+\d+\s+(\S+)', unit.get('cue_text', ''), re.I | re.M)
        if track_modes and all(mode.upper() == 'AUDIO' for mode in track_modes):
            row.update(confidence='audio-only', reason='Audio-only CD; not a game disc. WAV/CUE sets can be prepared with prepare-audio-disc.py; preserve originals until hardware playback is tested.',
                       category='Bonus Discs', category_review=False)
            entries.append(row)
            continue
        stats = {}
        data_files = [f for f in unit['files'] if Path(f).suffix.lower() != '.cue']
        for file in unit['files']:
            path = relative_path(root,file)
            size = path.stat().st_size
            reconstruct = (not quick and len(data_files) == 1 and file == unit['source'] and path.suffix.lower() == '.iso'
                           and size > 0 and size % 2048 == 0 and size//2048 <= sector_tools.MAX_SECTORS
                           and size//2048*2352 in raw_sizes)
            if reconstruct:
                if not announced:
                    encoder = sector_tools.native_encoder() or sector_tools.encode_python
                    print('Converted ISO verification: ' + ('native accelerator' if encoder != sector_tools.encode_python
                          else 'portable Python (slower; see build-sector-codec.py)'), flush=True)
                    announced = True
                stats[file] = sector_tools.digests(path,encoder)
            else:
                stats[file] = fingerprint(path, not quick or path.suffix.lower() == '.cue')
        game, confidence, reason, suggestions = identify(unit, games, stats,
            choice(overrides or {},unit), quick)
        if any(f.get('mode1_raw') for f in stats.values()):
            if confidence in ('filename','unmatched'):
                reason += '; reconstructed Mode 1 checksum did not match the catalog'
            elif confidence == 'manual':
                reason += '; explicit choice takes precedence over checksum identification'
        if row['ignored_cues']:
            reason += '; redundant ISO CUE will be archived if this entry is applied'
        row.update(confidence=confidence, reason=reason, suggestions=suggestions)
        if game is None:
            if confidence == 'unverified-release' and not quick:
                row['reason'] += '; no complete disc checksum matched'
            entries.append(row)
            continue
        canonical = game['_release_name']
        primary = next((REGIONS[tag.split(',')[0].strip().casefold()] for tag in re.findall(r'\(([^)]*)\)', canonical)
                        if tag.split(',')[0].strip().casefold() in REGIONS), 'Unknown')
        region = game.get('region') or primary
        # Opaque dump codes do not imply an edition shared by other discs.
        # Keep them in filenames; retain the common game folder.
        group = re.sub(r'\s*\[Variant [0-9A-F]{8}\]', '', without_disc(canonical)).strip()
        if game.get('_edition_unresolved'):
            group = without_disc(game['name'])
        override = choice(categories,unit)
        previous = next((saved[k] for k in [unit['source']] + unit.get('aliases', []) if k in saved),{})
        if override is None and previous.get('title') == canonical:
            override = previous['category']
        category, category_reason = classify(game,override,allowed)
        if override is None and any(re.sub(r'^\d+[\s._-]+','',part).casefold() == 'bonus discs'
                                    for part in PurePosixPath(unit['source']).parts[:-1]):
            category, category_reason = 'Bonus Discs', 'Source folder identifies a bonus disc'
        bonus_category=category == 'Bonus Discs'
        if override is None and category: category=destinations.get(category,category)
        directory = PurePosixPath(category) / safe_name(region) / safe_name(group) if category else None
        # An extra disc can share a base game's title but have its own audio or
        # pack-in labels. Keep those source labels instead of renaming it as the game.
        output_title = Path(unit.get('name_source', unit['source'])).stem if bonus_category else canonical
        main = str(directory / (disc_stem(output_title) + Path(unit['source']).suffix.lower())) if directory else None
        row.update(status='ready' if confidence in ('checksum', 'converted-checksum', 'manual') or accept_names else 'review',
                   canonical=canonical, region=region, disc=disc_of(canonical), target=main,
                   catalog_name=None if game.get('_edition_unresolved') else game['name'], variant=game['_variant'],
                   edition_unresolved=bool(game.get('_edition_unresolved')),
                   category=category, category_reason=category_reason, category_review=category is None,
                   metadata=game.get('metadata', {}), group=group,
                   cover=row_asset(associations['covers'], row),
                   preview=row_asset(associations['previews'], row))
        if category is None:
            row.update(status='category-review',reason=reason + '; ' + category_reason)
            entries.append(row)
            continue
        for track, file in enumerate(unit['files']):
            if file == unit['source']:
                target = main
            elif file in unit.get('ignored_cues', []):
                target = '.ohmy3do/ignored-cues/' + hashlib.sha256(file.encode()).hexdigest()[:24] + '.cue'
            else:
                # Disc tags keep common track01.bin names from colliding across discs.
                track_tag = f' (Track {track:02d})' if len(unit['files']) > 2 else ''
                target = str(directory / (disc_stem(output_title, 95) + track_tag + Path(file).suffix.lower()))
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
    if not quick:
        duplicates = collections.defaultdict(list)
        for row in entries:
            data = [m for m in row['moves'] if Path(m['source']).suffix.lower() != '.cue']
            if data and all(m['fingerprint'].get('sha1') for m in data):
                key = tuple(sorted((m['fingerprint']['size'], m['fingerprint']['sha1']) for m in data))
                duplicates[key].append(row)
        for group in duplicates.values():
            if len(group) > 1:
                for row in group:
                    row['duplicate_sources'] = [other['source'] for other in group if other is not row]
                    row['reason'] += '; identical data-file checksums also found at: ' + ', '.join(row['duplicate_sources']) + '; CUE layouts are not compared; no duplicate is deleted automatically'
    return dict(schema=2, root=str(root), created=time.time(), quick=quick,
                sources=catalog['sources'], entries=entries, categories=allowed)


def json_bytes(value):
    return (json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def review_bucket(row):
    if row['status'] in ('ready', 'review', 'registered'):
        return 'matched'
    if row['status'] == 'blocked' or (row['status'] == 'unmatched' and not row.get('confidence')):
        return 'problems'
    if row['status'] == 'category-review' or row.get('confidence') == 'ambiguous':
        return 'decisions'
    return 'unidentified'


def report_html(plan):
    escape = lambda value: html.escape(str(value) if value is not None else '')
    counts = collections.Counter(review_bucket(r) for r in plan['entries'])
    tabs = ''.join('<button type="button" data-view="' + key + '">' + label + ' (' + str(counts[key]) + ')</button>'
                   for key, label in (('decisions', 'Needs a choice'), ('unidentified', 'Unidentified'),
                                      ('problems', 'File problems'), ('matched', 'Matched')))
    rows = []
    for row in plan['entries']:
        meta = row.get('metadata', {})
        suggestions = list(dict.fromkeys(row.get('suggestions', [])))
        notes = escape(row['reason'])
        if row.get('category_reason'):
            notes += '<br><small>' + escape(row['category_reason']) + '</small>'
        controls = ''
        if row['status'] == 'category-review':
            controls = '<label>Category <select class="category"><option value="">Leave in place</option>' + ''.join(
                '<option>' + escape(c) + '</option>' for c in plan.get('categories',CATEGORIES)) + '</select></label>'
        elif suggestions and row['status'] == 'unmatched' and row.get('confidence'):
            controls = '<label>Release <select class="release"><option value="">Leave in place</option>' + ''.join(
                '<option>' + escape(name) + '</option>' for name in suggestions) + '</select></label>'
        if suggestions:
            notes += '<details><summary>Catalog suggestions (not selected)</summary><ul>' + ''.join(
                '<li>' + escape(name) + '</li>' for name in suggestions) + '</ul></details>'
        cells = [escape(row['status']) + '<br><small>' + escape(row.get('confidence', '')) + '</small>',
                 escape(row['source']) + '<br><strong>' + escape(row.get('target') or 'Stays in place') + '</strong>',
                 escape(row.get('category') or ('Needs classification' if row['status'] == 'category-review' else 'Unknown')),
                 '<br>'.join(escape(meta.get(field)) for field in ('genre', 'publisher', 'year')), notes + controls]
        rows.append('<tr hidden data-bucket="' + review_bucket(row) + '" data-source="' + escape(row['source']) + '" class="' + escape(row['status']) + '">' + ''.join('<td>' + cell + '</td>' for cell in cells) + '</tr>')
    return ('''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Oh My 3DO — Library preview</title><style>
body{font:16px system-ui,sans-serif;background:#101b2d;color:#e1e9f3;margin:2rem}h1{color:#3dd9ba}
input,select,button{padding:.6rem;margin:.3rem;font:inherit;background:#243c55;color:#e1e9f3;border:1px solid #52718d;border-radius:.3rem}
input{width:min(85%,36rem)}select{max-width:30rem;width:100%;margin:.5rem 0}button{cursor:pointer}button[aria-pressed=true]{border-color:#3dd9ba;color:#3dd9ba}
table{border-collapse:collapse;width:100%;background:#17283e}th,td{padding:.8rem;text-align:left;border-bottom:1px solid #344960;vertical-align:top;overflow-wrap:anywhere}
th{background:#243c55}small,summary{color:#bacbdb}.blocked td:first-child,.unmatched td:first-child{color:#ffb6a5}
.ready td:first-child{color:#59e0bf}.review td:first-child,.category-review td:first-child{color:#ffda85}strong{font-weight:500;color:#9be7d9}
@media(max-width:700px){body{margin:.6rem}table{font-size:13px}th,td{padding:.4rem}}
</style><h1>Library preview</h1><p>USB root: ''' + escape(plan['root']) + '''</p>
<p><strong>Matched entries can be accepted as a batch.</strong> Filename matches are unverified,
but already identify a single game/release or an explicitly marked game-level entry.
Use <code>--accept-name-matches</code> on apply after checking a sample of their proposed paths.
Unidentified entries, unresolved choices and file problems stay in place; they do not block the matched batch.</p>
<p>No USB files changed. Suggestions are never selected automatically. Category choices and release
choices can be downloaded as JSON files below. Scan again with <code>--quick --overrides overrides.json --categories categories.json</code>
to build a new plan from those choices. Only include a flag if its file was downloaded.
Download choices before closing this page; changing this report does not edit the plan or drive.</p>
<nav aria-label="Review groups">''' + tabs + '''<button type="button" data-view="all">All</button></nav>
<input id="filter" type="search" placeholder="Filter this group by title, category, region or publisher" aria-label="Filter library">
<p id="visible" aria-live="polite"></p><button id="export-releases" type="button">Download release choices</button>
<button id="export-categories" type="button">Download category choices</button><span id="choices" aria-live="polite"></span>
<noscript><p>Enable JavaScript for filtering and choice downloads.</p><style>tr[hidden]{display:table-row}</style></noscript>
<table><thead><tr><th>Status / match</th><th>Original → proposed path</th><th>Category</th><th>Genre / publisher / year</th><th>Notes / choice</th></tr></thead>
<tbody>''' + ''.join(rows) + '''</tbody></table>
<script>
let view='decisions';
const rows=Array.from(document.querySelectorAll('tbody tr'));
function filter(){const q=document.getElementById('filter').value.toLowerCase();let count=0;
rows.forEach(row=>{row.hidden=(view!=='all'&&row.dataset.bucket!==view)||!row.textContent.toLowerCase().includes(q);if(!row.hidden)count++;});
document.querySelectorAll('[data-view]').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.view===view)));
document.getElementById('visible').textContent=count+' entries shown. Unresolved entries may be left in place.';}
function choices(kind){return Object.fromEntries(rows.flatMap(row=>{const select=row.querySelector('select.'+kind);return select&&select.value?[[row.dataset.source,select.value]]:[];}));}
function update(){document.getElementById('choices').textContent=' '+Object.keys(choices('release')).length+' releases, '+Object.keys(choices('category')).length+' categories selected';}
function download(kind,name){const blob=new Blob([JSON.stringify(choices(kind),null,2)+String.fromCharCode(10)],{type:'application/json'});
const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
document.querySelectorAll('[data-view]').forEach(b=>b.addEventListener('click',()=>{view=b.dataset.view;filter();}));
document.getElementById('filter').addEventListener('input',filter);
document.querySelectorAll('select').forEach(s=>s.addEventListener('change',update));
document.getElementById('export-releases').addEventListener('click',()=>download('release','overrides.json'));
document.getElementById('export-categories').addEventListener('click',()=>download('category','categories.json'));
filter();update();
</script></html>''').encode('utf-8')


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

    def move_folder(self, source_name, target_name):
        source = relative_path(self.root, source_name)
        target = relative_path(self.root, target_name)
        if not source.is_dir() or target.exists():
            raise ValueError('Missing source or occupied folder: ' + target_name)
        self.parents(target)
        entry = dict(source=source_name, target=target_name)
        self.data.setdefault('folder_moves', []).append(entry)
        self.save()
        source.rename(target)

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
        for entry in self.data.get('folder_moves', []):
            source = relative_path(self.root, entry['source'])
            target = relative_path(self.root, entry['target'])
            if source.exists() and target.exists():
                raise ValueError('Cannot undo: original folder is occupied: ' + entry['source'])
            if not source.exists() and not target.is_dir():
                raise ValueError('Cannot undo: moved folder is missing: ' + entry['target'])
        for entry in self.data.get('copies', []):
            path = relative_path(self.root, entry['path'])
            if path.exists():
                h = hashlib.sha256()
                with path.open('rb') as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b''):
                        h.update(block)
                if h.hexdigest() != entry['sha256']:
                    raise ValueError('Cannot undo: copied file changed: ' + entry['path'])
        for name in self.data.get('removed_directories', []):
            path = relative_path(self.root,name)
            if path.exists() and not path.is_dir():
                raise ValueError('Cannot undo: original directory is occupied by a file: ' + name)
        # Preflight the entire undo before changing anything. Modified files are
        # never overwritten, including covers or favorites edited after apply.
        restored = {}
        for entry in reversed(self.data['writes']):
            path = relative_path(self.root, entry['path'])
            before = base64.b64decode(entry['before']) if entry['before'] is not None else None
            if path not in restored:
                restored[path] = path.read_bytes() if path.exists() else None
            current = restored[path]
            if current != before and (current is None or hashlib.sha256(current).hexdigest() != entry['after']):
                raise ValueError(f'Cannot undo: support file changed after organizing: {entry["path"]}')
            # A support index can be regenerated more than once during apply.
            # Validate each earlier write against the bytes the later undo will
            # restore, without changing any files during preflight.
            restored[path] = before
        written = {}
        for entry in self.data['writes']:
            written.setdefault(entry['path'], entry)
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
        for entry in reversed(self.data.get('copies', [])):
            relative_path(self.root, entry['path']).unlink(missing_ok=True)
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
        for entry in reversed(self.data.get('folder_moves', [])):
            source = relative_path(self.root, entry['source'])
            target = relative_path(self.root, entry['target'])
            if not source.exists():
                source.parent.mkdir(parents=True, exist_ok=True)
                target.rename(source)
        for name in self.data.get('removed_directories', []):
            relative_path(self.root,name).mkdir(parents=True,exist_ok=True)
        for directory in reversed(self.data['directories']):
            try:
                relative_path(self.root, directory).rmdir()
            except OSError:
                pass
        self.data['state'] = 'undone'
        self.save()


def prepare_text_remaps(root, mapping):
    """Parse menu text before moving discs; firmware text ends at its first NUL."""
    updates = {}
    folded = {index_tools.fold(k): v for k, v in mapping.items()}
    for name in ('favorites.txt', 'recent.txt'):
        path = relative_path(root, support_path(root, name))
        if not path.exists():
            continue
        raw = path.read_bytes()
        text = raw.split(b'\0', 1)[0].decode('utf-8-sig')
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
        updates[support_path(root,name)] = updated
    position = relative_path(root, support_path(root,'last-position.txt'))
    if position.exists():
        raw = position.read_bytes().split(b'\0', 1)[0]
        try:
            lines = raw.decode('utf-8-sig').splitlines()
            if len(raw) >= 9216 or len(lines) != 3 or lines[0] != 'OH3DO_POSITION1':
                raise ValueError('Invalid saved-position header or length')
            if lines[1] != '/Storage' and not lines[1].startswith('/Storage/'):
                raise ValueError('Invalid saved-position root')
            parts = lines[1][9:].split('/') if lines[1] != '/Storage' else []
            if lines[2]:
                parts.append(lines[2])
            if len(lines[1].encode()) > 8192 or any(not part or len(part.encode()) >= 1024 or part in ('.', '..') or
                    any(ord(c) < 32 or ord(c) == 127 or c in '/\\:' for c in part) for part in parts):
                raise ValueError('Invalid saved-position path')
        except (UnicodeError, ValueError):
            print('Warning: invalid last-position.txt; resetting the saved menu position to the USB root. Undo restores the original file.')
            updates[support_path(root,'last-position.txt')] = b'OH3DO_POSITION1\n/Storage\n\n'
            return updates
        folder = lines[1][9:] if lines[1].startswith('/Storage/') else ''
        current = (folder.rstrip('/') + '/' if folder else '') + lines[2]
        target = folded.get(index_tools.fold(current))
        if target:
            updates[support_path(root,'last-position.txt')] = ('OH3DO_POSITION1\n/Storage/' + str(PurePosixPath(target).parent) + '\n' + Path(target).name + '\n').encode()
        elif any(old.startswith(current.rstrip('/') + '/') or (folder and old.startswith(folder + '/')) for old in mapping):
            updates[support_path(root,'last-position.txt')] = b'OH3DO_POSITION1\n/Storage\n\n'
    return updates


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
        output = cache / 'prepared' / (key + '-fast96.3dc')
        large = cache / 'prepared' / (key + '-fast192.3dc')
        try:
            original = catalog_tools.download(catalog_tools.cover_url(chosen), cache / 'artwork' / (key + '.png'), 16 * 1024 * 1024, offline)
            if not output.exists():
                resizer.fast_convert(original, output)
            if not large.exists():
                resizer.fast_convert(original, large, resizer.LARGE_SIZE)
            basename = safe_name(row['group'], 65) + '-' + key[:8] + '.3dc'
            row['cover'] = basename
            row['cover_source'] = catalog_tools.cover_url(chosen)
            prepared[basename] = output.read_bytes()
            prepared[resizer.large_name(basename)] = large.read_bytes()
        except (ValueError, OSError) as error:
            warnings.append(f'Cover failed for {canonical}: {error}')
    return prepared, warnings


def local_media_files(directory, extensions):
    if not directory.is_dir() or directory.is_symlink():
        raise ValueError('Media directory must be a real, existing directory')
    directory = directory.resolve()
    paths = []
    for folder, dirs, files in os.walk(directory, followlinks=False):
        dirs[:] = sorted(d for d in dirs if not (Path(folder)/d).is_symlink())
        for name in sorted(files):
            path = Path(folder)/name
            if path.suffix.lower() in extensions and not path.is_symlink() and path.is_file():
                paths.append(path)
    return paths


def local_media_matches(row, paths, *, other_regions=False):
    """Prefer exact/compatible names; previews may fall back across regions."""
    def kind(name):
        match = re.search(r'\b(demo|demonstration|trial|sampler)\b', name, re.I)
        return match[1].casefold() if match else 'game'
    for name in filter(None, (row['canonical'], row.get('catalog_name'), Path(row['source']).stem)):
        exact = [p for p in paths if p.stem.casefold() == name.casefold()]
        if exact:
            return exact
    # Demo/sampler media must not become full-game art via title_key.
    candidates = [p for p in paths
        if identification_title(p.stem) == identification_title(row['canonical'])
        and kind(p.stem) == kind(row['canonical'])
        and (disc_of(p.stem) is None or disc_of(p.stem) == row.get('disc'))]
    compatible = [p for p in candidates
        if not region_of(p.stem) or bool(region_of(p.stem) & region_of(row['canonical']))]
    return candidates if other_regions and not compatible else compatible


def prepare_local_covers(rows, directory):
    """Prepare local front-box images, with no network requests or replacements."""
    resizer = module('resize-covers')
    paths = local_media_files(directory, resizer.EXTENSIONS)
    directory = directory.resolve()
    prepared, warnings, converted = {}, [], {}
    for row in rows:
        if row.get('cover'):
            continue
        candidates = local_media_matches(row, paths)
        if len(candidates) != 1:
            warnings.append(f'{"Ambiguous" if candidates else "No"} local cover found: {row["canonical"]}')
            continue
        image = candidates[0]
        try:
            if image not in converted:
                with tempfile.TemporaryDirectory(prefix='oh3do-cover-') as temp:
                    target, large = Path(temp)/'cover.3dc', Path(temp)/'large.3dc'
                    resizer.fast_convert(image, target)
                    resizer.fast_convert(image, large, resizer.LARGE_SIZE)
                    content, large_content = target.read_bytes(), large.read_bytes()
                key = hashlib.sha256(content).hexdigest()[:16]
                converted[image] = ('cover-' + key + '.3dc', content, large_content)
            basename, content, large_content = converted[image]
            prepared[basename] = content
            prepared[resizer.large_name(basename)] = large_content
            row['cover'] = basename
            row['cover_source'] = image.relative_to(directory).as_posix()
        except (ValueError, OSError) as error:
            warnings.append(f'Local cover failed for {row["canonical"]}: {error}')
    return prepared, warnings


def prepare_previews(rows, directory, start=0, duration=None, sound=True, replace=False):
    """Import local, already downloaded clips; ambiguous matches stay untouched."""
    clips = local_media_files(directory, ('.mp4', '.avi', '.mkv', '.mov', '.webm'))
    if not 0 <= start <= 86400:
        raise ValueError('Preview start must be 0-86400 seconds')
    directory = directory.resolve()
    converter = module('prepare-preview')
    prepared, warnings, converted = {}, [], {}
    for row in rows:
        if row.get('preview') and not replace:
            continue
        candidates = local_media_matches(row, clips, other_regions=True)
        if len(candidates) != 1:
            warnings.append(f'{"Ambiguous" if candidates else "No"} preview found: {row["canonical"]}')
            continue
        clip = candidates[0]
        try:
            if clip not in converted:
                # Conversion happens on the PC before any USB changes. Include
                # content and settings in the name so reruns never replace art.
                key = hashlib.sha256((digest(clip) + f':{start}:15:{duration or "15"}:{int(sound)}:5:160x120x30:{converter.V5_BYTES//1024}k:fixed-fps:cap15').encode()).hexdigest()[:16]
                with tempfile.TemporaryDirectory(prefix='oh3do-preview-') as temp:
                    target = Path(temp)/'clip.3dv'
                    converter.convert(clip, target, start=start, duration=duration, sound=sound)
                    converted[clip] = (key, target.read_bytes())
            key, content = converted[clip]
            basename = 'clip-' + key + '.3dv'
            prepared[basename] = content
            row['preview'] = basename
            row['preview_source'] = clip.relative_to(directory).as_posix()
        except (ValueError, OSError, subprocess.CalledProcessError) as error:
            warnings.append(f'Preview failed for {row["canonical"]}: {error}')
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
            category=row.get('category'),category_review=row.get('category_review',True),
            category_reason=row.get('category_reason'),
            catalog_name=row.get('catalog_name'), variant=row.get('variant'),
            edition_unresolved=row.get('edition_unresolved',False),
            disc=row.get('disc'), identification=row.get('confidence', 'unmatched'),
            status='organized' if row in selected else row['status'], metadata=row.get('metadata', {}),
            cover=row.get('cover'), cover_source=row.get('cover_source'),
            preview=row.get('preview'), preview_source=row.get('preview_source'), suggestions=row.get('suggestions', []))
        if row.get('registration'):
            records[path]['registration'] = row['registration']
    # Do not retain stale records for discs no longer present.
    records = [r for r in records.values() if relative_path(root, r['path']).is_file()]
    data = dict(schema=1, generated=time.time(), sources=plan['sources'], games=sorted(records, key=lambda r: r['path']))
    tx.write('.ohmy3do/library.json', json_bytes(data))
    games = [(index_tools.fold(r['path']), r['path'], '') for r in records]
    titles = {index_tools.fold(r['path']): r.get('title') or '' for r in records}
    names, count, cache = index_tools.save_names_text(root, index_tools.support_base(root), games, titles, write_cache=False)
    tx.write('.ohmy3do/save-names-cache.json', cache)
    tx.write('.ohmy3do/save-names.txt', names.encode('ascii'))
    print(f'Indexed {count} save names for the Save manager')
    tx.write('.ohmy3do/metadata.3di', index_tools.metadata_index(records, [r['path'] for r in records if Path(r['path']).suffix.lower() in ('.iso', '.cue')]))
    with tempfile.TemporaryDirectory(prefix='oh3do-db-') as directory:
        path = Path(directory) / 'library.sqlite'
        # SQLite's transaction context commits/rolls back, but does not close
        # the connection. Close it before copying/removing the file on Windows.
        with closing(sqlite3.connect(path)) as db, db:
            db.execute('CREATE TABLE games (path TEXT PRIMARY KEY, title TEXT, region TEXT, disc INTEGER, genre TEXT, publisher TEXT, year INTEGER, developer TEXT, identification TEXT, cover TEXT, metadata_source TEXT, variant TEXT, catalog_name TEXT, edition_unresolved INTEGER, category TEXT, category_review INTEGER, category_reason TEXT)')
            db.execute('CREATE TABLE sources (url TEXT, sha256 TEXT)')
            db.executemany('INSERT INTO sources VALUES (?, ?)', [(s['url'], s.get('sha256')) for s in plan['sources']])
            for record in records:
                meta = record['metadata']
                db.execute('INSERT INTO games VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)', (record['path'], record['title'], record['region'], record['disc'], meta.get('genre'), meta.get('publisher'), meta.get('year'), meta.get('developer'), record['identification'], record['cover'], meta.get('source'), record.get('variant'), record.get('catalog_name'), int(bool(record.get('edition_unresolved',False))),record.get('category'),int(bool(record.get('category_review',True))),record.get('category_reason')))
        tx.write('.ohmy3do/library.sqlite', path.read_bytes())


def write_indexes(root, tx, media_outputs=None):
    """Build in a PC mirror; journal index writes without altering media files."""
    if media_outputs is None:
        media_outputs = {folder: media_index(index_tools.existing_index(index_tools.support_base(root)/folder))
                         for folder in ('covers', 'previews')}
    with tempfile.TemporaryDirectory(prefix='oh3do-index-') as directory:
        mirror = Path(directory)
        for unit in scan_units(root):
            if unit.get('invalid_data'):
                continue
            for file in unit['files'][:1]:
                path = mirror / file
                path.parent.mkdir(parents=True, exist_ok=True)
                if path.suffix.lower() == '.iso':
                    # Preserve the validation prefix without copying images.
                    with relative_path(root,file).open('rb') as stream:
                        path.write_bytes(stream.read(16))
                else:
                    path.touch()
        # CUE sets were validated by scan_units; no CUE parsing is needed here.
        for folder in ('covers', 'previews'):
            usb = index_tools.support_base(root) / folder
            if usb.exists():
                target = index_tools.support_base(mirror) / folder
                target.mkdir(parents=True, exist_ok=True)
                for file in usb.iterdir():
                    if file.is_file() and not file.is_symlink() and file.suffix.lower() in ('.png', '.3dc', '.3dv', '.3di'):
                        if file.name == 'index.3di':
                            continue
                        # Indexing reads PNG headers and index bytes only.
                        with file.open('rb') as stream:
                            content = stream.read(24)
                        (target / file.name).write_bytes(content)
                (target / 'index.3di').write_bytes(media_outputs[folder])
        index_tools.index(mirror, include_save_names=False)
        for name in ('library.3di', 'covers/index.3di', 'previews/index.3di'):
            located = support_path(root,name)
            if (mirror / located).exists():
                tx.write(located, (mirror / located).read_bytes())


def register_disc(root, source, title, category, region='Unknown', apply=False):
    """Register an existing uncatalogued disc without moving it or inventing a release."""
    relative_path(root, source)
    category, category_reason = classify({}, category, category_names(root))
    if not title.strip() or len(title.encode('utf-8')) >= 1024 or any(ord(c) < 32 or ord(c) == 127 for c in title):
        raise ValueError('Use a nonempty title shorter than 1024 UTF-8 bytes, without control characters')
    if region != 'Unknown':
        region = REGIONS.get(region.casefold())
        if not region:
            raise ValueError('Use a known region name or Unknown')
    units = [u for u in scan_units(root) if source in [u['source']] + u.get('aliases', [])]
    if len(units) != 1 or units[0].get('error'):
        raise ValueError('Choose one existing, valid ISO/CUE disc entry; ' + (units[0].get('error', '') if units else 'entry not found'))
    unit = units[0]
    if Path(unit['source']).suffix.lower() == '.iso':
        path = relative_path(root, unit['source'])
        with path.open('rb') as stream:
            header = stream.read(7)
        if path.stat().st_size % 2048 or header != b'\x01ZZZZZ\x01':
            raise ValueError('ISO must have a 3DO volume header and whole 2048-byte sectors')
    previous = root/'.ohmy3do/library.json'
    database = json.loads(previous.read_text(encoding='utf-8')) if previous.exists() else {'sources':[], 'games':[]}
    existing = next((r for r in database['games'] if r['path'] == unit['source']), {})
    if existing.get('identification') in ('checksum','converted-checksum'):
        raise ValueError('Disc already has a verified catalog identity; use index-media.py to refresh Find')
    certificate = {name: fingerprint(relative_path(root,name)) for name in unit['files']}
    row = dict(source=unit['source'], target=unit['source'], canonical=title.strip(), region=region,
               category=category, category_reason=category_reason, category_review=False, metadata={},
               status='registered', confidence='unverified', registration=certificate, moves=[],
               reason='Explicit registration; catalog identity and bootability are not certified')
    for folder, key in (('covers','cover'), ('previews','preview')):
        row[key] = row_asset(asset_associations(root,folder), row)
    print(f'{"Register" if apply else "Would register"}: {unit["source"]}\nTitle: {title.strip()}\nCategory: {category}\nCatalog identity: unverified; ISO/CUE and data files stay in place.')
    if not apply:
        print('No changes made. Add --apply to update the database and Find.')
        return row
    tx = Transaction(root)
    print(f'Undo journal: {tx.path}', flush=True)
    try:
        write_indexes(root, tx)
        for name, expected in certificate.items():
            if fingerprint(relative_path(root,name)) != expected:
                raise ValueError('Disc changed during registration: ' + name)
        write_database(root, tx, dict(entries=[row], sources=database['sources']), [])
        tx.data['state'] = 'complete'; tx.save()
    except BaseException:
        tx.undo()
        raise
    print('Registered in the library and Find. No disc files moved or modified.')
    return row


def apply_plan(root, plan, cache, covers=True, offline=False, accept_names=False, preview_dir=None, preview_start=0, cover_dir=None, preview_duration=None, preview_sound=True, replace_previews=False):
    if plan.get('schema') != 2 or plan.get('root') != str(root):
        raise ValueError('Plan version or USB root does not match; scan this drive again')
    if cover_dir is not None and not covers:
        raise ValueError('--cover-dir cannot be combined with --no-covers')
    rows = [r for r in plan['entries'] if r['status'] == 'ready' or (accept_names and r['status'] == 'review')]
    for row in plan['entries']:
        if row.get('registration'):
            for name, expected in row['registration'].items():
                if fingerprint(relative_path(root,name)) != expected:
                    raise ValueError('Registered disc changed since scan: ' + name)
    destinations, sources, cues = set(), set(), {}
    for row in rows:
        if row.get('category_review') or row.get('category') not in category_names(root) or not isinstance(row.get('target'),str) or PurePosixPath(row['target']).parts[0] != row['category']:
            raise ValueError('Category is unresolved or does not match the destination; scan again with a category override')
        if row['confidence'] not in ('checksum', 'converted-checksum', 'manual') and not accept_names:
            raise ValueError('Filename and partial data-track matches need --accept-name-matches after reviewing the plan')
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
            if source.suffix.lower() != '.cue':
                issue = index_tools.disc_data_issue(source)
                if issue:
                    raise ValueError(issue + '; no files were reorganized')
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
        path = relative_path(root, support_path(root, name))
        if path.exists() and (not path.is_file() or path.stat().st_size > 16 * 1024 * 1024):
            raise ValueError(f'Invalid support file: {name}')
    mapping = {source:r['target'] for r in rows for source in [r['source']] + r.get('aliases', [])}
    text_updates = prepare_text_remaps(root, mapping)
    associations = {folder: asset_associations(root, folder) for folder in ('covers', 'previews')}
    # Media added after scan also takes precedence over imports and downloads.
    for row in rows:
        row['cover'] = row_asset(associations['covers'], row)
        row['preview'] = row_asset(associations['previews'], row)
    preview_files, preview_warnings = prepare_previews(rows, preview_dir, preview_start, preview_duration, preview_sound, replace_previews) if preview_dir is not None else ({}, [])
    if cover_dir is not None:
        prepared, warnings = prepare_local_covers(rows, cover_dir)
    else:
        prepared, warnings = prepare_covers(rows, cache, offline) if covers else ({}, [])
    for warning in warnings + preview_warnings:
        print(warning)
    for name, content in prepared.items():
        path = relative_path(root, support_path(root, 'covers/' + name))
        if path.exists() and path.read_bytes() != content:
            raise ValueError(f'Cover destination is occupied: {path}')
    for name, content in preview_files.items():
        path = relative_path(root, support_path(root, 'previews/' + name))
        if path.exists() and path.read_bytes() != content:
            raise ValueError(f'Preview destination is occupied: {path}')
    tx = Transaction(root, plan)
    print(f'Undo journal: {tx.path}', flush=True)
    try:
        for row in rows:
            for move in row['moves']:
                tx.move(move)
        for name, content in cues.items():
            tx.write(name, content)
        for name, content in text_updates.items():
            tx.write(name, content)
        for name, content in prepared.items():
            tx.write(support_path(root, 'covers/' + name), content)
        for name, content in preview_files.items():
            tx.write(support_path(root, 'previews/' + name), content)
        media_outputs = {}
        for folder in ('covers', 'previews'):
            path = relative_path(root, support_path(root, folder))
            if not path.exists():
                continue
            old = associations[folder]
            output = {key: value for key, value in old.get('previous', {}).items() if key not in {index_tools.fold(p) for p in mapping}}
            for row in plan['entries']:
                key = mapping.get(row['source'], row['source'])
                name = row.get('cover' if folder == 'covers' else 'preview') or existing_asset(old, row['source'])
                if name and relative_path(root, support_path(root, folder + '/' + name)).is_file():
                    output[index_tools.fold(key)] = name
            media_outputs[folder] = media_index(output)
        write_indexes(root, tx, media_outputs)
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


def directory_link(path):
    # Windows junctions are reparse points but aren't always reported as
    # symlinks, especially on Python versions before Path.is_junction().
    return path.is_symlink() or bool(getattr(path.lstat(), 'st_file_attributes', 0) & 0x400)


def cleanup_empty(root, apply=False):
    """Remove only empty directories, deepest first; never delete files."""
    if directory_link(root) or not root.is_dir():
        raise ValueError('USB root must be a real directory, not a link or junction')
    protected = SKIP | {'system volume information', '$recycle.bin'}
    candidates = []
    def visit(directory):
        empty = True
        for path in sorted(directory.iterdir()):
            if directory_link(path) or not path.is_dir():
                empty = False
            elif (path.name.casefold() in protected or path.name.startswith('.') or
                  getattr(path.lstat(), 'st_file_attributes', 0) & 0x6):
                empty = False
            elif not visit(path):
                empty = False
        if empty and directory != root:
            candidates.append(directory.relative_to(root).as_posix())
        return empty
    visit(root)
    removed = []
    for name in candidates:
        if not apply:
            print(f'Would remove: {name}')
            continue
        path = root / name
        # Recheck every component in case the drive changed after enumeration.
        if any(directory_link(p) for p in (path, *path.parents) if p == root or root in p.parents):
            print(f'Kept link/junction: {name}')
            continue
        try:
            path.rmdir()
            removed.append(name)
            print(f'Removed: {name}')
        except OSError as error:
            if error.errno not in (errno.ENOTEMPTY, errno.EEXIST):
                raise
            print(f'Kept folder that is no longer empty: {name}')
    if apply:
        print(f'Removed {len(removed)} empty directories. No files deleted.')
    else:
        print(f'{len(candidates)} empty directories could be removed. No changes made. Add --apply to remove them.')
    return removed if apply else candidates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version',action='version',version='Oh My 3DO library organizer '+SCANNER_VERSION)
    parser.add_argument('--cache', type=Path, default=default_cache())
    commands = parser.add_subparsers(dest='command', required=True)
    report_command = commands.add_parser('report', help='rebuild the interactive HTML report from an existing plan without scanning')
    report_command.add_argument('plan', type=Path)
    report_command.add_argument('--output', type=Path, help='HTML output (default: beside the plan)')
    register = commands.add_parser('register', help='preview adding an uncatalogued disc to the database and Find; never move disc files')
    register.add_argument('usb_root', type=Path)
    register.add_argument('disc', help='volume-relative ISO/CUE path')
    register.add_argument('--title', required=True)
    register.add_argument('--category', required=True)
    register.add_argument('--region', default='Unknown')
    register.add_argument('--apply', action='store_true')
    cleanup = commands.add_parser('clean-empty', help='preview empty directories; add --apply to remove them')
    cleanup.add_argument('usb_root', type=Path)
    cleanup.add_argument('--apply', action='store_true', help='remove empty directories; never delete files')
    scan = commands.add_parser('scan', help='read the drive and write a preview plan; USB files stay unchanged')
    scan.add_argument('usb_root', type=Path)
    scan.add_argument('--plan', type=Path, default=Path('library-plan.json'))
    scan.add_argument('--quick', action='store_true', help='skip checksum reads; filename matches require review')
    scan.add_argument('--overrides', type=Path, help='JSON object mapping relative paths to exact catalog names')
    scan.add_argument('--categories', type=Path, help='JSON object mapping relative disc paths to ' + ', '.join(CATEGORIES))
    scan.add_argument('--offline', action='store_true')
    scan.add_argument('--refresh-catalog', action='store_true')
    scan.add_argument('--dat', type=Path, help='use a local XML/Logiqx or clrmamepro DAT (plain text or ZIP)')
    scan.add_argument('--launchbox', type=Path, help='use a local LaunchBox Metadata.zip')
    apply = commands.add_parser('apply', help='apply a reviewed plan and build covers, database and menu indices')
    apply.add_argument('usb_root', type=Path)
    apply.add_argument('--plan', type=Path, default=Path('library-plan.json'))
    apply.add_argument('--accept-name-matches', action='store_true', help='also accept reviewed filename and partial data-track matches in this plan')
    apply.add_argument('--no-covers', action='store_true', help='skip all cover imports and downloads')
    apply.add_argument('--cover-dir', type=Path, help='import local front-box images instead of downloading covers; subfolders included')
    apply.add_argument('--preview-dir', type=Path, help='import matching clips downloaded with EmuMovies Sync or another source')
    apply.add_argument('--preview-start', type=float, default=0, help='clip offset in seconds (default: 0); requires --preview-dir')
    apply.add_argument('--preview-duration', default=None, help='maximum seconds (default and overall cap: 15); clip shortens further to preserve 15 fps; requires --preview-dir')
    apply.add_argument('--preview-silent', action='store_true', help='omit preview audio; requires --preview-dir')
    apply.add_argument('--replace-previews', action='store_true', help='import new previews even when an association exists; keep old media files')
    apply.add_argument('--offline', action='store_true')
    undo = commands.add_parser('undo', help='restore a transaction; refuses to overwrite later edits')
    undo.add_argument('usb_root', type=Path)
    undo.add_argument('journal', help='journal path relative to the USB root, printed by apply')
    args = parser.parse_args()
    try:
        if args.command == 'report':
            plan = json.loads(args.plan.read_text(encoding='utf-8'))
            if plan.get('schema') != 2:
                raise ValueError('Unsupported plan version; scan again')
            output = args.output or args.plan.with_suffix('.html')
            if output.suffix.lower() != '.html':
                raise ValueError('Report output must end in .html')
            root = Path(plan['root']).resolve()
            if output.resolve() == root or root in output.resolve().parents:
                raise ValueError('Keep the report on the PC, outside the USB root')
            atomic_write(output, report_html(plan))
            print(f'Open report: {output.resolve()}\nNo scan or USB changes performed.')
            return
        if args.usb_root.is_symlink() or not args.usb_root.is_dir():
            raise ValueError('USB root must be a real, existing directory')
        if args.command == 'clean-empty':
            if directory_link(args.usb_root):
                raise ValueError('USB root must not be a link or junction')
            cleanup_empty(args.usb_root.resolve(), args.apply)
            return
        root = args.usb_root.resolve()
        if directory_link(root):
            raise ValueError('USB root must not be a link or junction')
        if args.command == 'register':
            register_disc(root,args.disc,args.title,args.category,args.region,args.apply)
            return
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
            categories = json.loads(args.categories.read_text(encoding='utf-8')) if args.categories else None
            plan = build_plan(root, catalog, args.quick, overrides,categories=categories)
            atomic_write(args.plan, json_bytes(plan))
            report = args.plan.with_suffix('.html')
            atomic_write(report, report_html(plan))
            counts = collections.Counter(row['status'] for row in plan['entries'])
            groups = collections.Counter(review_bucket(row) for row in plan['entries'])
            for row in plan['entries']:
                print(f'{row["status"].upper():9} {row["source"]}\n          {row.get("target") or row["reason"]}')
            print(f'Plan: {args.plan.resolve()}\nOpen preview: {report.resolve()}\nCounts: {dict(counts)}\nNo USB files changed. Filename matches need review and --accept-name-matches.')
            print(f'Batch matches: {groups["matched"]}; choices: {groups["decisions"]}; unidentified: {groups["unidentified"]}; file problems: {groups["problems"]}. Unresolved entries stay in place and do not block batch apply.')
        elif args.command == 'apply':
            plan = json.loads(args.plan.read_text(encoding='utf-8'))
            if (args.preview_start or args.preview_duration or args.preview_silent or args.replace_previews) and args.preview_dir is None:
                raise ValueError('Preview options require --preview-dir')
            apply_plan(root, plan, cache, not args.no_covers, args.offline, args.accept_name_matches, args.preview_dir, args.preview_start, args.cover_dir, args.preview_duration, not args.preview_silent, args.replace_previews)
        else:
            tx = Transaction(root, path=args.journal)
            tx.undo()
            print('Original file paths and support files restored.')
    except (ValueError, OSError, sqlite3.Error, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f'Error: {error}\n')


if __name__ == '__main__':
    main()
