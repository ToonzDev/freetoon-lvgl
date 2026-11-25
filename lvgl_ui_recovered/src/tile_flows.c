/* "Homey flows" home tile and its chooser: see tile_flows.h. */
#include "tile_flows.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "ui_lazy.h"
#include "homey.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define COL_FLOW 0xc062d8      /* tile accent (layout editor uses the same) */

/* ---- storage: settings.tile_flows ---------------------------------------- */

static int is_auto(void) { return !strcmp(settings.tile_flows, "auto"); }

/* n-th field of the comma list ("" past the end). */
static void field(const char * list, int n, char * out, size_t sz) {
    const char * p = list;
    for (int i = 0; i < n && p; i++) {
        p = strchr(p, ',');
        if (p) p++;
    }
    out[0] = 0;
    if (!p) return;
    size_t l = strcspn(p, ",");
    if (l >= sz) l = sz - 1;
    memcpy(out, p, l);
    out[l] = 0;
}

void tf_get(int slot, char * id, size_t n) {
    id[0] = 0;
    if (slot < 0 || slot >= TF_SLOTS) return;
    if (is_auto()) {                        /* not chosen yet: Homey's first flows */
        homey_lock();
        if (slot < homey_state.nflow) snprintf(id, n, "%s", homey_state.flow[slot].id);
        homey_unlock();
        return;
    }
    field(settings.tile_flows, slot, id, n);
}

int tf_slot_of(const char * id) {
    if (!id || !id[0]) return -1;
    char cur[HY_ID_LEN];
    for (int k = 0; k < TF_SLOTS; k++) {
        tf_get(k, cur, sizeof cur);
        if (!strcmp(cur, id)) return k;
    }
    return -1;
}

void tf_set(int slot, const char * id) {
    if (slot < 0 || slot >= TF_SLOTS) return;
    char cur[TF_SLOTS][HY_ID_LEN];
    for (int k = 0; k < TF_SLOTS; k++) tf_get(k, cur[k], sizeof cur[k]);   /* "auto" fixed here */
    for (int k = 0; k < TF_SLOTS; k++)
        if (id && id[0] && !strcmp(cur[k], id)) cur[k][0] = 0;              /* moves */
    snprintf(cur[slot], sizeof cur[slot], "%s", id ? id : "");
    char out[sizeof settings.tile_flows];
    size_t o = 0;
    out[0] = 0;
    for (int k = 0; k < TF_SLOTS; k++)
        o += (size_t)snprintf(out + o, o < sizeof out ? sizeof out - o : 0, "%s%s", k ? "," : "", cur[k]);
    if (o >= sizeof out) return;            /* can't happen: ids are <= 39 chars */
    snprintf(settings.tile_flows, sizeof settings.tile_flows, "%s", out);
    settings_save();
}

int tf_name(const char * id, char * name, size_t n) {
    name[0] = 0;
    if (!id || !id[0]) return 0;
    int found = 0;
    homey_lock();
    for (int i = 0; i < homey_state.nflow; i++)
        if (!strcmp(homey_state.flow[i].id, id)) {
            snprintf(name, n, "%s", homey_state.flow[i].name);
            found = 1;
            break;
        }
    homey_unlock();
    if (!found) snprintf(name, n, "%s", tr("(flow weg)", "(flow gone)"));
    return found;
}

/* ---- home tile ------------------------------------------------------------ */

static lv_obj_t * g_tile;
static lv_obj_t * g_hint;
static lv_obj_t * g_btn[TF_SLOTS];
static lv_obj_t * g_lbl[TF_SLOTS];
static char       g_txt[TF_SLOTS][HY_NAME_LEN + 8];
static char       g_hint_txt[120];
static int        g_cap;
static uint32_t   g_flash_until[TF_SLOTS];  /* lv_tick: button shows "started" until */
static int        g_flash_ok[TF_SLOTS];

int tf_capacity(void) { return g_tile ? g_cap : 0; }

static void open_chooser_async(void * u) { (void)u; ui_push_owned(screen_tile_flows_create()); }
static void on_longpress(lv_event_t * e) {
    (void)e;
    lv_indev_t * in = lv_indev_get_act();
    if (in) lv_indev_wait_release(in);      /* no click on release */
    lv_async_call(open_chooser_async, NULL);
}

static void on_flow_btn(lv_event_t * e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    char id[HY_ID_LEN];
    tf_get(k, id, sizeof id);
    if (!id[0]) return;
    int rc = homey_start_flow(id);
    g_flash_ok[k] = rc == 0;
    g_flash_until[k] = lv_tick_get() + 1500;
    fprintf(stderr, "[flows] start %s: %s\n", id, rc == 0 ? "queued" : "refused");
    tf_tile_update();
}

