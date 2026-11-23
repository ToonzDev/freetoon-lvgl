/*
 * Chooser for the Verlichting home tile: which light each of its places
 * shows (up to TL_SLOTS). The places are chips at the top: pick one, then
 * tap a light in the list (Domoticz, Home Assistant, Homey, Hue lamps and
 * rooms, Z-Wave) to put it there; tapping the light that is already there
 * clears the place. A light moves when it was on another place. Places the
 * tile is too small for are greyed: make the tile bigger in the layout
 * editor to show them.
 *
 * Opened with a long press on the Verlichting tile. Built per open
 * (ui_push_owned), freed on close. The choice lives in
 * settings.tile_light1..8 (tile_lights.h), the same slots the Domoticz T1/T2
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
#include "hue.h"
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

#define TLS_MAX (DOMOTICZ_MAX_DEV + HA_DEVICE_MAX + HY_MAX_DEV + HUE_MAX_LIGHTS + HUE_MAX_GROUPS + 40)

typedef struct {
    char       ref[TL_REF_LEN + 4];
    lv_obj_t * card;
    lv_obj_t * badge;       /* place number when the light is on the tile */
} trow_t;

static lv_obj_t   * scr_root = NULL;
static lv_obj_t   * list = NULL;
static lv_obj_t   * empty_hint = NULL;
static lv_obj_t   * cap_hint = NULL;
static lv_obj_t   * chip[TL_SLOTS];
static lv_obj_t   * chip_lbl[TL_SLOTS];
static char         chip_txt[TL_SLOTS][96];   /* text set last (LONG_DOT labels) */
static lv_timer_t * refresh_timer = NULL;
static trow_t       rows[TLS_MAX];
static int          nrows = 0;
static int          sel = 0;            /* the place a tap in the list fills */
static unsigned long built_sig = 0;

static int dz_on(void) { return settings.enable_domoticz || (settings.client_mode && domoticz_state.count > 0); }
static int hue_on(void) { return settings.hue_enabled && hue_state.connected; }

static unsigned long sig(void) {
    unsigned long h = 5381u;
    h = h * 33u + (unsigned long)(dz_on() ? domoticz_state.count : -1);
    h = h * 33u + (unsigned long)(settings.enable_ha ? ha_device_count : -1);
    h = h * 33u + (unsigned long)(settings.enable_homey ? homey_state.ndev : -1);
    h = h * 33u + (unsigned long)(hue_on() ? hue_state.count * 64 + hue_state.group_count : -1);
    h = h * 33u + (unsigned long)zwave_switch_count_cached();
    return h;
}

/* First empty place the tile shows, after `from`; else `from` itself. */
static int next_free(int from) {
    int cap = tl_capacity();
    for (int k = 1; k <= cap; k++) {
        int s = (from + k) % cap;
        tl_src_t t = tl_resolve(s);
        if (t.kind == TL_NONE) return s;
    }
    return from;
}

