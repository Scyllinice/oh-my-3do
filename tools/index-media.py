#!/usr/bin/env python3
"""Index unmodified PNG covers and prepared previews already on a USB drive."""
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
        if relative.parts[0].lower() in ("covers", "previews", "saves"):
            continue
        game = relative.as_posix()
        if any(ord(c) < 32 or ord(c) == 127 or c in "\\:" for c in game) or len(game.encode()) > 8192:
            raise ValueError(f"Unsupported game path: {game}")
        games.append((fold(game), game, fold(path.stem)))
    if len({key for key, _, _ in games}) != len(games):
        raise ValueError("Game paths collide when ignoring ASCII case")
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
