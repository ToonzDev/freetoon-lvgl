/* See pwa_server.h.
 *
 * One detached thread per accepted connection so a long-lived SSE stream
 * doesn't block the accept loop or other clients. Handlers are stateless
 * apart from reads of toon_state / ha_state.
 *
 * The only frontend served here is the WASM slave UI (the full LVGL UI
 * compiled to WebAssembly) under /ui/. Bare "/" 302-redirects to /ui/. There
 * is deliberately no separate HTML settings/about page or simple JS app — the
 * WASM UI is the single interface, plus the /api/ endpoints it drives.
 *
 * Routes:
 *   GET  /                  →  302 /ui/   (WASM slave UI)
 *   GET  /ui/…              →  static WASM bundle (index.html/js/wasm)
 *   GET  /api/state         →  one-shot toon_state JSON (legacy / curl)
 *   GET  /api/state/stream  →  SSE: emits on state change + 10s heartbeat
 *   POST /api/setpoint      →  body {"value": "18.50"} → roomSetpoint write
 *   POST /api/program       →  body {"state": -1 (Manual) | 0..3
 *                                     (Comfort/Home/Sleep/Away)}
 *   POST /api/curtain       →  body {"action": "open|close|stop"} via HA
 *
 * All static files live under PWA_ROOT (/mnt/data/pwa/). */
#define _GNU_SOURCE      /* strcasestr */
#include "pwa_server.h"
#include "boxtalk.h"
#include "homeassistant.h"
#include "schedule.h"
#include "settings.h"
#include "weather.h"
#include "wastecollection.h"
#include "homewizard.h"
#include "meteradapter.h"
#include "ventilation.h"
#include "inbox.h"
#include "calendar.h"
#include "anim_overlay.h"
#include "domoticz.h"
#include "news.h"
#include "tile_slots.h"
#include "layout.h"
#include "packages.h"
#include "screen_zwave.h"
#include "notify.h"
#include "update_check.h"
#include "canonical_state.h"
#include "otgw.h"
#include "ha_mqtt.h"
#include "mqtt_client.h"
#include "buienradar_rain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>          /* isalnum — install id sanity check */
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "ketellog.h"
#include "homey.h"
#include "solar_app.h"
#include "app_settings.h"
#include "energy.h"
#include "tile_lights.h"
#include "switch_schedule.h"
#include "boiler_remote.h"

#define PWA_PORT  10081
#define PWA_ROOT  "/mnt/data/pwa"

static int sock_send_all(int fd, const char * buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t k = send(fd, buf + off, n - off, MSG_NOSIGNAL);
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return 0;
}

static int send_status(int fd, int code, const char * status, const char * body) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
        code, status, strlen(body));
    if (sock_send_all(fd, hdr, n) < 0) return -1;
    return sock_send_all(fd, body, strlen(body));
}

/* The remote-control page (remote/index.html), compiled in by the Makefile.
 * Builds without the generated header (the WASM bundle) get a stub. */
#if defined(__has_include)
#  if __has_include("remote_page.h")
#    include "remote_page.h"
#    define HAVE_REMOTE_PAGE 1
#  endif
#endif
#ifndef HAVE_REMOTE_PAGE
static const char REMOTE_PAGE[] =
    "<!doctype html><meta charset=utf-8><title>FreeToon</title>"
    "<p>The remote page is not built into this binary.";
#endif

static int handle_remote_page(int fd) {
    size_t len = sizeof(REMOTE_PAGE) - 1;
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %u\r\nCache-Control: no-cache\r\n"
        "X-Frame-Options: DENY\r\nConnection: close\r\n\r\n", (unsigned)len);
    if (sock_send_all(fd, hdr, n) < 0) return -1;
    return sock_send_all(fd, REMOTE_PAGE, len);
}

/* Other static files: none (the old /mnt/data/pwa web UI was removed). */
static int serve_static(int fd, const char * path) {
    (void)path;
    return send_status(fd, 404, "Not Found", "Not Found");
}

/* Is "<prefix><id>" on one of the Verlichting tile's places? */
static int on_tile(char refs[TL_SLOTS][TL_REF_LEN + 4], const char * prefix, const char * id) {
    size_t pl = strlen(prefix);
    for (int k = 0; k < TL_SLOTS; k++)
        if (!strncmp(refs[k], prefix, pl) && !strcmp(refs[k] + pl, id)) return 1;
    return 0;
}

/* Pure-function snapshot of the live state into JSON. Reads volatile shared
 * structs once; safe to call from any thread (the writers update atomically
 * field-by-field — readers tolerate a briefly inconsistent frame). */
/* Append-helper for the JSON builders below: copies `src` into `dst` (cap
 * `dsz`) replacing JSON-troublesome chars (`"`, `\`, control bytes) with a
 * space. Cheap; good enough for free-text fields like family locations and
 * Itho's "last_source" labels. */
static void json_strcpy(char * dst, const char * src, size_t dsz) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < dsz; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c == '"' || c == '\\' || c < 0x20) ? ' ' : (char)c;
    }
    dst[i] = 0;
}

/* Emit one weather_hour_t / weather_day_t object. Field names mirror the
 * struct member names (kept short — these are inside arrays of many objects). */
static int emit_wx_hour(char * p, char * end, const weather_hour_t * h) {
    char ic[16], dir[16];
    json_strcpy(ic, h->icon, sizeof ic);
    json_strcpy(dir, h->wind_dir, sizeof dir);
    return snprintf(p, end - p,
        "{\"label\":\"%s\",\"temp\":%.2f,\"bft\":%d,\"dir\":\"%s\",\"icon\":\"%s\"}",
        h->label, (double)h->temperature, h->wind_bft, dir, ic);
}
static int emit_wx_day(char * p, char * end, const weather_day_t * d) {
    char ic[16], dir[16];
    json_strcpy(ic, d->icon, sizeof ic);
    json_strcpy(dir, d->wind_dir, sizeof dir);
    return snprintf(p, end - p,
        "{\"day\":\"%s\",\"min\":%.1f,\"max\":%.1f,\"bft\":%d,\"dir\":\"%s\","
        "\"icon\":\"%s\",\"rain\":%d}",
        d->day, (double)d->min_temp, (double)d->max_temp, d->wind_bft, dir,
        ic, d->rain_chance);
}

/* Pure-function snapshot of the live state into JSON. Reads volatile shared
 * structs once; safe to call from any thread (the writers update atomically
 * field-by-field — readers tolerate a briefly inconsistent frame).
 *
 * Scalar fields come from state_fields.def via X-macros — adding a new scalar
 * to ANY state struct is a one-line change in that .def, no edits here.
 * Array fields (forecast hours/days, waste pickups, …) are appended below by
 * dedicated marshallers because their per-element schemas don't fit the
 * scalar X-macro shape. */
