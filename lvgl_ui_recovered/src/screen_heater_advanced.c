/*
 * Heater detail → Advanced — full list of OT DataId values, pulled directly
 * from OTGW's HTTP API. Polls every 4 s and renders one row per field.
 *
 * URL: http://<settings.otgw_host>/api/v1/otgw/otmonitor
 * Response shape:
 *   {"otmonitor":[
 *     {"name":"flamestatus","value":"Off","unit":"","epoch":...},
 *     {"name":"boilertemperature","value":21.42,"unit":"°C","epoch":...},
 *     ...
 *   ]}
 *
 * No mapping table — we display whatever otmonitor returns, which is the
 * authoritative list of DIDs OTGW is currently tracking. ~33 fields today.
 */
#include "screens.h"
#include "i18n.h"
#include "display.h"
#include "http.h"
#include "settings.h"
#include "theme.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef WASM_BUILD
#include <pthread.h>
#endif

static lv_obj_t * scr_root = NULL;
static lv_obj_t * list_col = NULL;
static lv_obj_t * lbl_status = NULL;
static lv_timer_t * refresh_timer = NULL;
static char        last_json[16384];

/* The OTGW fetch (up to 8 s when the gateway hangs) runs on a short-lived
 * worker thread, never on the LVGL thread: it used to run inline every 4 s,
 * for the rest of uptime once the screen had been opened, freezing the UI
 * whenever OTGW stopped answering. The UI timer only kicks a fetch while
 * this screen is the visible one and picks up the finished result. */
#define FETCH_EVERY_MS 4000
static char           fetch_buf[16384];        /* written by the worker */
static volatile int   fetch_busy = 0;          /* worker running */
static volatile int   fetch_result = 0;        /* 0 none, 1 ok, -1 failed */
static uint32_t       last_kick_ms = 0;
#ifndef WASM_BUILD
static pthread_mutex_t fetch_mtx = PTHREAD_MUTEX_INITIALIZER;

static void * fetch_worker(void * arg) {
    char * url = (char *)arg;
    static char tmp[sizeof fetch_buf];         /* only one worker at a time */
    int rc = http_fetch(url, tmp, sizeof tmp);
    free(url);
    pthread_mutex_lock(&fetch_mtx);
    if (rc == 0) memcpy(fetch_buf, tmp, sizeof fetch_buf);
    fetch_result = (rc == 0) ? 1 : -1;
    fetch_busy = 0;
    pthread_mutex_unlock(&fetch_mtx);
    return NULL;
}
#endif

/* Extract next "name"/"value"/"unit" triple starting at *p; advances *p past it.
   Returns 1 on success, 0 at end-of-stream. */
static int next_field(const char **p, char *name, size_t namesz,
                      char *value, size_t valsz, char *unit, size_t unitsz) {
    const char *q = strstr(*p, "\"name\":");
    if (!q) return 0;
    q = strchr(q, '"'); if (!q) return 0;        /* opening quote of "name" */
    q = strchr(q + 1, '"'); if (!q) return 0;    /* closing quote of name */
    q = strchr(q + 1, '"'); if (!q) return 0;    /* opening quote of value text */
    /* copy name */
    {
        const char *s = q + 1;
        const char *e = strchr(s, '"'); if (!e) return 0;
        size_t n = e - s; if (n > namesz - 1) n = namesz - 1;
        memcpy(name, s, n); name[n] = 0;
        q = e;
    }
    /* find "value": */
    q = strstr(q, "\"value\":"); if (!q) return 0;
    q += 8; while (*q == ' ') q++;
    /* read string or number */
    if (*q == '"') {
        q++;
        const char *e = strchr(q, '"'); if (!e) return 0;
        size_t n = e - q; if (n > valsz - 1) n = valsz - 1;
        memcpy(value, q, n); value[n] = 0;
        q = e + 1;
    } else {
        size_t n = 0;
        while (*q && (*q == '.' || *q == '-' || *q == '+'
                   || (*q >= '0' && *q <= '9')) && n + 1 < valsz)
            value[n++] = *q++;
        value[n] = 0;
    }
    /* unit */
    unit[0] = 0;
    const char *u = strstr(q, "\"unit\":");
    if (u && (u - q) < 80) {
        u = strchr(u, '"'); if (!u) return 1;
        u = strchr(u + 1, '"'); if (!u) return 1;
        u = strchr(u + 1, '"'); if (!u) return 1;
        const char *s = u + 1;
        const char *e = strchr(s, '"'); if (!e) return 1;
        size_t n = e - s; if (n > unitsz - 1) n = unitsz - 1;
        memcpy(unit, s, n); unit[n] = 0;
        q = e;
    }
    *p = q;
    return 1;
}

