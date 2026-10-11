#!/bin/sh
# Grab a screenshot of Toon's framebuffer over SSH and save as PNG.
# Visible area: 1024x600 @ 32bpp BGRA, stride=4096 (== 1024*4, so contiguous).
# Usage: ./toonshot.sh [output_path]
#   default output: /tmp/toonshot.png

OUT="${1:-/tmp/toonshot.png}"
HOST="${TOON_HOST:-toon}"
BYTES=$((1024 * 600 * 4))

sshpass -p "${TOON_PASS:-toon}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR \
    "root@$HOST" "dd if=/dev/fb0 bs=4096 count=600 2>/dev/null" \
  | python3 -c "
import sys
from PIL import Image
data = sys.stdin.buffer.read($BYTES)
# Toon fb is BGRA at 32bpp. Pillow's raw decoder accepts source order 'BGRA'.
img = Image.frombuffer('RGBA', (1024, 600), data, 'raw', 'BGRA', 0, 1)
img.save('$OUT')
print('wrote $OUT')
"