static int render_state_json(char * body, size_t sz) {
    char * p = body;
    char * end = body + sz;
    /* Scratch for JSON-escaped strings: largest STATE_STR field is the
     * Buienradar weather-report body at weather_state.weatherreport_text
     * (sizeof 2400), so we size for it with headroom. */
    char esc[3072];
    (void)esc;                              /* may go unused if no STATE_STR fires */

    if (p < end) *p++ = '{';

    /* ---- scalars: expand state_fields.def with macros that emit JSON ---- */
    #define STATE_NUM(g, f, j, fmt)  do { if (p < end) p += snprintf(p, end-p, \
        "\"" #j "\":" fmt ",", (double)g.f); } while (0);
    #define STATE_INT(g, f, j)       do { if (p < end) p += snprintf(p, end-p, \
        "\"" #j "\":%d,", (int)g.f); } while (0);
    #define STATE_STR(g, f, j)       do { json_strcpy(esc, (const char *)g.f, sizeof esc); \
        if (p < end) p += snprintf(p, end-p, "\"" #j "\":\"%s\",", esc); } while (0);
    #include "state_fields.def"
    #undef STATE_NUM
    #undef STATE_INT
    #undef STATE_STR

    /* ---- UI language (0 = NL, 1 = EN) and light tiles, for the remote page.
     * Not in state_fields.def: client_link applies every field there to the
     * client Toon, which must keep its own language and theme. */
    if (p < end) p += snprintf(p, end-p, "\"ui_language\":%d,\"ui_light\":%d,",
        settings.language, settings.home_light_mode);

    /* ---- home-screen layout, so the remote page mirrors it live: the visible
     * tiles of the custom layout (type, page, grid cell, span; a slot tile also
     * carries its bound integration id). Without a custom layout the home
     * screen uses its built-in arrangement, which the page knows itself. */
    /* What the four right-column tiles (Energie / Familie / Ventilatie /
     * Water = TILE_SLOT_ENERGY..WATER) are assigned to: an integration id or
     * "local:agenda" etc.; "" = the built-in function. */
    {
        char b[4][INTEG_ID_MAX];
        for (int i = 0; i < 4; i++) {
            const char * s = tile_slots_binding(i);
            json_strcpy(b[i], s ? s : "", sizeof b[i]);
        }
        if (p < end) p += snprintf(p, end-p, "\"tile_bind\":[\"%s\",\"%s\",\"%s\",\"%s\"],",
            b[0], b[1], b[2], b[3]);
    }
    if (p < end) p += snprintf(p, end-p, "\"layout_custom\":%d,\"layout\":[",
        settings.custom_layout_enabled ? 1 : 0);
    if (settings.custom_layout_enabled) {
        int first = 1;
        for (int i = 0; i < g_layout.count && i < LAYOUT_MAX_TILES; i++) {
            const layout_tile_t * L = &g_layout.tiles[i];
            if (!L->visible) continue;
            char id[INTEG_ID_MAX] = "";
            if (L->type == LT_SLOT) {
                const char * b = tile_slots_binding(L->slot);
                if (b) json_strcpy(id, b, sizeof id);
            }
            if (p < end) p += snprintf(p, end-p,
                "%s{\"t\":%d,\"p\":%d,\"c\":%d,\"r\":%d,\"w\":%d,\"h\":%d,\"id\":\"%s\"}",
                first ? "" : ",", L->type, L->page, L->col, L->row, L->w, L->h, id);
            first = 0;
        }
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- weather forecast: hourly array ---- */
    if (p < end) p += snprintf(p, end-p, "\"wx_hours\":[");
    for (int i = 0; i < weather_state.hour_count && i < WEATHER_FORECAST_HOURS; i++) {
        if (i && p < end) *p++ = ',';
        if (p < end) p += emit_wx_hour(p, end, &weather_state.hours[i]);
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- weather forecast: daily array ---- */
    if (p < end) p += snprintf(p, end-p, "\"wx_days\":[");
    for (int i = 0; i < weather_state.day_count && i < WEATHER_FORECAST_DAYS; i++) {
        if (i && p < end) *p++ = ',';
        if (p < end) p += emit_wx_day(p, end, &weather_state.days[i]);
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- next-up waste pickups: the few entries the home tile + dim use ---- */
    if (p < end) p += snprintf(p, end-p, "\"wt_next\":[");
    {
        waste_pickup_t wp[3];
        int nw = waste_next_n_windowed(7, wp, 3);     /* up to 3 within 7 days */
        for (int i = 0; i < nw; i++) {
            char lbl[64];
            json_strcpy(lbl, wp[i].labels, sizeof lbl);
            if (i && p < end) *p++ = ',';
            if (p < end) p += snprintf(p, end-p,
                "{\"date\":\"%s\",\"labels\":\"%s\",\"days\":%ld}",
                wp[i].date, lbl, waste_days_until(wp[i].date));
        }
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- waste connectivity (covers the dim/home tile's "configure waste"
     * fallback). The waste items array beyond next-up isn't shipped — the
     * UI uses waste_next_pickup / waste_next_n_windowed everywhere. */
    if (p < end) p += snprintf(p, end-p, "\"waste_connected\":%d,",
        waste_state.connected);

    /* ---- HA lights (configured set; ha_state-coupled live on/brightness) */
    if (p < end) p += snprintf(p, end-p, "\"ha_lights\":[");
    for (int i = 0; i < ha_light_count && i < HA_LIGHT_COUNT; i++) {
        const ha_light_t * L = &ha_lights[i];
        char name[40], area[40], eid[64];
        json_strcpy(name, L->name,      sizeof name);
        json_strcpy(area, L->area,      sizeof area);
        json_strcpy(eid,  L->entity_id, sizeof eid);
        if (i && p < end) *p++ = ',';
        if (p < end) p += snprintf(p, end-p,
            "{\"id\":\"%s\",\"name\":\"%s\",\"area\":\"%s\","
            "\"on\":%d,\"av\":%d,\"br\":%d}",
            eid, name, area, L->on, L->available, L->brightness);
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- HA devices from the device manager (Settings > Home Assistant >
     * Apparaten beheren): the lights and switches, for the remote page's
     * Lampen tile (shown + switchable there; POST /api/ha/toggle). */
    /* What the Verlichting tile shows (tile_lights.h): those are "pin" below,
     * so the remote page lists them first. */
    char tile_ref[TL_SLOTS][TL_REF_LEN + 4];
    for (int k = 0; k < TL_SLOTS; k++) { tl_src_t s = tl_resolve(k); tl_ref(&s, tile_ref[k], sizeof tile_ref[k]); }
    if (p < end) p += snprintf(p, end-p, "\"ha_devs\":[");
    if (settings.enable_ha) {
        int first = 1;
        for (int i = 0; i < ha_device_count && i < HA_DEVICE_MAX; i++) {
            const ha_device_t * D = &ha_devices[i];
            if (D->type != HADEV_LIGHT && D->type != HADEV_SWITCH) continue;
            char name[72], eid[136];
            json_strcpy(name, D->name,      sizeof name);
            json_strcpy(eid,  D->entity_id, sizeof eid);
            if (!first && p < end) *p++ = ',';
            first = 0;
            if (p < end) p += snprintf(p, end-p,
                "{\"id\":\"%s\",\"name\":\"%s\",\"on\":%d,\"av\":%d,\"pin\":%d}",
                eid, name, D->on, D->available,
                D->pin_home || on_tile(tile_ref, "ha:", D->entity_id));
        }
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- Homey on/off devices (#28) for the Lampen tile: the two pinned to
     * the Verlichting tile first, at most 40 (the page shows 12). Switched
     * with POST /api/homey/toggle. Locks and garage doors are never listed. */
    if (p < end) p += snprintf(p, end-p, "\"hy_devs\":[");
    if (settings.enable_homey) {
        int first = 1, count = 0;
        homey_lock();
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < homey_state.ndev && count < 40; i++) {
                const hy_dev_t * D = &homey_state.dev[i];
                if (!homey_switchable(D)) continue;
                int pin = on_tile(tile_ref, "hy:", D->id);
                if (pin != (pass == 0)) continue;
                const hy_cap_t * c = hy_dev_cap(D, "onoff");
                char name[HY_NAME_LEN];
                json_strcpy(name, D->name, sizeof name);
                if (!first && p < end) *p++ = ',';
                first = 0;
                count++;
                if (p < end) p += snprintf(p, end-p,
                    "{\"id\":\"%s\",\"name\":\"%s\",\"on\":%d,\"av\":%d,\"pin\":%d}",
                    D->id, name, c && c->num != 0, D->available, pin);
            }
        homey_unlock();
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- schedule entries (target_state + day/hh:mm window) */
    if (p < end) p += snprintf(p, end-p, "\"schedule\":[");
    for (int i = 0; i < schedule_count && i < SCHEDULE_MAX; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        if (i && p < end) *p++ = ',';
        if (p < end) p += snprintf(p, end-p,
            "{\"s\":%d,\"sd\":%d,\"sh\":%d,\"sm\":%d,"
            "\"ed\":%d,\"eh\":%d,\"em\":%d}",
            e->target_state, e->start_day, e->start_hour, e->start_min,
            e->end_day, e->end_hour, e->end_min);
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- inbox messages (unread flag + text) */
    if (p < end) p += snprintf(p, end-p, "\"inbox\":[");
    for (int i = 0; i < inbox_count && i < INBOX_MAX; i++) {
        const inbox_msg_t * m = &inbox_msgs[i];
        char text[INBOX_TEXT_LEN], type[INBOX_TYPE_LEN], sub[INBOX_TYPE_LEN];
        json_strcpy(text, m->text,     sizeof text);
        json_strcpy(type, m->type,     sizeof type);
        json_strcpy(sub,  m->sub_type, sizeof sub);
        if (i && p < end) *p++ = ',';
        if (p < end) p += snprintf(p, end-p,
            "{\"uuid\":\"%s\",\"type\":\"%s\",\"sub\":\"%s\","
            "\"text\":\"%s\",\"ts\":%ld,\"read\":%d}",
            m->uuid, type, sub, text, m->creation_date, m->read);
    }
    if (p < end) p += snprintf(p, end-p, "],\"inbox_unread\":%d,", inbox_unread);

    /* ---- calendar events (next-up) */
    if (p < end) p += snprintf(p, end-p, "\"cal\":[");
    /* PWA connection thread: calendar_state is written on the UI thread. */
    calendar_lock();
    for (int i = 0; i < calendar_state.count; i++) {
        const calendar_event_t * e = &calendar_state.ev[i];
        char sum[96];
        json_strcpy(sum, e->summary, sizeof sum);
        if (i && p < end) *p++ = ',';
        if (p < end) p += snprintf(p, end-p,
            "{\"date\":\"%s\",\"time\":\"%s\",\"sum\":\"%s\"}",
            e->date, e->time, sum);
    }
    int cal_conn = calendar_state.connected;
    calendar_unlock();
    if (p < end) p += snprintf(p, end-p, "],\"cal_connected\":%d,", cal_conn);

    /* ---- news ticker (title + link + body + feed index)
     * Body is the RSS <description> (HTML-stripped, up to ~900 chars). Without
     * it the article-card view shows "geen samenvatting beschikbaar" on the
     * WASM slave. Each item adds up to ~1.5 KB, and NEWS_MAX_ITEMS allows 96,
     * which would overrun the 64 KB frame — so stop adding items while 16 KB
     * is still free for the sections that follow. */
    if (p < end) p += snprintf(p, end-p, "\"news\":[");
    for (int i = 0, n = news_count(); i < n; i++) {
        if (p >= end || (size_t)(end - p) < 16384) break;
        char title[NEWS_TITLE_MAX], link[NEWS_LINK_MAX], body[1024];
        char title_esc[NEWS_TITLE_MAX], link_esc[NEWS_LINK_MAX], body_esc[1024];
        if (news_item(i, title, sizeof title, link, sizeof link) != 0) continue;
        body[0] = 0;
        news_body(i, body, sizeof body);
        json_strcpy(title_esc, title, sizeof title_esc);
        json_strcpy(link_esc,  link,  sizeof link_esc);
        json_strcpy(body_esc,  body,  sizeof body_esc);
        if (i && p < end) *p++ = ',';
        if (p < end) p += snprintf(p, end-p,
            "{\"t\":\"%s\",\"u\":\"%s\",\"b\":\"%s\",\"f\":%d}",
            title_esc, link_esc, body_esc, news_item_feed(i));
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- installed marketplace integrations (registry + live values) ----
     * Sends each integration's manifest fields + latest_value / latest_subtitle
     * so the WASM slave can mirror the registry without scanning a local
     * /mnt/data/integrations. New install on the master => next SSE frame
     * carries it => WASM client renders the new tile, no rebuild. */
    if (p < end) p += snprintf(p, end-p, "\"integrations\":[");
    for (int i = 0, ni = tile_slots_integration_count(); i < ni; i++) {
        const integration_meta_t * M = tile_slots_integration_at(i);
        if (!M) continue;
        char id[INTEG_ID_MAX], name[INTEG_NAME_MAX], svc[INTEG_SERVICE_MAX];
        char ttitle[INTEG_NAME_MAX], tcolor[16], ticon[24];
        char vfield[INTEG_FIELD_MAX], vunit[INTEG_UNIT_MAX];
        char sfield[INTEG_FIELD_MAX], sunit[INTEG_UNIT_MAX];
        char afield[INTEG_FIELD_MAX];
        char lval[INTEG_VALUE_MAX], lsub[INTEG_VALUE_MAX], lalrt[INTEG_VALUE_MAX];
        json_strcpy(id,     M->id,             sizeof id);
        json_strcpy(name,   M->name,           sizeof name);
        json_strcpy(svc,    M->service_id,     sizeof svc);
        json_strcpy(ttitle, M->tile_title,     sizeof ttitle);
        json_strcpy(tcolor, M->tile_color,     sizeof tcolor);
        json_strcpy(ticon,  M->tile_icon,      sizeof ticon);
        json_strcpy(vfield, M->value_field,    sizeof vfield);
        json_strcpy(vunit,  M->value_unit,     sizeof vunit);
        json_strcpy(sfield, M->subtitle_field, sizeof sfield);
        json_strcpy(sunit,  M->subtitle_unit,  sizeof sunit);
        json_strcpy(afield, M->alert_field,    sizeof afield);
        json_strcpy(lval,   (const char *)M->latest_value,    sizeof lval);
        json_strcpy(lsub,   (const char *)M->latest_subtitle, sizeof lsub);
        json_strcpy(lalrt,  (const char *)M->latest_alert,    sizeof lalrt);
        if (i && p < end) *p++ = ',';
        if (p < end) p += snprintf(p, end-p,
            "{\"id\":\"%s\",\"name\":\"%s\",\"svc\":\"%s\","
            "\"ttitle\":\"%s\",\"tcolor\":\"%s\",\"ticon\":\"%s\","
            "\"vfield\":\"%s\",\"vunit\":\"%s\","
            "\"sfield\":\"%s\",\"sunit\":\"%s\","
            "\"afield\":\"%s\","
            "\"lval\":\"%s\",\"lsub\":\"%s\",\"lalrt\":\"%s\",\"lts\":%ld}",
            id, name, svc, ttitle, tcolor, ticon,
            vfield, vunit, sfield, sunit, afield,
            lval, lsub, lalrt, M->latest_epoch);
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- packages: delivery banner queue ----
     * The home + dim screens render the head of this queue as a banner.
     * Mirroring it makes the same notifications pop on every browser. */
    if (p < end) p += snprintf(p, end-p, "\"pkg_banners\":[");
    {
        int nb = packages_banner_count();
        packages_banner_t b;
        for (int i = 0; i < nb; i++) {
            if (!packages_banner_at(i, &b)) break;
            char key[100], title[80], msg[180], url[280];
            json_strcpy(key,   b.key,   sizeof key);
            json_strcpy(title, b.title, sizeof title);
            json_strcpy(msg,   b.msg,   sizeof msg);
            json_strcpy(url,   b.url,   sizeof url);
            if (i && p < end) *p++ = ',';
            if (p < end) p += snprintf(p, end-p,
                "{\"key\":\"%s\",\"title\":\"%s\",\"msg\":\"%s\",\"url\":\"%s\"}",
                key, title, msg, url);
        }
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- Z-Wave controller device list (admin screen) ---- */
    if (p < end) p += snprintf(p, end-p, "\"zwave\":[");
    {
        int nz = zwave_dev_count();
        zwave_dev_t z;
        for (int i = 0; i < nz; i++) {
            if (!zwave_dev_at(i, &z)) break;
            char uuid[48], name[80], type[60];
            json_strcpy(uuid, z.uuid, sizeof uuid);
            json_strcpy(name, z.name, sizeof name);
            json_strcpy(type, z.type, sizeof type);
            if (i && p < end) *p++ = ',';
            if (p < end) p += snprintf(p, end-p,
                "{\"uuid\":\"%s\",\"name\":\"%s\",\"type\":\"%s\","
                "\"node\":%d,\"sw\":%d,\"st\":%d}",
                uuid, name, type, z.node_id, z.is_switch, z.state);
        }
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* ---- domoticz devices (current on/level state). Only while Domoticz
     * is in use: the list survives switching Domoticz off at runtime, and the
     * remote page would keep showing those devices. */
    int dz_n = (settings.enable_domoticz || settings.client_mode) ? domoticz_state.count : 0;
    if (p < end) p += snprintf(p, end-p, "\"dz\":[");
    for (int i = 0; i < dz_n && i < DOMOTICZ_MAX_DEV; i++) {
        const domoticz_dev_t * d = &domoticz_state.dev[i];
        char name[40];
        json_strcpy(name, d->name, sizeof name);
        if (i && p < end) *p++ = ',';
        if (p < end) p += snprintf(p, end-p,
            "{\"idx\":%d,\"kind\":%d,\"name\":\"%s\",\"on\":%d,\"level\":%d}",
            d->idx, d->kind, name, d->on, d->level);
    }
    if (p < end) p += snprintf(p, end-p, "],\"dz_connected\":%d,",
        domoticz_state.connected);

    /* ---- HomeWizard Energy Sockets (#37), for client Toons */
    if (p < end) p += snprintf(p, end-p, "\"hws\":[");
    {
        int first = 1;
        for (int i = 0; i < HW_SOCKET_MAX; i++) {
            if (!settings.hw_socket_host[i][0]) continue;
            char name[48];
            json_strcpy(name, homewizard_socket_name(i), sizeof name);
            if (!first && p < end) *p++ = ',';
            first = 0;
            if (p < end) p += snprintf(p, end-p,
                "{\"i\":%d,\"n\":\"%s\",\"c\":%d,\"on\":%d,\"w\":%.0f}",
                i, name, hw_sockets[i].connected, hw_sockets[i].on, (double)hw_sockets[i].power_w);
        }
    }
    if (p < end) p += snprintf(p, end-p, "],");

    /* snprintf returns the length it *wanted*, so after a truncated write p
     * points past `end`. Then p[-1] and the returned length would both be out
     * of bounds (the SSE path memcpy'd n+1 bytes into a buffer of sz) —
     * report failure instead; callers skip the frame. */
    if (p >= end - 1) {
        fprintf(stderr, "[pwa] state JSON exceeded %zu bytes — frame dropped\n", sz);
        body[0] = 0;
        return -1;
    }
    /* Replace the trailing comma with a closing brace. */
    if (p > body + 1 && p[-1] == ',') p[-1] = '}';
    else *p++ = '}';
    *p = 0;
    return (int)(p - body);
}

static int handle_state(int fd) {
    char * body = malloc(65536); /* full state incl. lights/schedule/inbox/calendar/news/dz arrays */
    if (!body) return -1;
    int n = render_state_json(body, 65536);
    if (n < 0) { free(body); return send_status(fd, 500, "Server Error", "{\"err\":\"state too large\"}"); }
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n", n);
    int rc = (sock_send_all(fd, hdr, hn) < 0) ? -1 : sock_send_all(fd, body, n);
    free(body);
    return rc;
}

/* SSE stream — emits a `data: {…}` event whenever the JSON snapshot changes,
 * plus a `: keepalive\n\n` comment every ~10s so proxies don't drop the conn.
 * Loops until the client disconnects (sock_send_all returns -1). */
/* Proxy /api/tile?z=Z&x=X&y=Y to OpenStreetMap. The WASM client's Life360
 * map needs PNG tiles, but a browser fetch direct to tile.openstreetmap.org
 * gets blocked by cross-origin / referrer-policy / OSM tile-usage rules. The
 * master fetches them itself (server-to-server, OSM is happy with a sane
 * User-Agent) and serves them at /api/tile, same-origin to the WASM client. */
static int handle_tile_proxy(int fd, const char * query) {
    int z = -1, x = -1, y = -1;
    if (query) {
        const char * p;
        if ((p = strstr(query, "z=")))  z = atoi(p + 2);
        if ((p = strstr(query, "x=")))  x = atoi(p + 2);
        if ((p = strstr(query, "y=")))  y = atoi(p + 2);
    }
    if (z < 0 || z > 19 || x < 0 || y < 0)
        return send_status(fd, 400, "Bad Request", "{\"err\":\"need z/x/y\"}");

    /* Use curl: ships with the Toon firmware and has TLS already set up.
     * --tlsv1.2 because some Toon-firmware curls negotiate weaker defaults.
     * -A identifies us per OSM's tile-usage policy. */
    char cmd[400];
    char out[64];
    snprintf(out, sizeof out, "/tmp/_tile_%d_%d_%d.png", z, x, y);
    snprintf(cmd, sizeof cmd,
        "curl -fsSL -m 12 -A 'freetoon-master/1.0 (+https://github.com/ToonSoftwareCollective/freetoon-lvgl)' "
        "-o '%s' 'https://tile.openstreetmap.org/%d/%d/%d.png'",
        out, z, x, y);
    int rc = system(cmd);
    if (rc != 0)
        return send_status(fd, 502, "Bad Gateway", "{\"err\":\"osm fetch\"}");

    struct stat st;
    if (stat(out, &st) != 0)
        return send_status(fd, 502, "Bad Gateway", "{\"err\":\"no file\"}");
    FILE * f = fopen(out, "rb");
    if (!f) return send_status(fd, 502, "Bad Gateway", "{\"err\":\"fopen\"}");
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %lld\r\n"
        "Cache-Control: public, max-age=86400\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n",
        (long long)st.st_size);
    if (sock_send_all(fd, hdr, hn) < 0) { fclose(f); return -1; }
    char buf[4096]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        if (sock_send_all(fd, buf, r) < 0) { fclose(f); return -1; }
    fclose(f);
    unlink(out);  /* tmp tile — pwa_server doesn't keep a cache itself */
    return 0;
}

/* Proxy /api/rrd?<qs> to local hcb_rrd's /hcb_rrd?<qs>. Lets the WASM client's
 * stats screen pull historical RRD archives — energy/water/gas/temps — without
 * needing localhost/raw sockets. The reply body (date-keyed JSON) is forwarded
 * unchanged so stats.c's existing parser works on both sides. */
static int handle_rrd_proxy(int fd, const char * query) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return send_status(fd, 502, "Bad Gateway", "{\"err\":\"socket\"}");
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(10080);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(sock, (struct sockaddr *)&a, sizeof a) != 0) {
        close(sock);
        return send_status(fd, 502, "Bad Gateway", "{\"err\":\"hcb_rrd connect\"}");
    }
    char req[1024];
    int n = snprintf(req, sizeof req,
        "GET /hcb_rrd?%s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        query ? query : "");
    if (send(sock, req, n, MSG_NOSIGNAL) != n) { close(sock); return -1; }
    /* Per-request buffer: this handler runs on every connection thread, and a
     * shared static buffer let two parallel stats fetches corrupt each other
     * (strstr could then find the *other* request's header end beyond `total`
     * and body_len would wrap). */
    const size_t cap = 256 * 1024;
    char * buf = malloc(cap);
    if (!buf) { close(sock); return send_status(fd, 503, "Busy", "{\"err\":\"oom\"}"); }
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t k = recv(sock, buf + total, cap - 1 - total, 0);
        if (k <= 0) break;
        total += (size_t)k;
    }
    close(sock);
    buf[total] = 0;
    const char * body = strstr(buf, "\r\n\r\n");
    body = body ? body + 4 : buf;
    size_t body_len = (body <= buf + total) ? total - (size_t)(body - buf) : 0;
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n",
        body_len);
    int rc = 0;
    if (sock_send_all(fd, hdr, hn) < 0) rc = -1;
    else if (body_len) sock_send_all(fd, body, body_len);
    free(buf);
    return rc;
}

/* Serve the cached Buienradar radar GIF the weather thread writes to
 * /tmp/toonui_radar.gif. The WASM client's JS shim fetches this on boot
 * and on a refresh interval, dropping the bytes into Emscripten's MEMFS so
 * the forecast screen's lv_gif_set_src("S:/tmp/toonui_radar.gif") binds. */
static int handle_radar_gif(int fd) {
    const char * path = "/tmp/toonui_radar.gif";
    struct stat st;
    if (stat(path, &st) != 0) return send_status(fd, 404, "Not Found", "no");
    FILE * f = fopen(path, "rb");
    if (!f) return send_status(fd, 404, "Not Found", "no");
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\nContent-Length: %lld\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n",
        (long long)st.st_size);
    if (sock_send_all(fd, hdr, n) < 0) { fclose(f); return -1; }
    char buf[4096]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        if (sock_send_all(fd, buf, r) < 0) { fclose(f); return -1; }
    fclose(f); return 0;
}

static int handle_state_stream(int fd) {
    const char * hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";
    if (sock_send_all(fd, hdr, strlen(hdr)) < 0) return -1;
    /* Cap one-second send timeout so a dead peer surfaces fast. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Dynamically allocate buffers to protect 256KB thread stack and prevent
     * buffer overflow (full state JSON routinely exceeds 4-16KB). */
    char * last = malloc(65536);
    char * body = malloc(65536);
    char * ev   = malloc(65800);
    if (!last || !body || !ev) {
        free(last); free(body); free(ev);
        return -1;
    }
    last[0] = '\0';
    int idle_ticks = 0;
    int ret = 0;

    /* Push the first frame immediately so the client doesn't sit on default values. */
    int n = render_state_json(body, 65536);
    if (n > 0) {
        int en = snprintf(ev, 65800, "data: %s\n\n", body);
        if (sock_send_all(fd, ev, en) < 0) {
            ret = -1;
            goto cleanup;
        }
        memcpy(last, body, (size_t)n + 1);
    }
    while (1) {
        usleep(1000 * 1000);   /* 1 Hz poll of shared state */
        n = render_state_json(body, 65536);
        if (n <= 0) continue;
        if (strcmp(body, last) != 0) {
            int en = snprintf(ev, 65800, "data: %s\n\n", body);
            if (sock_send_all(fd, ev, en) < 0) {
                ret = -1;
                break;
            }
            memcpy(last, body, (size_t)n + 1);
            idle_ticks = 0;
        } else if (++idle_ticks >= 10) {
            /* No state change for 10s — emit SSE comment line as a heartbeat. */
            if (sock_send_all(fd, ": ka\n\n", 6) < 0) {
                ret = -1;
                break;
            }
            idle_ticks = 0;
        }
    }
cleanup:
    free(last);
    free(body);
    free(ev);
    return ret;
}

/* Tiny extractor: find "key":<num> in flat JSON and return it as float.
 * Tolerates whitespace + optional quotes around the value. */
static int extract_float(const char * body, const char * key, float * out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(body, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '"') p++;
    char * end;
    float v = strtof(p, &end);
    if (end == p) return 0;
    *out = v; return 1;
}
static int extract_str(const char * body, const char * key,
                       char * out, size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(body, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    const char * e = strchr(p, '"');
    if (!e) return 0;
    size_t L = e - p; if (L > outsz - 1) L = outsz - 1;
    memcpy(out, p, L); out[L] = 0;
    /* Strip control chars (a non-browser client could send raw newlines etc.)
     * so PWA-supplied values can't corrupt the cfg or inject into shell-outs.
     * Mirrors settings_sanitize_str on the LVGL side. */
    char * w = out;
    for (char * r = out; *r; r++) if ((unsigned char)*r >= 0x20) *w++ = *r;
    *w = 0;
    return 1;
}

static int handle_setpoint(int fd, const char * body) {
    float v;
    if (!extract_float(body, "value", &v)) {
        return send_status(fd, 400, "Bad Request",
            "{\"err\":\"need {\\\"value\\\":18.50}\"}");
    }
    boxtalk_set_setpoint(v);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* -------- ventilation vremote command bridge ---------------------------
 * Slave/WASM clients POST {"cmd":"low|medium|high|auto|timer1|timer2|timer3"}
 * to /api/vent and the master forwards it to the local Itho via the existing
 * vent_send_vremote() (which can reach the Itho on the master's LAN). */
static int handle_vent_post(int fd, const char * body) {
    /* Two shapes: {"speed":N} (0..255 PWM, the slider) OR
     * {"cmd":"low|high|auto|timerN"} (legacy preset buttons). */
    float spd;
    if (extract_float(body, "speed", &spd)) {
        vent_set_speed_async((int)spd);
        return send_status(fd, 200, "OK", "{\"ok\":1}");
    }
    char cmd[24] = {0};
    extract_str(body, "cmd", cmd, sizeof cmd);
    if (!cmd[0])
        return send_status(fd, 400, "Bad Request",
            "{\"err\":\"need {\\\"speed\\\":N} or {\\\"cmd\\\":\\\"low|high|auto|timerN\\\"}\"}");
    vent_send_vremote_async(cmd);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* -------- marketplace install / uninstall (plug-and-play bridge) --------
 * POST /api/install {"id":"<integration-id>"}   → run the install helper
 * POST /api/uninstall {"id":"<integration-id>"} → run helper with "remove"
 *
 * Lets a WASM/PWA client install on the master without shell access. The
 * helper is the same `/mnt/data/integrations-install.sh` the Marketplace
 * screen on the Toon's own panel uses; we just give it an HTTP wrapper.
 * Both forks are detached so the request returns immediately — the next
 * SSE frame after install carries the new entry in `"integrations":[…]`
 * and the WASM client shows the new tile. */
static int handle_install_post(int fd, const char * body, int uninstall) {
    char id[64] = {0};
    extract_str(body, "id", id, sizeof id);
    if (!id[0])
        return send_status(fd, 400, "Bad Request",
            "{\"err\":\"need {\\\"id\\\":\\\"<integration-id>\\\"}\"}");
    /* id sanity check — alnum + '-_/' only, prevents shell injection into
     * the system() call below. */
    for (const char * c = id; *c; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '_' || *c == '.')) {
            return send_status(fd, 400, "Bad Request",
                "{\"err\":\"id has illegal char\"}");
        }
    }
    char cmd[256];
    snprintf(cmd, sizeof cmd,
        "(/mnt/data/integrations-install.sh %s %s >> /var/volatile/tmp/integ-install.log 2>&1) &",
        uninstall ? "remove" : "install", id);
    int rc = system(cmd);
    (void)rc;        /* fire-and-forget; status flows back via SSE registry diff */
    return send_status(fd, 202, "Accepted", "{\"ok\":1,\"async\":1}");
}

/* -------- weekly schedule (Comfort/Home/Sleep/Away) -------------------- */
/* GET → array of {state, start_day, start_hour, start_min, end_day, end_hour,
 *                 end_min}; POST replaces the whole list atomically. The LVGL
 * schedule screen and this endpoint share schedule_entries[] — both keep the
 * canonical hcb_config copy in sync via schedule_load/schedule_save. */
static int handle_schedule_get(int fd) {
    if (schedule_load() != 0)
        return send_status(fd, 502, "Bad Gateway", "{\"err\":\"load\"}");
    char body[8192];
    int off = snprintf(body, sizeof(body), "{\"entries\":[");
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        off += snprintf(body + off, sizeof(body) - off,
            "%s{\"state\":%d,\"start_day\":%d,\"start_hour\":%d,\"start_min\":%d,"
            "\"end_day\":%d,\"end_hour\":%d,\"end_min\":%d}",
            i ? "," : "",
            e->target_state, e->start_day, e->start_hour, e->start_min,
            e->end_day, e->end_hour, e->end_min);
    }
    off += snprintf(body + off, sizeof(body) - off, "]}");
    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n", off);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, body, off);
}

/* Parse one entry from a JSON object substring starting at `*p`. Advances
 * `*p` past the matching '}'. Returns 1 on success, 0 at end-of-list. */
static int sched_parse_entry(const char ** p, schedule_entry_t * e) {
    const char * q = strchr(*p, '{');
    if (!q) return 0;
    memset(e, 0, sizeof(*e));
    /* Each int field is "key":NN — small JSON, lookup-by-needle is fine. */
    struct { const char * key; int * out; } fields[] = {
        {"\"state\":",      &e->target_state},
        {"\"start_day\":",  &e->start_day},
        {"\"start_hour\":", &e->start_hour},
        {"\"start_min\":",  &e->start_min},
        {"\"end_day\":",    &e->end_day},
        {"\"end_hour\":",   &e->end_hour},
        {"\"end_min\":",    &e->end_min},
    };
    const char * end = strchr(q, '}');
    if (!end) return 0;
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
        const char * k = strstr(q, fields[i].key);
        if (!k || k > end) return 0;
        *fields[i].out = atoi(k + strlen(fields[i].key));
    }
    *p = end + 1;
    return 1;
}

static int handle_schedule_post(int fd, const char * body) {
    const char * arr = strstr(body, "\"entries\"");
    if (!arr) return send_status(fd, 400, "Bad Request",
        "{\"err\":\"need {\\\"entries\\\":[...]}\"}");
    const char * p = strchr(arr, '[');
    if (!p) return send_status(fd, 400, "Bad Request", "no [");
    p++;
    schedule_count = 0;
    while (schedule_count < SCHEDULE_MAX) {
        const char * cb = strchr(p, '{');
        const char * eb = strchr(p, ']');
        if (!cb || (eb && cb > eb)) break;
        if (!sched_parse_entry(&p, &schedule_entries[schedule_count])) break;
        schedule_count++;
    }
    if (schedule_save() != 0)
        return send_status(fd, 502, "Bad Gateway", "{\"err\":\"save\"}");
    char ok[64];
    snprintf(ok, sizeof(ok), "{\"ok\":1,\"count\":%d}", schedule_count);
    return send_status(fd, 200, "OK", ok);
}

static int handle_program(int fd, const char * body) {
    float v;
    /* {"resume":1}: back to the weekly program (the home tile's "Programma"). */
    if (extract_float(body, "resume", &v) && v != 0.0f) {
        boxtalk_resume_schedule();
        return send_status(fd, 200, "OK", "{\"ok\":1}");
    }
    /* {"preset":0-3}: a preset button, with the home tile's semantics — while
     * the program runs it switches until the next program point, in manual
     * mode it sets the preset's temperature and stays manual. */
    if (extract_float(body, "preset", &v)) {
        int s = (int)v;
        if (s < 0 || s > 3)
            return send_status(fd, 400, "Bad Request", "{\"err\":\"preset must be 0-3\"}");
        int ps = toon_state.program_state;
        int on_program = (ps == 1 || ps == 2 || ps == 3 || ps == 8) ||
                         (boxtalk_temp_override_origin() >= 0);
        if (!on_program || ps == 0) boxtalk_set_manual_preset(s);
        else                        boxtalk_set_program(s);
        return send_status(fd, 200, "OK", "{\"ok\":1}");
    }
    if (!extract_float(body, "state", &v)) {
        return send_status(fd, 400, "Bad Request",
            "{\"err\":\"need {\\\"state\\\":0-3 or -1}\"}");
    }
    int s = (int)v;
    if (s < 0) boxtalk_set_manual();
    else       boxtalk_set_program(s);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* -------------------------------------------------------------------- */
/* Carrier-agnostic delivery tracker. State lives in /mnt/data/packages.json
 * (JSON array of {id,label,eta,url,place,status,actual_place,added_at,
 * received_at}). pwa_server is the sole writer; no length cap. PWA + HA
 * webhook both POST here. Concurrency: a single coarse mutex around the
 * file, fine for handful-per-day write rate. */
#define PACKAGES_PATH "/mnt/data/packages.json"
static pthread_mutex_t g_pkg_mtx = PTHREAD_MUTEX_INITIALIZER;

static int pkg_read_all(char * out, size_t outsz) {
    FILE * f = fopen(PACKAGES_PATH, "r");
    if (!f) { snprintf(out, outsz, "[]"); return 0; }
    size_t n = fread(out, 1, outsz - 1, f);
    out[n] = 0;
    fclose(f);
    if (n == 0) snprintf(out, outsz, "[]");
    return 0;
}

static int pkg_write_all(const char * data) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), PACKAGES_PATH ".new");
    FILE * f = fopen(tmp, "w");
    if (!f) return -1;
    size_t n = strlen(data);
    int ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    if (!ok) { unlink(tmp); return -1; }
    return rename(tmp, PACKAGES_PATH);
}

static int handle_packages_get(int fd) {
    char body[16384];
    pthread_mutex_lock(&g_pkg_mtx);
    pkg_read_all(body, sizeof(body));
    pthread_mutex_unlock(&g_pkg_mtx);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n", strlen(body));
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, body, strlen(body));
}

/* Append the POSTed JSON object to the list. Generates id + added_at +
 * defaults status="pending". Caller's body is a single JSON object. */
/* Notify on package state change — native Toon banner (happ_usermsg), no HA.
 * subType is keyed per package so a later DeleteNotification could clear it. */
static void notify_pkg_state_change(const char * merchant, const char * label,
                                    const char * old_status, const char * new_status,
                                    const char * eta) {
    char text[256];
    snprintf(text, sizeof text, "%s: %s — %s (verwacht %s)",
             merchant, new_status, label, eta);
    notify_show("package", merchant, text);
    fprintf(stderr, "[pkg] notify (Toon): %s '%s' %s->%s\n",
            merchant, label, old_status, new_status);
}

static int handle_packages_post(int fd, const char * body) {
    char label[128] = "", eta[16] = "", url[256] = "", place[64] = "",
         source[16] = "manual",
         tracking[64] = "", postal[16] = "",
         merchant[32] = "", order_id[32] = "",
         status[16] = "pending";
    extract_str(body, "label",       label,    sizeof(label));
    extract_str(body, "eta",         eta,      sizeof(eta));
    extract_str(body, "url",         url,      sizeof(url));
    extract_str(body, "place",       place,    sizeof(place));
    extract_str(body, "source",      source,   sizeof(source));
    extract_str(body, "tracking",    tracking, sizeof(tracking));
    extract_str(body, "postal_code", postal,   sizeof(postal));
    extract_str(body, "merchant",    merchant, sizeof(merchant));
    extract_str(body, "order_id",    order_id, sizeof(order_id));
    extract_str(body, "status",      status,   sizeof(status));
    if (!label[0]) return send_status(fd, 400, "Bad Request",
        "{\"err\":\"label required\"}");
    /* default eta = today */
    if (!eta[0]) {
        time_t now = time(NULL);
        struct tm tm; localtime_r(&now, &tm);
        strftime(eta, sizeof(eta), "%Y-%m-%d", &tm);
    }
    char ts[24]; time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    /* id needs sub-second resolution so rapid-fire adds don't collide
     * (the earlier seconds-only id duplicated when the email poller
     * dropped a batch). Use ms since epoch from gettimeofday. */
    struct timeval tv; gettimeofday(&tv, NULL);
    char id[32];
    snprintf(id, sizeof(id), "%lld%03d",
             (long long)tv.tv_sec, (int)(tv.tv_usec / 1000));

    /* If the caller supplied a (merchant + order_id) tuple, try upsert
     * first — finds an existing entry with the same pair and rebuilds it
     * (preserving id + added_at). On state advancement we fire HA notify.
     * If no match (or no merchant+order_id provided), fall through to
     * the append-as-new path below. */
    pthread_mutex_lock(&g_pkg_mtx);
    char cur[16384]; pkg_read_all(cur, sizeof(cur));

    if (merchant[0] && order_id[0]) {
        /* Scan objects for matching merchant+order_id. */
        const char * scan = cur;
        char target_m[64], target_o[64];
        snprintf(target_m, sizeof(target_m), "\"merchant\":\"%s\"", merchant);
        snprintf(target_o, sizeof(target_o), "\"order_id\":\"%s\"", order_id);
        while ((scan = strchr(scan, '{'))) {
            const char * obj_end = strchr(scan, '}');
            if (!obj_end) break;
            size_t L = (size_t)(obj_end - scan + 1);
            if (L < sizeof(cur) && memmem(scan, L, target_m, strlen(target_m))
                                && memmem(scan, L, target_o, strlen(target_o))) {
                /* HIT — extract the existing entry's preserved fields and
                 * its current status, then rebuild with new status/eta. */
                char obj[1024]; if (L >= sizeof(obj)) L = sizeof(obj) - 1;
                memcpy(obj, scan, L); obj[L] = 0;
                char xid[32]="", xadded[24]="", xplace[64]="", xactual[64]="",
                     xreceived[24]="", xstatus[16]="", xurl[256]="", xtrk[64]="",
                     xpostal[16]="";
                extract_str(obj, "id",         xid,      sizeof(xid));
                extract_str(obj, "added_at",   xadded,   sizeof(xadded));
                extract_str(obj, "place",      xplace,   sizeof(xplace));
                extract_str(obj, "actual_place", xactual,sizeof(xactual));
                extract_str(obj, "received_at",xreceived,sizeof(xreceived));
                extract_str(obj, "status",     xstatus,  sizeof(xstatus));
                extract_str(obj, "url",        xurl,     sizeof(xurl));
                extract_str(obj, "tracking",   xtrk,     sizeof(xtrk));
                extract_str(obj, "postal_code",xpostal,  sizeof(xpostal));
                /* Don't downgrade status (delivered > shipped > ordered > pending) */
                int rank_old = !strcmp(xstatus,"delivered")?4:
                               !strcmp(xstatus,"shipped")?3:
                               !strcmp(xstatus,"ordered")?2:
                               !strcmp(xstatus,"received")?5:1;
                int rank_new = !strcmp(status,"delivered")?4:
                               !strcmp(status,"shipped")?3:
                               !strcmp(status,"ordered")?2:
                               !strcmp(status,"received")?5:1;
                const char * eff_status = (rank_new > rank_old) ? status : xstatus;
                /* Keep first-seen added_at + existing place/url/etc if caller didn't override */
                if (!place[0])  strcpy(place,  xplace);
                if (!url[0])    strcpy(url,    xurl);
                if (!tracking[0]) strcpy(tracking, xtrk);
                if (!postal[0]) strcpy(postal, xpostal);
                if (!xadded[0]) strcpy(xadded, ts);

                char rebuilt[1024];
                snprintf(rebuilt, sizeof(rebuilt),
                    "{\"id\":\"%s\",\"label\":\"%s\",\"eta\":\"%s\",\"url\":\"%s\","
                    "\"place\":\"%s\",\"actual_place\":\"%s\",\"source\":\"%s\","
                    "\"tracking\":\"%s\",\"postal_code\":\"%s\","
                    "\"merchant\":\"%s\",\"order_id\":\"%s\","
                    "\"status\":\"%s\",\"added_at\":\"%s\",\"received_at\":\"%s\"}",
                    xid, label, eta, url, place, xactual, source,
                    tracking, postal, merchant, order_id,
                    eff_status, xadded, xreceived);
                char merged[17000];
                size_t pre = (size_t)(scan - cur);
                size_t post_len = strlen(obj_end + 1);
                size_t rb_len = strlen(rebuilt);
                if (pre + rb_len + post_len >= sizeof(merged)) {
                    /* Would overflow the stack buffer — refuse, keep the file. */
                    pthread_mutex_unlock(&g_pkg_mtx);
                    return send_status(fd, 413, "Payload Too Large", "{\"err\":\"packages store full\"}");
                }
                memcpy(merged, cur, pre);
                memcpy(merged + pre, rebuilt, rb_len);
                memcpy(merged + pre + rb_len, obj_end + 1, post_len);
                merged[pre + rb_len + post_len] = 0;
                pkg_write_all(merged);
                pthread_mutex_unlock(&g_pkg_mtx);
                /* Fire notify only on actual state advancement */
                if (rank_new > rank_old && strcmp(xstatus, status) != 0) {
                    notify_pkg_state_change(merchant, label, xstatus, status, eta);
                }
                return send_status(fd, 200, "OK", "{\"ok\":1,\"upsert\":\"updated\"}");
            }
            scan = obj_end + 1;
        }
    }

    /* No match — append as new */
    char merged[17000];
    char entry[1024];
    snprintf(entry, sizeof(entry),
        "{\"id\":\"%s\",\"label\":\"%s\",\"eta\":\"%s\",\"url\":\"%s\","
        "\"place\":\"%s\",\"actual_place\":\"\",\"source\":\"%s\","
        "\"tracking\":\"%s\",\"postal_code\":\"%s\","
        "\"merchant\":\"%s\",\"order_id\":\"%s\","
        "\"status\":\"%s\",\"added_at\":\"%s\",\"received_at\":\"\"}",
        id, label, eta, url, place, source, tracking, postal,
        merchant, order_id, status, ts);
    size_t L = strlen(cur);
    while (L > 0 && (cur[L-1] == '\n' || cur[L-1] == ' ')) L--;
    int mn;
    if (L < 2 || cur[L-1] != ']')      mn = snprintf(merged, sizeof(merged), "[%s]", entry);
    else if (L == 2)                   mn = snprintf(merged, sizeof(merged), "[%s]", entry);
    else {
        cur[L-1] = 0;
        mn = snprintf(merged, sizeof(merged), "%s,%s]", cur, entry);
    }
    if (mn < 0 || (size_t)mn >= sizeof(merged)) {
        /* Truncated JSON would be unparseable, and the next append would then
         * replace the whole file with [entry] — refuse instead. */
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 413, "Payload Too Large", "{\"err\":\"packages store full\"}");
    }
    pkg_write_all(merged);
    pthread_mutex_unlock(&g_pkg_mtx);
    if (merchant[0] && !strcmp(status, "ordered")) {
        /* First time we hear of an order — fire a notify too */
        notify_pkg_state_change(merchant, label, "(new)", status, eta);
    }
    return send_status(fd, 200, "OK", "{\"ok\":1,\"upsert\":\"created\"}");
}

/* Surgical edit: find the {…} object whose "id":"<id>" matches, then
 * either delete it (DELETE) or rewrite its status to received (PATCH-ish).
 * We do this with string ops instead of a real JSON parser to avoid
 * pulling in a JSON dep on Toon. The objects we write are flat so this
 * is safe — no nested braces. */
static const char * find_obj_by_id(const char * json, const char * id) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"id\":\"%s\"", id);
    return strstr(json, needle);
}

static int handle_packages_delete(int fd, const char * id) {
    pthread_mutex_lock(&g_pkg_mtx);
    char cur[16384]; pkg_read_all(cur, sizeof(cur));
    const char * needle = find_obj_by_id(cur, id);
    if (!needle) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 404, "Not Found", "{\"err\":\"id not found\"}");
    }
    /* Walk back to the opening '{' */
    const char * start = needle;
    while (start > cur && *start != '{') start--;
    /* Walk forward to the closing '}' */
    const char * end = needle;
    while (*end && *end != '}') end++;
    if (!*end) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 500, "Server Error", "{\"err\":\"parse fail\"}");
    }
    end++;
    /* Eat a trailing or preceding comma so we don't leave [,] or [..,,..]. */
    if (*end == ',') end++;
    else if (start > cur && *(start - 1) == ',') start--;

    char out[16384];
    size_t pre = (size_t)(start - cur);
    size_t post_len = strlen(end);
    if (pre + post_len + 1 > sizeof(out)) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 500, "Server Error", "{\"err\":\"too large\"}");
    }
    memcpy(out, cur, pre);
    memcpy(out + pre, end, post_len);
    out[pre + post_len] = 0;
    pkg_write_all(out);
    pthread_mutex_unlock(&g_pkg_mtx);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* POST /api/packages/<id>/receive  body: {"actual_place":"keuken"} */
