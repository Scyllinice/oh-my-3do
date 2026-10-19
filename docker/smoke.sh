#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo 'Usage: devkit-smoke OUTPUT_DIRECTORY' >&2
    exit 2
fi

mkdir -p "$1"
output_dir=$(cd "$1" && pwd)
smoke_dir=$(mktemp -d /tmp/oh-my-3do-smoke.XXXXXX)
trap 'rm -rf "$smoke_dir"' EXIT

# Bootstrap into an empty directory, never over the real project's Makefile.
sh "${TDO_DEVKIT_PATH:?}/bin/buildtools/linux/bootstrap-3do-project" "$smoke_dir"
make -C "$smoke_dir" NAME=devkit-smoke

# Explicit signing covers devkit revisions whose Makefile only packs the disc.
3dt sign "$smoke_dir/iso/devkit-smoke.iso" \
    --output "$smoke_dir/iso/devkit-smoke-signed.iso"
3dt verify "$smoke_dir/iso/devkit-smoke-signed.iso"
test -s "$smoke_dir/iso/devkit-smoke-signed.iso"
cp "$smoke_dir/iso/devkit-smoke-signed.iso" "$output_dir/devkit-smoke.iso"
cp /opt/3do-devkit.revision "$output_dir/devkit.revision"
echo "Example disc: $output_dir/devkit-smoke.iso"
