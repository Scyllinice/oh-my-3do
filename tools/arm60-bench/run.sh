#!/usr/bin/env bash
# Count ARM60 clocks for decoding a preview clip with the console's decoder.
#   tools/arm60-bench/run.sh CLIP.3dv [FRAMES] [--profile]
# Runs inside the development container (armasm/armcc/armlink and Unicorn).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
clip=$(realpath --relative-to="$root" "$1"); shift
exec "$root/tools/dev-container" run bash /workspace/tools/arm60-bench/bench.sh "/workspace/$clip" "$@"
