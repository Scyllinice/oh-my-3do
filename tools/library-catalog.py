#!/usr/bin/env python3
"""Runtime data adapters for organize-library.py. No third-party data is bundled."""
import hashlib
import json
from pathlib import Path
import re
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

REDUMP = ('https://raw.githubusercontent.com/libretro/libretro-database/master/'
          'metadat/redump/The%203DO%20Company%20-%203DO.dat')
LAUNCHBOX = 'https://gamesdb.launchbox-app.com/Metadata.zip'
ART = 'https://thumbnails.libretro.com/The%203DO%20Company%20-%203DO/Named_Boxarts/'
AGENT = 'Mozilla/5.0 OhMy3DO-Library/1.0 (https://github.com/Scyllinice/oh-my-3do)'


def download(url, target, maximum, offline=False):
    """Cache complete responses only; interrupted downloads never become cache hits."""
    target = Path(target)
    if target.is_file():
        if target.stat().st_size > maximum:
            raise ValueError(f'Oversized cached download: {target}')
        return target
    if offline:
        raise ValueError(f'Offline cache is missing: {target.name}')
    if not url.startswith('https://'):
        raise ValueError('Downloads require HTTPS')
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(target.suffix + '.download')
    print(f'Downloading {url}', flush=True)
    try:
        req = urllib.request.Request(url, headers={'User-Agent': AGENT})
        with urllib.request.urlopen(req, timeout=45) as response, temporary.open('wb') as output:
            size = 0
            expected = response.headers.get('Content-Length')
            while True:
                block = response.read(1024 * 1024)
                if not block:
                    break
                size += len(block)
                if size > maximum:
                    raise ValueError(f'Download exceeds {maximum} bytes: {url}')
                output.write(block)
            if expected is not None and size != int(expected):
                raise ValueError(f'Incomplete download: {url}')
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def dat_games(text):
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+', text)
    pos = 0

    def block(nested=False):
        nonlocal pos
        fields = {}
        while pos < len(tokens):
            key = tokens[pos]
            pos += 1
            if key == ')':
                if not nested:
                    raise ValueError('Unexpected DAT closing bracket')
                return fields
            if pos >= len(tokens):
                raise ValueError('Truncated DAT')
            value = tokens[pos]
            pos += 1
            if value == '(':
                value = block(True)
            elif value.startswith('"'):
                value = value[1:-1].replace('\\"', '"').replace('\\\\', '\\')
            fields.setdefault(key, []).append(value)
        if nested:
            raise ValueError('Truncated DAT block')
        return fields

    result = []
    for item in block().get('game', []):
        row = {k: v[0] for k, v in item.items() if k != 'rom'}
        row['roms'] = [{k: v[0] for k, v in rom.items()} for rom in item.get('rom', [])]
        if row.get('name') and row['roms']:
            result.append(row)
    if not result:
        raise ValueError('No games in identification DAT')
    return result


def title_key(name):
    """Normalize formatting, never invent fuzzy matches."""
    import unicodedata
    name = re.sub(r'\([^)]*\)|\[[^]]*\]', '', name)
    name = name.strip()
    moved = re.match(r'^(.*),\s*(The|A|An)$', name, re.I)
    if moved:
        name = moved[2] + ' ' + moved[1]
    name = re.sub(r'^(the|a|an)\s+', '', name, flags=re.I)
    name = ''.join(c for c in unicodedata.normalize('NFKD', name) if not unicodedata.combining(c))
    return ''.join(c for c in name.casefold() if c.isalnum())


