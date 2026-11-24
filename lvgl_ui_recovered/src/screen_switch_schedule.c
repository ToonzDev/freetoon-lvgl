/*
 * Schakelschema screen: the rules of switch_schedule.c (switch a device on
 * or off at a time or at sunrise / sunset, on chosen weekdays). Opened from
 * the Devices hub. A list of rules (tap = edit, switch = active), the next
 * action at the top, "+ Regel" to add one. The editor and the device picker
 * are full-screen layers on this screen. Built per open (ui_push_owned).
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "ui_lazy.h"
#include "tile_lights.h"
#include "switch_schedule.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define COL_ACCENT 0xddaa44
#define COL_DANGER 0xb94a48

static lv_obj_t   * scr_root, * list, * next_lbl, * empty_lbl;
static lv_timer_t * refresh_timer;

/* editor */
static lv_obj_t * ed, * ed_dev_lbl, * ed_msg, * ed_on_btn[2], * ed_mode_btn[3];
static lv_obj_t * ed_time_row, * ed_off_row, * ed_hh, * ed_mm, * ed_off, * ed_day[7];
static ss_rule_t  g_ed;
static int        g_ed_idx = -1;
/* device picker */
static lv_obj_t * picker, * picker_list;
static char       g_pick_refs[80][TL_REF_LEN + 4];
static int        g_pick_n;
static char       g_pick_group[48];

static int t1(void) { return DISP_VER < 600; }

static void rebuild_list(void);
static void ed_refresh(void);

/* ---- small widgets ---- */
static lv_obj_t * mk_btn(lv_obj_t * parent, const char * txt, int w, int h, uint32_t col,
                         lv_event_cb_t cb, intptr_t ud) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_ext_click_area(b, 4);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_font(l, SF(20), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}