static int handle_packages_receive(int fd, const char * id, const char * body) {
    char place[64] = "";
    extract_str(body, "actual_place", place, sizeof(place));

    pthread_mutex_lock(&g_pkg_mtx);
    char cur[16384]; pkg_read_all(cur, sizeof(cur));
    char * needle = (char *)find_obj_by_id(cur, id);
    if (!needle) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 404, "Not Found", "{\"err\":\"id not found\"}");
    }
    char * start = needle;
    while (start > cur && *start != '{') start--;
    char * end = needle;
    while (*end && *end != '}') end++;
    if (!*end) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 500, "Server Error", "{\"err\":\"parse fail\"}");
    }
    /* Pull existing label/eta/url/place out of the matched object so we
     * can rebuild it with status=received + the new actual_place + a
     * received_at timestamp. Bounded by '{' .. '}'. */
    char obj[1024];
    size_t L = (size_t)(end - start + 1);
    if (L >= sizeof(obj)) L = sizeof(obj) - 1;
    memcpy(obj, start, L); obj[L] = 0;
    char xid[32] = "", xlabel[128] = "", xeta[16] = "", xurl[256] = "",
         xplace[64] = "", xsource[16] = "", xadded[24] = "",
         xtracking[64] = "", xpostal[16] = "";
    extract_str(obj, "id",          xid,      sizeof(xid));
    extract_str(obj, "label",       xlabel,   sizeof(xlabel));
    extract_str(obj, "eta",         xeta,     sizeof(xeta));
    extract_str(obj, "url",         xurl,     sizeof(xurl));
    extract_str(obj, "place",       xplace,   sizeof(xplace));
    extract_str(obj, "source",      xsource,  sizeof(xsource));
    extract_str(obj, "added_at",    xadded,   sizeof(xadded));
    extract_str(obj, "tracking",    xtracking,sizeof(xtracking));
    extract_str(obj, "postal_code", xpostal,  sizeof(xpostal));
    if (!place[0]) snprintf(place, sizeof(place), "%s", xplace);
    char ts[24]; time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    char rebuilt[1024];
    snprintf(rebuilt, sizeof(rebuilt),
        "{\"id\":\"%s\",\"label\":\"%s\",\"eta\":\"%s\",\"url\":\"%s\","
        "\"place\":\"%s\",\"actual_place\":\"%s\",\"source\":\"%s\","
        "\"tracking\":\"%s\",\"postal_code\":\"%s\","
        "\"status\":\"received\",\"added_at\":\"%s\",\"received_at\":\"%s\"}",
        xid, xlabel, xeta, xurl, xplace, place, xsource,
        xtracking, xpostal, xadded, ts);

    char merged[17000];
    size_t pre = (size_t)(start - cur);
    size_t post_len = strlen(end + 1);
    if (pre + strlen(rebuilt) + post_len + 1 > sizeof(merged)) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 500, "Server Error", "{\"err\":\"too large\"}");
    }
    memcpy(merged, cur, pre);
    memcpy(merged + pre, rebuilt, strlen(rebuilt));
    memcpy(merged + pre + strlen(rebuilt), end + 1, post_len);
    merged[pre + strlen(rebuilt) + post_len] = 0;
    pkg_write_all(merged);
    pthread_mutex_unlock(&g_pkg_mtx);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* POST /api/email — accepts {"from":"...", "subject":"...", "body":"..."}
 * (as fired by HA's imap_content event after Jinja substitution). Pipes
 * the JSON to /mnt/data/parse_email.py via popen; the script extracts
 * merchant + status + order_id and POSTs back to /api/packages here.
 * Synchronous — returns the script's stdout so the caller (HA) gets
 * a readable trace in the log. */
static int handle_email_post(int fd, const char * body) {
    if (!body || !body[0]) return send_status(fd, 400, "Bad Request",
        "{\"err\":\"empty body\"}");
    /* Write the body to a temp file so we don't have to escape it through
     * the shell. parse_email.py reads JSON from stdin. */
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/email-%ld.json", (long)time(NULL));
    FILE * t = fopen(tmp, "w");
    if (!t) return send_status(fd, 500, "Server Error", "{\"err\":\"tmp\"}");
    fwrite(body, 1, strlen(body), t);
    fclose(t);
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "TARGET_URL=http://127.0.0.1:10081/api/packages "
        "python3 /mnt/data/parse_email.py < %s 2>&1; rm -f %s", tmp, tmp);
    FILE * p = popen(cmd, "r");
    if (!p) return send_status(fd, 500, "Server Error", "{\"err\":\"popen\"}");
    char out[2048]; size_t n = fread(out, 1, sizeof(out) - 1, p);
    out[n] = 0;
    pclose(p);
    /* Pass through whatever parse_email.py printed. */
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n", n);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, out, n);
}