static void on_tile_delete(lv_event_t * e) {
    (void)e;
    g_tile = g_hint = NULL;
    for (int k = 0; k < TF_SLOTS; k++) g_btn[k] = g_lbl[k] = NULL;
    g_cap = 0;
}

void tf_tile_build(lv_obj_t * tile, lv_obj_t * title) {
    int is_t1 = DISP_VER < 600;
    g_tile = tile;
    lv_obj_add_event_cb(tile, on_tile_delete, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, on_longpress, LV_EVENT_LONG_PRESSED, NULL);

    /* Grid under the title: as many buttons as fit, at most TF_SLOTS. */
    lv_obj_update_layout(tile);
    int cw = lv_obj_get_content_width(tile), ch = lv_obj_get_content_height(tile);
    int top = is_t1 ? 30 : SY(40);
    if (title) top = lv_obj_get_y(title) + lv_obj_get_height(title) + (is_t1 ? 4 : SY(6));
    int gap = is_t1 ? 6 : SX(8);
    int min_w = is_t1 ? 100 : SX(130), bh = is_t1 ? 36 : SY(44);
    if (ch - top < bh && title) {           /* a one-row tile: buttons over the title's place */
        ui_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
        top = 8;                            /* under the accent bar */
    }
    if (bh > ch - top) bh = ch - top;
    int cols = (cw + gap) / (min_w + gap);
    int rows = (ch - top + gap) / (bh + gap);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    g_cap = cols * rows > TF_SLOTS ? TF_SLOTS : cols * rows;
    if (g_cap < cols) cols = g_cap;
    int bw = (cw - (cols - 1) * gap) / cols;
    /* rows actually used: spread them over the height */
    int used_rows = (g_cap + cols - 1) / cols;
    int vgap = used_rows > 1 ? (ch - top - used_rows * bh) / (used_rows - 1) : 0;
    if (vgap > gap * 3) vgap = gap * 3;
    if (vgap < gap) vgap = gap;

    for (int k = 0; k < g_cap; k++) {
        lv_obj_t * b = lv_btn_create(tile);
        g_btn[k] = b;
        lv_obj_set_size(b, bw, bh);
        lv_obj_set_pos(b, (k % cols) * (bw + gap), top + (k / cols) * (bh + vgap));
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_BG), 0);
        lv_obj_add_event_cb(b, on_flow_btn, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        lv_obj_add_event_cb(b, on_longpress, LV_EVENT_LONG_PRESSED, NULL);
        g_lbl[k] = lv_label_create(b);
        lv_obj_set_style_text_font(g_lbl[k], SF(is_t1 ? 16 : 18), 0);
        lv_obj_set_style_text_color(g_lbl[k], lv_color_hex(THEME_TEXT_HI), 0);
        lv_label_set_long_mode(g_lbl[k], LV_LABEL_LONG_DOT);
        lv_obj_set_width(g_lbl[k], bw - 12);
        /* one line, "..." when too long (LONG_DOT wraps without a fixed height) */
        lv_obj_set_height(g_lbl[k], lv_font_get_line_height(SF(is_t1 ? 16 : 18)));
        lv_obj_set_style_text_align(g_lbl[k], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(g_lbl[k], "");
        lv_obj_center(g_lbl[k]);
        g_txt[k][0] = 0;
    }

    g_hint = lv_label_create(tile);
    lv_obj_set_style_text_font(g_hint, SF(16), 0);
    lv_obj_set_style_text_color(g_hint, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_label_set_long_mode(g_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_hint, cw);
    lv_label_set_text(g_hint, "");
    lv_obj_set_pos(g_hint, 0, top + 4);
    g_hint_txt[0] = 0;
    tf_tile_update();
}

void tf_tile_update(void) {
    if (!g_tile) return;
    int shown = 0;
    uint32_t now = lv_tick_get();
    for (int k = 0; k < g_cap; k++) {
        char id[HY_ID_LEN], name[HY_NAME_LEN];
        tf_get(k, id, sizeof id);
        if (!id[0]) { ui_obj_add_flag(g_btn[k], LV_OBJ_FLAG_HIDDEN); continue; }
        int known = tf_name(id, name, sizeof name);
        int flash = g_flash_until[k] && (int32_t)(g_flash_until[k] - now) > 0;
        if (!flash) g_flash_until[k] = 0;
        char txt[sizeof g_txt[0]];
        if (flash) snprintf(txt, sizeof txt, "%s", g_flash_ok[k] ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
        else snprintf(txt, sizeof txt, "%s", name);
        ui_label_set_text_cached(g_lbl[k], txt, g_txt[k], sizeof g_txt[k]);
        ui_obj_set_style_bg_color(g_btn[k], lv_color_hex(flash ? (g_flash_ok[k] ? 0x3fbf7f : 0xd04040)
                                                               : THEME_BTN_BG), 0);
        ui_obj_set_style_text_color(g_lbl[k], lv_color_hex(known || flash ? THEME_TEXT_HI : THEME_TEXT_DIM), 0);
        ui_obj_clear_flag(g_btn[k], LV_OBJ_FLAG_HIDDEN);
        shown++;
    }
    const char * h = "";
    if (!settings.enable_homey || !settings.homey_host[0])
        h = tr("Homey is niet ingesteld (Instellingen > Homey).", "Homey is not set up (Settings > Homey).");
    else if (!shown && homey_state.nflow == 0)
        h = tr("Geen startbare flows gevonden.", "No flows that can be started.");
    else if (!shown)
        h = tr("Lang drukken om flows te kiezen.", "Long press to choose flows.");
    ui_label_set_text_cached(g_hint, h, g_hint_txt, sizeof g_hint_txt);
    if (h[0]) ui_obj_clear_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
    else ui_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
}

/* ---- chooser screen ------------------------------------------------------- */

typedef struct {
    char       id[HY_ID_LEN];
    lv_obj_t * card;
    lv_obj_t * badge;
} frow_t;

static lv_obj_t   * c_root, * c_list, * c_empty, * c_hint;
static lv_obj_t   * c_chip[TF_SLOTS], * c_chip_lbl[TF_SLOTS];
static char         c_chip_txt[TF_SLOTS][HY_NAME_LEN + 8];
static char         c_hint_txt[160];
static lv_timer_t * c_timer;
static frow_t       c_rows[HY_MAX_FLOW];
static int          c_nrows, c_sel;
static unsigned     c_sig;

/* The tile's capacity, or all places when the tile is not on the home screen. */
static int cap_now(void) { int c = tf_capacity(); return c > 0 ? c : TF_SLOTS; }

static unsigned flows_sig(void) {
    unsigned h = 5381u;
    homey_lock();
    h = h * 33u + (unsigned)homey_state.nflow;
    for (int i = 0; i < homey_state.nflow; i++)
        for (const char * p = homey_state.flow[i].id; *p; p++) h = h * 33u + (unsigned char)*p;
    homey_unlock();
    return h;
}

static int next_free(int from) {
    int cap = cap_now();
    char id[HY_ID_LEN];
    for (int k = 1; k <= cap; k++) {
        int s = (from + k) % cap;
        tf_get(s, id, sizeof id);
        if (!id[0]) return s;
    }
    return from;
}

static void c_marks(void) {
    char cur[TF_SLOTS][HY_ID_LEN];
    int cap = cap_now();
    for (int k = 0; k < TF_SLOTS; k++) {
        tf_get(k, cur[k], sizeof cur[k]);
        if (!c_chip[k]) continue;
        char name[HY_NAME_LEN], txt[sizeof c_chip_txt[0]];
        if (!cur[k][0]) snprintf(name, sizeof name, "%s", tr("(leeg)", "(empty)"));
        else tf_name(cur[k], name, sizeof name);
        snprintf(txt, sizeof txt, "%d  %s", k + 1, name);
        ui_label_set_text_cached(c_chip_lbl[k], txt, c_chip_txt[k], sizeof c_chip_txt[k]);
        int fits = k < cap;
        ui_obj_set_style_border_color(c_chip[k], lv_color_hex(k == c_sel ? COL_FLOW : THEME_BORDER), 0);
        ui_obj_set_style_border_width(c_chip[k], k == c_sel ? 3 : 1, 0);
        ui_obj_set_style_text_color(c_chip_lbl[k], lv_color_hex(fits ? THEME_TEXT_HI : THEME_TEXT_DIM), 0);
        ui_obj_set_style_bg_opa(c_chip[k], fits ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if (c_hint) {
        char h[160];
        if (c_sel >= cap)
            snprintf(h, sizeof h, tr("Plek %d past niet op de tegel: maak hem groter in de lay-out-editor.",
                                     "Place %d does not fit on the tile: make it bigger in the layout editor."), c_sel + 1);
        else
            snprintf(h, sizeof h, "%s", tr("Kies een plek, tik dan een flow. Nog eens tikken haalt hem weg.",
                                           "Pick a place, then tap a flow. Tap it again to remove it."));
        ui_label_set_text_cached(c_hint, h, c_hint_txt, sizeof c_hint_txt);
    }
    for (int i = 0; i < c_nrows; i++) {
        int at = -1;
        for (int k = 0; k < TF_SLOTS && at < 0; k++)
            if (!strcmp(c_rows[i].id, cur[k])) at = k;
        ui_obj_set_style_border_color(c_rows[i].card, lv_color_hex(at >= 0 ? COL_FLOW : THEME_BORDER), 0);
        ui_obj_set_style_border_width(c_rows[i].card, at >= 0 ? 2 : (settings.home_light_mode ? 1 : 0), 0);
        if (at >= 0) {
            ui_label_set_text_fmt(c_rows[i].badge, "%d", at + 1);
            ui_obj_clear_flag(c_rows[i].badge, LV_OBJ_FLAG_HIDDEN);
        } else ui_obj_add_flag(c_rows[i].badge, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_chip(lv_event_t * e) { c_sel = (int)(intptr_t)lv_event_get_user_data(e); c_marks(); }
static void on_pick(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= c_nrows) return;
    if (tf_slot_of(c_rows[i].id) == c_sel) tf_set(c_sel, "");
    else { tf_set(c_sel, c_rows[i].id); c_sel = next_free(c_sel); }
    c_marks();
    tf_tile_update();
}
static void on_clear(lv_event_t * e) { (void)e; tf_set(c_sel, ""); c_marks(); tf_tile_update(); }
static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

static void add_row(const char * id, const char * name) {
    if (c_nrows >= HY_MAX_FLOW) return;
    int is_t1 = DISP_VER < 600;
    frow_t * r = &c_rows[c_nrows];
    snprintf(r->id, sizeof r->id, "%s", id);
    lv_obj_t * c = lv_obj_create(c_list);
    r->card = c;
    lv_obj_set_size(c, lv_pct(100), is_t1 ? 46 : SY(52));
    lv_obj_set_style_bg_color(c, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(c, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 4, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, on_pick, LV_EVENT_CLICKED, (void *)(intptr_t)c_nrows);
    int bw = is_t1 ? 40 : SX(46);
    lv_obj_t * nm = lv_label_create(c);
    lv_obj_set_style_text_font(nm, SF(20), 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, DISP_HOR - 60 - bw);
    lv_label_set_text(nm, name);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 8, 0);
    r->badge = lv_label_create(c);
    lv_obj_set_size(r->badge, bw, is_t1 ? 34 : SY(38));
    lv_obj_set_style_bg_color(r->badge, lv_color_hex(COL_FLOW), 0);
    lv_obj_set_style_bg_opa(r->badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r->badge, 8, 0);
    lv_obj_set_style_text_font(r->badge, SF(22), 0);
    lv_obj_set_style_text_color(r->badge, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(r->badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(r->badge, is_t1 ? 4 : SY(6), 0);
    lv_label_set_text(r->badge, "");
    lv_obj_align(r->badge, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_flag(r->badge, LV_OBJ_FLAG_HIDDEN);
    c_nrows++;
}

static void c_rebuild(void) {
    if (!c_list) return;
    lv_obj_clean(c_list);
    c_nrows = 0;
    c_sig = flows_sig();
    static hy_flow_t fl[HY_MAX_FLOW];
    int n;
    homey_lock();
    n = homey_state.nflow;
    memcpy(fl, homey_state.flow, sizeof(hy_flow_t) * (size_t)n);
    homey_unlock();
    for (int i = 0; i < n; i++) add_row(fl[i].id, fl[i].name);
    if (c_nrows) lv_obj_add_flag(c_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(c_empty, LV_OBJ_FLAG_HIDDEN);
    c_marks();
}

static void c_tick(lv_timer_t * t) {
    (void)t;
    if (!c_root || lv_scr_act() != c_root) return;
    if (flows_sig() != c_sig) c_rebuild();
    else c_marks();
}

static void c_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) { if (c_timer) lv_timer_resume(c_timer); }
    else if (c == LV_EVENT_SCREEN_UNLOADED) { if (c_timer) lv_timer_pause(c_timer); }
    else if (c == LV_EVENT_DELETE) {
        if (c_timer) { lv_timer_del(c_timer); c_timer = NULL; }
        c_root = c_list = c_empty = c_hint = NULL;
        for (int k = 0; k < TF_SLOTS; k++) c_chip[k] = c_chip_lbl[k] = NULL;
        c_nrows = 0;
    }
}

lv_obj_t * screen_tile_flows_create(void) {
    int is_t1 = DISP_VER < 600;
    c_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(c_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(c_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(c_root, c_event, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(c_root, c_event, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(c_root, c_event, LV_EVENT_DELETE, NULL);
    c_nrows = 0;
    c_sel = next_free(cap_now() - 1);
    {
        char id[HY_ID_LEN];
        tf_get(c_sel, id, sizeof id);
        if (id[0]) c_sel = 0;
    }

    lv_obj_t * title = lv_label_create(c_root);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Homey flows-tegel", "Homey flows tile"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, is_t1 ? 12 : 16);

    int x_w = is_t1 ? 60 : 70, x_h = is_t1 ? 46 : 50;
    lv_obj_t * back = lv_btn_create(c_root);
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

    lv_obj_t * clr = lv_btn_create(c_root);
    lv_obj_set_size(clr, is_t1 ? 130 : SX(150), x_h);
    lv_obj_align_to(clr, back, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_set_style_bg_color(clr, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(clr, 12, 0);
    lv_obj_add_event_cb(clr, on_clear, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(clr);
    lv_label_set_text(cl, tr("Plek leeg", "Clear place"));
    lv_obj_set_style_text_color(cl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(cl, SF(18), 0);
    lv_obj_center(cl);

    int top = is_t1 ? 66 : SY(76);
    int gap = is_t1 ? 6 : SX(8);
    int cw = (DISP_HOR - 24 - 3 * gap) / 4;
    int chh = is_t1 ? 40 : SY(46);
    for (int k = 0; k < TF_SLOTS; k++) {
        lv_obj_t * c = lv_obj_create(c_root);
        c_chip[k] = c;
        lv_obj_set_size(c, cw, chh);
        lv_obj_set_pos(c, 12 + (k % 4) * (cw + gap), top + (k / 4) * (chh + gap));
        lv_obj_set_style_bg_color(c, lv_color_hex(THEME_CARD_BG), 0);
        lv_obj_set_style_border_color(c, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_radius(c, 10, 0);
        lv_obj_set_style_pad_all(c, 4, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(c, 3);
        lv_obj_add_event_cb(c, on_chip, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        c_chip_lbl[k] = lv_label_create(c);
        c_chip_txt[k][0] = 0;
        lv_obj_set_style_text_font(c_chip_lbl[k], SF(16), 0);
        lv_obj_set_style_text_color(c_chip_lbl[k], lv_color_hex(THEME_TEXT_HI), 0);
        lv_label_set_long_mode(c_chip_lbl[k], LV_LABEL_LONG_DOT);
        lv_obj_set_width(c_chip_lbl[k], cw - 14);
        lv_label_set_text(c_chip_lbl[k], "");
        lv_obj_align(c_chip_lbl[k], LV_ALIGN_LEFT_MID, 4, 0);
    }

    int hy = top + 2 * (chh + gap);
    c_hint = lv_label_create(c_root);
    c_hint_txt[0] = 0;
    lv_obj_set_style_text_color(c_hint, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(c_hint, SF(16), 0);
    lv_label_set_long_mode(c_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(c_hint, DISP_HOR - 24);
    lv_label_set_text(c_hint, "");
    lv_obj_set_pos(c_hint, 14, hy);

    int ly = hy + (is_t1 ? 22 : SY(26));
    c_list = lv_obj_create(c_root);
    lv_obj_set_size(c_list, DISP_HOR - 24, DISP_VER - ly - 6);
    lv_obj_align(c_list, LV_ALIGN_TOP_MID, 0, ly);
    lv_obj_set_style_bg_opa(c_list, 0, 0);
    lv_obj_set_style_border_width(c_list, 0, 0);
    lv_obj_set_style_pad_all(c_list, 2, 0);
    lv_obj_set_flex_flow(c_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c_list, 6, 0);

    c_empty = lv_label_create(c_root);
    lv_obj_set_style_text_color(c_empty, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(c_empty, SF(22), 0);
    lv_obj_set_width(c_empty, DISP_HOR - 120);
    lv_label_set_long_mode(c_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(c_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(c_empty, tr("Nog geen flows gevonden.\nAlleen flows met de kaart 'Deze flow is gestart' kunnen hier.",
                                  "No flows found yet.\nOnly flows with the 'This flow is started' card can be used."));
    lv_obj_align(c_empty, LV_ALIGN_CENTER, 0, SY(60));
    lv_obj_add_flag(c_empty, LV_OBJ_FLAG_HIDDEN);

    c_timer = lv_timer_create(c_tick, 1000, NULL);
    c_rebuild();
    return c_root;
}
