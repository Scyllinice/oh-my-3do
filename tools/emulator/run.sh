#!/usr/bin/env bash
# Run a script against the emulated console:
#   run.sh SCRIPT NAME     screenshots go to build/emu/shots/NAME/*.ppm
# The menu remembers its last position on the drive, so this clears it (and
# recent games, the save snapshot and old diagnostics) to make scripts start
# from the top level every time. Diagnostics a script exports land in
# build/emu/usb/.ohmy3do/diagnostics.txt.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${EMU_DIR:-$root/build/emu}
script=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
out=$work/shots/$2
rm -f "$work/usb/.ohmy3do/"{last-position.txt,recent.txt,save-snapshot.txt,diagnostics.txt}
rm -rf "$out" && mkdir -p "$out"
cd "$work"
./runner opera-libretro/opera_libretro.so system usb/boot.iso "$script" "$out" >"$out/log.txt" 2>&1
echo "$out"