static int handle_curtain(int fd, const char * body) {
    char action[16];
    if (!extract_str(body, "action", action, sizeof(action))) {
        return send_status(fd, 400, "Bad Request",
            "{\"err\":\"need {\\\"action\\\":\\\"open|close|stop\\\"}\"}");
    }
    if      (!strcmp(action, "open"))  ha_curtain_open_async();
    else if (!strcmp(action, "close")) ha_curtain_close_async();
    else if (!strcmp(action, "stop"))  ha_curtain_stop_async();
    else return send_status(fd, 400, "Bad Request",
        "{\"err\":\"action must be open/close/stop\"}");
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

static int extract_int(const char * body, const char * key, int * out);

/* POST /api/hw/socket {"i":0,"on":1} — switch a HomeWizard Energy Socket
 * (used by client Toons, which have no direct access to the device). */
static int handle_hw_socket(int fd, const char * body) {
    int i, on;
    if (!extract_int(body, "i", &i) || !extract_int(body, "on", &on) ||
        i < 0 || i >= HW_SOCKET_MAX || !settings.hw_socket_host[i][0])
        return send_status(fd, 400, "Bad Request", "{\"err\":\"need {\\\"i\\\":slot,\\\"on\\\":0|1}\"}");
    homewizard_socket_set_async(i, on);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* POST /api/ha/toggle {"id":"switch.x"} — toggle a Home Assistant light or
 * switch from the remote page. Only entities the user added in the device
 * manager can be switched: the id is looked up there, never passed on as-is. */
static int handle_ha_toggle(int fd, const char * body) {
    char id[64];
    if (!settings.enable_ha || !extract_str(body, "id", id, sizeof id) || !id[0])
        return send_status(fd, 400, "Bad Request", "{\"err\":\"need {\\\"id\\\":\\\"entity_id\\\"}\"}");
    for (int i = 0; i < ha_device_count && i < HA_DEVICE_MAX; i++) {
        const ha_device_t * D = &ha_devices[i];
        if ((D->type == HADEV_LIGHT || D->type == HADEV_SWITCH) && strcmp(D->entity_id, id) == 0) {
            ha_device_toggle_async(D->type, D->entity_id);
            return send_status(fd, 200, "OK", "{\"ok\":1}");
        }
    }
    return send_status(fd, 404, "Not Found", "{\"err\":\"not a managed light or switch\"}");
}

/* POST /api/homey/toggle {"id":"<device id>"} — switch a Homey on/off device
 * from the remote page. The id must be a switchable device in the polled
 * list (never a lock or garage door); homey_onoff() validates again. */
static int handle_homey_toggle(int fd, const char * body) {
    char id[64];
    if (!settings.enable_homey || !extract_str(body, "id", id, sizeof id) || !id[0])
        return send_status(fd, 400, "Bad Request", "{\"err\":\"need {\\\"id\\\":\\\"device id\\\"}\"}");
    int on = -1;
    homey_lock();
    for (int i = 0; i < homey_state.ndev; i++) {
        const hy_dev_t * D = &homey_state.dev[i];
        if (strcmp(D->id, id) || !homey_switchable(D)) continue;
        if (D->available) on = hy_dev_cap(D, "onoff")->num != 0;
        else on = -2;
        break;
    }
    homey_unlock();
    if (on == -1) return send_status(fd, 404, "Not Found", "{\"err\":\"not a Homey on/off device\"}");
    if (on == -2 || homey_onoff(id, !on) != 0)
        return send_status(fd, 409, "Conflict", "{\"err\":\"device unavailable\"}");
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* POST /api/dz/toggle {"idx":N} — toggle a Domoticz switch that is in the
 * current device list. */
static int handle_dz_toggle(int fd, const char * body) {
    int idx;
    if (!settings.enable_domoticz || !extract_int(body, "idx", &idx))
        return send_status(fd, 400, "Bad Request", "{\"err\":\"need {\\\"idx\\\":N}\"}");
    for (int i = 0; i < domoticz_state.count && i < DOMOTICZ_MAX_DEV; i++) {
        if (domoticz_state.dev[i].idx == idx) {
            domoticz_switch_async(idx, "Toggle");
            return send_status(fd, 200, "OK", "{\"ok\":1}");
        }
    }
    return send_status(fd, 404, "Not Found", "{\"err\":\"unknown Domoticz device\"}");
}

/* -------- full settings page (web mirror of Settings screen) ---------- */
static int extract_int(const char * body, const char * key, int * out) {
    float v;
    if (!extract_float(body, key, &v)) return 0;
    *out = (int)v;
    return 1;
}

static int handle_settings_get(int fd) {
    char body[6144];   /* ~3.5 KB of settings + the HomeWizard block */
    int n = snprintf(body, sizeof body,
        "{"
        "\"auto_dim_enabled\":%d,\"auto_dim_seconds\":%d,"
        "\"active_brightness\":%d,\"dim_brightness\":%d,\"dim_sleep_away_black\":%d,\"auto_brightness\":%d,"
        "\"temp_offset_centi\":%d,\"show_dim_weather\":%d,"
        "\"show_dim_waste\":%d,\"dim_waste_lead_days\":%d,\"dim_waste_hour\":%d,"
        "\"waste_postcode\":\"%s\",\"waste_housenr\":\"%s\","
        "\"waste_provider\":%d,\"waste_ics_url\":\"%s\","
        "\"waste_plugin\":\"%s\",\"waste_icsid\":\"%s\",\"waste_street\":\"%s\",\"waste_city\":\"%s\","
        "\"weather_location\":\"%s\",\"weather_location_id\":%d,"
        "\"forecast_mode\":%d,\"weather_use_feel\":%d,\"ot_bridge_mode\":\"%s\",\"otgw_host\":\"%s\","
        "\"mqtt_enabled\":%d,\"mqtt_host\":\"%s\",\"mqtt_port\":%d,\"mqtt_user\":\"%s\","
        "\"enable_p1_elec\":%d,\"enable_p1_water\":%d,\"enable_vent\":%d,"
        "\"enable_ha\":%d,\"enable_zwave\":%d,\"vnc_enabled\":%d,"
        "\"enable_domoticz\":%d,\"domoticz_host\":\"%s\",\"domoticz_user\":\"%s\","
        "\"hide_offline_tiles\":%d,\"boot_picker_enabled\":%d,"
        "\"update_check_enabled\":%d,\"update_channel\":%d,"
        "\"ha_host\":\"%s\",\"life360_a_entity\":\"%s\",\"life360_a_name\":\"%s\","
        "\"life360_b_entity\":\"%s\",\"life360_b_name\":\"%s\","
        "\"life360_c_entity\":\"%s\",\"life360_c_name\":\"%s\","
        "\"curtain_entity\":\"%s\",\"curtain_bat_a\":\"%s\",\"curtain_bat_b\":\"%s\","
        "\"doorbell_entity\":\"%s\",\"doorbell_camera\":\"%s\",\"doorbell_seconds\":%d,"
        "\"doorbell_stream_url\":\"%s\","
        "\"p1_elec_host\":\"%s\",\"p1_water_host\":\"%s\",\"vent_host\":\"%s\",\"opnsense_host\":\"%s\","
        "\"energy_source\":%d,\"auto_update_enabled\":%d,\"auto_update_hour\":%d,"
        "\"news_enabled\":%d,\"news_rss_url\":\"%s\",\"news_scroll_speed\":%d,"
        "\"calendar_enabled\":%d,\"calendar_ha_entity\":\"%s\",\"calendar_ics_url\":\"%s\","
        "\"tile_rotate_enabled\":%d,\"tile_rotate_seconds\":%d,\"tile_rotate_members\":\"%s\","
        "\"client_mode\":%d,\"master_host\":\"%s\","
        "\"home_theme\":%d"
        "}",
        settings.auto_dim_enabled, settings.auto_dim_seconds,
        settings.active_brightness, settings.dim_brightness, settings.dim_sleep_away_black, settings.auto_brightness,
        (int)(toon_state.temp_offset * 100.0f + (toon_state.temp_offset < 0 ? -0.5f : 0.5f)), settings.show_dim_weather,
        settings.show_dim_waste, settings.dim_waste_lead_days, settings.dim_waste_hour,
        settings.waste_postcode, settings.waste_housenr,
        settings.waste_provider, settings.waste_ics_url,
        settings.waste_plugin, settings.waste_icsid, settings.waste_street, settings.waste_city,
        settings.weather_location, settings.weather_location_id,
        settings.forecast_mode, settings.weather_use_feel, settings.ot_bridge_mode, settings.otgw_host,
        settings.mqtt_enabled, settings.mqtt_host, settings.mqtt_port, settings.mqtt_user,
        settings.enable_p1_elec, settings.enable_p1_water, settings.enable_vent,
        settings.enable_ha, settings.enable_zwave, settings.vnc_enabled,
        settings.enable_domoticz, settings.domoticz_host, settings.domoticz_user,
        settings.hide_offline_tiles, settings.boot_picker_enabled,
        settings.update_check_enabled, settings.update_channel,
        settings.ha_host, settings.life360_a_entity, settings.life360_a_name,
        settings.life360_b_entity, settings.life360_b_name,
        settings.life360_c_entity, settings.life360_c_name,
        settings.curtain_entity, settings.curtain_bat_a, settings.curtain_bat_b,
        settings.doorbell_entity, settings.doorbell_camera, settings.doorbell_seconds,
        settings.doorbell_stream_url,
        settings.p1_elec_host, settings.p1_water_host, settings.vent_host, settings.opnsense_host,
        settings.energy_source, settings.auto_update_enabled, settings.auto_update_hour,
        settings.news_enabled, settings.news_rss_url, settings.news_scroll_speed,
        settings.calendar_enabled, settings.calendar_ha_entity, settings.calendar_ics_url,
        settings.tile_rotate_enabled, settings.tile_rotate_seconds, settings.tile_rotate_members,
        settings.client_mode, settings.master_host,
        settings.home_theme);
    /* HomeWizard kWh meter + Energy Sockets (#37): appended in place of the
     * closing brace, JSON-escaped. */
    if (n > 1 && n < (int)sizeof body && body[n - 1] == '}') {
        char kwh[48], kwh2[48];
        json_strcpy(kwh, settings.hw_kwh_host, sizeof kwh);
        json_strcpy(kwh2, settings.hw_kwh2_host, sizeof kwh2);
        n--;
        n += snprintf(body + n, sizeof body - (size_t)n, ",\"hw_kwh_host\":\"%s\",\"hw_kwh2_host\":\"%s\"", kwh, kwh2);
        for (int i = 0; i < HW_SOCKET_MAX && n < (int)sizeof body; i++) {
            char nm[48], ip[48];
            json_strcpy(nm, settings.hw_socket_name[i], sizeof nm);
            json_strcpy(ip, settings.hw_socket_host[i], sizeof ip);
            n += snprintf(body + n, sizeof body - (size_t)n,
                          ",\"hw_socket_name_%d\":\"%s\",\"hw_socket_host_%d\":\"%s\"", i, nm, i, ip);
        }
        if (n < (int)sizeof body - 1) { body[n++] = '}'; body[n] = 0; }
    }
    if (n >= (int)sizeof body) n = (int)sizeof body - 1;
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n\r\n", n);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, body, n);
}

static int handle_settings_post(int fd, const char * body) {
    int iv; char sv[1024];
    /* MQTT before the update: the web client re-sends these on every save,
     * so only a real change may rewrite mqtt.cfg and reconnect (see below). */
    char mq_before[sizeof settings.mqtt_host + sizeof settings.mqtt_user + sizeof settings.mqtt_pass + 32];
    snprintf(mq_before, sizeof mq_before, "%s|%s|%s|%d|%d", settings.mqtt_host, settings.mqtt_user,
             settings.mqtt_pass, settings.mqtt_port, settings.mqtt_enabled);
    if (extract_int(body, "auto_dim_enabled", &iv))   settings.auto_dim_enabled = !!iv;
    if (extract_int(body, "auto_dim_seconds", &iv))   settings.auto_dim_seconds = iv < 5 ? 5 : (iv > 300 ? 300 : iv);
    if (extract_int(body, "active_brightness", &iv))  settings.active_brightness = iv < 0 ? 0 : (iv > 1000 ? 1000 : iv);
    if (extract_int(body, "dim_brightness", &iv))     settings.dim_brightness = iv < 0 ? 0 : (iv > 1000 ? 1000 : iv);
    if (extract_int(body, "dim_sleep_away_black", &iv)) settings.dim_sleep_away_black = !!iv;
    if (extract_int(body, "auto_brightness", &iv))    settings.auto_brightness = !!iv;
    /* Room-temp correction lives in happ_thermstat (shared with qt-gui); only
       write it when it actually changes, since a settings POST carries every
       field. Centi-°C on the wire for API compatibility. */
    if (extract_int(body, "temp_offset_centi", &iv) && !settings.client_mode) {
        int cur = (int)(toon_state.temp_offset * 100.0f + (toon_state.temp_offset < 0 ? -0.5f : 0.5f));
        if (toon_state.temp_offset_valid && iv / 10 != cur / 10)
            boxtalk_set_temp_offset(iv / 100.0f);
    }
    if (extract_int(body, "show_dim_weather", &iv))   settings.show_dim_weather = !!iv;
    if (extract_int(body, "show_dim_waste", &iv))     settings.show_dim_waste = !!iv;
    if (extract_int(body, "dim_waste_lead_days", &iv))settings.dim_waste_lead_days = iv < 0 ? 0 : (iv > 7 ? 7 : iv);
    if (extract_int(body, "dim_waste_hour", &iv))     settings.dim_waste_hour = iv < 0 ? 0 : (iv > 23 ? 23 : iv);
    if (extract_str(body, "waste_postcode", sv, sizeof sv))
        snprintf(settings.waste_postcode, sizeof settings.waste_postcode, "%s", sv);
    if (extract_str(body, "waste_housenr", sv, sizeof sv))
        snprintf(settings.waste_housenr, sizeof settings.waste_housenr, "%s", sv);
    if (extract_int(body, "waste_provider", &iv)) settings.waste_provider = !!iv;
    if (extract_str(body, "waste_ics_url", sv, sizeof sv))
        snprintf(settings.waste_ics_url, sizeof settings.waste_ics_url, "%s", sv);
    if (extract_str(body, "waste_plugin", sv, sizeof sv))
        snprintf(settings.waste_plugin, sizeof settings.waste_plugin, "%s", sv);
    if (extract_str(body, "waste_icsid", sv, sizeof sv))
        snprintf(settings.waste_icsid, sizeof settings.waste_icsid, "%s", sv);
    if (extract_str(body, "waste_street", sv, sizeof sv))
        snprintf(settings.waste_street, sizeof settings.waste_street, "%s", sv);
    if (extract_str(body, "waste_city", sv, sizeof sv))
        snprintf(settings.waste_city, sizeof settings.waste_city, "%s", sv);
    if (extract_int(body, "forecast_mode", &iv))      settings.forecast_mode = iv;
    if (extract_int(body, "weather_use_feel", &iv))   settings.weather_use_feel = !!iv;
    if (extract_int(body, "mqtt_port", &iv))          settings.mqtt_port = iv;
    if (extract_int(body, "enable_p1_elec", &iv))     settings.enable_p1_elec = !!iv;
    if (extract_int(body, "enable_p1_water", &iv))    settings.enable_p1_water = !!iv;
    if (extract_int(body, "enable_vent", &iv))        settings.enable_vent = !!iv;
    if (extract_int(body, "enable_ha", &iv))          settings.enable_ha = !!iv;
    if (extract_int(body, "enable_domoticz", &iv))    settings.enable_domoticz = !!iv;
    if (extract_str(body, "domoticz_host", sv, sizeof sv))
        snprintf(settings.domoticz_host, sizeof settings.domoticz_host, "%s", sv);
    if (extract_str(body, "domoticz_user", sv, sizeof sv))
        snprintf(settings.domoticz_user, sizeof settings.domoticz_user, "%s", sv);
    if (extract_str(body, "domoticz_pass", sv, sizeof sv))
        snprintf(settings.domoticz_pass, sizeof settings.domoticz_pass, "%s", sv);
    if (extract_int(body, "enable_zwave", &iv))       settings.enable_zwave = !!iv;
    if (extract_int(body, "vnc_enabled", &iv)) {
        int nv = !!iv;
        if (settings.vnc_enabled != nv) {
            settings.vnc_enabled = nv;
            vnc_apply_state(settings.vnc_enabled);
        }
    }
    if (extract_int(body, "hide_offline_tiles", &iv)) settings.hide_offline_tiles = !!iv;
    if (extract_int(body, "boot_picker_enabled", &iv))settings.boot_picker_enabled = !!iv;
    if (extract_int(body, "update_check_enabled", &iv))settings.update_check_enabled = !!iv;
    if (extract_int(body, "update_channel", &iv))      settings.update_channel = !!iv;
    if (extract_int(body, "mqtt_enabled", &iv))       settings.mqtt_enabled = !!iv;
    /* WASM slave Settings posts these via settings_save() → fetch /api/settings;
     * accepting them here keeps the slave UI in sync with the master cfg. */
    if (extract_int(body, "pwa_login_enabled", &iv))  settings.pwa_login_enabled = !!iv;
    if (extract_str(body, "pwa_login_user", sv, sizeof sv) && sv[0])
        snprintf(settings.pwa_login_user, sizeof settings.pwa_login_user, "%s", sv);
    /* Only accept a NON-EMPTY password here. The WASM slave always includes
     * pwa_login_pass in its settings_save() blob (empty when it has none), so
     * accepting "" would wipe a password just set via /set-password — which is
     * exactly what made the login never stick. The password is set/cleared only
     * through the dedicated /set-password flow, never a routine slave sync. */
    if (extract_str(body, "pwa_login_pass", sv, sizeof sv) && sv[0])
        snprintf(settings.pwa_login_pass, sizeof settings.pwa_login_pass, "%s", sv);
    if (extract_int(body, "pin_enabled", &iv))        settings.pin_enabled = !!iv;
    if (extract_str(body, "pin_code", sv, sizeof sv))
        snprintf(settings.pin_code, sizeof settings.pin_code, "%s", sv);
    /* City name is authoritative: when it changes, auto-resolve the Buienradar
     * location id (Open-Meteo geocoding). Only fall back to a manually-entered
     * id when the city is unchanged, so the id field still allows an override. */
    {
        int city_changed = 0;
        if (extract_str(body, "weather_location", sv, sizeof sv)) {
            city_changed = strcmp(sv, settings.weather_location) != 0;
            snprintf(settings.weather_location, sizeof settings.weather_location, "%s", sv);
        }
        if (city_changed && settings.weather_location[0]) {
            int gid = weather_geocode(settings.weather_location);
            if (gid > 0) settings.weather_location_id = gid;
            else if (extract_int(body, "weather_location_id", &iv) && iv > 0)
                settings.weather_location_id = iv;   /* name not found: keep the given id */
            weather_relocate_async();
            buienradar_rain_refresh_async();   /* rain tile follows, as on the device */
        } else if (extract_int(body, "weather_location_id", &iv) && iv > 0 &&
                   iv != settings.weather_location_id) {
            settings.weather_location_id = iv;
            weather_relocate_async();          /* was: stored, used only after a restart */
            buienradar_rain_refresh_async();
        }
    }
    if (extract_str(body, "otgw_host", sv, sizeof sv))
        snprintf(settings.otgw_host, sizeof settings.otgw_host, "%s", sv);
    /* proxy mode removed; wireless (Via OTGW) only with an OTGW host, else
     * the bridge answers for the keteladapter and the boiler never fires. */
    if (extract_str(body, "ot_bridge_mode", sv, sizeof sv) &&
        (!strcmp(sv, "off") || (!strcmp(sv, "wireless") && settings.otgw_host[0])))
        snprintf(settings.ot_bridge_mode, sizeof settings.ot_bridge_mode, "%s", sv);
    if (extract_str(body, "mqtt_host", sv, sizeof sv))
        snprintf(settings.mqtt_host, sizeof settings.mqtt_host, "%s", sv);
    if (extract_str(body, "mqtt_user", sv, sizeof sv))
        snprintf(settings.mqtt_user, sizeof settings.mqtt_user, "%s", sv);
    if (extract_str(body, "mqtt_pass", sv, sizeof sv))     /* was sent but never read */
        snprintf(settings.mqtt_pass, sizeof settings.mqtt_pass, "%s", sv);
    if (extract_int(body, "auto_home_enabled", &iv))  settings.auto_home_enabled = !!iv;
    if (extract_int(body, "auto_home_seconds", &iv))  settings.auto_home_seconds = iv < 5 ? 5 : (iv > 600 ? 600 : iv);
    if (extract_int(body, "client_mode", &iv))        settings.client_mode = !!iv;
    if (extract_str(body, "master_host", sv, sizeof sv))
        snprintf(settings.master_host, sizeof settings.master_host, "%s", sv);
    if (extract_int(body, "home_theme", &iv))         settings.home_theme = !!iv;
    /* Home Assistant host + Life360 + curtain entities */
    if (extract_str(body, "ha_host", sv, sizeof sv))
        snprintf(settings.ha_host, sizeof settings.ha_host, "%s", sv);
    if (extract_str(body, "life360_a_entity", sv, sizeof sv))
        snprintf(settings.life360_a_entity, sizeof settings.life360_a_entity, "%s", sv);
    if (extract_str(body, "life360_a_name", sv, sizeof sv))
        snprintf(settings.life360_a_name, sizeof settings.life360_a_name, "%s", sv);
    if (extract_str(body, "life360_b_entity", sv, sizeof sv))
        snprintf(settings.life360_b_entity, sizeof settings.life360_b_entity, "%s", sv);
    if (extract_str(body, "life360_b_name", sv, sizeof sv))
        snprintf(settings.life360_b_name, sizeof settings.life360_b_name, "%s", sv);
    if (extract_str(body, "life360_c_entity", sv, sizeof sv))
        snprintf(settings.life360_c_entity, sizeof settings.life360_c_entity, "%s", sv);
    if (extract_str(body, "life360_c_name", sv, sizeof sv))
        snprintf(settings.life360_c_name, sizeof settings.life360_c_name, "%s", sv);
    if (extract_str(body, "curtain_entity", sv, sizeof sv))
        snprintf(settings.curtain_entity, sizeof settings.curtain_entity, "%s", sv);
    if (extract_str(body, "curtain_bat_a", sv, sizeof sv))
        snprintf(settings.curtain_bat_a, sizeof settings.curtain_bat_a, "%s", sv);
    if (extract_str(body, "curtain_bat_b", sv, sizeof sv))
        snprintf(settings.curtain_bat_b, sizeof settings.curtain_bat_b, "%s", sv);
    if (extract_str(body, "doorbell_entity", sv, sizeof sv))
        snprintf(settings.doorbell_entity, sizeof settings.doorbell_entity, "%s", sv);
    if (extract_str(body, "doorbell_camera", sv, sizeof sv))
        snprintf(settings.doorbell_camera, sizeof settings.doorbell_camera, "%s", sv);
    if (extract_int(body, "doorbell_seconds", &iv))
        settings.doorbell_seconds = (iv < 3 || iv > 300) ? 30 : iv;
    if (extract_str(body, "doorbell_stream_url", sv, sizeof sv))
        snprintf(settings.doorbell_stream_url, sizeof settings.doorbell_stream_url, "%s", sv);
    /* Integration LAN hosts */
    if (extract_str(body, "p1_elec_host", sv, sizeof sv))
        snprintf(settings.p1_elec_host, sizeof settings.p1_elec_host, "%s", sv);
    if (extract_str(body, "p1_water_host", sv, sizeof sv))
        snprintf(settings.p1_water_host, sizeof settings.p1_water_host, "%s", sv);
    if (extract_str(body, "vent_host", sv, sizeof sv))
        snprintf(settings.vent_host, sizeof settings.vent_host, "%s", sv);
    if (extract_str(body, "opnsense_host", sv, sizeof sv))
        snprintf(settings.opnsense_host, sizeof settings.opnsense_host, "%s", sv);
    /* 0=meteradapter 1=HomeWizard 2=ESPHome/Zuidwijk 3=HA — not a boolean */
    if (extract_int(body, "energy_source", &iv) && iv >= 0 && iv <= 3)
        settings.energy_source = iv;
    /* Auto-update */
    if (extract_int(body, "auto_update_enabled", &iv))settings.auto_update_enabled = !!iv;
    if (extract_int(body, "auto_update_hour", &iv))   settings.auto_update_hour = (iv < 0 || iv > 23) ? 2 : iv;
    /* Newsreader */
    if (extract_int(body, "news_enabled", &iv))       settings.news_enabled = !!iv;
    if (extract_str(body, "news_rss_url", sv, sizeof sv))
        snprintf(settings.news_rss_url, sizeof settings.news_rss_url, "%s", sv);
    int cal_touched = 0;
    if (extract_int(body, "calendar_enabled", &iv)) { settings.calendar_enabled = !!iv; cal_touched = 1; }
    if (extract_str(body, "calendar_ha_entity", sv, sizeof sv)) {
        snprintf(settings.calendar_ha_entity, sizeof settings.calendar_ha_entity, "%s", sv); cal_touched = 1; }
    if (extract_str(body, "calendar_ics_url", sv, sizeof sv)) {
        snprintf(settings.calendar_ics_url, sizeof settings.calendar_ics_url, "%s", sv); cal_touched = 1; }
    if (cal_touched) { extern void calendar_refresh_async(void); calendar_refresh_async(); }
    if (extract_int(body, "news_scroll_speed", &iv)) settings.news_scroll_speed = (iv > 0 && iv < 30) ? 30 : (iv > 150 ? 150 : iv);
    /* Tile auto-rotate */
    if (extract_int(body, "tile_rotate_enabled", &iv))settings.tile_rotate_enabled = !!iv;
    if (extract_int(body, "tile_rotate_seconds", &iv))settings.tile_rotate_seconds = iv < 3 ? 3 : (iv > 120 ? 120 : iv);
    if (extract_str(body, "tile_rotate_members", sv, sizeof sv))
        snprintf(settings.tile_rotate_members, sizeof settings.tile_rotate_members, "%s", sv);
    /* HomeWizard kWh meter + Energy Sockets (#37). IPs: digits and dots only,
     * like the device field (homewizard.c connects by IPv4 address). */
    #define HW_IP_COPY(dst) do { size_t o_ = 0; \
        for (const char * c_ = sv; *c_ && o_ + 1 < sizeof(dst); c_++) \
            if ((*c_ >= '0' && *c_ <= '9') || *c_ == '.') (dst)[o_++] = *c_; \
        (dst)[o_] = 0; } while (0)
    int hw_touched = 0;
    if (extract_str(body, "hw_kwh_host", sv, sizeof sv)) { HW_IP_COPY(settings.hw_kwh_host); hw_touched = 1; }
    if (extract_str(body, "hw_kwh2_host", sv, sizeof sv)) { HW_IP_COPY(settings.hw_kwh2_host); hw_touched = 1; }
    for (int i = 0; i < HW_SOCKET_MAX; i++) {
        char key[24];
        snprintf(key, sizeof key, "hw_socket_name_%d", i);
        if (extract_str(body, key, sv, sizeof sv)) {
            snprintf(settings.hw_socket_name[i], sizeof settings.hw_socket_name[i], "%s", sv);
            hw_touched = 1;
        }
        snprintf(key, sizeof key, "hw_socket_host_%d", i);
        if (extract_str(body, key, sv, sizeof sv)) { HW_IP_COPY(settings.hw_socket_host[i]); hw_touched = 1; }
    }
    #undef HW_IP_COPY
    settings_save();

    /* Apply what changed now, like the device's own Settings screens do —
     * the web client ignores the "restart" note below, so values used to sit
     * unused until the next UI restart (backlog #36). Each call is a no-op
     * or a start-once/wake, so re-sending unchanged fields is harmless. */
    if (strstr(body, "\"waste_") || strstr(body, "\"show_dim_waste\""))
        waste_ensure_started();
    if (strstr(body, "\"news_"))
        news_wake_fetch();
    if (!settings.client_mode) {
        if (hw_touched || strstr(body, "\"enable_p1_") || strstr(body, "\"p1_elec_host\"") ||
            strstr(body, "\"p1_water_host\""))
            homewizard_ensure_started();
        if (strstr(body, "\"otgw_host\""))
            otgw_start();
        if (settings.enable_domoticz && (strstr(body, "\"enable_domoticz\"") || strstr(body, "\"domoticz_")))
            domoticz_start();
        if (settings.enable_ha && settings.ha_host[0] &&
            (strstr(body, "\"ha_host\"") || strstr(body, "\"doorbell_")))
            ha_start();
    }
    char mq_after[sizeof mq_before];
    snprintf(mq_after, sizeof mq_after, "%s|%s|%s|%d|%d", settings.mqtt_host, settings.mqtt_user,
             settings.mqtt_pass, settings.mqtt_port, settings.mqtt_enabled);
    if (strcmp(mq_before, mq_after) != 0) {
        /* mirror for the Itho subscriber + HA discovery, as Settings > MQTT does */
        FILE * mf = fopen("/mnt/data/mqtt.cfg", "w");
        if (mf) {
            fprintf(mf, "%s:%s:%s\n", settings.mqtt_host, settings.mqtt_user, settings.mqtt_pass);
            fclose(mf); chmod("/mnt/data/mqtt.cfg", 0600);
        }
        mqtt_client_restart();
        vent_mqtt_reload();
        ha_mqtt_reload();
    }
    return send_status(fd, 200, "OK", "{\"ok\":1,\"note\":\"theme, client mode, OT bridge mode, "
                                      "ventilation, HA on/off and the energy source apply after a toonui restart\"}");
}

/* GET /api/update/status — current build + whether the background poll has
 * found a newer release (so the PWA can show "v0.9.13 → v0.9.14"). */
static int handle_update_status(int fd) {
    char body[512];
    snprintf(body, sizeof body,
        "{\"build\":\"%s\",\"available\":%d,\"latest\":\"%s\",\"channel\":%d,"
        "\"last_check_ok\":%d}",
        BUILD_VERSION, g_update_state.available,
        g_update_state.latest_version, settings.update_channel,
        g_update_state.last_check_ok);
    return send_status(fd, 200, "OK", body);
}

/* POST /api/update — kick the self-installer now (detached), same path as the
 * on-device "Update now" button. Pulls the newest matching release. */
static int handle_update_post(int fd) {
    update_install_now();
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* GET /api/animation[?effect=name[&duration=s]|stop=1] */
static int handle_animation_get(int fd, const char * path) {
    const char * q = strchr(path, '?');
    if (q) {
        q++;
        if (strstr(q, "stop=1") || strstr(q, "action=stop")) {
            anim_overlay_stop();
            return send_status(fd, 200, "OK", "{\"ok\":true,\"action\":\"stop\"}");
        }
        const char * ef = strstr(q, "effect=");
        if (ef) {
            ef += 7;
            char eff_name[32] = "";
            int i = 0;
            while (ef[i] && ef[i] != '&' && i < (int)sizeof(eff_name) - 1) {
                eff_name[i] = ef[i];
                i++;
            }
            eff_name[i] = 0;
            int dur = 25;
            const char * du = strstr(q, "duration=");
            if (du) dur = atoi(du + 9);
            if (dur <= 0) dur = 25;
            anim_overlay_trigger_by_name(eff_name, dur);
            char resp[128];
            snprintf(resp, sizeof resp, "{\"ok\":true,\"effect\":\"%s\",\"duration\":%d}", eff_name, dur);
            return send_status(fd, 200, "OK", resp);
        }
    }
    char resp[128];
    snprintf(resp, sizeof resp, "{\"ok\":true,\"active\":%d,\"effect\":\"%s\"}",
             anim_overlay_is_active(), anim_effect_name(anim_overlay_get_current()));
    return send_status(fd, 200, "OK", resp);
}

/* POST /api/animation — body: {"effect":"balloons","duration":25} or {"action":"stop"} */
static int handle_animation_post(int fd, const char * body) {
    if (!body) return send_status(fd, 400, "Bad Request", "{\"err\":\"missing body\"}");
    char act[32] = "", eff[32] = "";
    int dur = 25;
    extract_str(body, "action", act, sizeof act);
    extract_str(body, "effect", eff, sizeof eff);
    extract_int(body, "duration", &dur);
    if (dur <= 0) dur = 25;

    if (!strcmp(act, "stop") || !strcmp(eff, "stop")) {
        anim_overlay_stop();
        return send_status(fd, 200, "OK", "{\"ok\":true,\"action\":\"stop\"}");
    }
    if (eff[0]) {
        anim_overlay_trigger_by_name(eff, dur);
        char resp[128];
        snprintf(resp, sizeof resp, "{\"ok\":true,\"effect\":\"%s\",\"duration\":%d}", eff, dur);
        return send_status(fd, 200, "OK", resp);
    }
    return send_status(fd, 400, "Bad Request", "{\"err\":\"missing effect or action\"}");
}


/* ============================================================
 *  PWA login: cookie-session auth
 * ============================================================
 *
 * Default-enabled (settings.pwa_login_enabled). Two states:
 *   1. pwa_login_pass is empty  → every route 302s to /set-password
 *      until the user mints a password (form posts back to /set-password).
 *   2. pwa_login_pass is set    → /login form gates everything; valid
 *      submission mints a random session token, sent as an HttpOnly
 *      cookie. Subsequent requests carry it; dispatch() verifies before
 *      handing off to the normal route handlers.
 *
 * A few session tokens in memory (one per browser — phone, tablet, PC — so
 * signing in on one doesn't sign out another; the oldest is dropped when all
 * slots are used). 30-day expiry: the remote page is meant to stay usable on
 * a phone. /logout ends only the session of the browser that asks.
 *
 * Login body is application/x-www-form-urlencoded (so the login page
 * works in any browser without JS), not the JSON the rest of the API
 * uses. form_get below decodes it.
 */
#include <pthread.h>

static pthread_mutex_t g_sess_mu = PTHREAD_MUTEX_INITIALIZER;
#define SESS_SLOTS 8
static struct {
    char   token[33];                        /* 32 hex chars + NUL, "" = free */
    time_t expires;
} g_sess[SESS_SLOTS];

#define SESS_TTL_SECONDS (30 * 24 * 3600)
#define SESS_COOKIE_NAME "ft_sess"

/* URL-decode a single byte sequence in-place (length adjusts). */
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void url_decode(char * s) {
    char * w = s;
    for (char * r = s; *r; ) {
        if (*r == '+')      { *w++ = ' '; r++; }
        else if (*r == '%' && r[1] && r[2]) {
            int a = hexv(r[1]), b = hexv(r[2]);
            if (a >= 0 && b >= 0) { *w++ = (char)((a << 4) | b); r += 3; }
            else                  { *w++ = *r++; }
        } else *w++ = *r++;
    }
    *w = 0;
}

/* Form-encoded key extraction (a=1&b=hello%20world). out is URL-decoded. */
static int form_get(const char * body, const char * key, char * out, size_t outsz) {
    if (!body || !*body) return 0;
    size_t klen = strlen(key);
    const char * p = body;
    while (p && *p) {
        const char * eq = strchr(p, '=');
        const char * amp = strchr(p, '&');
        if (!eq || (amp && eq > amp)) {
            p = amp ? amp + 1 : NULL; continue;
        }
        if ((size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            const char * vstart = eq + 1;
            const char * vend   = amp ? amp : vstart + strlen(vstart);
            size_t L = (size_t)(vend - vstart);
            if (L > outsz - 1) L = outsz - 1;
            memcpy(out, vstart, L); out[L] = 0;
            url_decode(out);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

/* Cookie header parser — find SESS_COOKIE_NAME=<value> in a Cookie line.
 * The whole request (`req`) is passed; we look for the Cookie: header. */
static int extract_cookie(const char * req, const char * name,
                          char * out, size_t outsz) {
    const char * h = strcasestr(req, "\nCookie:");
    if (!h) return 0;
    h += 8;  /* past "\nCookie:" */
    /* The Cookie line continues until CRLF. Scan name=value pairs. */
    const char * eol = strchr(h, '\n');
    size_t llen = eol ? (size_t)(eol - h) : strlen(h);
    size_t nlen = strlen(name);
    for (size_t i = 0; i < llen; ) {
        while (i < llen && (h[i] == ' ' || h[i] == ';')) i++;
        if (i + nlen + 1 > llen) break;
        if (strncmp(h + i, name, nlen) == 0 && h[i + nlen] == '=') {
            size_t j = i + nlen + 1, k = j;
            while (k < llen && h[k] != ';' && h[k] != '\r') k++;
            size_t L = k - j; if (L > outsz - 1) L = outsz - 1;
            memcpy(out, h + j, L); out[L] = 0;
            return 1;
        }
        while (i < llen && h[i] != ';') i++;
    }
    return 0;
}

/* Cryptographically-ish-random 32 hex chars from /dev/urandom (good enough
 * for a single-device session token; we're not minting JWTs). */
static void mint_session_token(char out[33]) {
    unsigned char buf[16];
    FILE * f = fopen("/dev/urandom", "rb");
    if (f) { (void)fread(buf, 1, sizeof buf, f); fclose(f); }
    else { for (int i = 0; i < 16; i++) buf[i] = (unsigned char)(rand() & 0xff); }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i*2]   = hex[(buf[i] >> 4) & 0xf];
        out[i*2+1] = hex[ buf[i]       & 0xf];
    }
    out[32] = 0;
}

/* True if the request carries a valid, unexpired session cookie. */
static int request_is_authed(const char * req) {
    char tok[33] = "";
    if (!extract_cookie(req, SESS_COOKIE_NAME, tok, sizeof tok) || strlen(tok) != 32) return 0;
    int ok = 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_sess_mu);
    for (int i = 0; i < SESS_SLOTS && !ok; i++)
        ok = (g_sess[i].token[0] && strcmp(tok, g_sess[i].token) == 0 && now < g_sess[i].expires);
    pthread_mutex_unlock(&g_sess_mu);
    return ok;
}

/* New session in a free or expired slot, else the one expiring first.
 * Copies the token to out. */
static void session_create(char out[33]) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_sess_mu);
    int pick = 0;
    for (int i = 0; i < SESS_SLOTS; i++) {
        if (!g_sess[i].token[0] || g_sess[i].expires <= now) { pick = i; break; }
        if (g_sess[i].expires < g_sess[pick].expires) pick = i;
    }
    mint_session_token(g_sess[pick].token);
    g_sess[pick].expires = now + SESS_TTL_SECONDS;
    memcpy(out, g_sess[pick].token, 33);
    pthread_mutex_unlock(&g_sess_mu);
}

/* Ends the session named by the request's cookie (if any). */
static void session_end(const char * req) {
    char tok[33] = "";
    if (!extract_cookie(req, SESS_COOKIE_NAME, tok, sizeof tok) || strlen(tok) != 32) return;
    pthread_mutex_lock(&g_sess_mu);
    for (int i = 0; i < SESS_SLOTS; i++)
        if (g_sess[i].token[0] && strcmp(tok, g_sess[i].token) == 0) {
            g_sess[i].token[0] = 0;
            g_sess[i].expires = 0;
        }
    pthread_mutex_unlock(&g_sess_mu);
}

/* All sessions end — the password was changed or reset on the Toon. */
void pwa_sessions_clear(void) {
    pthread_mutex_lock(&g_sess_mu);
    memset(g_sess, 0, sizeof g_sess);
    pthread_mutex_unlock(&g_sess_mu);
}

/* Per-path allowlist — these routes never require auth (the login form
 * itself, the set-password form, and the PWA shell bits that browsers
 * fetch alongside a login page). */
static int path_is_public(const char * path) {
    if (!strcmp(path, "/login"))         return 1;
    /* Only public while no password exists (first-run setup). Once one is
     * set, an unauthenticated /set-password would let any LAN host replace
     * it and lock the owner out. */
    if (!strcmp(path, "/set-password"))  return !settings.pwa_login_pass[0];
    if (!strcmp(path, "/logout"))        return 1;
    if (!strcmp(path, "/logs") || !strcmp(path, "/logs/") || !strcmp(path, "/api/logs")) return 1;
    if (!strcmp(path, "/diag") || !strcmp(path, "/diag?view")) return 1;   /* no secrets in it */
    /* Boiler serial log (#64): frames + thermostat values only; port 80
     * serves the same file without login anyway. */
    if (!strcmp(path, "/ketellog") || !strcmp(path, "/ketellog?view")) return 1;
    if (!strcmp(path, "/log") || !strcmp(path, "/api/log") || !strncmp(path, "/log?", 5) || !strncmp(path, "/api/log?", 9) || !strncmp(path, "/log/", 5) || !strncmp(path, "/api/log/", 9)) return 1;
    if (!strncmp(path, "/api/v1/marketplace/", 20)) return 1;
    /* inverter app daemon on the Toon itself; the handler refuses other peers */
    if (!strcmp(path, "/api/v1/solar/source")) return 1;
    /* heartbeat of the Toon that drives this Toon's boiler; the handler
     * only takes private addresses (and only the master when one is set) */
    if (!strcmp(path, "/api/v1/boiler/remote")) return 1;
    return 0;
}

/* Send a 302 redirect with optional Set-Cookie. */
static int send_redirect(int fd, const char * location, const char * set_cookie) {
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "%s%s%s"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        location,
        set_cookie ? "Set-Cookie: " : "",
        set_cookie ? set_cookie     : "",
        set_cookie ? "\r\n"         : "");
    return sock_send_all(fd, hdr, (size_t)n);
}

/* Tiny HTML page shell — title + body — for /login and /set-password. */
static int send_html(int fd, int code, const char * status, const char * html) {
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        code, status, strlen(html));
    if (sock_send_all(fd, hdr, (size_t)hn) < 0) return -1;
    return sock_send_all(fd, html, strlen(html));
}

