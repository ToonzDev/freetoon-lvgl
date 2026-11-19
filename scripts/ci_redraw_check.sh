#!/bin/sh
# Idle redraw budget: an idle screen must not redraw (most of) the whole
# screen on every refresh tick. That cost Toon 1 ~30% CPU on the home screen
# until 1df1144; a new tile or dim block that sets labels/styles without the
# ui_lazy.h setters brings it back, and only a Toon 1 shows it.
#
#     scripts/ci_redraw_check.sh <sim-binary> [seconds]
#
# Runs `<sim> redrawstat <s> <screen>` and fails when the "with refresh"
# rate exceeds the budget below (in full screens per second).
set -eu

SIM="$1"
SECS="${2:-5}"
# Absolute: the sim runs from a scratch dir (it writes its cfg/cache there).
case "$SIM" in /*) ;; *) SIM="$(pwd)/$SIM" ;; esac
[ -x "$SIM" ] || { echo "ci_redraw_check: no executable sim at $SIM" >&2; exit 2; }

# On GitHub Actions also emit an annotation, so the result is visible on the
# run summary (and through the API) without opening the step log.
note() {   # note <error|notice> <text>
    echo "$2"
    [ -n "${GITHUB_ACTIONS:-}" ] && echo "::$1 title=Idle redraw ($(basename "$SIM"))::$2"
    return 0
}

# screen  max-screens/s   (measured 2026-11-19: home 0.00, dim 0.16-0.20,
#                          dimgrid 0.13-0.17; a regression is ~2 screens/s)
BUDGET="
home       0.05
dim        0.50
dimgrid    0.50
thermostat 0.05
"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
: > "$work/failed"
# Every screen is measured, also after a failure, so one run shows them all.
echo "$BUDGET" | while read -r scr max; do
    [ -n "$scr" ] || continue
    out=$(cd "$work" && "$SIM" redrawstat "$SECS" "$scr" 2>&1) || true
    line=$(echo "$out" | grep 'redrawstat\] .* with refresh' | tail -1)
    rate=$(echo "$line" | sed -n 's/.*(\([0-9.]*\) screens\/s).*/\1/p')
    if [ -z "$rate" ]; then
        note error "FAIL $scr: no redrawstat result: $(echo "$out" | tail -3 | tr '\n' ' ')"
        echo "$scr" >> "$work/failed"
    elif awk -v r="$rate" -v m="$max" 'BEGIN { exit !(r > m) }'; then
        note error "FAIL $scr: $rate screens/s idle (budget $max)"
        echo "$scr" >> "$work/failed"
    else
        echo "ok   $scr: $rate screens/s idle (budget $max)"
    fi
done
[ ! -s "$work/failed" ] || exit 1
note notice "All idle screens within budget"
