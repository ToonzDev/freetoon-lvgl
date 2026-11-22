/*
 * Chooser for the Verlichting home tile: which light each of its two
 * buttons switches. Lists every switchable light FreeToon knows, by source
 * (Domoticz, Home Assistant, Homey, Z-Wave), each with a "1" and a "2"
 * button; the two current choices are shown at the top with a clear button.
 *
 * Opened with a long press on the Verlichting tile. Built per open
 * (ui_push_owned), freed on close. The choice lives in
 * settings.tile_light1/2 (tile_lights.h), the same slots the Domoticz T1/T2
 * buttons, the HA Home flag and the Homey home button write.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "ui_lazy.h"
#include "tile_lights.h"
#include "domoticz.h"
#include "homeassistant.h"
#include "homey.h"
#include "screen_zwave.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define COL_BG        THEME_BG
#define COL_CARD      THEME_CARD_BG
#define COL_TEXT_HI   THEME_TEXT_HI
#define COL_TEXT_DIM  THEME_TEXT_DIM
#define COL_OFF       THEME_OFF
#define COL_PICK      0xddaa44      /* Verlichting tile accent */

#define TLS_MAX (DOMOTICZ_MAX_DEV + HA_DEVICE_MAX + HY_MAX_DEV + 40)

typedef struct {
    char       ref[TL_REF_LEN + 4];
    lv_obj_t * b[2];
} trow_t;

static lv_obj_t   * scr_root = NULL;
static lv_obj_t   * list = NULL;
static lv_obj_t   * empty_hint = NULL;
static lv_obj_t   * slot_lbl[2];
static lv_obj_t   * slot_clr[2];
static lv_timer_t * refresh_timer = NULL;
static trow_t       rows[TLS_MAX];
static int          nrows = 0;
static unsigned long built_sig = 0;

static int dz_on(void) { return settings.enable_domoticz || (settings.client_mode && domoticz_state.count > 0); }

static unsigned long sig(void) {
    unsigned long h = 5381u;
    h = h * 33u + (unsigned long)(dz_on() ? domoticz_state.count : -1);
    h = h * 33u + (unsigned long)(settings.enable_ha ? ha_device_count : -1);
    h = h * 33u + (unsigned long)(settings.enable_homey ? homey_state.ndev : -1);
    h = h * 33u + (unsigned long)zwave_switch_count_cached();
    return h;
}

