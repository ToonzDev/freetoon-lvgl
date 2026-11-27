/* tile_wifiqr.c - see tile_wifiqr.h. */
#include "tile_wifiqr.h"
#include "settings.h"
#include "display.h"        /* SX/SY/SF, DISP_HOR/DISP_VER */
#include "i18n.h"
#include "screens.h"        /* screen_settings_open_wifiqr_modal */
#include "homey_parse.h"    /* hy_fold_text: names -> what the UI fonts draw */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------ the Toon's own network */
#define NETCON_XML "/qmf/config/config_hcb_netcon.xml"

static void xml_unescape(char * s) {
    static const struct { const char * e; char c; } ent[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
    char * w = s;
    for (char * r = s; *r; ) {
        int hit = 0;
        if (*r == '&')
            for (size_t i = 0; i < sizeof ent / sizeof ent[0]; i++) {
                size_t l = strlen(ent[i].e);
                if (!strncmp(r, ent[i].e, l)) { *w++ = ent[i].c; r += l; hit = 1; break; }
            }
        if (!hit) *w++ = *r++;
    }
    *w = 0;
}
/* <tag>value</tag> inside [from, to); 1 = found (may be empty). */
static int xml_val(const char * from, const char * to, const char * tag, char * out, size_t n) {
    char o[40], c[40];
    snprintf(o, sizeof o, "<%s>", tag);
    snprintf(c, sizeof c, "</%s>", tag);
    const char * a = strstr(from, o);
    if (!a || a >= to) { out[0] = 0; return 0; }
    a += strlen(o);
    const char * b = strstr(a, c);
    if (!b || b > to) { out[0] = 0; return 0; }
    size_t l = (size_t)(b - a);
    if (l >= n) l = n - 1;
    memcpy(out, a, l);
    out[l] = 0;
    xml_unescape(out);
    return 1;
}
/* The interface block that holds a WiFi network name. Read at most every
 * 30 s (the refresh runs twice a second). */
static int toon_network(char * ssid, size_t sn, char * key, size_t kn, int * sec) {
    static time_t last;
    static char c_ssid[64], c_key[96];
    static int c_sec, c_ok;
    time_t now = time(NULL);
    if (now - last >= 30 || now < last) {
        last = now;
        c_ok = 0;
        const char * path = getenv("WIFIQR_NETCON");          /* sim / tests */
        FILE * f = fopen(path && *path ? path : NETCON_XML, "r");
        if (f) {
            static char buf[16384];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            for (const char * p = buf; (p = strstr(p, "<interfaceSettings>")) != NULL; p++) {
                const char * end = strstr(p, "</interfaceSettings>");
                if (!end) end = buf + n;
                char auth[32];
                if (xml_val(p, end, "wifi_essid", c_ssid, sizeof c_ssid) && c_ssid[0]) {
                    xml_val(p, end, "wifi_key", c_key, sizeof c_key);
                    xml_val(p, end, "wifi_auth", auth, sizeof auth);
                    c_sec = strstr(auth, "WEP") ? 1 : (!c_key[0] || !strcmp(auth, "OPEN") || !strcmp(auth, "NONE")) ? 2 : 0;
                    c_ok = 1;
                    break;
                }
            }
        }
    }
    if (!c_ok) return 0;
    snprintf(ssid, sn, "%s", c_ssid);
    snprintf(key, kn, "%s", c_key);
    *sec = c_sec;
    return 1;
}

/* Escape \ ; , : " for the WIFI: format. */
static void qr_esc(const char * in, char * out, size_t n) {
    size_t o = 0;
    for (; *in && o + 2 < n; in++) {
        if (strchr("\\;,:\"", *in)) out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
}

int wifiqr_payload(char * qr, size_t qn, char * ssid, size_t sn, char * pass, size_t pn) {
    char s[96] = "", k[96] = "";
    int sec = 0, hidden = 0;
    if (settings.wifiqr_mode == 1) {
        snprintf(s, sizeof s, "%s", settings.wifiqr_ssid);
        snprintf(k, sizeof k, "%s", settings.wifiqr_pass);
        sec = settings.wifiqr_sec;
        hidden = settings.wifiqr_hidden;
    } else if (!toon_network(s, sizeof s, k, sizeof k, &sec)) {
        s[0] = 0;
    }
    if (ssid) snprintf(ssid, sn, "%s", s);
    if (pass) snprintf(pass, pn, "%s", sec == 2 ? "" : k);
    if (!s[0]) { if (qn) qr[0] = 0; return 0; }
    char es[200], ek[200];
    qr_esc(s, es, sizeof es);
    qr_esc(k, ek, sizeof ek);
    if (sec == 2) snprintf(qr, qn, "WIFI:S:%s;T:nopass;%s;", es, hidden ? "H:true;" : "");
    else snprintf(qr, qn, "WIFI:S:%s;T:%s;P:%s;%s;", es, sec == 1 ? "WEP" : "WPA", ek, hidden ? "H:true;" : "");
    return 1;
}

/* ------------------------------------------------------------- the tile */
static lv_obj_t * g_tile, * g_qr_box, * g_qr, * g_name, * g_hint;
static char g_last[300] = "\x01";      /* forces the first update */

void wq_tile_forget(void) {
    g_tile = g_qr_box = g_qr = g_name = g_hint = NULL;
    snprintf(g_last, sizeof g_last, "\x01");
}

static void on_tap(lv_event_t * e) { (void)e; wq_show_big(); }
static void on_hold(lv_event_t * e) {
    (void)e;
    screen_settings_open_wifiqr_modal();
}

void wq_tile_build(lv_obj_t * tile, lv_obj_t * title) {
    g_tile = tile;
    if (title) lv_label_set_text(title, "WiFi");
    lv_obj_update_layout(tile);
    int w = lv_obj_get_content_width(tile), h = lv_obj_get_content_height(tile);
    /* make_tile puts the title at y 14 in SF(18) (set lazily, so not read back) */
    int tl = 14 + lv_font_get_line_height(SF(18)) + 4;
    int nl = lv_font_get_line_height(SF(14)) + 4;
    int sz = h - tl - nl - 8;                        /* QR incl. its white border */
    if (sz > w - 8) sz = w - 8;
    if (sz < 60) sz = 60;
    g_qr_box = lv_obj_create(tile);
    lv_obj_remove_style_all(g_qr_box);
    lv_obj_set_size(g_qr_box, sz, sz);
    lv_obj_set_style_bg_color(g_qr_box, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(g_qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_qr_box, 6, 0);
    lv_obj_align(g_qr_box, LV_ALIGN_TOP_MID, 0, tl);
    lv_obj_clear_flag(g_qr_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    int q = sz - 12;                                  /* 6 px quiet zone each side */
    g_qr = lv_qrcode_create(g_qr_box, q, lv_color_hex(0x000000), lv_color_hex(0xffffff));
    lv_obj_center(g_qr);
    lv_obj_clear_flag(g_qr, LV_OBJ_FLAG_CLICKABLE);
    g_name = lv_label_create(tile);
    lv_obj_set_style_text_font(g_name, SF(14), 0);
    lv_obj_set_style_text_color(g_name, lv_color_hex(0xc4dcee), 0);
    lv_label_set_long_mode(g_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_name, w);
    lv_obj_set_style_text_align(g_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_name, LV_ALIGN_BOTTOM_MID, 0, 0);
    g_hint = lv_label_create(tile);
    lv_obj_set_style_text_font(g_hint, SF(14), 0);
    lv_obj_set_style_text_color(g_hint, lv_color_hex(0xc4dcee), 0);
    lv_label_set_long_mode(g_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_hint, w);
    lv_obj_set_style_text_align(g_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(g_hint, tr("Geen WiFi-netwerk bekend.\nLang indrukken om in te stellen.",
                                 "No WiFi network known.\nLong press to set one."));
    lv_obj_center(g_hint);
    lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, on_tap, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(tile, on_hold, LV_EVENT_LONG_PRESSED, NULL);
    wq_tile_update();
}

void wq_tile_update(void) {
    if (!g_qr) return;
    char qr[300], ssid[96];
    int ok = wifiqr_payload(qr, sizeof qr, ssid, sizeof ssid, NULL, 0);
    if (!ok) qr[0] = 0;
    if (!strcmp(qr, g_last)) return;
    snprintf(g_last, sizeof g_last, "%s", qr);
    if (ok) {
        lv_qrcode_update(g_qr, qr, (uint32_t)strlen(qr));
        hy_fold_text(ssid);
        lv_label_set_text(g_name, ssid);
        lv_obj_clear_flag(g_qr_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_qr_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------ an Integratie tile ("local:wifiqr") */
/* Slot tiles (home Integratie tiles, pages 2 and 3) bound to "local:wifiqr"
 * get a QR box over their value labels. */
#define WQ_SLOTS 40
static struct { lv_obj_t * tile, * box, * qr, * mn, * sb; int sb_was_hidden; char last[300]; } g_sl[WQ_SLOTS];

static void on_slot_box_delete(lv_event_t * e) {
    lv_obj_t * b = lv_event_get_target(e);
    for (int i = 0; i < WQ_SLOTS; i++) if (g_sl[i].box == b) memset(&g_sl[i], 0, sizeof g_sl[i]);
}

void wq_slot_render(lv_obj_t * title, lv_obj_t * mn, lv_obj_t * sb) {
    lv_obj_t * tile = mn ? lv_obj_get_parent(mn) : NULL;
    if (!tile) return;
    int k = -1, fr = -1;
    for (int i = 0; i < WQ_SLOTS; i++) {
        if (g_sl[i].tile == tile && g_sl[i].box) { k = i; break; }
        if (!g_sl[i].box && fr < 0) fr = i;
    }
    if (k < 0) {
        if (fr < 0) return;
        k = fr;
        lv_obj_update_layout(tile);
        int w = lv_obj_get_content_width(tile), h = lv_obj_get_content_height(tile);
        int top = 14 + lv_font_get_line_height(SF(18)) + 4;   /* make_tile's title */
        int sz = h - top - 2;
        if (sz > w - 4) sz = w - 4;
        if (sz < 56) { sz = h - 4; top = 2; }                /* a low tile: QR over the title */
        if (sz < 40) sz = 40;
        g_sl[k].tile = tile;
        g_sl[k].mn = mn;
        g_sl[k].sb = sb;
        g_sl[k].sb_was_hidden = sb ? lv_obj_has_flag(sb, LV_OBJ_FLAG_HIDDEN) : 1;
        g_sl[k].last[0] = 1; g_sl[k].last[1] = 0;
        lv_obj_t * b = lv_obj_create(tile);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, sz, sz);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, top);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, on_slot_box_delete, LV_EVENT_DELETE, NULL);
        g_sl[k].box = b;
        g_sl[k].qr = lv_qrcode_create(b, sz - 10, lv_color_hex(0x000000), lv_color_hex(0xffffff));
        lv_obj_center(g_sl[k].qr);
        lv_obj_clear_flag(g_sl[k].qr, LV_OBJ_FLAG_CLICKABLE);
    }
    char qr[300], ssid[96];
    int ok = wifiqr_payload(qr, sizeof qr, ssid, sizeof ssid, NULL, 0);
    if (title) lv_label_set_text(title, "WiFi");
    if (sb) lv_obj_add_flag(sb, LV_OBJ_FLAG_HIDDEN);
    if (!ok) qr[0] = 0;
    if (!strcmp(qr, g_sl[k].last)) return;
    snprintf(g_sl[k].last, sizeof g_sl[k].last, "%s", qr);
    if (ok) {
        lv_qrcode_update(g_sl[k].qr, qr, (uint32_t)strlen(qr));
        lv_obj_clear_flag(g_sl[k].box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_sl[k].box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(mn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(mn, tr("Geen WiFi-netwerk", "No WiFi network"));
    }
}

void wq_slot_clear(lv_obj_t * mn) {
    lv_obj_t * tile = mn ? lv_obj_get_parent(mn) : NULL;
    if (!tile) return;
    for (int i = 0; i < WQ_SLOTS; i++) {
        if (g_sl[i].tile != tile || !g_sl[i].box) continue;
        lv_obj_clear_flag(g_sl[i].mn, LV_OBJ_FLAG_HIDDEN);
        if (g_sl[i].sb && !g_sl[i].sb_was_hidden) lv_obj_clear_flag(g_sl[i].sb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_del(g_sl[i].box);                   /* the DELETE event clears the entry */
        return;
    }
}

/* ------------------------------------------------------ full-screen view */
static lv_obj_t * g_big;
static lv_timer_t * g_big_timer;

void wq_close_big(void) {
    if (g_big_timer) { lv_timer_del(g_big_timer); g_big_timer = NULL; }
    if (g_big) { lv_obj_del_async(g_big); g_big = NULL; }
}
static void on_big_close(lv_event_t * e) { (void)e; wq_close_big(); }
static void on_big_timeout(lv_timer_t * t) {
    (void)t;
    g_big_timer = NULL;        /* a repeat-count-1 timer deletes itself after this */
    wq_close_big();
}
static void on_big_settings(lv_event_t * e) {
    lv_event_stop_bubbling(e);
    wq_close_big();
    screen_settings_open_wifiqr_modal();
}

void wq_show_big(void) {
    wq_close_big();
    char qr[300], ssid[96], pass[96];
    int ok = wifiqr_payload(qr, sizeof qr, ssid, sizeof ssid, pass, sizeof pass);
    g_big = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_big);
    lv_obj_set_size(g_big, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(g_big, lv_color_hex(0x0b1422), 0);
    lv_obj_set_style_bg_opa(g_big, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_big, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_big, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_big, on_big_close, LV_EVENT_CLICKED, NULL);
    int sz = DISP_VER - SY(60);
    lv_obj_t * box = lv_obj_create(g_big);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, sz, sz);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_align(box, LV_ALIGN_LEFT_MID, SX(40), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    int tx = SX(40) + sz + SX(36), tw = LV_HOR_RES - tx - SX(24);
    if (ok && getenv("WIFIQR_DEBUG")) fprintf(stderr, "[wifiqr] payload: %s\n", qr);   /* sim / tests only */
    if (ok) {
        lv_obj_t * q = lv_qrcode_create(box, sz - SY(30), lv_color_hex(0x000000), lv_color_hex(0xffffff));
        lv_qrcode_update(q, qr, (uint32_t)strlen(qr));
        lv_obj_center(q);
    }
    char ns[96], ps[96];
    snprintf(ns, sizeof ns, "%s", ssid);
    snprintf(ps, sizeof ps, "%s", pass);
    hy_fold_text(ns);
    hy_fold_text(ps);
    /* no password here: the QR code is there so visitors need not see it */
    struct { const char * cap; const char * val; } rows[2] = {
        { tr("Scan met je telefoon", "Scan with your phone"), "" },
        { tr("Netwerk", "Network"), ok ? ns : tr("geen netwerk ingesteld", "no network set") } };
    (void)ps;
    int y = SY(90);
    for (int i = 0; i < 2; i++) {
        lv_obj_t * c = lv_label_create(g_big);
        lv_obj_set_style_text_font(c, SF(i == 0 ? 28 : 18), 0);
        lv_obj_set_style_text_color(c, lv_color_hex(i == 0 ? 0xffffff : 0x8fb3cc), 0);
        lv_label_set_text(c, rows[i].cap);
        lv_obj_set_width(c, tw);
        lv_obj_set_pos(c, tx, y);
        y += lv_font_get_line_height(SF(i == 0 ? 28 : 18)) + SY(i == 0 ? 30 : 4);
        if (i == 0) continue;
        lv_obj_t * v = lv_label_create(g_big);
        lv_obj_set_style_text_font(v, SF(28), 0);
        lv_obj_set_style_text_color(v, lv_color_hex(0xffffff), 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(v, tw);
        lv_label_set_text(v, rows[i].val);
        lv_obj_set_pos(v, tx, y);
        lv_obj_update_layout(v);
        y += lv_obj_get_height(v) + SY(26);
    }
    lv_obj_t * b = lv_btn_create(g_big);
    lv_obj_set_size(b, SX(180), SY(52));
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -SX(24), -SY(24));
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2e4e6e), 0);
    lv_obj_add_event_cb(b, on_big_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(b);
    lv_label_set_text_fmt(bl, LV_SYMBOL_SETTINGS "  %s", tr("Instellen", "Settings"));
    lv_obj_center(bl);
    lv_obj_t * hint = lv_label_create(g_big);
    lv_obj_set_style_text_font(hint, SF(14), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6f8fa6), 0);
    lv_label_set_text(hint, tr("Tik om te sluiten", "Tap to close"));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, tx, -SY(40));
    g_big_timer = lv_timer_create(on_big_timeout, 120000, NULL);   /* guests have scanned by then */
    lv_timer_set_repeat_count(g_big_timer, 1);
}
