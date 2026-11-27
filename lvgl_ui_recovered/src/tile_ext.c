/* tile_ext.c — a Home Assistant entity or Domoticz device on an Integratie
 * tile. See tile_ext.h. */
#include "tile_ext.h"
#include "tile_slots.h"
#include "settings.h"
#include "i18n.h"
#include "homeassistant.h"
#include "domoticz.h"
#include "homey_parse.h"     /* hy_fold_text: names -> what the UI fonts draw */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(TILE_EXT_SLOTS == TILE_SLOT_COUNT, "settings.tile_slot_ext must cover every slot");

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static tile_ext_t g_cache[TILE_EXT_SLOTS];
static int        g_fetched[TILE_EXT_SLOTS];   /* fetched since bound (else "dirty") */

/* settings value "ha:<id>" / "dz:<idx>" -> kind + id */
static int parse_bind(int slot, char * id, size_t n) {
    if (slot < 0 || slot >= TILE_EXT_SLOTS) return TEXT_NONE;
    const char * b = tile_slots_binding(slot);
    if (!b || strcmp(b, "local:ext")) return TEXT_NONE;
    const char * v = settings.tile_slot_ext[slot];
    int k = !strncmp(v, "ha:", 3) ? TEXT_HA : !strncmp(v, "dz:", 3) ? TEXT_DZ : TEXT_NONE;
    if (k && id) snprintf(id, n, "%s", v + 3);
    return k && v[3] ? k : TEXT_NONE;
}

int tile_ext_kind(int slot) { return parse_bind(slot, NULL, 0); }

void tile_ext_bind(int slot, int kind, const char * id, const char * name) {
    if (slot < 0 || slot >= TILE_EXT_SLOTS) return;
    pthread_mutex_lock(&g_mtx);
    memset(&g_cache[slot], 0, sizeof g_cache[slot]);
    g_fetched[slot] = 0;
    if (kind != TEXT_NONE && id && id[0]) {
        snprintf(settings.tile_slot_ext[slot], sizeof settings.tile_slot_ext[slot], "%s:%s",
                 kind == TEXT_HA ? "ha" : "dz", id);
        g_cache[slot].kind = kind;
        snprintf(g_cache[slot].id, sizeof g_cache[slot].id, "%s", id);
        snprintf(g_cache[slot].name, sizeof g_cache[slot].name, "%s", name ? name : "");
        hy_fold_text(g_cache[slot].name);
    } else {
        settings.tile_slot_ext[slot][0] = 0;
    }
    pthread_mutex_unlock(&g_mtx);
    tile_slots_bind(slot, kind != TEXT_NONE ? "local:ext" : "");   /* saves the settings */
    if (kind == TEXT_HA) ha_tiles_refresh_async();
    if (kind == TEXT_DZ) domoticz_tiles_refresh_async();
}

int tile_ext_get(int slot, tile_ext_t * out) {
    char id[96];
    int k = parse_bind(slot, id, sizeof id);
    if (!k) return 0;
    pthread_mutex_lock(&g_mtx);
    if (g_cache[slot].kind != k || strcmp(g_cache[slot].id, id)) {   /* loaded from settings */
        memset(&g_cache[slot], 0, sizeof g_cache[slot]);
        g_cache[slot].kind = k;
        snprintf(g_cache[slot].id, sizeof g_cache[slot].id, "%s", id);
        g_fetched[slot] = 0;
    }
    *out = g_cache[slot];
    pthread_mutex_unlock(&g_mtx);
    return 1;
}

void tile_ext_describe(int slot, char * out, size_t n) {
    char id[96];
    int k = parse_bind(slot, id, sizeof id);
    if (k == TEXT_HA) snprintf(out, n, "Home Assistant: %s", id);
    else if (k == TEXT_DZ) snprintf(out, n, "Domoticz: %s %s", tr("apparaat", "device"), id);
    else out[0] = 0;
}