/* ---- marks: which row is on which button, and the two slot labels ---- */
static void refresh_marks(void) {
    char cur[2][TL_REF_LEN + 4];
    for (int k = 0; k < 2; k++) {
        tl_src_t s = tl_resolve(k);
        tl_ref(&s, cur[k], sizeof cur[k]);
        if (!slot_lbl[k]) continue;
        char name[80];
        if (s.kind == TL_NONE) snprintf(name, sizeof name, "%s", tr("(leeg)", "(empty)"));
        else tl_status(&s, name, sizeof name, NULL);
        lv_label_set_text_fmt(slot_lbl[k], "%d   %s", k + 1, name);
        if (slot_clr[k]) {
            if (s.kind == TL_NONE) lv_obj_add_flag(slot_clr[k], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_clear_flag(slot_clr[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < nrows; i++)
        for (int k = 0; k < 2; k++) {
            int on = !strcmp(rows[i].ref, cur[k]);
            lv_obj_set_style_bg_color(rows[i].b[k], lv_color_hex(on ? COL_PICK : COL_OFF), 0);
            lv_obj_t * l = lv_obj_get_child(rows[i].b[k], 0);
            if (l) lv_obj_set_style_text_color(l, lv_color_hex(on ? 0x202020 : COL_TEXT_HI), 0);
        }
}

/* ---- actions ---- */
static void on_pick(lv_event_t * e) {
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int i = (int)(v >> 1), k = (int)(v & 1);
    if (i < 0 || i >= nrows) return;
    tl_set(k, tl_slot_of(rows[i].ref) == k ? "" : rows[i].ref);
    refresh_marks();
}
static void on_clear(lv_event_t * e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    tl_set(k, "");
    refresh_marks();
}
static void back_async(void * unused) { (void)unused; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

/* ---- widgets ---- */
static lv_obj_t * mk_btn(lv_obj_t * parent, const char * txt, int w, int h, lv_event_cb_t cb, intptr_t ud) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_font(l, SF(22), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_HI), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static void add_header(const char * txt) {
    lv_obj_t * h = lv_label_create(list);
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_style_text_font(h, SF(18), 0);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_pad_top(h, 6, 0);
    lv_label_set_text(h, txt);
}

static void add_row(const char * ref, const char * name, int available) {
    if (nrows >= TLS_MAX) return;
    int is_t1 = DISP_VER < 600;
    trow_t * r = &rows[nrows];
    snprintf(r->ref, sizeof r->ref, "%s", ref);

    lv_obj_t * c = lv_obj_create(list);
    lv_obj_set_size(c, lv_pct(100), is_t1 ? 50 : SY(56));
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(c, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 4, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    int bw = is_t1 ? 56 : SX(66), bh = is_t1 ? 40 : SY(44);
    lv_obj_t * nm = lv_label_create(c);
    lv_obj_set_style_text_font(nm, SF(20), 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(available ? COL_TEXT_HI : COL_TEXT_DIM), 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, DISP_HOR - 24 - 2 * bw - 60);
    if (available) lv_label_set_text(nm, name);
    else lv_label_set_text_fmt(nm, "%s  (%s)", name, tr("offline", "offline"));
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 8, 0);

    for (int k = 0; k < 2; k++) {
        r->b[k] = mk_btn(c, k ? "2" : "1", bw, bh, on_pick, ((intptr_t)nrows << 1) | k);
        lv_obj_align(r->b[k], LV_ALIGN_RIGHT_MID, k ? -4 : -(bw + 14), 0);
    }
    nrows++;
}

static void rebuild(void) {
    if (!list) return;
    lv_obj_clean(list);
    nrows = 0;
    built_sig = sig();
    char ref[TL_REF_LEN + 4];

    if (dz_on()) {
        int any = 0;
        for (int i = 0; i < domoticz_state.count && i < DOMOTICZ_MAX_DEV; i++) {
            const domoticz_dev_t * d = &domoticz_state.dev[i];
            if (d->kind == DZ_BLIND) continue;
            if (!any++) add_header("Domoticz");
            snprintf(ref, sizeof ref, "dz:%d", d->idx);
            add_row(ref, d->name, 1);
        }
    }
    if (settings.enable_ha) {
        int any = 0;
        for (int i = 0; i < ha_device_count && i < HA_DEVICE_MAX; i++) {
            const ha_device_t * d = &ha_devices[i];
            if (d->type != HADEV_LIGHT && d->type != HADEV_SWITCH) continue;
            if (!any++) add_header("Home Assistant");
            snprintf(ref, sizeof ref, "ha:%s", d->entity_id);
            add_row(ref, d->name, d->available);
        }
    }
    if (settings.enable_homey) {
        int any = 0;
        homey_lock();
        for (int i = 0; i < homey_state.ndev; i++) {
            const hy_dev_t * d = &homey_state.dev[i];
            if (!homey_switchable(d)) continue;
            if (!any++) add_header("Homey");
            snprintf(ref, sizeof ref, "hy:%s", d->id);
            add_row(ref, d->name, d->available);
        }
        homey_unlock();
    }
    if (zwave_switch_count_cached() > 0) {
        zwave_dev_t d;
        for (int i = 0; zwave_get_switch(i, &d); i++) {
            if (!i) add_header("Z-Wave");
            snprintf(ref, sizeof ref, "zw:%s", d.uuid);
            add_row(ref, d.name[0] ? d.name : d.type, 1);
        }
    }

    if (empty_hint) {
        if (nrows) lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_marks();
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root || lv_scr_act() != scr_root) return;
    if (sig() != built_sig) rebuild();
    else refresh_marks();
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    } else if (c == LV_EVENT_DELETE) {
        if (refresh_timer) { lv_timer_del(refresh_timer); refresh_timer = NULL; }
        scr_root = list = empty_hint = NULL;
        slot_lbl[0] = slot_lbl[1] = slot_clr[0] = slot_clr[1] = NULL;
        nrows = 0;
    }
}

lv_obj_t * screen_tile_lights_create(void) {
    int is_t1 = DISP_VER < 600;
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED,   NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_DELETE,          NULL);
    nrows = 0;

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Verlichting-tegel", "Lighting tile"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 16);

    lv_obj_t * sub = lv_label_create(scr_root);
    lv_obj_set_style_text_color(sub, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(sub, SF(16), 0);
    lv_label_set_text(sub, tr("Kies per lamp knop 1 of 2 van de tegel.", "Pick button 1 or 2 of the tile per light."));
    lv_obj_align_to(sub, title, LV_ALIGN_OUT_RIGHT_BOTTOM, 16, -4);

    int x_w = is_t1 ? 60 : 70, x_h = is_t1 ? 46 : 50;
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, x_w, x_h);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, SX(-12), SY(12));
    lv_obj_set_style_bg_color(back, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_ext_click_area(back, 25);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(bl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_obj_center(bl);

    /* The two buttons as they are now, each with a clear button. */
    int top = is_t1 ? 70 : SY(76);
    int sh = is_t1 ? 48 : SY(54);
    int sw = (DISP_HOR - 24 - 12) / 2;
    for (int k = 0; k < 2; k++) {
        lv_obj_t * s = lv_obj_create(scr_root);
        lv_obj_set_size(s, sw, sh);
        lv_obj_set_pos(s, 12 + k * (sw + 12), top);
        lv_obj_set_style_bg_color(s, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_color(s, lv_color_hex(COL_PICK), 0);
        lv_obj_set_style_border_width(s, 2, 0);
        lv_obj_set_style_radius(s, 10, 0);
        lv_obj_set_style_pad_all(s, 4, 0);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        int cw = is_t1 ? 44 : SX(52);
        slot_lbl[k] = lv_label_create(s);
        lv_obj_set_style_text_font(slot_lbl[k], SF(20), 0);
        lv_obj_set_style_text_color(slot_lbl[k], lv_color_hex(COL_TEXT_HI), 0);
        lv_label_set_long_mode(slot_lbl[k], LV_LABEL_LONG_DOT);
        lv_obj_set_width(slot_lbl[k], sw - cw - 28);
        lv_label_set_text(slot_lbl[k], "");
        lv_obj_align(slot_lbl[k], LV_ALIGN_LEFT_MID, 8, 0);
        slot_clr[k] = mk_btn(s, LV_SYMBOL_CLOSE, cw, sh - 14, on_clear, k);
        lv_obj_align(slot_clr[k], LV_ALIGN_RIGHT_MID, -2, 0);
    }

    int ly = top + sh + 8;
    list = lv_obj_create(scr_root);
    lv_obj_set_size(list, DISP_HOR - 24, DISP_VER - ly - 6);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, ly);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);

    empty_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(empty_hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(empty_hint, SF(22), 0);
    lv_obj_set_width(empty_hint, DISP_HOR - 120);
    lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(empty_hint, tr("Nog geen lampen gevonden.\nStel Domoticz, Home Assistant, Homey of Z-Wave in via Instellingen.",
                                     "No lights found yet.\nSet up Domoticz, Home Assistant, Homey or Z-Wave in Settings."));
    lv_obj_align(empty_hint, LV_ALIGN_CENTER, 0, SY(40));
    lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    rebuild();
    return scr_root;
}