static const char * LOGIN_PAGE_FMT =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>freetoon login</title>"
    "<style>body{font-family:system-ui,sans-serif;background:#181818;color:#eee;"
    "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
    "form{background:#222;padding:24px;border-radius:8px;min-width:280px}"
    "h1{margin:0 0 16px;font-size:20px}"
    "label{display:block;margin:12px 0 4px;font-size:13px;color:#aaa}"
    "input{width:100%%;padding:10px;border:1px solid #444;background:#111;color:#fff;"
    "border-radius:4px;box-sizing:border-box;font-size:14px}"
    "button{margin-top:16px;width:100%%;padding:12px;background:#3a8;color:#fff;"
    "border:0;border-radius:4px;font-size:14px;cursor:pointer}"
    "button:hover{background:#4b9}.err{color:#f77;margin-top:8px;font-size:13px}</style>"
    "</head><body><form method=POST action=/login>"
    "<h1>freetoon</h1>"
    "<label>Username</label><input name=user autofocus value=\"%s\">"
    "<label>Password</label><input name=pass type=password>"
    "<button type=submit>Sign in</button>"
    "%s"
    "</form></body></html>";

/* HTML-attribute escape (the stored username is echoed into value="..."). */
static void html_attr_esc(char * dst, size_t dstsz, const char * s) {
    size_t o = 0;
    for (; s && *s && o + 7 < dstsz; s++) {
        switch (*s) {
        case '&':  memcpy(dst + o, "&amp;", 5);  o += 5; break;
        case '<':  memcpy(dst + o, "&lt;", 4);   o += 4; break;
        case '>':  memcpy(dst + o, "&gt;", 4);   o += 4; break;
        case '"':  memcpy(dst + o, "&quot;", 6); o += 6; break;
        case '\'': memcpy(dst + o, "&#39;", 5);  o += 5; break;
        default:   dst[o++] = *s;
        }
    }
    if (dstsz) dst[o < dstsz ? o : dstsz - 1] = 0;
}

static int handle_login_get(int fd, int wrong) {
    char html[2048], user_esc[200];
    html_attr_esc(user_esc, sizeof user_esc, settings.pwa_login_user);
    snprintf(html, sizeof html, LOGIN_PAGE_FMT,
             user_esc,
             wrong ? "<div class=err>Wrong username or password.</div>" : "");
    return send_html(fd, 200, "OK", html);
}

static int handle_login_post(int fd, const char * body) {
    char user[32] = "", pass[32] = "";
    form_get(body, "user", user, sizeof user);
    form_get(body, "pass", pass, sizeof pass);
    if (strcmp(user, settings.pwa_login_user) != 0 ||
        strcmp(pass, settings.pwa_login_pass) != 0) {
        return handle_login_get(fd, 1);
    }
    /* Auth OK — mint session, set cookie, redirect home. */
    char tok[33];
    session_create(tok);
    char cookie[128];
    snprintf(cookie, sizeof cookie,
             "%s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax",
             SESS_COOKIE_NAME, tok, SESS_TTL_SECONDS);
    return send_redirect(fd, "/", cookie);
}

