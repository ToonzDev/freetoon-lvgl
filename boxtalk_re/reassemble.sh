#!/bin/sh
# Reassemble 7-byte frames from the bytestream-style capture.
# We treat the W and R streams independently as two byte streams,
# emit consecutive bytes per direction, and group every 7 bytes
# starting after a "header byte" with bit7=1 (c2/cb) into a request,
# or a 7-byte block starting with 0x42 (response).

LOG=/tmp/qt_rebuild/captures/ttymxc0_capture.log

# Flatten reads: tag every byte with a sequential number to preserve order.
grep "^R " "$LOG" | grep -v "n=1 6a " > /tmp/qt_rebuild/boxtalk_re/_reads.txt
grep "^W " "$LOG" | grep -v "n=1 6a " > /tmp/qt_rebuild/boxtalk_re/_writes.txt
wc -l /tmp/qt_rebuild/boxtalk_re/_reads.txt /tmp/qt_rebuild/boxtalk_re/_writes.txt
