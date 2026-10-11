#!/bin/bash
# Inject a synthetic touch on the Toon screen via /dev/input/event1.
# Usage: ./toontap.sh <x> <y>
# Coordinates are in framebuffer pixels (0..1023 × 0..599).
# Calls the cross-compiled /mnt/data/toontap binary on the device
# (Toon lacks python/perl-with-q-pack, so a native binary is the
# reliable path).
X="$1"
Y="$2"
if [[ -z "$X" || -z "$Y" ]]; then echo "usage: $0 X Y"; exit 1; fi

HOST="${TOON_HOST:-toon}"
sshpass -p "${TOON_PASS:-toon}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR "root@$HOST" "/mnt/data/toontap $X $Y"
