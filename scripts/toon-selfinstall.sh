#!/bin/sh
# freetoon — on-device installer wrapper.
# Delegates directly to the main install.sh script in repository root.

HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$HERE/../install.sh" ]; then
    exec sh "$HERE/../install.sh" "$@"
fi

# Fallback for curl ... | sh execution: download install.sh and run from ToonzDev
ARGS="${*:-"--update"}"
curl -fsSL -4 https://raw.githubusercontent.com/ToonzDev/freetoon-lvgl/main/install.sh | env FORCE_DOWNLOAD=1 sh -s -- $ARGS
