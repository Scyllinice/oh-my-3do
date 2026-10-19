#!/usr/bin/env bash
set -euo pipefail

# A bind-mounted project may run under any host UID. Wine needs a private,
# writable prefix owned by that UID, rather than the image builder's prefix.
if [[ -z ${WINEPREFIX:-} ]]; then
    WINEPREFIX=$(mktemp -d /tmp/oh-my-3do-wine.XXXXXX)
    export WINEPREFIX
fi

exec "$@"