static lv_obj_t * mk_label(lv_obj_t * parent, const char * txt, int font_px, uint32_t col) {
    lv_obj_t * l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, SF(font_px), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_label_set_text(l, txt);
    return l;
}
static void select_btn(lv_obj_t * b, int on) {
    ui_obj_set_style_bg_color(b, lv_color_hex(on ? COL_ACCENT : THEME_BTN_BG), 0);
    lv_obj_t * l = lv_obj_get_child(b, 0);
    if (l) ui_obj_set_style_text_color(l, lv_color_hex(on ? 0x202020 : THEME_TEXT_HI), 0);
}
static lv_obj_t * full_layer(void) {
    lv_obj_t * o = lv_obj_create(scr_root);
    lv_obj_set_size(o, DISP_HOR, DISP_VER);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* ---- device picker ---- */
static void picker_close(void) { if (picker) { lv_obj_del(picker); picker = picker_list = NULL; } }
static void picker_close_async(void * u) { (void)u; picker_close(); }
static void on_picker_close(lv_event_t * e) { (void)e; lv_async_call(picker_close_async, NULL); }
static void on_pick(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= g_pick_n) return;
    snprintf(g_ed.ref, sizeof g_ed.ref, "%s", g_pick_refs[i]);
    lv_async_call(picker_close_async, NULL);
    ed_refresh();
}
static void pick_row(void * ctx, const char * group, const char * ref,
                     const char * name, int available, int dimmable) {
    (void)ctx; (void)dimmable;
    if (g_pick_n >= (int)(sizeof g_pick_refs / sizeof g_pick_refs[0])) return;
    if (strcmp(group, g_pick_group)) {
        lv_obj_t * h = mk_label(picker_list, group, 18, THEME_TEXT_DIM);
        lv_obj_set_width(h, lv_pct(100));
        lv_obj_set_style_pad_top(h, 6, 0);
        snprintf(g_pick_group, sizeof g_pick_group, "%s", group);
    }
    snprintf(g_pick_refs[g_pick_n], sizeof g_pick_refs[0], "%s", ref);
    lv_obj_t * c = lv_obj_create(picker_list);
    lv_obj_set_size(c, lv_pct(100), t1() ? 46 : SY(52));
    lv_obj_set_style_bg_color(c, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(c, !strcmp(ref, g_ed.ref) ? 2 : 0, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 4, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, on_pick, LV_EVENT_CLICKED, (void *)(intptr_t)g_pick_n);
    lv_obj_t * l = mk_label(c, "", 20, available ? THEME_TEXT_HI : THEME_TEXT_DIM);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, DISP_HOR - 80);
    if (available) lv_label_set_text(l, name);
    else lv_label_set_text_fmt(l, "%s  (offline)", name);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
    g_pick_n++;
}
static void picker_open(void) {
    picker_close();
    picker = full_layer();
    lv_obj_t * t = mk_label(picker, tr("Kies een apparaat", "Choose a device"), 28, THEME_TEXT_HI);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 24, 16);
    lv_obj_t * x = mk_btn(picker, LV_SYMBOL_CLOSE, t1() ? 60 : 70, t1() ? 46 : 50, THEME_BTN_BG, on_picker_close, 0);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, SX(-12), SY(12));
    picker_list = lv_obj_create(picker);
    int y = t1() ? 66 : SY(76);
    lv_obj_set_size(picker_list, DISP_HOR - 24, DISP_VER - y - 6);
    lv_obj_align(picker_list, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(picker_list, 0, 0);
    lv_obj_set_style_border_width(picker_list, 0, 0);
    lv_obj_set_style_pad_all(picker_list, 2, 0);
    lv_obj_set_flex_flow(picker_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(picker_list, 6, 0);
    g_pick_n = 0;
    g_pick_group[0] = 0;
    tl_enumerate(pick_row, NULL);
    if (!g_pick_n) {
        lv_obj_t * h = mk_label(picker_list,
            tr("Nog geen apparaten. Koppel stekkers of lampen via Instellingen.",
               "No devices yet. Connect plugs or lights in Settings."), 20, THEME_TEXT_DIM);
        lv_obj_set_width(h, lv_pct(100));
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    }
}

/* ---- editor ---- */
static void ed_close(void) { picker_close(); if (ed) { lv_obj_del(ed); ed = NULL; } }
static void ed_close_async(void * u) { (void)u; ed_close(); rebuild_list(); }

static void ed_refresh(void) {
    if (!ed) return;
    char name[80];
    if (g_ed.ref[0]) sched_device_name(g_ed.ref, name, sizeof name);
    else snprintf(name, sizeof name, "%s", tr("(kies een apparaat)", "(choose a device)"));
    lv_label_set_text_fmt(ed_dev_lbl, "%s  " LV_SYMBOL_RIGHT, name);
    select_btn(ed_on_btn[0], g_ed.on);
    select_btn(ed_on_btn[1], !g_ed.on);
    for (int i = 0; i < 3; i++) select_btn(ed_mode_btn[i], g_ed.mode == i);
    if (g_ed.mode == SS_CLOCK) {
        lv_obj_clear_flag(ed_time_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ed_off_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ed_time_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ed_off_row, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text_fmt(ed_hh, "%02d", g_ed.hhmm / 100);
    lv_label_set_text_fmt(ed_mm, "%02d", g_ed.hhmm % 100);
    if (g_ed.offset) lv_label_set_text_fmt(ed_off, "%+d min", g_ed.offset);
    else lv_label_set_text(ed_off, tr("precies", "exactly"));
    for (int i = 0; i < 7; i++) select_btn(ed_day[i], (g_ed.days >> i) & 1);
}

static void on_ed_dev(lv_event_t * e) { (void)e; picker_open(); }
static void on_ed_on(lv_event_t * e) { g_ed.on = (int)(intptr_t)lv_event_get_user_data(e); ed_refresh(); }
static void on_ed_mode(lv_event_t * e) { g_ed.mode = (int)(intptr_t)lv_event_get_user_data(e); ed_refresh(); }
static void on_ed_day(lv_event_t * e) { g_ed.days ^= 1 << (int)(intptr_t)lv_event_get_user_data(e); ed_refresh(); }
static void on_ed_step(lv_event_t * e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int h = g_ed.hhmm / 100, m = g_ed.hhmm % 100;
    switch (k) {
    case 0: h = (h + 23) % 24; break;
    case 1: h = (h + 1) % 24; break;
    case 2: m = m % 5 ? m - m % 5 : (m + 55) % 60; break;       /* to the 5 below */
    case 3: m = (m - m % 5 + 5) % 60; break;                    /* to the 5 above */
    case 4: if (g_ed.offset > -180) g_ed.offset -= 5; break;
    case 5: if (g_ed.offset < 180) g_ed.offset += 5; break;
    }
    g_ed.hhmm = h * 100 + m;
    ed_refresh();
}
static void on_ed_save(lv_event_t * e) {
    (void)e;
    if (!g_ed.ref[0]) { lv_label_set_text(ed_msg, tr("Kies eerst een apparaat.", "Choose a device first.")); return; }
    if (!(g_ed.days & SS_ALL_DAYS)) { lv_label_set_text(ed_msg, tr("Kies minstens een dag.", "Choose at least one day.")); return; }
    if (sched_put(g_ed_idx, &g_ed) != 0) { lv_label_set_text(ed_msg, tr("Opslaan mislukt.", "Could not save.")); return; }
    lv_async_call(ed_close_async, NULL);
}
static void on_ed_delete(lv_event_t * e) {
    (void)e;
    if (g_ed_idx >= 0) sched_delete(g_ed_idx);
    lv_async_call(ed_close_async, NULL);
}
static void on_ed_cancel(lv_event_t * e) { (void)e; lv_async_call(ed_close_async, NULL); }
static void on_ed_step_repeat(lv_event_t * e) { on_ed_step(e); }

static lv_obj_t * step_btn(lv_obj_t * parent, const char * txt, int k, int w, int h) {
    lv_obj_t * b = mk_btn(parent, txt, w, h, THEME_BTN_BG, on_ed_step, k);
    lv_obj_add_event_cb(b, on_ed_step_repeat, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)k);
    return b;
}

static void ed_open(int idx) {
    ed_close();
    g_ed_idx = idx;
    if (idx < 0 || !sched_get(idx, &g_ed)) {
        memset(&g_ed, 0, sizeof g_ed);
        g_ed.active = 1; g_ed.on = 1; g_ed.mode = SS_CLOCK; g_ed.hhmm = 700; g_ed.days = SS_ALL_DAYS;
        g_ed_idx = -1;
    }
    int T1 = t1();
    int bh = T1 ? 44 : SY(50), gap = T1 ? 8 : SX(10), lx = 24, cx = T1 ? 190 : SX(240);
    int row = T1 ? 56 : SY(64);
    ed = full_layer();
    lv_obj_t * t = mk_label(ed, g_ed_idx < 0 ? tr("Nieuwe regel", "New rule") : tr("Regel", "Rule"), 28, THEME_TEXT_HI);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, lx, 16);
    int y = T1 ? 64 : SY(78);

    lv_obj_t * l = mk_label(ed, tr("Apparaat", "Device"), 20, THEME_TEXT_DIM);
    lv_obj_set_pos(l, lx, y + bh / 2 - 12);
    lv_obj_t * db = mk_btn(ed, "", DISP_HOR - cx - 24, bh, THEME_CARD_BG, on_ed_dev, 0);
    lv_obj_set_pos(db, cx, y);
    ed_dev_lbl = lv_obj_get_child(db, 0);
    lv_label_set_long_mode(ed_dev_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ed_dev_lbl, DISP_HOR - cx - 48);
    y += row;

    l = mk_label(ed, tr("Actie", "Action"), 20, THEME_TEXT_DIM);
    lv_obj_set_pos(l, lx, y + bh / 2 - 12);
    int bw = T1 ? 110 : SX(130);
    ed_on_btn[0] = mk_btn(ed, tr("Aan", "On"), bw, bh, THEME_BTN_BG, on_ed_on, 1);
    lv_obj_set_pos(ed_on_btn[0], cx, y);
    ed_on_btn[1] = mk_btn(ed, tr("Uit", "Off"), bw, bh, THEME_BTN_BG, on_ed_on, 0);
    lv_obj_set_pos(ed_on_btn[1], cx + bw + gap, y);
    y += row;

    l = mk_label(ed, tr("Moment", "When"), 20, THEME_TEXT_DIM);
    lv_obj_set_pos(l, lx, y + bh / 2 - 12);
    static const char * const mn[3][2] = { { "Tijd", "Time" }, { "Zonsopkomst", "Sunrise" }, { "Zonsondergang", "Sunset" } };
    int mw = (DISP_HOR - cx - 24 - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        ed_mode_btn[i] = mk_btn(ed, tr(mn[i][0], mn[i][1]), mw, bh, THEME_BTN_BG, on_ed_mode, i);
        lv_obj_set_pos(ed_mode_btn[i], cx + i * (mw + gap), y);
    }
    y += row;

    /* clock time: [-] HH [+]  :  [-] MM [+] ; sun: [-] offset [+] */
    int sw = T1 ? 56 : SX(64), vw = T1 ? 56 : SX(70);
    ed_time_row = lv_obj_create(ed);
    ed_off_row = lv_obj_create(ed);
    lv_obj_t * rows[2] = { ed_time_row, ed_off_row };
    for (int r = 0; r < 2; r++) {
        lv_obj_set_size(rows[r], DISP_HOR - 24, bh + 4);
        lv_obj_set_pos(rows[r], 0, y);
        lv_obj_set_style_bg_opa(rows[r], 0, 0);
        lv_obj_set_style_border_width(rows[r], 0, 0);
        lv_obj_set_style_pad_all(rows[r], 0, 0);
        lv_obj_clear_flag(rows[r], LV_OBJ_FLAG_SCROLLABLE);
        l = mk_label(rows[r], r ? tr("Verschuiving", "Offset") : tr("Tijd", "Time"), 20, THEME_TEXT_DIM);
        lv_obj_set_pos(l, lx, bh / 2 - 12);
    }
    int x = cx;
    lv_obj_t * b = step_btn(ed_time_row, LV_SYMBOL_MINUS, 0, sw, bh); lv_obj_set_pos(b, x, 0); x += sw + 4;
    ed_hh = mk_label(ed_time_row, "07", 28, THEME_TEXT_HI);
    lv_obj_set_width(ed_hh, vw); lv_obj_set_style_text_align(ed_hh, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ed_hh, x, bh / 2 - 17); x += vw + 4;
    b = step_btn(ed_time_row, LV_SYMBOL_PLUS, 1, sw, bh); lv_obj_set_pos(b, x, 0); x += sw + gap;
    l = mk_label(ed_time_row, ":", 28, THEME_TEXT_HI); lv_obj_set_pos(l, x, bh / 2 - 17); x += 14 + gap;
    b = step_btn(ed_time_row, LV_SYMBOL_MINUS, 2, sw, bh); lv_obj_set_pos(b, x, 0); x += sw + 4;
    ed_mm = mk_label(ed_time_row, "00", 28, THEME_TEXT_HI);
    lv_obj_set_width(ed_mm, vw); lv_obj_set_style_text_align(ed_mm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ed_mm, x, bh / 2 - 17); x += vw + 4;
    b = step_btn(ed_time_row, LV_SYMBOL_PLUS, 3, sw, bh); lv_obj_set_pos(b, x, 0);
    x = cx;
    b = step_btn(ed_off_row, LV_SYMBOL_MINUS, 4, sw, bh); lv_obj_set_pos(b, x, 0); x += sw + 4;
    ed_off = mk_label(ed_off_row, "", 24, THEME_TEXT_HI);
    lv_obj_set_width(ed_off, T1 ? 130 : SX(150)); lv_obj_set_style_text_align(ed_off, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ed_off, x, bh / 2 - 14); x += (T1 ? 130 : SX(150)) + 4;
    b = step_btn(ed_off_row, LV_SYMBOL_PLUS, 5, sw, bh); lv_obj_set_pos(b, x, 0);
    y += row;

    l = mk_label(ed, tr("Dagen", "Days"), 20, THEME_TEXT_DIM);
    lv_obj_set_pos(l, lx, y + bh / 2 - 12);
    static const char * const dn[7] = { "ma", "di", "wo", "do", "vr", "za", "zo" };
    int dw = (DISP_HOR - cx - 24 - 6 * 6) / 7;
    for (int i = 0; i < 7; i++) {
        ed_day[i] = mk_btn(ed, dn[i], dw, bh, THEME_BTN_BG, on_ed_day, i);
        lv_obj_set_pos(ed_day[i], cx + i * (dw + 6), y);
    }

    /* bottom: message, Verwijderen | Annuleren, Opslaan */
    int fw = T1 ? 160 : SX(190), fy = DISP_VER - bh - (T1 ? 12 : SY(16));
    ed_msg = mk_label(ed, "", 18, 0xfbbf24);
    lv_obj_set_pos(ed_msg, lx, fy - (T1 ? 26 : SY(30)));
    b = mk_btn(ed, tr("Opslaan", "Save"), fw, bh, THEME_BTN_ACCENT, on_ed_save, 0);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -24, -(DISP_VER - fy - bh));
    b = mk_btn(ed, tr("Annuleren", "Cancel"), fw, bh, THEME_BTN_BG, on_ed_cancel, 0);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -24 - fw - gap, -(DISP_VER - fy - bh));
    if (g_ed_idx >= 0) {
        b = mk_btn(ed, tr("Verwijderen", "Delete"), fw, bh, COL_DANGER, on_ed_delete, 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, lx, -(DISP_VER - fy - bh));
    }
    ed_refresh();
}