static int handle_logout(int fd, const char * req) {
    session_end(req);
    char cookie[128];
    snprintf(cookie, sizeof cookie,
             "%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax", SESS_COOKIE_NAME);
    return send_redirect(fd, "/login", cookie);
}

static const char * SETPASS_PAGE_FMT =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>freetoon — set password</title>"
    "<style>body{font-family:system-ui,sans-serif;background:#181818;color:#eee;"
    "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
    "form{background:#222;padding:24px;border-radius:8px;min-width:320px}"
    "h1{margin:0 0 8px;font-size:20px}"
    "p{margin:0 0 16px;color:#aaa;font-size:13px}"
    "label{display:block;margin:12px 0 4px;font-size:13px;color:#aaa}"
    "input{width:100%%;padding:10px;border:1px solid #444;background:#111;color:#fff;"
    "border-radius:4px;box-sizing:border-box;font-size:14px}"
    "button{margin-top:16px;width:100%%;padding:12px;background:#3a8;color:#fff;"
    "border:0;border-radius:4px;font-size:14px;cursor:pointer}"
    ".err{color:#f77;margin-top:8px;font-size:13px}</style>"
    "</head><body><form method=POST action=/set-password>"
    "<h1>Set a password</h1>"
    "<p>Web access is enabled but no password is set yet. Choose one now to "
    "continue. You can change it later in Settings &rarr; Web Access.</p>"
    "<label>Username</label><input name=user value=\"%s\">"
    "<label>Password (min 8 chars)</label><input name=pass type=password autofocus>"
    "<label>Confirm</label><input name=pass2 type=password>"
    "<button type=submit>Save &amp; sign in</button>"
    "%s"
    "</form></body></html>";

static int handle_setpass_get(int fd, const char * err) {
    char html[2048], user_esc[200];
    html_attr_esc(user_esc, sizeof user_esc,
                  settings.pwa_login_user[0] ? settings.pwa_login_user : "admin");
    snprintf(html, sizeof html, SETPASS_PAGE_FMT,
             user_esc,
             err ? err : "");
    return send_html(fd, 200, "OK", html);
}

static int handle_setpass_post(int fd, const char * body) {
    /* First-run only. Changing an existing password goes through Settings
     * (authenticated); this form must never overwrite one. */
    if (settings.pwa_login_pass[0])
        return send_status(fd, 403, "Forbidden",
            "A password is already set. Change it in Settings -> Web Access.");
    char user[32] = "", pass[32] = "", pass2[32] = "";
    form_get(body, "user", user, sizeof user);
    form_get(body, "pass", pass, sizeof pass);
    form_get(body, "pass2", pass2, sizeof pass2);
    if (!user[0]) snprintf(user, sizeof user, "admin");
    /* The username is echoed back into the login page's HTML — keep it to
     * characters that need no escaping. */
    for (const char * p = user; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-' || c == '@'))
            return handle_setpass_get(fd, "<div class=err>Username may only contain letters, digits and . _ - @</div>");
    }
    if (strlen(pass) < 8)
        return handle_setpass_get(fd, "<div class=err>Password must be at least 8 characters.</div>");
    if (strcmp(pass, pass2) != 0)
        return handle_setpass_get(fd, "<div class=err>Passwords don't match.</div>");
    /* Persist + auto-login. */
    snprintf(settings.pwa_login_user, sizeof settings.pwa_login_user, "%s", user);
    snprintf(settings.pwa_login_pass, sizeof settings.pwa_login_pass, "%s", pass);
    settings_save();
    return handle_login_post(fd, body);
}

/* Returns 1 if dispatch may proceed with normal route handling, 0 if
 * this function already wrote the response (auth gate fired). */
static int auth_gate(int fd, const char * method, const char * path, const char * req) {
    if (!settings.pwa_login_enabled) return 1;
    if (path_is_public(path)) {
        /* Even public paths need to dispatch to their handlers — but the
         * login handlers live in dispatch() too, so we just return 1 and
         * let the route table handle them. */
        return 1;
    }
    /* If no password set, force the set-password flow. */
    if (!settings.pwa_login_pass[0]) {
        send_redirect(fd, "/set-password", NULL);
        return 0;
    }
    if (!request_is_authed(req)) {
        send_redirect(fd, "/login", NULL);
        return 0;
    }
    (void)method;
    return 1;
}

/* -------------------------------------------------------------------------
 * /api/v1 Core REST & SSE Event Stream
 * ------------------------------------------------------------------------- */

static int send_json_response(int fd, int code, const char * body) {
    size_t len = body ? strlen(body) : 0;
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n",
        code, (code == 200) ? "OK" : "Error", len);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return len ? sock_send_all(fd, body, len) : 0;
}

static int handle_v1_state(int fd) {
    char buf[4096];
    canonical_state_render_full_json(buf, sizeof(buf));
    return send_json_response(fd, 200, buf);
}

static int handle_v1_thermostat_get(int fd) {
    char buf[512];
    canonical_state_render_thermostat_json(buf, sizeof(buf));
    return send_json_response(fd, 200, buf);
}

static int handle_v1_setpoint_post(int fd, const char * body) {
    float v;
    if (!extract_float(body, "value", &v)) {
        return send_json_response(fd, 400, "{\"error\":\"Missing or invalid 'value' (expected e.g. 20.5)\"}");
    }
    if (canonical_state_set_setpoint(v) != 0) {
        return send_json_response(fd, 500, "{\"error\":\"Failed to apply setpoint\"}");
    }
    return send_json_response(fd, 200, "{\"ok\":true}");
}

static int handle_v1_preset_post(int fd, const char * body) {
    int s;
    if (!extract_int(body, "state", &s)) {
        return send_json_response(fd, 400, "{\"error\":\"Missing 'state' (0=Comfort, 1=Home, 2=Sleep, 3=Away, -1=Manual)\"}");
    }
    if (canonical_state_set_preset(s) != 0) {
        return send_json_response(fd, 500, "{\"error\":\"Failed to apply preset\"}");
    }
    return send_json_response(fd, 200, "{\"ok\":true}");
}

static int handle_v1_boiler_get(int fd) {
    char buf[512];
    canonical_state_render_boiler_json(buf, sizeof(buf));
    return send_json_response(fd, 200, buf);
}

static int handle_v1_boiler_mode_post(int fd, const char * body) {
    int mode;
    if (!extract_int(body, "mode", &mode)) {
        return send_json_response(fd, 400, "{\"error\":\"Missing 'mode' (0=OpenTherm, 1=On/Off)\"}");
    }
    if (canonical_state_set_boiler_mode(mode) != 0) {
        return send_json_response(fd, 500, "{\"error\":\"Failed to set boiler mode\"}");
    }
    return send_json_response(fd, 200, "{\"ok\":true}");
}

static int handle_v1_sensors_get(int fd) {
    char buf[512];
    canonical_state_render_sensors_json(buf, sizeof(buf));
    return send_json_response(fd, 200, buf);
}

static int handle_v1_energy_live_get(int fd) {
    char buf[512];
    canonical_state_render_energy_json(buf, sizeof(buf));
    return send_json_response(fd, 200, buf);
}

static int handle_v1_events(int fd) {
    const char * hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";
    if (sock_send_all(fd, hdr, strlen(hdr)) < 0) return -1;
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char * buf = malloc(8192);
    char * last_therm = malloc(512);
    char * last_boiler = malloc(512);
    char * last_sensors = malloc(512);
    char * last_energy = malloc(512);
    char * cur = malloc(512);

    if (!buf || !last_therm || !last_boiler || !last_sensors || !last_energy || !cur) {
        free(buf); free(last_therm); free(last_boiler);
        free(last_sensors); free(last_energy); free(cur);
        return -1;
    }

    last_therm[0] = last_boiler[0] = last_sensors[0] = last_energy[0] = 0;

    /* Push full initial canonical state */
    int n = canonical_state_render_full_json(buf, 8192);
    if (n > 0) {
        char prefix[64];
        int pn = snprintf(prefix, sizeof(prefix), "event: state\ndata: ");
        if (sock_send_all(fd, prefix, pn) < 0 ||
            sock_send_all(fd, buf, n) < 0 ||
            sock_send_all(fd, "\n\n", 2) < 0) {
            goto cleanup;
        }
    }

    /* Baseline snapshots */
    canonical_state_render_thermostat_json(last_therm, 512);
    canonical_state_render_boiler_json(last_boiler, 512);
    canonical_state_render_sensors_json(last_sensors, 512);
    canonical_state_render_energy_json(last_energy, 512);

    int ticks = 0;
    while (1) {
        usleep(1000 * 1000); /* 1 Hz event loop */
        ticks++;

        /* Thermostat diff */
        canonical_state_render_thermostat_json(cur, 512);
        if (strcmp(cur, last_therm) != 0) {
            int en = snprintf(buf, 8192, "event: thermostat\ndata: %s\n\n", cur);
            if (sock_send_all(fd, buf, en) < 0) break;
            strcpy(last_therm, cur);
        }

        /* Boiler diff */
        canonical_state_render_boiler_json(cur, 512);
        if (strcmp(cur, last_boiler) != 0) {
            int en = snprintf(buf, 8192, "event: boiler\ndata: %s\n\n", cur);
            if (sock_send_all(fd, buf, en) < 0) break;
            strcpy(last_boiler, cur);
        }

        /* Sensors diff */
        canonical_state_render_sensors_json(cur, 512);
        if (strcmp(cur, last_sensors) != 0) {
            int en = snprintf(buf, 8192, "event: sensors\ndata: %s\n\n", cur);
            if (sock_send_all(fd, buf, en) < 0) break;
            strcpy(last_sensors, cur);
        }

        /* Energy diff */
        canonical_state_render_energy_json(cur, 512);
        if (strcmp(cur, last_energy) != 0) {
            int en = snprintf(buf, 8192, "event: energy\ndata: %s\n\n", cur);
            if (sock_send_all(fd, buf, en) < 0) break;
            strcpy(last_energy, cur);
        }

        /* Heartbeat every 10 seconds */
        if (ticks >= 10) {
            ticks = 0;
            if (sock_send_all(fd, ": keepalive\n\n", 13) < 0) break;
        }
    }

cleanup:
    free(buf);
    free(last_therm);
    free(last_boiler);
    free(last_sensors);
    free(last_energy);
    free(cur);
    return 0;
}

static int handle_v1_app_state_post(int fd, const char * app_id, const char * body) {
    char val[INTEG_VALUE_MAX] = "";
    char sub[INTEG_VALUE_MAX] = "";
    char alert[INTEG_VALUE_MAX] = "";
    char title[INTEG_NAME_MAX] = "";
    extract_str(body, "value", val, sizeof(val));
    extract_str(body, "subtitle", sub, sizeof(sub));
    extract_str(body, "alert", alert, sizeof(alert));
    extract_str(body, "title", title, sizeof(title));

    tile_slots_set_value(app_id, val[0] ? val : NULL, sub[0] ? sub : NULL, alert[0] ? alert : NULL);
    if (title[0]) tile_slots_set_title(app_id, title);
    return send_json_response(fd, 200, "{\"ok\":true}");
}

static int handle_log_get(int fd, const char * path) {
    const char * filename = "toonui.log";
    if (strstr(path, "toonui.log.1")) filename = "toonui.log.1";
    else if (strstr(path, "ui_launcher.log")) filename = "ui_launcher.log";
    else if (strstr(path, "x11vnc.log")) filename = "x11vnc.log";

    char filepath[128];
    snprintf(filepath, sizeof(filepath), "/var/volatile/tmp/%s", filename);

    FILE * f = fopen(filepath, "rb");
    if (!f) {
        snprintf(filepath, sizeof(filepath), "/tmp/%s", filename);
        f = fopen(filepath, "rb");
    }
    if (!f) return send_status(fd, 404, "Not Found", "Log file not found\n");

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char hdr[384];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Disposition: inline; filename=\"%s\"\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n", filename, sz);
    if (sock_send_all(fd, hdr, hn) < 0) { fclose(f); return -1; }

    char buf[4096];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (sock_send_all(fd, buf, rd) < 0) break;
    }
    fclose(f);
    return 0;
}

/* /diag: run /mnt/data/freetoon_diag.sh now and stream its report as a
 * download (support without SSH). The same report is refreshed every minute
 * on port 80 as /freetoon-diag.txt by ui_launcher.sh, also under the stock
 * UI, so users can send one from each UI. Public like /logs: the script only
 * prints process names and a fixed set of non-secret settings. Each
 * connection has its own thread, so the few seconds it takes block nothing. */
static int handle_diag_get(int fd, int inline_view) {
    /* Via sh, so a copy without the exec bit (scp/sftp from Windows) works. */
    if (access("/mnt/data/freetoon_diag.sh", R_OK) != 0)
        return send_status(fd, 404, "Not Found", "freetoon_diag.sh is not installed\n");
    FILE * p = popen("sh /mnt/data/freetoon_diag.sh 2>&1", "r");
    if (!p) return send_status(fd, 500, "Internal Server Error", "cannot run freetoon_diag.sh\n");

    char fname[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(fname, sizeof fname, "freetoon-diag-%Y%m%d-%H%M.txt", &tm);

    /* No Content-Length (the size isn't known up front): the body ends when
     * the connection closes. */
    char hdr[384];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Disposition: %s; filename=\"%s\"\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n", inline_view ? "inline" : "attachment", fname);
    if (sock_send_all(fd, hdr, hn) < 0) { pclose(p); return -1; }

    char buf[4096];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), p)) > 0) {
        if (sock_send_all(fd, buf, rd) < 0) break;
    }
    pclose(p);
    return 0;
}

/* /ketellog: the boiler serial capture (#64) as a view or a download. */
static int handle_ketellog_get(int fd, int inline_view) {
    FILE * f = fopen(KETELLOG_LOG, "r");
    if (!f) return send_status(fd, 404, "Not Found", "no boiler log yet (start one first)\n");
    struct stat st;
    long sz = (fstat(fileno(f), &st) == 0) ? (long)st.st_size : 0;
    char fname[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(fname, sizeof fname, "freetoon-ketellog-%Y%m%d-%H%M.txt", &tm);
    /* No Content-Length: the tap may still append while we send. */
    char hdr[384];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Disposition: %s; filename=\"%s\"\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n", inline_view ? "inline" : "attachment", fname);
    if (sock_send_all(fd, hdr, hn) < 0) { fclose(f); return -1; }
    char buf[4096];
    size_t rd;
    long sent = 0;
    while (sent < sz && (rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (sock_send_all(fd, buf, rd) < 0) break;
        sent += (long)rd;
    }
    fclose(f);
    return 0;
}

static int handle_logs_index_get(int fd) {
    const char * files[] = {
        "toonui.log",
        "toonui.log.1",
        "ui_launcher.log",
        "x11vnc.log"
    };
    int nfiles = sizeof(files) / sizeof(files[0]);

    char body[12288];  /* 4 KB was nearly full before the diagnose block; + ketel-log block */
    int pos = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>FreeToon Logs</title>"
        "<style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f1a2a;color:#e2e8f0;margin:0;padding:24px;line-height:1.5}"
        ".card{max-width:600px;margin:0 auto;background:#1e293b;border-radius:12px;padding:24px;box-shadow:0 4px 12px rgba(0,0,0,0.3)}"
        "h1{margin-top:0;font-size:22px;color:#38bdf8;display:flex;align-items:center;gap:10px}"
        "p{color:#94a3b8;font-size:14px;margin-bottom:20px}"
        "ul{list-style:none;padding:0;margin:0}"
        "li{display:flex;justify-content:space-between;align-items:center;padding:12px 14px;background:#0f172a;border-radius:8px;margin-bottom:10px}"
        ".fname{font-weight:600;font-family:monospace;font-size:15px;color:#f8fafc}"
        ".fsize{font-size:13px;color:#64748b;margin-left:8px}"
        ".actions{display:flex;gap:8px}"
        ".btn{background:#2563eb;color:#fff;text-decoration:none;padding:6px 14px;border-radius:6px;font-size:13px;font-weight:500;transition:background 0.2s}"
        ".btn:hover{background:#1d4ed8}"
        ".btn-sec{background:#334155;color:#e2e8f0}"
        ".btn-sec:hover{background:#475569}"
        "</style></head><body><div class=\"card\">"
        "<h1><svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><path d=\"M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z\"/><polyline points=\"14 2 14 8 20 8\"/><line x1=\"16\" y1=\"13\" x2=\"8\" y2=\"13\"/><line x1=\"16\" y1=\"17\" x2=\"8\" y2=\"17\"/><polyline points=\"10 9 9 9 8 9\"/></svg> FreeToon Logs</h1>"
        "<p>Logfiles direct vanuit het geheugen (max. 256 KB per bestand):</p><ul>");

    for (int i = 0; i < nfiles; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/var/volatile/tmp/%s", files[i]);
        struct stat st;
        if (stat(path, &st) != 0) {
            snprintf(path, sizeof(path), "/tmp/%s", files[i]);
            if (stat(path, &st) != 0) st.st_size = -1;
        }

        if (st.st_size >= 0) {
            char sz_str[32];
            if (st.st_size < 1024) snprintf(sz_str, sizeof(sz_str), "%ld B", (long)st.st_size);
            else snprintf(sz_str, sizeof(sz_str), "%.1f KB", (double)st.st_size / 1024.0);

            pos += snprintf(body + pos, sizeof(body) - pos,
                "<li><div><span class=\"fname\">%s</span><span class=\"fsize\">(%s)</span></div>"
                "<div class=\"actions\"><a class=\"btn btn-sec\" href=\"/log?file=%s\" target=\"_blank\">Bekijken</a>"
                "<a class=\"btn\" href=\"/log?file=%s\" download=\"%s\">Download</a></div></li>",
                files[i], sz_str, files[i], files[i], files[i]);
        }
    }

    pos += snprintf(body + pos, sizeof(body) - pos,
        "</ul>"
        "<p style=\"margin-top:20px\">Diagnose voor support: processen, OT Bridge, ketelstatus en "
        "de laatste ketelregels uit de logs, zonder wachtwoorden. Werkt ook onder de Eneco-software: "
        "<span style=\"font-family:monospace\">http://&lt;ip-van-de-toon&gt;/freetoon-diag.txt</span> "
        "(elke minuut ververst).</p>"
        "<ul><li><div><span class=\"fname\">freetoon-diag.txt</span></div>"
        "<div class=\"actions\"><a class=\"btn btn-sec\" href=\"/diag?view\" target=\"_blank\">Bekijken</a>"
        "<a class=\"btn\" href=\"/diag\" download>Download</a></div></li></ul>");

    /* Boiler serial capture (#64): only where the tap is installed. */
    ketellog_status_t kl;
    ketellog_get_status(&kl);
    if (kl.available) {
        char state[200];
        if (kl.starting)
            snprintf(state, sizeof state, "Bezig met starten (happ_thermstat herstart, max. 1 minuut)...");
        else if (kl.active)
            snprintf(state, sizeof state, "Loopt nog %ld min, %ld kB.", kl.minutes_left, kl.log_bytes / 1024);
        else if (kl.result[0] && strncmp(kl.result, "OK", 2) && strcmp(kl.result, "stopped"))
            snprintf(state, sizeof state, "Fout: %s", kl.result);   /* script text, no user input */
        else if (kl.log_bytes > 0)
            snprintf(state, sizeof state, "Gestopt, log %ld kB.", kl.log_bytes / 1024);
        else
            snprintf(state, sizeof state, "Uit.");
        pos += snprintf(body + pos, sizeof(body) - pos,
            "<p style=\"margin-top:20px\">Ketel-log voor support: legt 60 minuten vast wat de thermostaat "
            "via de seri&euml;le lijn met de ketelmodule uitwisselt, ook na wisselen naar de Eneco-software "
            "(ophalen: <span style=\"font-family:monospace\">http://&lt;ip-van-de-toon&gt;/freetoon-ketellog.txt</span>). "
            "Starten herstart happ_thermstat even.<br><b>%s</b></p>"
            "<ul><li><div><span class=\"fname\">freetoon-ketellog.txt</span></div>"
            "<div class=\"actions\">"
            "<form method=\"post\" action=\"/ketellog/%s\" style=\"margin:0\">"
            "<button class=\"btn btn-sec\" style=\"border:0;cursor:pointer\">%s</button></form>"
            "%s</div></li></ul>",
            state, (kl.active || kl.starting) ? "stop" : "start",
            (kl.active || kl.starting) ? "Stoppen" : "Starten (60 min)",
            kl.log_bytes > 0 ? "<a class=\"btn btn-sec\" href=\"/ketellog?view\" target=\"_blank\">Bekijken</a>"
                               "<a class=\"btn\" href=\"/ketellog\" download>Download</a>" : "");
    }

    pos += snprintf(body + pos, sizeof(body) - pos,
        "<p style=\"margin-top:20px\">Koppelingen: <a href=\"/homey\" style=\"color:#38bdf8\">Homey (adres en API-sleutel)</a>"
        " &middot; <a href=\"/solar\" style=\"color:#38bdf8\">Zonnepanelen (omvormer)</a>"
        " &middot; <a href=\"/schema\" style=\"color:#38bdf8\">Schakelschema</a></p>"
        "<p style=\"margin-top:20px;text-align:center\"><a href=\"/\" class=\"btn btn-sec\">&larr; Terug naar Toon</a></p>"
        "</div></body></html>");

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n", pos);

    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, body, pos);
}

