/*
 * Homey devices screen (BACKLOG #28): everything the devices hub cannot do
 * in one row. Devices grouped by Homey zone, one card each:
 *   - on/off button (onoff), dim slider (dim), position slider
 *     (windowcoverings_set), open/stop/close (windowcoverings_state);
 *   - thermostat / airco setpoint with - and + (target_temperature, 0.5 C);
 *   - sensor values: temperature, humidity, power, energy, light, contact,
 *     motion, smoke, water, battery;
 *   - scene / button devices: Start.
 * Locks and garage doors are shown read-only: no one-tap unlock or open on
 * a wall panel.
 *
 * Opened from the "Homey" chip in the devices hub. Built per open
 * (ui_push_owned) and freed on close. All controls go through the
 * non-blocking homey_* setters (optimistic state, coalesced requests).
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "ui_lazy.h"
#include "homey.h"
#include "tile_lights.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>

#define COL_BG        THEME_BG
#define COL_CARD      THEME_CARD_BG
#define COL_TEXT_HI   THEME_TEXT_HI
#define COL_TEXT_DIM  THEME_TEXT_DIM
#define COL_ON        THEME_ACCENT_BLUE
#define COL_OFF       THEME_OFF
#define COL_OFFLINE   (settings.home_light_mode ? 0x94a3b8 : 0x6e6e6e)
#define COL_ALARM     0xe05050

enum { SL_NONE = 0, SL_DIM, SL_POS };

typedef struct {
    char       id[HY_ID_LEN];
    int        hint;                 /* index into homey_state.dev last seen */
    uint8_t    toggle, slider, temp, cover, run;
    lv_obj_t * lbl_state;
    lv_obj_t * btn;
    lv_obj_t * btn_lbl;
    lv_obj_t * sl;
    lv_obj_t * pin;                  /* "on the Verlichting home tile" toggle */
} hrow_t;

static lv_obj_t   * scr_root = NULL;
static lv_obj_t   * list = NULL;
static lv_obj_t   * lbl_status = NULL;
static lv_obj_t   * empty_hint = NULL;
static lv_timer_t * refresh_timer = NULL;
static hrow_t       rows[HY_MAX_DEV];
static int          nrows = 0;
static unsigned long built_sig = 0;

/* ---- device lookup (caller holds homey_lock) ---- */
static const hy_dev_t * row_dev(hrow_t * r) {
    if (r->hint >= 0 && r->hint < homey_state.ndev && !strcmp(homey_state.dev[r->hint].id, r->id))
        return &homey_state.dev[r->hint];
    for (int i = 0; i < homey_state.ndev; i++)
        if (!strcmp(homey_state.dev[i].id, r->id)) { r->hint = i; return &homey_state.dev[i]; }
    return NULL;
}
static int setable(const hy_dev_t * d, const char * cap) {
    const hy_cap_t * c = hy_dev_cap(d, cap);
    return c && c->setable;
}
static int read_only(const hy_dev_t * d) { return homey_read_only(d); }
static double capnum(const hy_dev_t * d, const char * cap, double dflt) {
    const hy_cap_t * c = hy_dev_cap(d, cap);
    return (c && (c->type == HY_V_NUM || c->type == HY_V_BOOL)) ? c->num : dflt;
}
static int capbool(const hy_dev_t * d, const char * cap) {   /* -1 unknown, 0, 1 */
    const hy_cap_t * c = hy_dev_cap(d, cap);
    return (c && (c->type == HY_V_BOOL || c->type == HY_V_NUM)) ? (c->num != 0) : -1;
}

static unsigned long sig(void) {
    unsigned long h = 5381u;
    h = h * 33u + (unsigned long)homey_state.ndev;
    h = h * 33u + (unsigned long)homey_state.nzone;
    h = h * 33u + (unsigned long)settings.enable_homey;
    return h;
}

/* ---- actions ---- */
static hrow_t * ev_row(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    return (i >= 0 && i < nrows) ? &rows[i] : NULL;
}
static void refresh_rows(void);