def launchbox_games(path):
    """Stream the large XML; discard every completed top-level record."""
    games, aliases = {}, []
    with zipfile.ZipFile(path) as archive:
        info = archive.getinfo('Metadata.xml')
        if info.file_size > 1200 * 1024 * 1024:
            raise ValueError('LaunchBox XML is oversized')
        with archive.open(info) as stream:
            depth, root = 0, None
            for event, element in ET.iterparse(stream, events=('start', 'end')):
                if event == 'start':
                    depth += 1
                    if depth == 1:
                        root = element
                    continue
                if depth == 2:
                    if element.tag == 'Game' and '3DO' in (element.findtext('Platform') or ''):
                        game = {k: element.findtext(k) or '' for k in (
                            'Name', 'DatabaseID', 'Genres', 'Publisher', 'Developer', 'ReleaseDate', 'ReleaseYear')}
                        game['aliases'] = []
                        games[game['DatabaseID']] = game
                    elif element.tag == 'GameAlternateName':
                        aliases.append((element.findtext('DatabaseID'), element.findtext('AlternateName')))
                    root.remove(element)
                    element.clear()
                depth -= 1
    for key, name in aliases:
        if key in games and name:
            games[key]['aliases'].append(name)
    if not games:
        raise ValueError('No 3DO games in LaunchBox database; source schema may have changed')
    return list(games.values())


def enrich(games, metadata):
    lookup = {}
    for row in metadata:
        for name in [row['Name']] + row['aliases']:
            key = title_key(name)
            if row not in lookup.setdefault(key, []):
                lookup[key].append(row)
    for game in games:
        matches = lookup.get(title_key(game['name']), [])
        game['metadata'] = {}
        if len(matches) == 1:
            match = matches[0]
            date = match['ReleaseYear'] or match['ReleaseDate']
            year = re.match(r'^\d{4}', date)
            game['metadata'] = dict(title=match['Name'], genre=match['Genres'] or None,
                                    publisher=match['Publisher'] or None, developer=match['Developer'] or None,
                                    year=int(year[0]) if year else None, release_date=match['ReleaseDate'] or None,
                                    launchbox_id=match['DatabaseID'], source=LAUNCHBOX)
    return games


def load_catalog(cache, offline=False, refresh=False, dat=None, launchbox=None):
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    output = cache / 'catalog.json'
    if not refresh and dat is None and launchbox is None and output.is_file():
        result = json.loads(output.read_text(encoding='utf-8'))
        if result.get('schema') == 1:
            return result
    local_dat, local_launchbox = dat is not None, launchbox is not None
    dat = Path(dat) if local_dat else cache / 'redump.dat'
    launchbox = Path(launchbox) if local_launchbox else cache / 'launchbox.zip'
    if refresh:
        (cache / 'cover-list.html').unlink(missing_ok=True)
        # Explicit local source files are never removed by refresh.
        for file, local in ((dat, local_dat), (launchbox, local_launchbox)):
            if not local:
                file.unlink(missing_ok=True)
    if not dat.exists():
        download(REDUMP, dat, 8 * 1024 * 1024, offline)
    if not launchbox.exists():
        print('First metadata download is about 104 MiB; later scans use the local cache.', flush=True)
        download(LAUNCHBOX, launchbox, 256 * 1024 * 1024, offline)
    result = dict(schema=1, fetched=time.time(), sources=[
        dict(url=REDUMP, sha256=hashlib.sha256(dat.read_bytes()).hexdigest()),
        dict(url=LAUNCHBOX, sha256=hashlib.sha256(launchbox.read_bytes()).hexdigest()),
        dict(url=ART)], games=enrich(dat_games(dat.read_text(encoding='utf-8-sig')), launchbox_games(launchbox)))
    temporary = output.with_suffix('.tmp')
    temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    temporary.replace(output)
    return result


def cover_names(cache, offline=False):
    page = download(ART, Path(cache) / 'cover-list.html', 4 * 1024 * 1024, offline)
    names = []
    for href in re.findall(r'href="([^"]+)"', page.read_text(encoding='utf-8')):
        name = urllib.parse.unquote(href)
        if name.lower().endswith('.png') and '/' not in name and '\\' not in name:
            names.append(name)
    return names


def cover_url(name):
    return ART + urllib.parse.quote(name, safe='')