/* ---------------------------------------------------------------- /homey
 * Homey address + API key from a browser (BACKLOG #28): a ~110-character key
 * is tedious on the Toon keyboard. Behind the normal login gate. The key is
 * write-only: the page shows only whether one is set.
 *
 * With the web login off, port 10081 is open to the whole LAN and any web
 * page can make the browser POST a form here. Two rules keep the key from
 * being redirected to somebody else's server:
 *   - a cross-site POST (Origin/Referer host != Host) is refused;
 *   - changing the address requires entering the key again in the same
 *     submit, so the stored key is never sent to a new address unasked. */

/* Host part of the request's Host header, or of an Origin/Referer URL. */
static int req_header(const char * req, const char * name, char * out, size_t n) {
    char pat[32];
    snprintf(pat, sizeof pat, "\n%s:", name);
    const char * h = strcasestr(req, pat);
    if (!h) return 0;
    h += strlen(pat);
    while (*h == ' ' || *h == '\t') h++;
    size_t L = strcspn(h, "\r\n");
    if (L >= n) L = n - 1;
    memcpy(out, h, L); out[L] = 0;
    return 1;
}
static void url_hostport(const char * url, char * out, size_t n) {
    const char * p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t L = strcspn(p, "/?#");
    if (L >= n) L = n - 1;
    memcpy(out, p, L); out[L] = 0;
}
static int homey_same_origin(const char * req) {
    char host[128] = "", src[256] = "", hp[128];
    if (!req_header(req, "Host", host, sizeof host)) return 0;
    if (req_header(req, "Origin", src, sizeof src)) {
        if (!strcmp(src, "null")) return 0;   /* sandboxed/opaque origin: never ours */
    } else if (!req_header(req, "Referer", src, sizeof src)) {
        return 1;   /* no Origin and no Referer: not a cross-site browser form (curl, scripts) */
    }
    url_hostport(src, hp, sizeof hp);
    return strcasecmp(hp, host) == 0;
}

static int homey_host_ok(const char * h) {
    if (!*h) return 1;   /* empty = clear */
    for (const char * p = h; *p; p++)
        if (!(isalnum((unsigned char)*p) || strchr(".:-_/[]", *p))) return 0;
    return 1;
}

static int handle_homey_get(int fd, const char * msg) {
    char host_esc[200], key[160], status[200];
    html_attr_esc(host_esc, sizeof host_esc, settings.homey_host);
    homey_get_key(key, sizeof key);
    int st = homey_state.status;
    homey_lock();
    int nd = homey_state.ndev, nf = homey_state.nflow;
    homey_unlock();
    if (!settings.enable_homey || !settings.homey_host[0]) snprintf(status, sizeof status, "Uit.");
    else if (st == HOMEY_ST_OK)
        snprintf(status, sizeof status, "Verbonden: %d apparaten, %d flows (%lds geleden bijgewerkt).",
                 nd, nf, (long)time(NULL) - homey_state.last_ok);
    else if (st == HOMEY_ST_AUTH)    snprintf(status, sizeof status, "API-sleutel geweigerd (verkeerde sleutel of te weinig rechten).");
    else if (st == HOMEY_ST_NOCONN)  snprintf(status, sizeof status, "Geen verbinding - controleer het adres.");
    else if (st == HOMEY_ST_BADDATA) snprintf(status, sizeof status, "Onverwacht antwoord - is dit een Homey Pro?");
    else                             snprintf(status, sizeof status, "Verbinden...");
    /* While the outcome is not known yet (just saved, or the poller is still
     * trying the new address/key), reload every 2 s until it is. */
    int refresh = settings.enable_homey && settings.homey_host[0] && st == HOMEY_ST_OFF;

    char body[6144];
    int pos = snprintf(body, sizeof body,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">%s"
        "<title>FreeToon - Homey</title><style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f1a2a;color:#e2e8f0;margin:0;padding:24px;line-height:1.5}"
        ".card{max-width:600px;margin:0 auto;background:#1e293b;border-radius:12px;padding:24px}"
        "h1{margin-top:0;font-size:22px;color:#38bdf8}p{color:#94a3b8;font-size:14px}"
        "label{display:block;margin:14px 0 4px;font-size:14px}"
        "input[type=text],input[type=password]{width:100%%;box-sizing:border-box;padding:9px;border-radius:6px;border:1px solid #334155;background:#0f172a;color:#f8fafc;font-size:15px}"
        ".btn{background:#2563eb;color:#fff;border:0;padding:9px 18px;border-radius:6px;font-size:14px;cursor:pointer;margin-top:18px;text-decoration:none;display:inline-block}"
        ".btn-sec{background:#334155;color:#e2e8f0}.st{background:#0f172a;border-radius:8px;padding:10px 14px;margin-top:18px}"
        ".msg{color:#fbbf24}</style></head><body><div class=\"card\"><h1>Homey</h1>"
        "<p>Adres en API-sleutel van een Homey Pro (2023). Een API-sleutel maak je in de Homey web-app "
        "(my.homey.app) &gt; Instellingen &gt; API-sleutels, met de rechten: apparaten bekijken en bedienen, "
        "zones bekijken, flows bekijken en starten.</p>"
        "%s%s%s"
        "<form method=\"post\" action=\"/homey\">"
        "<label>Adres (ip of ip:poort)</label>"
        "<input type=\"text\" name=\"host\" value=\"%s\" placeholder=\"192.168.1.50\" autocomplete=\"off\">"
        "<label>API-sleutel%s</label>"
        "<input type=\"password\" name=\"key\" placeholder=\"%s\" autocomplete=\"off\">"
        "<button class=\"btn\">Opslaan + verbinden</button></form>"
        "<div class=\"st\"><b>Status:</b> %s</div>"
        "<p style=\"margin-top:20px;text-align:center\"><a href=\"/\" class=\"btn btn-sec\">&larr; Terug naar Toon</a></p>"
        "</div></body></html>",
        refresh ? "<meta http-equiv=\"refresh\" content=\"2\">" : "",
        msg ? "<p class=\"msg\">" : "", msg ? msg : "", msg ? "</p>" : "",
        host_esc,
        key[0] ? " (ingesteld - leeg laten om te houden)" : "",
        key[0] ? "********" : "API key",
        status);
    if (pos < 0 || pos >= (int)sizeof body) return send_status(fd, 500, "Internal Server Error", "page too large");
    char hdr[200];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\n"
        "Cache-Control: no-store\r\nX-Frame-Options: DENY\r\nConnection: close\r\n\r\n", pos);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, body, pos);
}

static int handle_homey_post(int fd, const char * req, const char * body) {
    if (!homey_same_origin(req))
        return send_status(fd, 403, "Forbidden", "cross-site request refused");
    char host[sizeof settings.homey_host], key[160] = "";
    if (!form_get(body, "host", host, sizeof host)) host[0] = 0;
    form_get(body, "key", key, sizeof key);
    /* trim */
    char * h = host; while (*h == ' ') h++;
    memmove(host, h, strlen(h) + 1);
    for (size_t L = strlen(host); L && host[L - 1] == ' '; ) host[--L] = 0;

    if (!homey_host_ok(host))
        return handle_homey_get(fd, "Ongeldig adres: alleen letters, cijfers en . : - _ / [ ].");
    if (strcmp(host, settings.homey_host) != 0 && !key[0]) {
        char cur[160]; homey_get_key(cur, sizeof cur);
        if (cur[0])
            return handle_homey_get(fd, "Vul bij een nieuw adres ook de API-sleutel opnieuw in.");
    }
    if (key[0] && homey_set_key(key) != 0)
        return handle_homey_get(fd, "Dit is geen geldige API-sleutel (alleen letters, cijfers en : . _ -).");
    snprintf(settings.homey_host, sizeof settings.homey_host, "%s", host);
    settings.enable_homey = host[0] ? 1 : 0;
    settings_save();
    if (settings.enable_homey) homey_start();   /* starts, or wakes it with the new values */
    fprintf(stderr, "[pwa] homey settings saved via web (host %s, key %s)\n",
            host[0] ? host : "-", key[0] ? "changed" : "kept");
    return send_redirect(fd, "/homey?saved", NULL);
}

/* ---------------------------------------------------------------- /solar
 * Solar production (BACKLOG #60). An inverter app (marketplace, e.g.
 * "inverters") POSTs its readings to /api/v1/solar/source; this page shows
 * which source the core uses and the app's settings form, generated from
 * the "settings" block in its manifest (app_settings.c). Same cross-site
 * rule as /homey, and a changed address needs its secrets typed again. */

/* POST /api/v1/boiler/remote: heartbeat of the Toon that drives this Toon's
 * boiler (boiler_remote.h). Private IPv4 peers only; when this Toon is a
 * client of a master given by IP, only that master. */
static int handle_boiler_remote_post(int fd, const char * body) {
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    if (getpeername(fd, (struct sockaddr *)&peer, &pl) != 0 || peer.sin_family != AF_INET)
        return send_status(fd, 403, "Forbidden", "{\"err\":\"ipv4 only\"}");
    uint32_t a = ntohl(peer.sin_addr.s_addr);
    int priv = (a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8 || (a >> 24) == 127;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
    struct in_addr m;
    if (!priv || (settings.master_host[0] && inet_pton(AF_INET, settings.master_host, &m) == 1 &&
                  m.s_addr != peer.sin_addr.s_addr && (a >> 24) != 127))
        return send_status(fd, 403, "Forbidden", "{\"err\":\"not from this LAN / master\"}");
    if (boiler_remote_heartbeat(ip, body) != 0)
        return send_status(fd, 400, "Bad Request", "{\"err\":\"need {\\\"on\\\":0|1}\"}");
    return send_json_response(fd, 200, "{\"ok\":true}");
}

/* POST /api/v1/solar/source: only from the Toon itself (the app's daemon). */
static int handle_solar_source_post(int fd, const char * body) {
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    if (getpeername(fd, (struct sockaddr *)&peer, &pl) != 0 || peer.sin_family != AF_INET ||
        ntohl(peer.sin_addr.s_addr) != 0x7f000001)
        return send_status(fd, 403, "Forbidden", "{\"err\":\"local only\"}");
    if (solar_app_post(body) != 0)
        return send_status(fd, 400, "Bad Request", "{\"err\":\"need {\\\"app\\\":...}\"}");
    return send_json_response(fd, 200, "{\"ok\":true}");
}

static time_t g_solar_saved_at;

/* Every source that counts now (they add up, see energy_solar_w()). */
static const char * solar_source_name(char * buf, size_t n) {
    solar_app_state_t a;
    solar_app_get(&a);
    size_t o = 0;
    buf[0] = 0;
#define SRC_ADD(...) do { if (o < n) { if (o) o += (size_t)snprintf(buf + o, n - o, " + "); \
                          if (o < n) o += (size_t)snprintf(buf + o, n - o, __VA_ARGS__); } } while (0)
    if (solar_app_active()) SRC_ADD("Omvormer-app (%s)", a.brand);
    if ((settings.hw_kwh_host[0] || settings.hw_kwh2_host[0]) && hw_state.connected_kwh)
        SRC_ADD("HomeWizard kWh-meter%s", settings.hw_kwh_host[0] && settings.hw_kwh2_host[0] ? "s" : "");
    if (settings.enable_ha && ha_state.have_solar) SRC_ADD("Home Assistant");
    if (meteradapter_have_solar()) SRC_ADD("Zonnesensor op de meteradapter");
#undef SRC_ADD
    if (!buf[0]) snprintf(buf, n, "geen");
    return buf;
}

static int handle_solar_get(int fd, const char * msg) {
    size_t cap = 24 * 1024;
    char * body = malloc(cap);
    if (!body) return send_status(fd, 500, "Internal Server Error", "oom");
    char app_id[48] = "", app_name[80] = "", src[80], ename[200];
    int have_app = app_settings_find_solar(app_id, sizeof app_id, app_name, sizeof app_name);
    solar_app_state_t a;
    solar_app_get(&a);
    time_t now = time(NULL);
    /* Just saved: reload every 3 s until the app has posted with the new settings (max 30 s). */
    int refresh = g_solar_saved_at && now - g_solar_saved_at < 30 && a.last_post <= g_solar_saved_at;

    int pos = snprintf(body, cap,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">%s"
        "<title>FreeToon - Zonnepanelen</title><style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f1a2a;color:#e2e8f0;margin:0;padding:24px;line-height:1.5}"
        ".card{max-width:600px;margin:0 auto;background:#1e293b;border-radius:12px;padding:24px}"
        "h1{margin-top:0;font-size:22px;color:#38bdf8}h2{font-size:17px;color:#38bdf8;margin:24px 0 4px}p{color:#94a3b8;font-size:14px}"
        "label{display:block;margin:14px 0 4px;font-size:14px}.cb label{display:inline;margin-left:6px}"
        "input[type=text],input[type=password],select{width:100%%;box-sizing:border-box;padding:9px;border-radius:6px;border:1px solid #334155;background:#0f172a;color:#f8fafc;font-size:15px}"
        ".help{margin:4px 0 0;font-size:13px}"
        ".btn{background:#2563eb;color:#fff;border:0;padding:9px 18px;border-radius:6px;font-size:14px;cursor:pointer;margin-top:18px;text-decoration:none;display:inline-block}"
        ".btn-sec{background:#334155;color:#e2e8f0}.st{background:#0f172a;border-radius:8px;padding:10px 14px;margin-top:14px}"
        ".msg{color:#fbbf24}</style></head><body><div class=\"card\"><h1>Zonnepanelen</h1>",
        refresh ? "<meta http-equiv=\"refresh\" content=\"3\">" : "");
    if (msg) {
        html_attr_esc(ename, sizeof ename, msg);
        pos += snprintf(body + pos, cap - pos, "<p class=\"msg\">%s</p>", ename);
    }
    pos += snprintf(body + pos, cap - pos,
        "<div class=\"st\"><b>Bron:</b> %s<br><b>Nu:</b> %.0f W", solar_source_name(src, sizeof src),
        energy_have_solar() ? (double)energy_solar_w() : 0.0);
    if (have_app && a.last_post) {
        char m[200];
        html_attr_esc(m, sizeof m, a.msg);
        if (a.ok) pos += snprintf(body + pos, cap - pos, "<br><b>Omvormer:</b> %.0f W", a.power_w);
        else pos += snprintf(body + pos, cap - pos, "<br><b>Omvormer:</b> <span class=\"msg\">%s</span>", m);
        if (a.today_wh >= 0) pos += snprintf(body + pos, cap - pos, ", vandaag %.1f kWh", a.today_wh / 1000.0);
        if (a.total_wh >= 0) pos += snprintf(body + pos, cap - pos, ", totaal %.0f kWh", a.total_wh / 1000.0);
        pos += snprintf(body + pos, cap - pos, " <small>(%lds geleden)</small>", (long)(now - a.last_post));
    } else if (have_app) {
        pos += snprintf(body + pos, cap - pos, "<br><b>Omvormer:</b> nog geen bericht van de app");
    }
    pos += snprintf(body + pos, cap - pos, "</div>");

    if (have_app) {
        html_attr_esc(ename, sizeof ename, app_name);
        pos += snprintf(body + pos, cap - pos, "<h2>Omvormer (app %s)</h2>", ename);
        if (app_settings_form(app_id, "/solar", body, cap, &pos) != 0)
            pos += snprintf(body + pos, cap - pos, "<p>De app heeft geen instellingen.</p>");
    } else {
        pos += snprintf(body + pos, cap - pos,
            "<h2>Omvormer</h2><p>Wil je je omvormer rechtstreeks uitlezen (Enphase, Solis, Omnik)? "
            "Installeer dan de app <b>Omvormers</b> via de Marketplace op de Toon. "
            "Daarna stel je hem hier in.</p>");
    }
    pos += snprintf(body + pos, cap - pos,
        "<p style=\"margin-top:20px;text-align:center\"><a href=\"/\" class=\"btn btn-sec\">&larr; Terug naar Toon</a></p>"
        "</div></body></html>");
    if (pos < 0 || (size_t)pos >= cap) { free(body); return send_status(fd, 500, "Internal Server Error", "page too large"); }
    char hdr[200];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\n"
        "Cache-Control: no-store\r\nX-Frame-Options: DENY\r\nConnection: close\r\n\r\n", pos);
    int rc = sock_send_all(fd, hdr, hn) < 0 ? -1 : sock_send_all(fd, body, pos);
    free(body);
    return rc;
}

static int handle_solar_post(int fd, const char * req, const char * body) {
    if (!homey_same_origin(req))
        return send_status(fd, 403, "Forbidden", "cross-site request refused");
    const char * cl = strcasestr(req, "Content-Length:");
    if (cl && strlen(body) < (size_t)atoi(cl + 15))
        return handle_solar_get(fd, "Het formulier was te groot en is niet opgeslagen.");
    char app_id[48] = "", app_name[80] = "", msg[300];
    if (!app_settings_find_solar(app_id, sizeof app_id, app_name, sizeof app_name))
        return handle_solar_get(fd, "Geen omvormer-app geïnstalleerd.");
    if (app_settings_save(app_id, body, msg, sizeof msg) != 0)
        return handle_solar_get(fd, msg);
    g_solar_saved_at = time(NULL);
    fprintf(stderr, "[pwa] solar: settings of app %s saved via web\n", app_id);
    return send_redirect(fd, "/solar?saved", NULL);
}

/* --------------------------------------------------------------- /schema
 * Schakelschema (switch_schedule.c): the rules that switch plugs and lights
 * on or off at set times, also editable on the Toon (Apparaten ->
 * Schakelschema). Behind the web login like /homey; POSTs only from this
 * page (same origin). */
struct sched_opts { char * buf; size_t cap; int pos; const char * sel; char group[48]; int open; };
static void sched_opt(void * ctx, const char * group, const char * ref,
                      const char * name, int available, int dimmable) {
    (void)dimmable;
    struct sched_opts * o = ctx;
    if (o->pos < 0 || (size_t)o->pos + 1024 > o->cap) return;   /* page full: stop adding */
    char g[96], r[160], n[200];
    if (strcmp(group, o->group)) {
        html_attr_esc(g, sizeof g, group);
        o->pos += snprintf(o->buf + o->pos, o->cap - o->pos, "%s<optgroup label=\"%s\">",
                           o->open ? "</optgroup>" : "", g);
        snprintf(o->group, sizeof o->group, "%s", group);
        o->open = 1;
    }
    html_attr_esc(r, sizeof r, ref);
    html_attr_esc(n, sizeof n, name);
    o->pos += snprintf(o->buf + o->pos, o->cap - o->pos, "<option value=\"%s\"%s>%s%s</option>",
                       r, (o->sel && !strcmp(o->sel, ref)) ? " selected" : "", n, available ? "" : " (offline)");
}

