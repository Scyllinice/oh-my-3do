#!/usr/bin/env bash
# Build the emulator test rig in build/emu: the Opera libretro core (upstream
# commit 684926f plus opera-fcare.patch, which makes its emulated FCare ODE
# behave like the real firmware), the headless runner, and the BIOS link.
# BIOS defaults to /mnt/d/3DO/panafz1.bin (a Panasonic FZ-1 ROM the user
# supplies; never commit it).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${EMU_DIR:-$root/build/emu}
bios=${BIOS:-/mnt/d/3DO/panafz1.bin}
mkdir -p "$work/system"
if [[ ! -d $work/opera-libretro ]]; then
    git clone https://github.com/libretro/opera-libretro.git "$work/opera-libretro"
    git -C "$work/opera-libretro" checkout --detach 684926f
    git -C "$work/opera-libretro" apply "$here/opera-fcare.patch"
fi
make -C "$work/opera-libretro" -j"$(nproc)" >"$work/core-build.log"
cc -O2 -Wall -Wextra -I"$work/opera-libretro/libretro-common/include" "$here/runner.c" -ldl -o "$work/runner"
[[ -e $work/system/panafz1.bin ]] || ln -s "$bios" "$work/system/panafz1.bin"
echo "Emulator ready in $work"