static void on_toggle(lv_event_t * e) {
    hrow_t * r = ev_row(e);
    if (!r) return;
    if (r->run) { homey_press(r->id); return; }
    homey_lock();
    const hy_dev_t * d = row_dev(r);
    int on = d ? capbool(d, "onoff") : -1;
    homey_unlock();
    if (on >= 0 && homey_onoff(r->id, !on) == 0) refresh_rows();
}
static void on_slider(lv_event_t * e) {
    hrow_t * r = ev_row(e);
    if (!r || !r->sl) return;
    double v = lv_slider_get_value(r->sl) / 100.0;
    int rc = r->slider == SL_DIM ? homey_dim(r->id, v) : homey_set_num(r->id, "windowcoverings_set", v);
    if (rc == 0) refresh_rows();
}
static void temp_step(lv_event_t * e, double step) {
    hrow_t * r = ev_row(e);
    if (!r) return;
    homey_lock();
    const hy_dev_t * d = row_dev(r);
    double t = d ? capnum(d, "target_temperature", -100) : -100;
    homey_unlock();
    if (t > -100 && homey_set_num(r->id, "target_temperature", t + step) == 0) refresh_rows();
}
static void on_temp_down(lv_event_t * e) { temp_step(e, -0.5); }
static void on_temp_up  (lv_event_t * e) { temp_step(e, +0.5); }
static void cover(lv_event_t * e, const char * st) {
    hrow_t * r = ev_row(e);
    if (r && homey_cover(r->id, st) == 0) refresh_rows();
}
static void on_open (lv_event_t * e) { cover(e, "up"); }
static void on_stop (lv_event_t * e) { cover(e, "idle"); }
static void on_close(lv_event_t * e) { cover(e, "down"); }

/* The home button puts a device on the Verlichting tile (tile_lights.h):
 * a free button, or else the second one (the first moves up). */
static char g_tile_ref[TL_SLOTS][TL_REF_LEN + 4];
static void tile_refs(void) {
    for (int k = 0; k < TL_SLOTS; k++) {
        tl_src_t s = tl_resolve(k);
        tl_ref(&s, g_tile_ref[k], sizeof g_tile_ref[k]);
    }
}
static int is_pinned(const char * id) {
    for (int k = 0; k < TL_SLOTS; k++)
        if (!strncmp(g_tile_ref[k], "hy:", 3) && !strcmp(g_tile_ref[k] + 3, id)) return 1;
    return 0;
}
static void on_pin(lv_event_t * e) {
    hrow_t * r = ev_row(e);
    if (!r) return;
    char ref[TL_REF_LEN + 4];
    snprintf(ref, sizeof ref, "hy:%s", r->id);
    tile_refs();
    tl_pin(ref, !is_pinned(r->id), 1);
    refresh_rows();
}

static void back_async(void * unused) { (void)unused; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

/* ---- widgets ---- */
static lv_obj_t * mk_btn(lv_obj_t * parent, const char * txt, int w, int h, uint32_t col,
                         lv_event_cb_t cb, int row) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)row);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, SF(14), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static void add_header(const char * txt) {
    lv_obj_t * h = lv_label_create(list);
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(h, SF(16), 0);
    lv_obj_set_style_pad_top(h, 6, 0);
    lv_label_set_text(h, txt);
}