static int handle_schema_get(int fd, const char * query, const char * msg) {
    size_t cap = 96 * 1024;
    char * body = malloc(cap);
    if (!body) return send_status(fd, 500, "Internal Server Error", "oom");
    int edit = -2;                                   /* -2 none, -1 new, >= 0 rule */
    if (query && !strncmp(query, "edit=", 5)) edit = atoi(query + 5);
    else if (query && !strcmp(query, "new")) edit = -1;
    ss_rule_t er;
    if (edit >= 0 && !sched_get(edit, &er)) edit = -2;
    if (edit == -1) {
        memset(&er, 0, sizeof er);
        er.active = 1; er.on = 1; er.mode = SS_CLOCK; er.hhmm = 700; er.days = SS_ALL_DAYS;
    }

    int pos = snprintf(body, cap,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>FreeToon - Schakelschema</title><style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f1a2a;color:#e2e8f0;margin:0;padding:24px;line-height:1.5}"
        ".card{max-width:680px;margin:0 auto;background:#1e293b;border-radius:12px;padding:24px}"
        "h1{margin-top:0;font-size:22px;color:#38bdf8}h2{font-size:17px;color:#38bdf8;margin:24px 0 4px}p{color:#94a3b8;font-size:14px}"
        "label{display:block;margin:12px 0 4px;font-size:14px}.days label,.cb label{display:inline-block;margin:4px 12px 0 0}"
        "select,input[type=time],input[type=number]{box-sizing:border-box;padding:8px;border-radius:6px;border:1px solid #334155;background:#0f172a;color:#f8fafc;font-size:15px}"
        "select{width:100%%}"
        ".btn{background:#2563eb;color:#fff;border:0;padding:8px 16px;border-radius:6px;font-size:14px;cursor:pointer;margin-top:16px;text-decoration:none;display:inline-block}"
        ".btn-sec{background:#334155;color:#e2e8f0}.btn-del{background:#b94a48}.sm{padding:4px 10px;margin:0 0 0 6px;font-size:13px}"
        ".rule{display:flex;align-items:center;justify-content:space-between;background:#0f172a;border-radius:8px;padding:8px 12px;margin-top:8px}"
        ".rule .t{color:#94a3b8;font-size:13px}.off{opacity:.55}.msg{color:#fbbf24}.st{background:#0f172a;border-radius:8px;padding:10px 14px;margin-top:8px}"
        "form.inl{display:inline}</style></head><body><div class=\"card\"><h1>Schakelschema</h1>"
        "<p>Zet stekkers en lampen op vaste tijden, of bij zonsopkomst of zonsondergang, aan of uit. "
        "Ook in te stellen op de Toon: Apparaten &rarr; Schakelschema.</p>");
    if (msg) {
        char m[300];
        html_attr_esc(m, sizeof m, msg);
        pos += snprintf(body + pos, cap - pos, "<p class=\"msg\">%s</p>", m);
    }
    char nx[200], nxe[400];
    if (settings.client_mode)
        snprintf(nx, sizeof nx, "Deze Toon is een client: het schema draait op de hoofd-Toon.");
    else if (sched_next_text(nx, sizeof nx) < 0) snprintf(nx, sizeof nx, "geen actieve regels");
    html_attr_esc(nxe, sizeof nxe, nx);
    pos += snprintf(body + pos, cap - pos, "<div class=\"st\"><b>Volgende:</b> %s</div>", nxe);

    int n = sched_count();
    for (int i = 0; i < n && pos < (int)cap - 2048; i++) {
        ss_rule_t r;
        if (!sched_get(i, &r)) continue;
        char name[80], txt[120], ne[200], te[260];
        sched_device_name(r.ref, name, sizeof name);
        sched_rule_text(&r, txt, sizeof txt);
        html_attr_esc(ne, sizeof ne, name);
        html_attr_esc(te, sizeof te, txt);
        pos += snprintf(body + pos, cap - pos,
            "<div class=\"rule%s\"><div><b>%s</b><br><span class=\"t\">%s%s</span></div><div>"
            "<a class=\"btn btn-sec sm\" href=\"/schema?edit=%d\">Bewerken</a>"
            "<form class=\"inl\" method=\"post\" action=\"/schema\"><input type=\"hidden\" name=\"act\" value=\"toggle\">"
            "<input type=\"hidden\" name=\"i\" value=\"%d\"><button class=\"btn btn-sec sm\">%s</button></form>"
            "<form class=\"inl\" method=\"post\" action=\"/schema\" onsubmit=\"return confirm('Regel verwijderen?')\">"
            "<input type=\"hidden\" name=\"act\" value=\"delete\"><input type=\"hidden\" name=\"i\" value=\"%d\">"
            "<button class=\"btn btn-del sm\">Verwijderen</button></form></div></div>",
            r.active ? "" : " off", ne, te, r.active ? "" : " (uit)", i, i, r.active ? "Uitzetten" : "Aanzetten", i);
    }
    if (!n) pos += snprintf(body + pos, cap - pos, "<p>Nog geen regels.</p>");
    if (edit == -2)
        pos += snprintf(body + pos, cap - pos, "<a class=\"btn\" href=\"/schema?new\">+ Nieuwe regel</a>");

    if (edit >= -1) {
        pos += snprintf(body + pos, cap - pos,
            "<h2>%s</h2><form method=\"post\" action=\"/schema\">"
            "<input type=\"hidden\" name=\"act\" value=\"save\"><input type=\"hidden\" name=\"i\" value=\"%d\">"
            "<label>Apparaat</label><select name=\"ref\"><option value=\"\">(kies een apparaat)</option>",
            edit < 0 ? "Nieuwe regel" : "Regel bewerken", edit);
        struct sched_opts o = { body, cap, pos, er.ref, "", 0 };
        tl_enumerate(sched_opt, &o);
        pos = o.pos;
        if (o.open) pos += snprintf(body + pos, cap - pos, "</optgroup>");
        static const char * const dn[7] = { "ma", "di", "wo", "do", "vr", "za", "zo" };
        pos += snprintf(body + pos, cap - pos,
            "</select>"
            "<label>Actie</label><select name=\"on\"><option value=\"1\"%s>Aan</option><option value=\"0\"%s>Uit</option></select>"
            "<label>Moment</label><select name=\"mode\"><option value=\"0\"%s>Vaste tijd</option>"
            "<option value=\"1\"%s>Zonsopkomst</option><option value=\"2\"%s>Zonsondergang</option></select>"
            "<label>Tijd (bij vaste tijd)</label><input type=\"time\" name=\"time\" value=\"%02d:%02d\">"
            "<label>Verschuiving in minuten (bij zon, -180..180)</label><input type=\"number\" name=\"offset\" min=\"-180\" max=\"180\" step=\"5\" value=\"%d\">"
            "<label>Dagen</label><div class=\"days\">",
            er.on ? " selected" : "", er.on ? "" : " selected",
            er.mode == SS_CLOCK ? " selected" : "", er.mode == SS_SUNRISE ? " selected" : "",
            er.mode == SS_SUNSET ? " selected" : "", er.hhmm / 100, er.hhmm % 100, er.offset);
        for (int d = 0; d < 7; d++)
            pos += snprintf(body + pos, cap - pos, "<label><input type=\"checkbox\" name=\"d%d\" value=\"1\"%s> %s</label>",
                            d, (er.days >> d) & 1 ? " checked" : "", dn[d]);
        pos += snprintf(body + pos, cap - pos,
            "</div><div class=\"cb\"><label><input type=\"checkbox\" name=\"random\" value=\"1\"%s> "
            "Willekeurig &plusmn;20 min (elke dag anders, voor vakanties)</label></div>"
            "<div class=\"cb\"><label><input type=\"checkbox\" name=\"active\" value=\"1\"%s> Actief</label></div>"
            "<button class=\"btn\">Opslaan</button> <a class=\"btn btn-sec\" href=\"/schema\">Annuleren</a></form>",
            er.random ? " checked" : "", er.active ? " checked" : "");
    }
    pos += snprintf(body + pos, cap - pos,
        "<p style=\"margin-top:24px;text-align:center\"><a href=\"/\" class=\"btn btn-sec\">&larr; Terug naar Toon</a></p>"
        "</div></body></html>");
    if (pos < 0 || pos >= (int)cap) { free(body); return send_status(fd, 500, "Internal Server Error", "page too large"); }
    char hdr[200];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\n"
        "Cache-Control: no-store\r\nX-Frame-Options: DENY\r\nConnection: close\r\n\r\n", pos);
    int rc = sock_send_all(fd, hdr, hn) < 0 ? -1 : sock_send_all(fd, body, pos);
    free(body);
    return rc;
}

static int handle_schema_post(int fd, const char * req, const char * body) {
    if (!homey_same_origin(req))
        return send_status(fd, 403, "Forbidden", "cross-site request refused");
    char act[16] = "", v[96];
    form_get(body, "act", act, sizeof act);
    int i = form_get(body, "i", v, sizeof v) ? atoi(v) : -1;
    if (!strcmp(act, "delete")) {
        if (sched_delete(i) != 0) return handle_schema_get(fd, NULL, "Regel niet gevonden.");
        fprintf(stderr, "[pwa] schema: rule %d deleted via web\n", i);
        return send_redirect(fd, "/schema", NULL);
    }
    if (!strcmp(act, "toggle")) {
        ss_rule_t r;
        if (!sched_get(i, &r)) return handle_schema_get(fd, NULL, "Regel niet gevonden.");
        r.active = !r.active;
        sched_put(i, &r);
        return send_redirect(fd, "/schema", NULL);
    }
    if (strcmp(act, "save")) return send_status(fd, 400, "Bad Request", "unknown action");
    ss_rule_t r;
    memset(&r, 0, sizeof r);
    form_get(body, "ref", r.ref, sizeof r.ref);
    r.on = form_get(body, "on", v, sizeof v) && atoi(v) ? 1 : 0;
    r.mode = form_get(body, "mode", v, sizeof v) ? atoi(v) : SS_CLOCK;
    int hh = 0, mm = 0;
    if (form_get(body, "time", v, sizeof v)) sscanf(v, "%d:%d", &hh, &mm);
    r.hhmm = hh * 100 + mm;
    r.offset = form_get(body, "offset", v, sizeof v) ? atoi(v) : 0;
    for (int d = 0; d < 7; d++) {
        char k[4];
        snprintf(k, sizeof k, "d%d", d);
        if (form_get(body, k, v, sizeof v)) r.days |= 1 << d;
    }
    r.active = form_get(body, "active", v, sizeof v) ? 1 : 0;
    r.random = form_get(body, "random", v, sizeof v) ? 1 : 0;
    char q[24];
    snprintf(q, sizeof q, i >= 0 ? "edit=%d" : "new", i);
    if (!r.ref[0]) return handle_schema_get(fd, q, "Kies een apparaat.");
    if (!r.days) return handle_schema_get(fd, q, "Kies minstens een dag.");
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || r.mode < SS_CLOCK || r.mode > SS_SUNSET ||
        r.offset < -180 || r.offset > 180 || sched_put(i >= 0 ? i : -1, &r) != 0)
        return handle_schema_get(fd, q, "Ongeldige regel, niet opgeslagen.");
    fprintf(stderr, "[pwa] schema: rule %s via web (%s)\n", i >= 0 ? "changed" : "added", r.ref);
    return send_redirect(fd, "/schema", NULL);
}

static int dispatch(int fd, char * req) {
    char method[8] = "", path[256] = "";
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        return send_status(fd, 400, "Bad Request", "bad request line");
    }
    char * body = strstr(req, "\r\n\r\n");
    if (body) body += 4; else body = "";

    /* Auth gate — redirects to /login or /set-password and returns 0
     * if the request shouldn't reach the normal route handlers.
     * Login routes themselves are on the public allowlist so the user
     * can actually see the form. */
    if (!auth_gate(fd, method, path, req)) return 0;

    /* Login / set-password / logout — explicit routes (even though they
     * land here only for public-allowlisted paths, dispatch still needs
     * to know which handler to invoke). */
    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/login"))         return handle_login_get(fd, 0);
        if (!strcmp(path, "/set-password"))  return handle_setpass_get(fd, NULL);
        if (!strcmp(path, "/logout"))        return handle_logout(fd, req);
    }
    if (!strcmp(method, "POST")) {
        if (!strcmp(path, "/login"))         return handle_login_post(fd, body);
        if (!strcmp(path, "/set-password"))  return handle_setpass_post(fd, body);
    }

    if (!strcmp(method, "GET")) {
        /* ---- /api/v1 REST & SSE Engine ---- */
        if (!strcmp(path, "/api/v1/state"))              return handle_v1_state(fd);
        if (!strcmp(path, "/api/v1/events"))             return handle_v1_events(fd);
        if (!strcmp(path, "/api/v1/thermostat"))         return handle_v1_thermostat_get(fd);
        if (!strcmp(path, "/api/v1/boiler"))             return handle_v1_boiler_get(fd);
        if (!strcmp(path, "/api/v1/sensors"))            return handle_v1_sensors_get(fd);
        if (!strcmp(path, "/api/v1/energy/live"))        return handle_v1_energy_live_get(fd);

        /* ---- Legacy / PWA endpoints ---- */
        if (!strcmp(path, "/api/state"))         return handle_state(fd);
        if (!strcmp(path, "/api/state/stream"))  return handle_state_stream(fd);
        if (!strcmp(path, "/api/radar.gif") || !strncmp(path, "/api/radar.gif?", 15))
            return handle_radar_gif(fd);          /* ?t=… = cache-buster */
        if (!strncmp(path, "/api/rrd", 8)) {
            const char * q = strchr(path, '?');
            return handle_rrd_proxy(fd, q ? q + 1 : "");
        }
        if (!strncmp(path, "/api/tile", 9)) {
            const char * q = strchr(path, '?');
            return handle_tile_proxy(fd, q ? q + 1 : "");
        }
        if (!strcmp(path, "/api/packages"))      return handle_packages_get(fd);
        if (!strncmp(path, "/api/animation", 14)) return handle_animation_get(fd, path);
        if (!strcmp(path, "/api/schedule"))      return handle_schedule_get(fd);
        if (!strcmp(path, "/api/settings"))      return handle_settings_get(fd);
        if (!strcmp(path, "/api/update/status")) return handle_update_status(fd);
        if (!strcmp(path, "/logs") || !strcmp(path, "/logs/") || !strcmp(path, "/api/logs"))
            return handle_logs_index_get(fd);
        if (!strcmp(path, "/log") || !strcmp(path, "/api/log") || !strncmp(path, "/log?", 5) || !strncmp(path, "/api/log?", 9) || !strncmp(path, "/log/", 5) || !strncmp(path, "/api/log/", 9))
            return handle_log_get(fd, path);
        if (!strcmp(path, "/diag") || !strcmp(path, "/diag?view"))
            return handle_diag_get(fd, path[5] == '?');
        if (!strcmp(path, "/ketellog") || !strcmp(path, "/ketellog?view"))
            return handle_ketellog_get(fd, path[9] == '?');
        if (!strcmp(path, "/homey"))       return handle_homey_get(fd, NULL);
        if (!strcmp(path, "/homey?saved")) return handle_homey_get(fd, "Opgeslagen.");
        if (!strcmp(path, "/solar"))       return handle_solar_get(fd, NULL);
        if (!strcmp(path, "/schema"))      return handle_schema_get(fd, NULL, NULL);
        if (!strncmp(path, "/schema?", 8)) return handle_schema_get(fd, path + 8, NULL);
        if (!strcmp(path, "/solar?saved")) return handle_solar_get(fd, "Opgeslagen. De app leest de omvormer binnen enkele seconden opnieuw uit.");
        if (!strcmp(path, "/") || !strcmp(path, "/index.html") || !strncmp(path, "/?", 2))
            return handle_remote_page(fd);
        return serve_static(fd, path);
    }
    if (!strcmp(method, "POST")) {
        /* ---- /api/v1 Actions ---- */
        if (!strcmp(path, "/api/v1/thermostat/setpoint")) return handle_v1_setpoint_post(fd, body);
        if (!strcmp(path, "/api/v1/thermostat/state"))    return handle_v1_preset_post(fd, body);
        if (!strcmp(path, "/api/v1/boiler/mode"))         return handle_v1_boiler_mode_post(fd, body);

        /* ---- Legacy / PWA endpoints ---- */
        if (!strcmp(path, "/api/setpoint")) return handle_setpoint(fd, body);
        if (!strcmp(path, "/api/animation")) return handle_animation_post(fd, body);
        if (!strcmp(path, "/api/program"))  return handle_program(fd, body);
        if (!strcmp(path, "/api/schedule")) return handle_schedule_post(fd, body);
        if (!strcmp(path, "/api/curtain"))  return handle_curtain(fd, body);
        if (!strcmp(path, "/api/hw/socket")) return handle_hw_socket(fd, body);
        if (!strcmp(path, "/api/ha/toggle")) return handle_ha_toggle(fd, body);
        if (!strcmp(path, "/api/dz/toggle")) return handle_dz_toggle(fd, body);
        if (!strcmp(path, "/api/homey/toggle")) return handle_homey_toggle(fd, body);
        if (!strcmp(path, "/api/settings")) return handle_settings_post(fd, body);
        if (!strcmp(path, "/api/update"))   return handle_update_post(fd);
        if (!strcmp(path, "/api/packages")) return handle_packages_post(fd, body);
        if (!strcmp(path, "/api/email"))    return handle_email_post(fd, body);
        if (!strcmp(path, "/api/install"))  return handle_install_post(fd, body, 0);
        if (!strcmp(path, "/api/uninstall")) return handle_install_post(fd, body, 1);
        if (!strcmp(path, "/api/vent"))     return handle_vent_post(fd, body);
        /* Boiler serial capture (#64), from the /logs page. Not public: it
         * restarts happ_thermstat. */
        if (!strcmp(path, "/ketellog/start")) { ketellog_start(60); return send_redirect(fd, "/logs", NULL); }
        if (!strcmp(path, "/ketellog/stop"))  { ketellog_stop();    return send_redirect(fd, "/logs", NULL); }
        if (!strcmp(path, "/homey"))          return handle_homey_post(fd, req, body);
        if (!strcmp(path, "/solar"))          return handle_solar_post(fd, req, body);
        if (!strcmp(path, "/schema"))         return handle_schema_post(fd, req, body);
        if (!strcmp(path, "/api/v1/boiler/remote")) return handle_boiler_remote_post(fd, body);
        if (!strcmp(path, "/api/v1/solar/source")) return handle_solar_source_post(fd, body);
        /* POST /api/v1/marketplace/apps/<id>/state */
        if (!strncmp(path, "/api/v1/marketplace/apps/", 25)) {
            const char * tail = path + 25;
            const char * slash = strchr(tail, '/');
            if (slash && !strcmp(slash, "/state")) {
                char app_id[48]; size_t L = (size_t)(slash - tail);
                if (L >= sizeof(app_id)) L = sizeof(app_id) - 1;
                memcpy(app_id, tail, L); app_id[L] = 0;
                return handle_v1_app_state_post(fd, app_id, body);
            }
        }

        /* POST /api/packages/<id>/receive */
        if (!strncmp(path, "/api/packages/", 14)) {
            const char * tail = path + 14;
            const char * slash = strchr(tail, '/');
            if (slash && !strcmp(slash, "/receive")) {
                char id[32]; size_t L = (size_t)(slash - tail);
                if (L >= sizeof(id)) L = sizeof(id) - 1;
                memcpy(id, tail, L); id[L] = 0;
                return handle_packages_receive(fd, id, body);
            }
        }
        return send_status(fd, 404, "Not Found", "no");
    }
    if (!strcmp(method, "DELETE")) {
        /* DELETE /api/packages/<id> */
        if (!strncmp(path, "/api/packages/", 14))
            return handle_packages_delete(fd, path + 14);
        return send_status(fd, 404, "Not Found", "no");
    }
    if (!strcmp(method, "OPTIONS")) {
        const char * hdr =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n\r\n";
        return sock_send_all(fd, hdr, strlen(hdr));
    }
    return send_status(fd, 405, "Method Not Allowed", "no");
}

#define PWA_MAX_CLIENTS 6
static int g_pwa_active_clients = 0;
static pthread_mutex_t g_pwa_clients_mu = PTHREAD_MUTEX_INITIALIZER;

/* Worker thread for a single accepted connection. Owns the fd. */
static void * conn_thread(void * arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[4096];
    size_t r = 0;
    while (r < sizeof(req) - 1) {
        ssize_t k = recv(fd, req + r, sizeof(req) - 1 - r, 0);
        if (k <= 0) break;
        r += (size_t)k;
        req[r] = 0;
        char * eoh = strstr(req, "\r\n\r\n");
        if (!eoh) continue;
        const char * cl = strcasestr(req, "Content-Length:");
        if (!cl) break;
        size_t need = (size_t)atoi(cl + 15);
        size_t have = r - (eoh + 4 - req);
        if (have >= need) break;
    }
    if (r > 0) dispatch(fd, req);
    close(fd);
    pthread_mutex_lock(&g_pwa_clients_mu);
    g_pwa_active_clients--;
    pthread_mutex_unlock(&g_pwa_clients_mu);
    return NULL;
}

static void * pwa_thread(void * arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("[pwa] socket"); return NULL; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_port   = htons(PWA_PORT),
                             .sin_addr   = { htonl(INADDR_ANY) } };
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0) {
        perror("[pwa] bind"); close(srv); return NULL;
    }
    listen(srv, 8);
    fprintf(stderr, "[pwa] listening on :%d  root=%s (max %d clients)\n",
            PWA_PORT, PWA_ROOT, PWA_MAX_CLIENTS);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    /* 256 KB stack is plenty for our tiny handlers and keeps fork cost low. */
    pthread_attr_setstacksize(&at, 256 * 1024);

    while (1) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        pthread_mutex_lock(&g_pwa_clients_mu);
        if (g_pwa_active_clients >= PWA_MAX_CLIENTS) {
            pthread_mutex_unlock(&g_pwa_clients_mu);
            send_status(c, 503, "Service Unavailable", "Too many connections\n");
            close(c);
            continue;
        }
        g_pwa_active_clients++;
        pthread_mutex_unlock(&g_pwa_clients_mu);

        pthread_t t;
        if (pthread_create(&t, &at, conn_thread, (void *)(intptr_t)c) != 0) {
            pthread_mutex_lock(&g_pwa_clients_mu);
            g_pwa_active_clients--;
            pthread_mutex_unlock(&g_pwa_clients_mu);
            close(c);
        }
    }
    pthread_attr_destroy(&at);
    return NULL;
}

int pwa_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, pwa_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