int tile_ext_next(int kind, int * it, char * id, size_t n, int only_dirty) {
    for (int s = *it < 0 ? 0 : *it; s < TILE_EXT_SLOTS; s++) {
        char v[96];
        if (parse_bind(s, v, sizeof v) != kind) continue;
        if (only_dirty) {
            pthread_mutex_lock(&g_mtx);
            int f = g_fetched[s] && g_cache[s].kind == kind && !strcmp(g_cache[s].id, v);
            pthread_mutex_unlock(&g_mtx);
            if (f) continue;
        }
        snprintf(id, n, "%s", v);
        *it = s + 1;
        return s;
    }
    *it = TILE_EXT_SLOTS;
    return -1;
}

int tile_ext_wants_ha(const char * entity_id) {
    for (int s = 0; s < TILE_EXT_SLOTS; s++) {
        char v[96];
        if (parse_bind(s, v, sizeof v) == TEXT_HA && !strcmp(v, entity_id)) return 1;
    }
    return 0;
}

/* ---- small JSON reads (first occurrence; "key" : "value", spaces allowed) */
static int jget(const char * j, const char * end, const char * key, char * out, size_t n) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    out[0] = 0;
    for (const char * p = j; p && (p = strstr(p, pat)) != NULL && (!end || p < end); p += strlen(pat)) {
        const char * q = p + strlen(pat);
        while (*q == ' ') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ') q++;
        size_t o = 0;
        if (*q == '"') {
            q++;
            while (*q && *q != '"' && (!end || q < end) && o + 1 < n) {
                if (*q == '\\' && q[1]) q++;            /* \" \\ \/ : keep the char */
                out[o++] = *q++;
            }
        } else {                                        /* number / true / null */
            while (*q && !strchr(",}] \r\n", *q) && o + 1 < n) out[o++] = *q++;
        }
        out[o] = 0;
        return o > 0;
    }
    return 0;
}

/* Unit -> UI fonts: keep the degree sign, ³ ² µ as 3 2 u, drop other non-ASCII. */
static void fold_unit(const char * in, char * out, size_t n) {
    size_t o = 0;
    for (const unsigned char * p = (const unsigned char *)in; *p && o + 3 < n; p++) {
        if (*p < 0x80) { out[o++] = (char)*p; continue; }
        if (p[0] == 0xC2 && p[1] == 0xB0) { out[o++] = (char)0xC2; out[o++] = (char)0xB0; p++; continue; }
        if (p[0] == 0xC2 && p[1] == 0xB3) { out[o++] = '3'; p++; continue; }
        if (p[0] == 0xC2 && p[1] == 0xB2) { out[o++] = '2'; p++; continue; }
        if (p[0] == 0xC2 && p[1] == 0xB5) { out[o++] = 'u'; p++; continue; }
        while ((p[1] & 0xC0) == 0x80) p++;              /* skip the rest of that char */
    }
    out[o] = 0;
}

/* "21.4300003" -> "21.43": at most 2 decimals, no trailing zeros added. */
static void fmt_number(const char * s, char * out, size_t n) {
    char * e;
    double v = strtod(s, &e);
    if (e == s || *e) { snprintf(out, n, "%s", s); return; }
    const char * dot = strchr(s, '.');
    int dec = dot ? (int)strlen(dot + 1) : 0;
    if (dec <= 2) { snprintf(out, n, "%s", s); return; }
    snprintf(out, n, "%.2f", v);
    char * d = strchr(out, '.');
    if (d) {
        char * z = out + strlen(out) - 1;
        while (z > d && *z == '0') *z-- = 0;
        if (z == d) *z = 0;
    }
}

/* "2026-11-27T14:05:01.123456+00:00" -> epoch */
static time_t parse_iso(const char * s) {
    int Y, M, D, h, m, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    struct tm tm = { 0 };
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = sec;
    time_t t = timegm(&tm);
    const char * z = s + 19;
    while (*z == '.' || (*z >= '0' && *z <= '9')) z++;
    if (*z == '+' || *z == '-') {
        int oh = 0, om = 0;
        sscanf(z + 1, "%d:%d", &oh, &om);
        long off = (oh * 60L + om) * 60L;
        t -= (*z == '+') ? off : -off;
    }
    return t;
}