/* One card for homey_state.dev[i]. Caller holds the lock. */
static void build_card(int i) {
    if (nrows >= HY_MAX_DEV) return;
    const hy_dev_t * d = &homey_state.dev[i];
    int ri = nrows++;
    hrow_t * r = &rows[ri];
    memset(r, 0, sizeof *r);
    snprintf(r->id, sizeof r->id, "%s", d->id);
    r->hint = i;
    int ro = read_only(d);
    r->cover  = !ro && setable(d, "windowcoverings_state");
    r->toggle = !ro && !r->cover && setable(d, "onoff");
    r->run    = !ro && !r->toggle && !r->cover && (setable(d, "play_scene") || setable(d, "button"));
    r->temp   = !ro && setable(d, "target_temperature");
    r->slider = ro ? SL_NONE : setable(d, "dim") ? SL_DIM : setable(d, "windowcoverings_set") ? SL_POS : SL_NONE;

    int tall = r->slider || r->temp;
    lv_obj_t * c = lv_obj_create(list);
    int h_tall = SY(92) < 86 ? 86 : SY(92);   /* Toon 1: room for the slider knob under the text */
    lv_obj_set_size(c, lv_pct(49), tall ? h_tall : SY(64));
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    int ctl_w = r->cover ? 3 * 52 + 8 : r->toggle ? 64 + 44 : r->run ? 84 : 0;
    lv_obj_t * nm = lv_label_create(c);
    lv_obj_set_width(nm, lv_pct(100));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_right(nm, ctl_w + 4, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(nm, SF(18), 0);
    lv_label_set_text(nm, d->name);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, -2);

    r->lbl_state = lv_label_create(c);
    lv_obj_set_width(r->lbl_state, lv_pct(100));
    lv_label_set_long_mode(r->lbl_state, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_right(r->lbl_state, (tall && r->temp) ? 2 * 44 + 8 : ctl_w + 4, 0);
    lv_obj_set_style_text_color(r->lbl_state, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(r->lbl_state, SF(14), 0);
    lv_label_set_text(r->lbl_state, "--");
    lv_obj_align(r->lbl_state, LV_ALIGN_TOP_LEFT, 0, SY(24));

    if (r->cover) {
        lv_obj_align(mk_btn(c, tr("Dicht", "Close"), 52, 32, COL_OFF, on_close, ri), LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_align(mk_btn(c, "Stop", 52, 32, COL_OFF, on_stop, ri), LV_ALIGN_TOP_RIGHT, -56, 0);
        lv_obj_align(mk_btn(c, "Open", 52, 32, COL_OFF, on_open, ri), LV_ALIGN_TOP_RIGHT, -112, 0);
    } else if (r->toggle) {
        r->btn = mk_btn(c, "--", 64, 34, COL_OFF, on_toggle, ri);
        r->btn_lbl = lv_obj_get_child(r->btn, 0);
        lv_obj_align(r->btn, LV_ALIGN_TOP_RIGHT, 0, 0);
        r->pin = mk_btn(c, LV_SYMBOL_HOME, 38, 34, COL_OFF, on_pin, ri);
        lv_obj_align(r->pin, LV_ALIGN_TOP_RIGHT, -70, 0);
    } else if (r->run) {
        lv_obj_t * b = mk_btn(c, tr("Start", "Run"), 84, 36, 0x2e5e8a, on_toggle, ri);
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(0xffffff), 0);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    if (r->temp) {
        lv_obj_align(mk_btn(c, "+", 40, 32, COL_OFF, on_temp_up, ri), LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_align(mk_btn(c, "-", 40, 32, COL_OFF, on_temp_down, ri), LV_ALIGN_BOTTOM_RIGHT, -46, 0);
    }
    if (r->slider) {
        r->sl = lv_slider_create(c);
        lv_obj_set_size(r->sl, r->temp ? lv_pct(60) : lv_pct(92), 14);
        lv_obj_align(r->sl, LV_ALIGN_BOTTOM_LEFT, 8, -6);
        lv_slider_set_range(r->sl, 0, 100);
        lv_obj_set_style_bg_color(r->sl, lv_color_hex(COL_OFF), LV_PART_MAIN);   /* visible track on the card */
        lv_obj_set_style_bg_opa(r->sl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(r->sl, lv_color_hex(COL_ON), LV_PART_INDICATOR);
        lv_obj_set_ext_click_area(r->sl, 12);
        lv_obj_add_event_cb(r->sl, on_slider, LV_EVENT_RELEASED, (void *)(intptr_t)ri);
    }
}

/* ---- state text ---- */
#define ADD(...) do { if (n < sizeof buf) n += (size_t)snprintf(buf + n, sizeof buf - n, __VA_ARGS__); } while (0)
#define SEP() do { if (n) ADD("  "); } while (0)

static int row_text(hrow_t * r, const hy_dev_t * d, char * out, size_t osz) {
    char buf[160]; size_t n = 0; buf[0] = 0;
    int state = 0;                                    /* 1 = on (blue), 2 = alarm (red) */
    if (!d->available) { snprintf(out, osz, "offline"); return -1; }

    int on = capbool(d, "onoff");
    if (on >= 0) {
        ADD("%s", on ? tr("aan", "on") : tr("uit", "off"));
        if (on && hy_dev_cap(d, "dim")) ADD(" %d%%", (int)(capnum(d, "dim", 0) * 100 + 0.5));
        if (on) state = 1;
    }
    const hy_cap_t * ws = hy_dev_cap(d, "windowcoverings_state");
    if (ws && ws->type == HY_V_STR) {
        SEP();
        ADD("%s", !strcmp(ws->str, "up") ? "open" : !strcmp(ws->str, "down") ? tr("dicht", "closed")
                : !strcmp(ws->str, "idle") ? tr("gestopt", "stopped") : ws->str);
    }
    if (hy_dev_cap(d, "windowcoverings_set") && !hy_dev_cap(d, "dim")) {
        SEP(); ADD("%d%%", (int)(capnum(d, "windowcoverings_set", 0) * 100 + 0.5));
    }
    int b;
    if ((b = capbool(d, "windowcoverings_closed")) >= 0) { SEP(); ADD("%s", b ? tr("dicht", "closed") : "open"); }
    if ((b = capbool(d, "locked")) >= 0) { SEP(); ADD("%s", b ? tr("op slot", "locked") : tr("niet op slot", "unlocked")); }
    if (hy_dev_cap(d, "target_temperature")) { SEP(); ADD("%s %.1f\xc2\xb0", tr("doel", "target"), capnum(d, "target_temperature", 0)); }
    if (hy_dev_cap(d, "measure_temperature")) { SEP(); ADD("%.1f\xc2\xb0" "C", capnum(d, "measure_temperature", 0)); }
    if (hy_dev_cap(d, "measure_humidity"))    { SEP(); ADD("%.0f%%", capnum(d, "measure_humidity", 0)); }
    if (hy_dev_cap(d, "measure_power"))       { SEP(); ADD("%.0f W", capnum(d, "measure_power", 0)); }
    if (hy_dev_cap(d, "meter_power"))         { SEP(); ADD("%.1f kWh", capnum(d, "meter_power", 0)); }
    if (hy_dev_cap(d, "measure_luminance"))   { SEP(); ADD("%.0f lx", capnum(d, "measure_luminance", 0)); }
    if ((b = capbool(d, "alarm_contact")) >= 0)  { SEP(); ADD("%s", b ? "open" : tr("dicht", "closed")); }
    if ((b = capbool(d, "alarm_motion")) == 1)   { SEP(); ADD("%s", tr("beweging", "motion")); state = 1; }
    if ((b = capbool(d, "alarm_presence")) == 1) { SEP(); ADD("%s", tr("aanwezig", "present")); }
    if ((b = capbool(d, "alarm_smoke")) == 1)    { SEP(); ADD("%s", tr("ROOK", "SMOKE")); state = 2; }
    if ((b = capbool(d, "alarm_water")) == 1)    { SEP(); ADD("%s", tr("LEKKAGE", "LEAK")); state = 2; }
    if (hy_dev_cap(d, "measure_battery"))        { SEP(); ADD("bat %.0f%%", capnum(d, "measure_battery", 0)); }
    if ((b = capbool(d, "alarm_battery")) == 1)  { SEP(); ADD("%s", tr("batterij leeg", "battery low")); }
    if (r->run && !n) ADD("%s", hy_dev_cap(d, "play_scene") ? "scene" : tr("knop", "button"));
    snprintf(out, osz, "%s", n ? buf : "--");
    return state;
}

static void refresh_rows(void) {
    tile_refs();
    homey_lock();
    for (int i = 0; i < nrows; i++) {
        hrow_t * r = &rows[i];
        const hy_dev_t * d = row_dev(r);
        char txt[160];
        int st = d ? row_text(r, d, txt, sizeof txt) : -1;
        if (!d) snprintf(txt, sizeof txt, "--");
        ui_label_set_text(r->lbl_state, txt);
        ui_obj_set_style_text_color(r->lbl_state, lv_color_hex(
            st == 2 ? COL_ALARM : st == 1 ? COL_ON : st == 0 ? COL_TEXT_DIM : COL_OFFLINE), 0);
        int on = d && d->available ? capbool(d, "onoff") : -1;
        if (r->btn) {
            ui_label_set_text(r->btn_lbl, on == 1 ? tr("Aan", "On") : on == 0 ? tr("Uit", "Off") : "--");
            ui_obj_set_style_bg_color(r->btn, lv_color_hex(on == 1 ? COL_ON : COL_OFF), 0);
        }
        if (r->pin)
            ui_obj_set_style_bg_color(r->pin, lv_color_hex(is_pinned(r->id) ? 0xddaa44 : COL_OFF), 0);   /* Verlichting tile accent */
        if (r->sl && d && !lv_obj_has_state(r->sl, LV_STATE_PRESSED)) {
            if (!d->available) lv_obj_add_state(r->sl, LV_STATE_DISABLED);
            else lv_obj_clear_state(r->sl, LV_STATE_DISABLED);
            double v = r->slider == SL_DIM ? (on == 0 ? 0 : capnum(d, "dim", 0))
                                           : capnum(d, "windowcoverings_set", 0);
            int iv = (int)(v * 100 + 0.5);
            if (lv_slider_get_value(r->sl) != iv) lv_slider_set_value(r->sl, iv, LV_ANIM_OFF);
        }
    }
    homey_unlock();

    if (lbl_status) {
        int s = homey_state.status;
        char t[96];
        if (s == HOMEY_ST_OK) snprintf(t, sizeof t, tr("verbonden - %d apparaten", "connected - %d devices"), homey_state.ndev);
        else if (s == HOMEY_ST_AUTH) snprintf(t, sizeof t, "%s", tr("API-sleutel geweigerd", "API key rejected"));
        else if (s == HOMEY_ST_NOCONN) snprintf(t, sizeof t, "%s", tr("geen verbinding", "no connection"));
        else if (s == HOMEY_ST_BADDATA) snprintf(t, sizeof t, "%s", tr("onverwacht antwoord", "unexpected answer"));
        else snprintf(t, sizeof t, "%s", settings.enable_homey ? tr("verbinden...", "connecting...") : tr("uit", "off"));
        ui_label_set_text(lbl_status, t);
    }
}

static int cmp_str(const void * a, const void * b) {
    return strcasecmp(*(const char * const *)a, *(const char * const *)b);
}

static void rebuild(void) {
    if (!list) return;
    lv_obj_clean(list);
    nrows = 0;
    built_sig = sig();

    homey_lock();
    /* Zones in use, alphabetical; then devices without a (known) zone. */
    const char * zn[HY_MAX_ZONE];
    int nz = 0;
    for (int z = 0; z < homey_state.nzone; z++) {
        int used = 0;
        for (int i = 0; i < homey_state.ndev && !used; i++)
            used = !strcmp(homey_state.dev[i].zone, homey_state.zone[z].id);
        if (used) zn[nz++] = homey_state.zone[z].name;
    }
    qsort(zn, (size_t)nz, sizeof zn[0], cmp_str);
    for (int z = 0; z < nz; z++) {
        add_header(zn[z]);
        for (int i = 0; i < homey_state.ndev; i++)
            if (!strcasecmp(homey_zone_name(homey_state.dev[i].zone), zn[z])) build_card(i);
    }
    int any = 0;
    for (int i = 0; i < homey_state.ndev; i++) {
        if (homey_zone_name(homey_state.dev[i].zone)[0]) continue;
        if (!any) { add_header(tr("Zonder zone", "No zone")); any = 1; }
        build_card(i);
    }
    homey_unlock();

    if (empty_hint) {
        if (nrows) lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_rows();
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root || lv_scr_act() != scr_root) return;
    if (sig() != built_sig) rebuild();
    else refresh_rows();
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    } else if (c == LV_EVENT_DELETE) {
        if (refresh_timer) { lv_timer_del(refresh_timer); refresh_timer = NULL; }
        scr_root = list = lbl_status = empty_hint = NULL;
        nrows = 0;
    }
}

lv_obj_t * screen_homey_create(void) {
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
    lv_label_set_text(title, "Homey");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 16);

    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_status, SF(16), 0);
    lv_label_set_text(lbl_status, "");
    lv_obj_align_to(lbl_status, title, LV_ALIGN_OUT_RIGHT_BOTTOM, 16, -4);

    int x_w = (DISP_VER < 600) ? 60 : 70;
    int x_h = (DISP_VER < 600) ? 46 : 50;
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

    list = lv_obj_create(scr_root);
    lv_obj_set_size(list, DISP_HOR - 24, DISP_VER - SY(78));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SY(70));
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_style_pad_column(list, 8, 0);

    empty_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(empty_hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(empty_hint, SF(22), 0);
    lv_obj_set_width(empty_hint, DISP_HOR - 120);
    lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(empty_hint, tr("Nog geen Homey-apparaten.\nStel de Homey in via Instellingen > Homey.",
                                     "No Homey devices yet.\nSet up Homey in Settings > Homey."));
    lv_obj_align(empty_hint, LV_ALIGN_CENTER, 0, SY(30));
    lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    rebuild();
    return scr_root;
}
