#!/usr/bin/env python3
"""Index games for Find and associate PNG covers/prepared previews on USB."""
import argparse
import binascii
from pathlib import Path
import struct


def fold(value):
    return value.translate(str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"))


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


def index(root):
    if not root.is_dir():
        raise ValueError("USB root must be an existing directory")
    games = []
    for path in root.rglob("*"):
        if path.is_symlink() or not path.is_file() or path.suffix.lower() not in (".iso", ".cue"):
            continue
        relative = path.relative_to(root)
        if len(relative.parts) == 1 and path.name.lower() in ("boot.iso", "menu-demo.iso"):
            continue
        if relative.parts[0].lower() in ("covers", "previews", "saves", "music"):
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
    library=root/'library.3di'
    if library.is_symlink() or (library.exists() and not library.is_file()):
        raise ValueError('Search index must be a regular file')
    oversized = []
    outputs = []
    for folder, suffix in (("covers", ".png"), ("previews", ".3dv")):
        directory = root/folder
        if not directory.exists():
            continue
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError(f"Media folder must be a real directory: {directory}")
        previous=existing_index(directory)
        assets = {}
        for path in directory.iterdir():
            if path.is_symlink() or not path.is_file() or path.suffix.lower() != suffix:
                continue
            name = path.name
            if len(name.encode()) > 96 or name.startswith(".") or any(ord(c) < 32 or ord(c) == 127 or c in "\\:|" for c in name):
                raise ValueError(f"Unsupported media basename: {name}")
            if suffix == '.png':
                with path.open('rb') as stream:
                    header = stream.read(24)
                if len(header) == 24 and header[:8] == b'\x89PNG\r\n\x1a\n' and header[12:16] == b'IHDR':
                    w, h = struct.unpack_from('>II', header, 16)
                    if (w,h) != (84,105):
                        oversized.append((name,w,h))
            key = fold(path.stem)
            if key in assets:
                raise ValueError(f"Media filenames collide: {name}")
            if suffix == ".png" and path.stat().st_size > 512*1024:
                raise ValueError(f"PNG exceeds 512 KiB: {name}")
            assets[key] = name
        rows = []
        for folded, game, stem in sorted(games):
            name=assets.get(stem,previous.get(folded))
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
        print(f'{len(oversized)} PNG cover(s) differ from the native 84x105 cover size:')
        for name,w,h in oversized[:5]:
            print(f'  {name}: {w}x{h}')
        print('Use resize-covers.py with a separate output folder for 84x105 PNGs; originals were not changed.')
    # Validate every index before replacing any existing output.
    temporary=root/'library.3di.tmp'
    if temporary.is_symlink(): raise ValueError('Temporary search index must not be a symlink')
    temporary.write_bytes(library_data); temporary.replace(library)
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