void tile_ext_apply_ha(const char * entity_id, const char * json, int ok) {
    char state[64] = "", fname[64] = "", unit[24] = "", lc[40] = "", dclass[24] = "";
    if (ok && json) {
        jget(json, NULL, "state", state, sizeof state);
        jget(json, NULL, "friendly_name", fname, sizeof fname);
        jget(json, NULL, "unit_of_measurement", unit, sizeof unit);
        jget(json, NULL, "last_changed", lc, sizeof lc);
        jget(json, NULL, "device_class", dclass, sizeof dclass);
    }
    char dom[24];
    snprintf(dom, sizeof dom, "%.*s", (int)strcspn(entity_id, "."), entity_id);
    int sw = !strcmp(dom, "switch") || !strcmp(dom, "light") || !strcmp(dom, "input_boolean");
    int onoff = sw || !strcmp(dom, "binary_sensor");

    char val[64] = "";
    if (!state[0]) snprintf(val, sizeof val, "--");
    else if (!strcmp(state, "unavailable")) snprintf(val, sizeof val, "%s", tr("n.b.", "n/a"));
    else if (!strcmp(state, "unknown")) snprintf(val, sizeof val, "--");
    else if (onoff && (!strcmp(state, "on") || !strcmp(state, "off"))) {
        int on = !strcmp(state, "on");
        int opening = !strcmp(dclass, "door") || !strcmp(dclass, "window") ||
                      !strcmp(dclass, "opening") || !strcmp(dclass, "garage_door");
        snprintf(val, sizeof val, "%s", opening ? (on ? tr("Open", "Open") : tr("Dicht", "Closed"))
                                                : (on ? tr("Aan", "On") : tr("Uit", "Off")));
    } else {
        char num[48], u[24];
        fmt_number(state, num, sizeof num);
        hy_fold_text(num);
        fold_unit(unit, u, sizeof u);
        if (u[0]) snprintf(val, sizeof val, "%s %s", num, u);
        else snprintf(val, sizeof val, "%s", num);
    }
    hy_fold_text(fname);
    time_t ch = lc[0] ? parse_iso(lc) : 0;

    pthread_mutex_lock(&g_mtx);
    for (int s = 0; s < TILE_EXT_SLOTS; s++) {
        char v[96];
        if (parse_bind(s, v, sizeof v) != TEXT_HA || strcmp(v, entity_id)) continue;
        tile_ext_t * c = &g_cache[s];
        c->kind = TEXT_HA;
        snprintf(c->id, sizeof c->id, "%s", entity_id);
        g_fetched[s] = 1;
        c->error = !ok || !state[0];
        if (c->error) continue;
        snprintf(c->value, sizeof c->value, "%s", val);
        if (fname[0]) snprintf(c->name, sizeof c->name, "%s", fname);
        if (ch) c->changed = ch;
        c->switchable = sw;
        c->on = !strcmp(state, "on");
        c->have = 1;
    }
    pthread_mutex_unlock(&g_mtx);
}