/* ---- marks: the chips, and which row is on which place ---- */
static void refresh_marks(void) {
    char cur[TL_SLOTS][TL_REF_LEN + 4];
    int cap = tl_capacity();
    for (int k = 0; k < TL_SLOTS; k++) {
        tl_src_t s = tl_resolve(k);
        tl_ref(&s, cur[k], sizeof cur[k]);
        if (!chip[k]) continue;
        char name[80];
        if (s.kind == TL_NONE) snprintf(name, sizeof name, "%s", tr("(leeg)", "(empty)"));
        else tl_status(&s, name, sizeof name, NULL);
        char txt[96];
        snprintf(txt, sizeof txt, "%d  %s", k + 1, name);
        ui_label_set_text_cached(chip_lbl[k], txt, chip_txt[k], sizeof chip_txt[k]);
        int fits = k < cap;
        ui_obj_set_style_border_color(chip[k], lv_color_hex(k == sel ? COL_PICK : THEME_BORDER), 0);
        ui_obj_set_style_border_width(chip[k], k == sel ? 3 : 1, 0);
        ui_obj_set_style_text_color(chip_lbl[k], lv_color_hex(fits ? COL_TEXT_HI : COL_TEXT_DIM), 0);
        ui_obj_set_style_bg_opa(chip[k], fits ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if (cap_hint) {
        static char cap_txt[160];
        char h[160];
        if (sel >= cap)
            snprintf(h, sizeof h, tr("Plek %d past niet op de tegel: maak hem groter in de lay-out-editor.",
                                     "Place %d does not fit on the tile: make it bigger in the layout editor."), sel + 1);
        else
            snprintf(h, sizeof h, "%s", tr("Kies een plek, tik dan een lamp. Nog eens tikken haalt hem weg.",
                                           "Pick a place, then tap a light. Tap it again to remove it."));
        if (!lv_label_get_text(cap_hint)[0]) cap_txt[0] = 0;    /* new screen: label starts empty */
        ui_label_set_text_cached(cap_hint, h, cap_txt, sizeof cap_txt);
    }
    for (int i = 0; i < nrows; i++) {
        int at = -1;
        for (int k = 0; k < TL_SLOTS && at < 0; k++)
            if (!strcmp(rows[i].ref, cur[k])) at = k;
        ui_obj_set_style_border_color(rows[i].card, lv_color_hex(at >= 0 ? COL_PICK : THEME_BORDER), 0);
        ui_obj_set_style_border_width(rows[i].card, at >= 0 ? 2 : (settings.home_light_mode ? 1 : 0), 0);
        if (at >= 0) {
            ui_label_set_text_fmt(rows[i].badge, "%d", at + 1);
            ui_obj_clear_flag(rows[i].badge, LV_OBJ_FLAG_HIDDEN);
        } else ui_obj_add_flag(rows[i].badge, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- actions ---- */
static void on_chip(lv_event_t * e) {
    sel = (int)(intptr_t)lv_event_get_user_data(e);
    refresh_marks();
}
static void on_pick(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= nrows) return;
    int at = tl_slot_of(rows[i].ref);
    if (at == sel) {                      /* already here: clear the place */
        tl_set(sel, "");
    } else {
        tl_set(sel, rows[i].ref);         /* moves it when it was elsewhere */
        sel = next_free(sel);
    }
    refresh_marks();
}
static void on_clear_sel(lv_event_t * e) {
    (void)e;
    tl_set(sel, "");
    refresh_marks();
}
static void back_async(void * unused) { (void)unused; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

/* ---- widgets ---- */
static void add_header(const char * txt) {
    lv_obj_t * h = lv_label_create(list);
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_style_text_font(h, SF(18), 0);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_pad_top(h, 6, 0);
    lv_label_set_text(h, txt);
}

static void add_row(const char * ref, const char * name, int available, int dimmable) {
    if (nrows >= TLS_MAX) return;
    int is_t1 = DISP_VER < 600;
    trow_t * r = &rows[nrows];
    snprintf(r->ref, sizeof r->ref, "%s", ref);

    lv_obj_t * c = lv_obj_create(list);
    r->card = c;
    lv_obj_set_size(c, lv_pct(100), is_t1 ? 46 : SY(52));
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(c, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 4, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, on_pick, LV_EVENT_CLICKED, (void *)(intptr_t)nrows);

    int bw = is_t1 ? 40 : SX(46);
    lv_obj_t * nm = lv_label_create(c);
    lv_obj_set_style_text_font(nm, SF(20), 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(available ? COL_TEXT_HI : COL_TEXT_DIM), 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, DISP_HOR - 24 - bw - (is_t1 ? 150 : SX(170)));
    if (available) lv_label_set_text(nm, name);
    else lv_label_set_text_fmt(nm, "%s  (%s)", name, tr("offline", "offline"));
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 8, 0);

    if (dimmable) {
        lv_obj_t * d = lv_label_create(c);
        lv_obj_set_style_text_font(d, SF(16), 0);
        lv_obj_set_style_text_color(d, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text(d, tr("dimbaar", "dimmable"));
        lv_obj_align(d, LV_ALIGN_RIGHT_MID, -(bw + 16), 0);
    }

    r->badge = lv_label_create(c);
    lv_obj_set_size(r->badge, bw, is_t1 ? 34 : SY(38));
    lv_obj_set_style_bg_color(r->badge, lv_color_hex(COL_PICK), 0);
    lv_obj_set_style_bg_opa(r->badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r->badge, 8, 0);
    lv_obj_set_style_text_font(r->badge, SF(22), 0);
    lv_obj_set_style_text_color(r->badge, lv_color_hex(0x202020), 0);
    lv_obj_set_style_text_align(r->badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(r->badge, is_t1 ? 4 : SY(6), 0);
    lv_label_set_text(r->badge, "");
    lv_obj_align(r->badge, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_flag(r->badge, LV_OBJ_FLAG_HIDDEN);
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
            add_row(ref, d->name, 1, d->kind == DZ_DIMMER);
        }
    }
    if (settings.enable_ha) {
        int any = 0;
        for (int i = 0; i < ha_device_count && i < HA_DEVICE_MAX; i++) {
            const ha_device_t * d = &ha_devices[i];
            if (d->type != HADEV_LIGHT && d->type != HADEV_SWITCH) continue;
            if (!any++) add_header("Home Assistant");
            snprintf(ref, sizeof ref, "ha:%s", d->entity_id);
            add_row(ref, d->name, d->available, d->type == HADEV_LIGHT && d->dimmable);
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
            const hy_cap_t * dc = hy_dev_cap(d, "dim");
            add_row(ref, d->name, d->available, dc && dc->setable);
        }
        homey_unlock();
    }
    if (hue_on()) {
        if (hue_state.group_count > 0) {
            add_header(tr("Hue - kamers en zones", "Hue - rooms and zones"));
            for (int i = 0; i < hue_state.group_count && i < HUE_MAX_GROUPS; i++) {
                const hue_group_t * g = &hue_state.groups[i];
                snprintf(ref, sizeof ref, "hg:%s", g->id);
                add_row(ref, g->name, 1, 1);
            }
        }
        if (hue_state.count > 0) {
            add_header(tr("Hue - lampen", "Hue - lamps"));
            for (int i = 0; i < hue_state.count && i < HUE_MAX_LIGHTS; i++) {
                const hue_light_t * L = &hue_state.lights[i];
                snprintf(ref, sizeof ref, "hu:%s", L->id);
                add_row(ref, L->name, L->reachable, L->is_dimmable);
            }
        }
    }
    if (zwave_switch_count_cached() > 0) {
        zwave_dev_t d;
        for (int i = 0; zwave_get_switch(i, &d); i++) {
            if (!i) add_header("Z-Wave");
            snprintf(ref, sizeof ref, "zw:%s", d.uuid);
            add_row(ref, d.name[0] ? d.name : d.type, 1, 0);
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
        scr_root = list = empty_hint = cap_hint = NULL;
        for (int k = 0; k < TL_SLOTS; k++) chip[k] = chip_lbl[k] = NULL;
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
    sel = next_free(tl_capacity() - 1);   /* the first empty place, else 1 */
    {
        tl_src_t s0 = tl_resolve(sel);
        if (s0.kind != TL_NONE) sel = 0;
    }

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Verlichting-tegel", "Lighting tile"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, is_t1 ? 12 : 16);

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

    /* Clear the selected place. */
    lv_obj_t * clr = lv_btn_create(scr_root);
    lv_obj_set_size(clr, is_t1 ? 130 : SX(150), x_h);
    lv_obj_align_to(clr, back, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_set_style_bg_color(clr, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(clr, 12, 0);
    lv_obj_add_event_cb(clr, on_clear_sel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(clr);
    lv_label_set_text(cl, tr("Plek leeg", "Clear place"));
    lv_obj_set_style_text_color(cl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(cl, SF(18), 0);
    lv_obj_center(cl);

    /* The places: two rows of four chips. */
    int top = is_t1 ? 66 : SY(76);
    int gap = is_t1 ? 6 : SX(8);
    int cw = (DISP_HOR - 24 - 3 * gap) / 4;
    int chh = is_t1 ? 40 : SY(46);
    for (int k = 0; k < TL_SLOTS; k++) {
        lv_obj_t * c = lv_obj_create(scr_root);
        chip[k] = c;
        lv_obj_set_size(c, cw, chh);
        lv_obj_set_pos(c, 12 + (k % 4) * (cw + gap), top + (k / 4) * (chh + gap));
        lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_color(c, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_radius(c, 10, 0);
        lv_obj_set_style_pad_all(c, 4, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(c, 3);
        lv_obj_add_event_cb(c, on_chip, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        chip_lbl[k] = lv_label_create(c);
        chip_txt[k][0] = 0;
        lv_obj_set_style_text_font(chip_lbl[k], SF(16), 0);
        lv_obj_set_style_text_color(chip_lbl[k], lv_color_hex(COL_TEXT_HI), 0);
        lv_label_set_long_mode(chip_lbl[k], LV_LABEL_LONG_DOT);
        lv_obj_set_width(chip_lbl[k], cw - 14);
        lv_label_set_text(chip_lbl[k], "");
        lv_obj_align(chip_lbl[k], LV_ALIGN_LEFT_MID, 4, 0);
    }

    int hy = top + 2 * (chh + gap);
    cap_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(cap_hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(cap_hint, SF(16), 0);
    lv_label_set_long_mode(cap_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(cap_hint, DISP_HOR - 24);
    lv_label_set_text(cap_hint, "");
    lv_obj_set_pos(cap_hint, 14, hy);

    int ly = hy + (is_t1 ? 22 : SY(26));
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
    lv_label_set_text(empty_hint, tr("Nog geen lampen gevonden.\nStel Domoticz, Home Assistant, Homey, Hue of Z-Wave in via Instellingen.",
                                     "No lights found yet.\nSet up Domoticz, Home Assistant, Homey, Hue or Z-Wave in Settings."));
    lv_obj_align(empty_hint, LV_ALIGN_CENTER, 0, SY(60));
    lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    rebuild();
    return scr_root;
}
