/*******************************************************************************
 * Size: 22 px
 * Bpp: 4
 * Opts: --font lvgl/scripts/built_in_font/Montserrat-Medium.ttf -r 0x20AC --size 22 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h -o lv_font_euro_22.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef LV_FONT_EURO_22
#define LV_FONT_EURO_22 1
#endif

#if LV_FONT_EURO_22

/* Only the euro sign, in Montserrat Medium (the built-in Montserrat has no
 * U+20AC). Every other glyph falls back to Montserrat 22, and the metrics
 * are Montserrat's, so a label can use this font for any text with a euro
 * sign in it. Generated with lv_font_conv 1.5.2 (-r 0x20AC --bpp 4). */

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+20AC "€" */
    0x0, 0x0, 0x0, 0x3, 0x9d, 0xff, 0xd9, 0x40,
    0x0, 0x0, 0x0, 0x1a, 0xff, 0xff, 0xff, 0xff,
    0xb1, 0x0, 0x0, 0x1c, 0xff, 0x93, 0x0, 0x27,
    0xff, 0xa0, 0x0, 0xa, 0xff, 0x40, 0x0, 0x0,
    0x1, 0xa1, 0x0, 0x4, 0xff, 0x40, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0xaf, 0xc0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x5f, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x70, 0x0, 0x1, 0x34, 0xff, 0x53, 0x33,
    0x33, 0x31, 0x0, 0x0, 0x0, 0x1f, 0xf2, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x5, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xf7, 0x0, 0x0, 0x13, 0x3b, 0xfc,
    0x33, 0x33, 0x33, 0x10, 0x0, 0x0, 0x0, 0x5f,
    0xf3, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xcf, 0xe3, 0x0, 0x0, 0x0, 0x19, 0x10, 0x0,
    0x1, 0xdf, 0xf8, 0x30, 0x2, 0x7e, 0xfa, 0x0,
    0x0, 0x1, 0xbf, 0xff, 0xff, 0xff, 0xfb, 0x10,
    0x0, 0x0, 0x0, 0x39, 0xdf, 0xfd, 0xa4, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 284, .box_w = 17, .box_h = 16, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 8364, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t lv_font_euro_22 = {
#else
lv_font_t lv_font_euro_22 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 24,          /*The maximum line height required by the font*/
    .base_line = 4,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
    .fallback = &lv_font_montserrat_22,
};



#endif /*#if LV_FONT_EURO_22*/

