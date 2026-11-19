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

# screen  max-screens/s   (measured 2026-11-19: home 0.00, dim 0.16-0.20,
#                          dimgrid 0.13-0.17; a regression is ~2 screens/s)
BUDGET="
home       0.05
dim        0.50
dimgrid    0.50
thermostat 0.05
"

fail=0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
echo "$BUDGET" | while read -r scr max; do
    [ -n "$scr" ] || continue
    line=$(cd "$work" && "$SIM" redrawstat "$SECS" "$scr" 2>&1 | grep 'redrawstat\] .* with refresh' | tail -1)
    rate=$(echo "$line" | sed -n 's/.*(\([0-9.]*\) screens\/s).*/\1/p')
    if [ -z "$rate" ]; then
        echo "FAIL $scr: no redrawstat result"; echo "$line"; exit 1
    fi
    if awk -v r="$rate" -v m="$max" 'BEGIN { exit !(r > m) }'; then
        echo "FAIL $scr: $rate screens/s idle (budget $max)"; exit 1
    fi
    echo "ok   $scr: $rate screens/s idle (budget $max)"
done || fail=1
exit "$fail"