void tile_ext_apply_dz(int idx, const char * obj, const char * end, int ok) {
    char name[64] = "", data[64] = "", stype[32] = "", status[40] = "", lu[32] = "", usage[40] = "";
    if (ok && obj) {
        jget(obj, end, "Name", name, sizeof name);
        jget(obj, end, "Data", data, sizeof data);
        jget(obj, end, "Usage", usage, sizeof usage);
        /* P1 / counters: Data is "1234;567;..." - the current usage reads better */
        if (strchr(data, ';') && usage[0]) snprintf(data, sizeof data, "%s", usage);
        jget(obj, end, "SwitchType", stype, sizeof stype);
        jget(obj, end, "Status", status, sizeof status);
        jget(obj, end, "LastUpdate", lu, sizeof lu);
    }
    int sw = !strcmp(stype, "On/Off") || !strcmp(stype, "Dimmer");
    char val[64] = "";
    int on = 0;
    if (sw) {
        on = status[0] && strcmp(status, "Off") != 0;
        if (!strcmp(status, "On") || !strcmp(status, "Off")) snprintf(val, sizeof val, "%s", on ? tr("Aan", "On") : tr("Uit", "Off"));
        else snprintf(val, sizeof val, "%s", status);        /* "Set Level: 40 %" */
    } else {
        /* "21.5 C" / "21.5 C, 58 %": Domoticz writes Celsius as a bare C */
        size_t o = 0;
        for (const char * p = data; *p && o + 4 < sizeof val; p++) {
            if (*p == ' ' && p[1] == 'C' && (p[2] == 0 || p[2] == ',')) {
                val[o++] = ' '; val[o++] = (char)0xC2; val[o++] = (char)0xB0; val[o++] = 'C'; p++;
                continue;
            }
            val[o++] = *p;
        }
        val[o] = 0;
    }
    hy_fold_text(name);
    time_t ch = 0;
    int Y, M, D, h, m, sec;
    if (sscanf(lu, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) == 6) {   /* Domoticz: local time */
        struct tm tm = { 0 };
        tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
        tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = sec; tm.tm_isdst = -1;
        ch = mktime(&tm);
    }
    char want[16];
    snprintf(want, sizeof want, "%d", idx);
    pthread_mutex_lock(&g_mtx);
    for (int s = 0; s < TILE_EXT_SLOTS; s++) {
        char v[96];
        if (parse_bind(s, v, sizeof v) != TEXT_DZ || strcmp(v, want)) continue;
        tile_ext_t * c = &g_cache[s];
        c->kind = TEXT_DZ;
        snprintf(c->id, sizeof c->id, "%s", want);
        g_fetched[s] = 1;
        c->error = !ok || !name[0];
        if (c->error) continue;
        snprintf(c->value, sizeof c->value, "%s", val[0] ? val : "--");
        snprintf(c->name, sizeof c->name, "%s", name);
        if (ch > 0) c->changed = ch;
        c->switchable = sw;
        c->on = on;
        c->have = 1;
    }
    pthread_mutex_unlock(&g_mtx);
}

int tile_ext_tap(int slot) {
    tile_ext_t c;
    if (!tile_ext_get(slot, &c)) return 0;
    if (!c.switchable || !c.have) return 1;         /* a sensor: a tap does nothing */
    if (c.kind == TEXT_HA) ha_entity_toggle_async(c.id);
    else if (c.kind == TEXT_DZ) domoticz_switch_async(atoi(c.id), "Toggle");
    /* show the new state at once; the backend's next value confirms it */
    pthread_mutex_lock(&g_mtx);
    tile_ext_t * g = &g_cache[slot];
    if (g->kind == c.kind && !strcmp(g->id, c.id) && (!strcmp(g->value, tr("Aan", "On")) || !strcmp(g->value, tr("Uit", "Off")))) {
        g->on = !g->on;
        snprintf(g->value, sizeof g->value, "%s", g->on ? tr("Aan", "On") : tr("Uit", "Off"));
        g->changed = time(NULL);
    }
    pthread_mutex_unlock(&g_mtx);
    return 1;
}

void tile_ext_age(time_t changed, char * out, size_t n) {
    out[0] = 0;
    if (changed <= 0) return;
    long d = (long)(time(NULL) - changed);
    if (d < 0) d = 0;
    if (d < 60) snprintf(out, n, "%s", tr("zojuist", "just now"));
    else if (d < 3600) snprintf(out, n, tr("%ld min geleden", "%ld min ago"), d / 60);
    else if (d < 24 * 3600) snprintf(out, n, tr("%ld uur geleden", "%ld h ago"), d / 3600);
    else snprintf(out, n, tr("%ld dagen geleden", "%ld days ago"), d / 86400);
}
