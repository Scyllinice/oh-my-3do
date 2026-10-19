#!/usr/bin/env bash
# Build the emulator's USB drive (build/emu/usb): the folder the emulated FCare
# ODE serves. Games and previews link to the real library read-only; everything
# the menu may write (settings, history, saves, diagnostics) is a private copy.
#   make-usb.sh [BOOT.iso]     default: iso/boot.iso
# LIBRARY=/mnt/d/3DO is the library; CLIPS=dir adds .3dv files that replace the
# library's clips of the same name (default build/console-test/previews).
# BESIDE=1 lays media out the new way: each disc's cover and preview in a
# .ohmy3do folder beside it, with the indexes flagged accordingly.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${EMU_DIR:-$root/build/emu}
library=${LIBRARY:-/mnt/d/3DO}
iso=${1:-$root/iso/boot.iso}
clips=${CLIPS:-$root/build/console-test/previews}
usb=$work/usb
rm -rf "$usb"
mkdir -p "$usb/.ohmy3do/previews" "$usb/.ohmy3do/saves"
cp "$iso" "$usb/boot.iso"
if [[ ${BESIDE:-0} == 1 ]]; then
    # Real folders (so .ohmy3do can be created inside them), linked disc files.
    (cd "$library" && find . -mindepth 1 -path ./.ohmy3do -prune -o -type d -print) | while read -r dir; do
        mkdir -p "$usb/$dir"
    done
    (cd "$library" && find . -path ./.ohmy3do -prune -o -type f -path './*/*' -print) | while read -r file; do
        ln -s "$library/${file#./}" "$usb/$file"
    done
else
    for entry in "$library"/*/; do
        name=$(basename "$entry")
        ln -s "$library/$name" "$usb/$name"
    done
fi
for file in ohmy3do.ini library.3di metadata.3di; do
    cp "$library/.ohmy3do/$file" "$usb/.ohmy3do/$file"
done
cp -r "$library/.ohmy3do/covers" "$usb/.ohmy3do/covers"
for clip in "$library/.ohmy3do/previews"/*; do
    # Link clips read-only; copy the index, which BESIDE=1 rewrites.
    if [[ $(basename "$clip") == index.3di ]]; then cp "$clip" "$usb/.ohmy3do/previews/"
    else ln -s "$clip" "$usb/.ohmy3do/previews/$(basename "$clip")"; fi
done
# Test clips (same names as the library's) replace the linked originals.
if [[ -d $clips ]]; then
    for clip in "$clips"/*.3dv; do
        rm -f "$usb/.ohmy3do/previews/$(basename "$clip")"
        cp "$clip" "$usb/.ohmy3do/previews/"
    done
fi
if [[ ${BESIDE:-0} == 1 ]]; then
    python3 - "$usb" <<'PY'
import os, shutil, struct, sys
from pathlib import Path
usb = Path(sys.argv[1])
folded = {}
for parent, dirs, files in os.walk(usb):
    dirs[:] = [d for d in dirs if d != '.ohmy3do']
    rel = Path(parent).relative_to(usb).as_posix()
    folded[rel.casefold()] = rel
for folder in ('covers', 'previews'):
    index = usb / '.ohmy3do' / folder / 'index.3di'
    if index.is_symlink():
        raise SystemExit(f'Refusing to write through a link into the library: {index}')
    data = bytearray(index.read_bytes())
    count = struct.unpack_from('>I', data, 8)[0]
    pos, moved = 24, 0
    for _ in range(count):
        key, name = struct.unpack_from('>HH', data, pos); pos += 4
        game = data[pos:pos+key-1].decode(); file = data[pos+key:pos+key+name-1].decode(); pos += key + name
        home = folded[str(Path(game).parent.as_posix()).casefold()]
        target = usb / home / '.ohmy3do'
        target.mkdir(exist_ok=True)
        source = usb / '.ohmy3do' / folder / file
        if not (target / file).exists():
            shutil.copy(source, target / file, follow_symlinks=False); moved += 1
    struct.pack_into('>I', data, 20, 1)  # media beside each disc
    index.write_bytes(bytes(data))
    print(folder, 'placed beside', moved, 'discs')
PY
fi
echo "USB drive: $usb"
# Match the console's overscan margin; keep the file's size and CRLF endings.
sed -i 's/screen_margin=0/screen_margin=8/' "$usb/.ohmy3do/ohmy3do.ini"