/* Clear existing list rows, repopulate from `last_json`. */
static void rebuild_list(void) {
    lv_obj_clean(list_col);
    const char *p = last_json;
    char name[64], value[64], unit[16];
    int rows = 0;
    while (next_field(&p, name, sizeof(name), value, sizeof(value),
                      unit, sizeof(unit))) {
        lv_obj_t * row = lv_obj_create(list_col);
        lv_obj_set_size(row, DISP_HOR - SX(44), SY(40));
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, SF(18), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
        lv_label_set_text_fmt(lbl, "%-26s %s %s", name, value, unit);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SX(4), 0);
        rows++;
    }
    if (lbl_status)
        lv_label_set_text_fmt(lbl_status, tr("%d velden", "%d fields"), rows);
}

static void on_refresh(lv_timer_t * t) {
    (void)t;
    /* Only while this screen is showing: no network or rebuilds in the
     * background (dimmed, or a sub-screen on top). */
    if (!scr_root || lv_scr_act() != scr_root) return;
    if (!settings.otgw_host[0]) {
        if (lbl_status) lv_label_set_text(lbl_status, tr("geen OTGW-host ingesteld", "no OTGW host configured"));
        return;
    }
#ifndef WASM_BUILD
    /* Pick up a finished fetch. */
    int res = 0;
    pthread_mutex_lock(&fetch_mtx);
    if (fetch_result != 0) {
        res = fetch_result;
        if (res > 0) memcpy(last_json, fetch_buf, sizeof last_json);
        fetch_result = 0;
    }
    pthread_mutex_unlock(&fetch_mtx);
    if (res > 0) rebuild_list();
    else if (res < 0 && lbl_status) lv_label_set_text(lbl_status, tr("ophalen mislukt", "fetch failed"));

    /* Kick the next one. */
    uint32_t now = lv_tick_get();
    if (!fetch_busy && (last_kick_ms == 0 || now - last_kick_ms >= FETCH_EVERY_MS)) {
        char * url = malloc(160);
        if (!url) return;
        snprintf(url, 160, "http://%s/api/v1/otgw/otmonitor", settings.otgw_host);
        fetch_busy = 1;
        last_kick_ms = now ? now : 1;
        pthread_t th;
        if (pthread_create(&th, NULL, fetch_worker, url) == 0) {
            pthread_detach(th);
        } else {
            free(url);
            fetch_busy = 0;
        }
    }
#else
    char url[160];
    snprintf(url, sizeof(url), "http://%s/api/v1/otgw/otmonitor", settings.otgw_host);
    if (http_fetch(url, last_json, sizeof(last_json)) == 0) rebuild_list();
    else if (lbl_status) lv_label_set_text(lbl_status, tr("ophalen mislukt", "fetch failed"));
#endif
}

static void on_back(lv_event_t * e) {
    (void)e;
    ui_pop();
}

/* Built per open; the stack deletes it when it leaves (ui_push_owned).
 * Before, nothing ever deleted the screen or its timer. A fetch still in
 * flight only writes fetch_buf, never widgets, so it can finish safely. */
static void on_scr_delete(lv_event_t * e) {
    if (lv_event_get_target(e) != scr_root) return;
    if (refresh_timer) { lv_timer_del(refresh_timer); refresh_timer = NULL; }
    scr_root = list_col = lbl_status = NULL;
}

lv_obj_t * screen_heater_advanced_create(void) {
    if (refresh_timer) { lv_timer_del(refresh_timer); refresh_timer = NULL; }
    last_kick_ms = 0;
    scr_root = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr_root, on_scr_delete, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_pad_all(scr_root, 0, 0);

    /* header */
    lv_obj_t * hdr = lv_label_create(scr_root);
    lv_obj_set_style_text_font(hdr, SF(28), 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_text(hdr, tr("Ketel - Geavanceerd (OTGW)", "Boiler - Advanced (OTGW)"));
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, SX(20), SY(16));

    /* close "X" button */
    int x_w = (DISP_VER < 600) ? 60 : 70;
    int x_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, x_w, x_h);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, SX(-12), SY(12));
    lv_obj_set_style_bg_color(back, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_border_color(back, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(back, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_ext_click_area(back, 25);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(bl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_obj_center(bl);

    /* status (field count or error) */
    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_status, SF(14), 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(THEME_TEXT_MUTED), 0);
    lv_label_set_text(lbl_status, tr("laden...", "loading..."));
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, SX(20), SY(56));

    /* scrollable column for the field list */
    list_col = lv_obj_create(scr_root);
    lv_obj_set_size(list_col, DISP_HOR - SX(20), DISP_VER - SY(90));
    lv_obj_align(list_col, LV_ALIGN_TOP_LEFT, SX(10), SY(82));
    lv_obj_set_style_bg_color(list_col, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(list_col, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(list_col, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_flex_flow(list_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_col, 0, 0);

    /* The timer runs every 500 ms but only picks up results / kicks a
     * background fetch every 4 s, and only while this screen is showing.
     * The first tick (right after the screen is loaded) starts a fetch. */
    refresh_timer = lv_timer_create(on_refresh, 500, NULL);

    return scr_root;
}
