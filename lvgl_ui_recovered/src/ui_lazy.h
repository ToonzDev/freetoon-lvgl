/* ui_lazy.h -- compare-before-set wrappers for LVGL 8.3 setters.
 *
 * In LVGL 8.3 lv_label_set_text(), lv_obj_add_flag/clear_flag(HIDDEN),
 * lv_img_set_src(), lv_obj_align() and every lv_obj_set_style_*() invalidate
 * the object even when nothing changes. A periodic refresh that re-applies
 * the same state each tick therefore redraws its whole screen: once more
 * than LV_INV_BUF_SIZE (32) areas are dirty, LVGL falls back to a
 * full-screen redraw. On a Toon 1 the 500 ms home refresh cost ~13% CPU
 * that way. These wrappers only call the real setter when the value
 * actually differs, so an idle screen redraws nothing.
 *
 * Use them in periodic refresh code; one-off build code can keep using the
 * plain LVGL calls.
 */
#ifndef UI_LAZY_H
#define UI_LAZY_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"

static inline void ui_label_set_text(lv_obj_t * obj, const char * txt)
{
    if (txt) {
        const char * cur = lv_label_get_text(obj);
        if (cur && strcmp(cur, txt) == 0) return;
    }
    lv_label_set_text(obj, txt);
}

static inline void ui_label_set_text_fmt(lv_obj_t * obj, const char * fmt, ...)
    __attribute__((format(printf, 2, 3)));
static inline void ui_label_set_text_fmt(lv_obj_t * obj, const char * fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof buf) { ui_label_set_text(obj, buf); return; }
    char * big = malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    ui_label_set_text(obj, big);
    free(big);
}

static inline void ui_obj_add_flag(lv_obj_t * obj, lv_obj_flag_t f)
{
    if (lv_obj_has_flag(obj, f)) return;          /* all bits already set */
    lv_obj_add_flag(obj, f);
}

static inline void ui_obj_clear_flag(lv_obj_t * obj, lv_obj_flag_t f)
{
    if (!lv_obj_has_flag_any(obj, f)) return;     /* all bits already clear */
    lv_obj_clear_flag(obj, f);
}

static inline void ui_img_set_src(lv_obj_t * obj, const void * src)
{
    const void * cur = lv_img_get_src(obj);
    if (cur == src) return;
    if (cur && src) {
        lv_img_src_t t = lv_img_src_get_type(src);
        /* FILE/SYMBOL sources are copied by lv_img, so compare the strings. */
        if (t != LV_IMG_SRC_VARIABLE && t == lv_img_src_get_type(cur)
            && strcmp((const char *)cur, (const char *)src) == 0) return;
    }
    lv_img_set_src(obj, src);
}

static inline void ui_obj_align(lv_obj_t * obj, lv_align_t align, lv_coord_t x, lv_coord_t y)
{
    lv_style_value_t v;
    if (lv_obj_get_local_style_prop(obj, LV_STYLE_ALIGN, &v, 0) != LV_STYLE_RES_FOUND
        || v.num != (int32_t)align)
        lv_obj_set_style_align(obj, align, 0);
    lv_obj_set_pos(obj, x, y);                    /* already compare-before-set */
}

/* Rate-limited rotation for an endless spin animation: an lv_anim runs its
 * exec_cb every display refresh (~30 fps), and each new angle redraws the
 * rotated image -- costly on a Toon 1. Applying at most one angle per
 * `min_ms` keeps the spin visible on idle screens at a fraction of the cost.
 * `last` is the caller's per-object timestamp. */
static inline void ui_img_set_angle_every(lv_obj_t * obj, int32_t angle,
                                          uint32_t * last, uint32_t min_ms)
{
    if (lv_tick_elaps(*last) < min_ms) return;
    *last = lv_tick_get();
    lv_img_set_angle(obj, (int16_t)angle);
}

/* Local style props: skip when the same local value is already present. */
static inline bool ui__style_has(lv_obj_t * obj, lv_style_prop_t prop,
                                 lv_style_selector_t sel, lv_style_value_t * out)
{
    return lv_obj_get_local_style_prop(obj, prop, out, sel) == LV_STYLE_RES_FOUND;
}

#define UI_LAZY_STYLE_COLOR(name, PROP)                                              \
    static inline void ui_obj_set_style_##name(lv_obj_t * obj, lv_color_t c,         \
                                                lv_style_selector_t sel) {           \
        lv_style_value_t v;                                                          \
        if (ui__style_has(obj, PROP, sel, &v) && v.color.full == c.full) return;     \
        lv_obj_set_style_##name(obj, c, sel);                                        \
    }
#define UI_LAZY_STYLE_NUM(name, PROP, T)                                             \
    static inline void ui_obj_set_style_##name(lv_obj_t * obj, T n,                  \
                                                lv_style_selector_t sel) {           \
        lv_style_value_t v;                                                          \
        if (ui__style_has(obj, PROP, sel, &v) && v.num == (int32_t)n) return;        \
        lv_obj_set_style_##name(obj, n, sel);                                        \
    }
#define UI_LAZY_STYLE_PTR(name, PROP, T)                                             \
    static inline void ui_obj_set_style_##name(lv_obj_t * obj, T p,                  \
                                                lv_style_selector_t sel) {           \
        lv_style_value_t v;                                                          \
        if (ui__style_has(obj, PROP, sel, &v) && v.ptr == (const void *)p) return;   \
        lv_obj_set_style_##name(obj, p, sel);                                        \
    }

UI_LAZY_STYLE_COLOR(text_color,   LV_STYLE_TEXT_COLOR)
UI_LAZY_STYLE_COLOR(bg_color,     LV_STYLE_BG_COLOR)
UI_LAZY_STYLE_COLOR(border_color, LV_STYLE_BORDER_COLOR)
UI_LAZY_STYLE_COLOR(img_recolor,  LV_STYLE_IMG_RECOLOR)
UI_LAZY_STYLE_COLOR(line_color,   LV_STYLE_LINE_COLOR)
UI_LAZY_STYLE_COLOR(arc_color,    LV_STYLE_ARC_COLOR)
UI_LAZY_STYLE_NUM(border_width,   LV_STYLE_BORDER_WIDTH,   lv_coord_t)
UI_LAZY_STYLE_NUM(bg_opa,         LV_STYLE_BG_OPA,         lv_opa_t)
UI_LAZY_STYLE_NUM(opa,            LV_STYLE_OPA,            lv_opa_t)
UI_LAZY_STYLE_NUM(img_recolor_opa, LV_STYLE_IMG_RECOLOR_OPA, lv_opa_t)
UI_LAZY_STYLE_NUM(text_opa,       LV_STYLE_TEXT_OPA,       lv_opa_t)
UI_LAZY_STYLE_PTR(text_font,      LV_STYLE_TEXT_FONT,      const lv_font_t *)

#endif /* UI_LAZY_H */