/* ---- list ---- */
static void on_row(lv_event_t * e) { ed_open((int)(intptr_t)lv_event_get_user_data(e)); }
static void on_new(lv_event_t * e) { (void)e; ed_open(-1); }
static void on_active(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ss_rule_t r;
    if (!sched_get(i, &r)) return;
    r.active = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    sched_put(i, &r);
}

static void refresh_next(void) {
    if (!next_lbl) return;
    char nx[160];
    if (settings.client_mode)
        ui_label_set_text(next_lbl, tr("Deze Toon is een client: het schema draait op de hoofd-Toon.",
                                       "This Toon is a client: the schedule runs on the main Toon."));
    else if (sched_next_text(nx, sizeof nx) > 0)
        ui_label_set_text_fmt(next_lbl, "%s %s", tr("Volgende:", "Next:"), nx);
    else
        ui_label_set_text(next_lbl, tr("Geen actieve regels.", "No active rules."));
}

static void rebuild_list(void) {
    if (!list) return;
    lv_obj_clean(list);
    int n = sched_count(), T1 = t1();
    for (int i = 0; i < n; i++) {
        ss_rule_t r;
        if (!sched_get(i, &r)) continue;
        lv_obj_t * c = lv_obj_create(list);
        lv_obj_set_size(c, lv_pct(100), T1 ? 62 : SY(72));
        lv_obj_set_style_bg_color(c, lv_color_hex(THEME_CARD_BG), 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_radius(c, 10, 0);
        lv_obj_set_style_pad_all(c, 6, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char name[80], txt[120];
        sched_device_name(r.ref, name, sizeof name);
        sched_rule_text(&r, txt, sizeof txt);
        lv_obj_t * nl = mk_label(c, name, 20, r.active ? THEME_TEXT_HI : THEME_TEXT_DIM);
        lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nl, DISP_HOR - (T1 ? 160 : SX(190)));
        lv_obj_align(nl, LV_ALIGN_TOP_LEFT, 6, 0);
        lv_obj_t * tl = mk_label(c, txt, 16, THEME_TEXT_DIM);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(tl, DISP_HOR - (T1 ? 160 : SX(190)));
        lv_obj_align(tl, LV_ALIGN_BOTTOM_LEFT, 6, 0);
        lv_obj_t * sw = lv_switch_create(c);
        lv_obj_set_size(sw, T1 ? 60 : SX(70), T1 ? 32 : SY(36));
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (r.active) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, on_active, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
    if (empty_lbl) {
        if (n) lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_next();
}

static void refresh_cb(lv_timer_t * t) { (void)t; if (scr_root && lv_scr_act() == scr_root) refresh_next(); }
static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }
static void on_scr_event(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        if (refresh_timer) { lv_timer_del(refresh_timer); refresh_timer = NULL; }
        scr_root = list = next_lbl = empty_lbl = ed = picker = picker_list = NULL;
    }
}

lv_obj_t * screen_switch_schedule_create(void) {
    int T1 = t1();
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_DELETE, NULL);
    ed = picker = picker_list = NULL;

    lv_obj_t * title = mk_label(scr_root, tr("Schakelschema", "Switch schedule"), 28, THEME_TEXT_HI);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 16);
    int x_w = T1 ? 60 : 70, x_h = T1 ? 46 : 50;
    lv_obj_t * back = mk_btn(scr_root, LV_SYMBOL_CLOSE, x_w, x_h, THEME_BTN_BG, on_back, 0);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, SX(-12), SY(12));
    lv_obj_set_ext_click_area(back, 25);
    lv_obj_t * add = mk_btn(scr_root, tr("+ Regel", "+ Rule"), T1 ? 120 : SX(140), x_h, THEME_BTN_ACCENT, on_new, 0);
    lv_obj_align_to(add, back, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    next_lbl = mk_label(scr_root, "", 18, THEME_TEXT_DIM);
    lv_label_set_long_mode(next_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(next_lbl, DISP_HOR - 48);
    lv_obj_set_pos(next_lbl, 24, T1 ? 62 : SY(72));

    int ly = T1 ? 90 : SY(104);
    list = lv_obj_create(scr_root);
    lv_obj_set_size(list, DISP_HOR - 24, DISP_VER - ly - 6);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, ly);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);

    empty_lbl = mk_label(scr_root,
        tr("Nog geen regels.\nMet '+ Regel' zet je een stekker of lamp op vaste tijden,\nof bij zonsopkomst of zonsondergang, aan of uit.",
           "No rules yet.\nWith '+ Rule' you switch a plug or light on or off at set times,\nor at sunrise or sunset."), 20, THEME_TEXT_DIM);
    lv_obj_set_width(empty_lbl, DISP_HOR - 120);
    lv_label_set_long_mode(empty_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, SY(30));

    refresh_timer = lv_timer_create(refresh_cb, 10000, NULL);
    rebuild_list();
    return scr_root;
}
