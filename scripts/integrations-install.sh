#!/bin/sh
# integrations-install.sh — pulls an integration from the freetoon-integrations
# catalog, drops it in /mnt/data/integrations/<id>/, wires an inittab respawn
# row, HUPs init. Called by toonui's Marketplace screen via system().
#
# Usage:  integrations-install.sh <integration-id>
#
# Identical contract to scripts/install.sh in the freetoon-integrations
# repo — we ship a copy here so toonui has a local trigger that doesn't
# depend on a separate curl-fetch.

set -eu

CATALOG_URL=https://raw.githubusercontent.com/ToonSoftwareCollective/freetoon-integrations/main/catalog/index.json
INTEGRATIONS_DIR=/mnt/data/integrations
LOG=/var/volatile/tmp/integration_install.log

log()  { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
die()  { log "ERROR: $*"; exit 1; }

ID="${1:-}"
[ -n "$ID" ] || die "usage: $0 <integration-id>"

log "fetching catalog..."
CATALOG=$(curl -fsSL --max-time 20 "$CATALOG_URL") || die "catalog fetch failed"

# Brittle but jq-free: pull the URL field of the matching id from the
# catalog text. The catalog format keeps each integration's fields on
# their own lines, so we collapse newlines first and then regex-pluck.
URL=$(printf '%s' "$CATALOG" | tr '\n' ' ' \
        | sed -n "s/.*\"id\":[ ]*\"$ID\"[^}]*\"url\":[ ]*\"\\([^\"]*\\)\".*/\\1/p")
[ -n "$URL" ] || die "integration '$ID' not in catalog"

log "installing '$ID' from $URL"
mkdir -p "$INTEGRATIONS_DIR/$ID"
cd "$INTEGRATIONS_DIR/$ID"
curl -fsSL --max-time 60 "$URL" -o /tmp/integration.tgz || die "tarball fetch failed"
tar xzf /tmp/integration.tgz || die "tarball extract failed"
rm -f /tmp/integration.tgz

[ -f manifest.json ] || die "no manifest.json in tarball"
chmod +x "$INTEGRATIONS_DIR/$ID"/* 2>/dev/null || true

BIN=$(sed -n 's/.*"binary":[ ]*"\([^"]*\)".*/\1/p' manifest.json)
[ -n "$BIN" ] || die "manifest missing binary field"
[ -x "$INTEGRATIONS_DIR/$ID/$BIN" ] || die "binary '$BIN' missing or not executable"

# 4-char inittab id from the integration id; safe to collide with existing
# rows because upsert keys on this id and replaces.
ROW_ID=$(echo "$ID" | tr -dc 'a-zA-Z0-9' | cut -c1-4)
LOG_PATH="/var/volatile/tmp/integration-$ID.log"
ROW="$ROW_ID:345:respawn:$INTEGRATIONS_DIR/$ID/$BIN >> $LOG_PATH 2>&1"

log "wiring inittab row '$ROW_ID' -> $BIN"
grep -v "^${ROW_ID}:" /etc/inittab > /etc/inittab.new
echo "$ROW" >> /etc/inittab.new
mv -f /etc/inittab.new /etc/inittab

pkill -9 -f "$INTEGRATIONS_DIR/$ID/$BIN" 2>/dev/null || true
kill -HUP 1
log "installed - integration logs at $LOG_PATH"
log "toonui will pick up the new BoxTalk service on its next subscribe cycle."
