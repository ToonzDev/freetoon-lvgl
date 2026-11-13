#!/bin/sh
# Generate the Latin-1 supplement fonts lv_font_ft_<size>.c.
#
# LVGL's built-in Montserrat fonts (lv_font_montserrat_*) only carry ASCII,
# the degree sign, the bullet and the LV_SYMBOL_* icons. Accented letters
# (é è à ç ë ...) and typographic punctuation (— … ’ →) are therefore not
# drawn, which breaks the French UI and some Dutch strings ("Geïnstalleerd").
#
# Each lv_font_ft_<size> holds ONLY those extra glyphs and falls back to the
# matching built-in lv_font_montserrat_<size> for everything else, so the
# vendored LVGL fonts stay untouched. line_height / base_line are copied from
# the built-in font so existing layouts do not move.
#
# Requires lv_font_conv (npm i lv_font_conv@1.5.2). Run from anywhere:
#   LV_FONT_CONV=/path/to/lv_font_conv tools/gen_latin_fonts.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../src"
FC=${LV_FONT_CONV:-lv_font_conv}
cd "$SRC"
TTF=lvgl/scripts/built_in_font/Montserrat-Medium.ttf
DEJAVU=lvgl/scripts/built_in_font/DejaVuSans.ttf
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
RANGE="0xA0-0xFF,0x152-0x153,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2026,0x2190,0x2192"

for sz in 12 14 18 20 22 24 28 36 48; do
    out="lv_font_ft_$sz.c"
    builtin="lvgl/src/font/lv_font_montserrat_$sz.c"
    lh=$(sed -n 's/^ *\.line_height = \([0-9]*\),.*/\1/p' "$builtin")
    bl=$(sed -n 's/^ *\.base_line = \([0-9]*\),.*/\1/p' "$builtin")
    "$FC" --no-compress --no-prefilter --bpp 4 --size "$sz" \
        --font "$TTF" -r "$RANGE" --font "$DEJAVU" -r 0x2500 \
        --format lvgl --force-fast-kern-format --lv-include lvgl.h \
        -o "$TMP/$out" >/dev/null
    sed -e "s/\.line_height = [0-9]*,/.line_height = $lh,/" \
        -e "s/\.base_line = [0-9]*,/.base_line = $bl,/" \
        -e "s/^\( *\)\.base_line = \(.*\)$/\1.base_line = \2\n#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9\n\1.fallback = \&lv_font_montserrat_$sz,   \/*ASCII + LV_SYMBOL_* from the built-in font*\/\n#endif/" \
        -e "s| -o $TMP/| -o |" \
        "$TMP/$out" > "$out"
    echo "lv_font_ft_$sz.c  line_height=$lh base_line=$bl"
done
