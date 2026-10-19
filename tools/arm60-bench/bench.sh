#!/usr/bin/env bash
# In the development container: build the bench image and run arm60.py.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cflags="-I$TDO_DEVKIT_PATH/include/3do -I$root/src -DOH3DO_SDK=1 -DNDEBUG=1 -O2 -bigend -za1 -zi4 -fpu none -arch 3 -apcs 3/32/nofp/swst/wide/softfp"
armasm -bigend -fpu none -arch 3 -apcs 3/32/nofp/swst "$root/src/preview_v5_arm.s" -o "$out/decoder.o"
armcc $cflags -c "$here/bench.c" -o "$out/bench.o" 2>&1 | grep -v "wide is less\|^$" || true
armcc $cflags -c "$here/stubs.c" -o "$out/stubs.o" 2>&1 | grep -v "wide is less\|^$" || true
armlink -bin -ro-base 0x10000 -nodebug -remove -entry 0x10000 -symbols "$out/bench.sym" -o "$out/bench.bin" \
    "$out/bench.o" "$out/decoder.o" "$out/stubs.o" "$TDO_DEVKIT_PATH/lib/3do/clib.lib" >/dev/null
cc -O1 -shared -fPIC -I"$root/src" "$root/src/preview_v5.c" -o "$out/preview_v5.so"
python3 "$here/arm60.py" "$out/bench.bin" "$out/bench.sym" "$out/preview_v5.so" "$@"
