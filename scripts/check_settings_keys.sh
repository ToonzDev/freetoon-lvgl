#!/bin/sh
# Every key settings_load() reads must also be written by settings_save(),
# and the other way round. A key that is read but never written survives
# only until the next save: 1.0.14 shipped dyn_tariff and both dynamic
# surcharges that way, so the dynamic tariff reset on every restart.
#
#     scripts/check_settings_keys.sh [path/to/settings.c]
#
# Static: it reads the source (strcmp/strncmp keys in settings_load,
# fprintf keys in settings_write_file), so it needs no build.
set -eu

SRC="${1:-$(dirname "$0")/../lvgl_ui_recovered/src/settings.c}"

# Read-only on purpose: old names that settings_load still accepts and maps
# onto a key that IS saved. Add a key here only with that reason.
LEGACY="
camera_enabled
camera_overlay
camera_rtp
camera_size_pct
camera_src_h
camera_src_w
camera_x
camera_y
dyn_tariff
"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

body() {   # print the function whose definition line starts with $1
    awk -v start="$1" 'index($0, start) == 1 {p=1} p {print} p && /^}/ {exit}' "$SRC"
}

body 'void settings_load(void) {' > "$tmp/load.c"
body 'static void settings_write_file(void) {' > "$tmp/save.c"
[ -s "$tmp/load.c" ] && [ -s "$tmp/save.c" ] || {
    echo "check_settings_keys: settings_load / settings_write_file not found in $SRC" >&2; exit 2; }

{
    grep -oE 'strcmp\(k, *"[A-Za-z0-9_]+"' "$tmp/load.c" | sed -E 's/.*"([^"]+)"/\1/'
    # prefix keys (strncmp(k, "tile_slot_p1_", 13)) are the indexed ones
    grep -oE 'strncmp\(k, *"[A-Za-z0-9_]+_"' "$tmp/load.c" | sed -E 's/.*"([^"]+)"/\1%d/'
} | sort -u > "$tmp/load"
grep -oE 'fprintf\(f, *"[A-Za-z0-9_%]+=' "$tmp/save.c" | sed -E 's/fprintf\(f, *"//; s/=$//' \
    | sort -u > "$tmp/save"
printf '%s\n' $LEGACY | sort -u > "$tmp/legacy"

fail=0
missing=$(comm -23 "$tmp/load" "$tmp/save" | comm -23 - "$tmp/legacy")
if [ -n "$missing" ]; then
    echo "FAIL: read by settings_load() but never written by settings_save():"
    echo "$missing" | sed 's/^/    /'
    echo "  (lost on the next save - add an fprintf in settings_write_file,"
    echo "   or list it in LEGACY in $0 if it is an old alias of a saved key)"
    fail=1
fi
unread=$(comm -13 "$tmp/load" "$tmp/save")
if [ -n "$unread" ]; then
    echo "FAIL: written by settings_save() but never read by settings_load():"
    echo "$unread" | sed 's/^/    /'
    fail=1
fi
stale=$(comm -23 "$tmp/legacy" "$tmp/load")
if [ -n "$stale" ]; then
    echo "FAIL: LEGACY lists keys settings_load() no longer reads (remove them):"
    echo "$stale" | sed 's/^/    /'
    fail=1
fi
[ "$fail" = 0 ] && echo "OK: $(wc -l < "$tmp/save" | tr -d ' ') settings keys saved and loaded ($(wc -l < "$tmp/legacy" | tr -d ' ') legacy aliases read-only)"
exit "$fail"
