/* Homey local Web API parser (BACKLOG #28). See homey_parse.h.
 *
 * A small bounds-checked JSON walker: every read is limited by `end`, so a
 * truncated or hostile body can only make a parse fail, never overrun. The
 * device list of a real install is several hundred kB, mostly app-specific
 * `settings` and `data` blobs; those are skipped token by token without
 * copying. */
#include "homey_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char * p; const char * end; } jp_t;

static void ws(jp_t * j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static int peek(jp_t * j) { ws(j); return j->p < j->end ? (unsigned char)*j->p : -1; }

static int eat(jp_t * j, char c) {
    if (peek(j) != (unsigned char)c) return 0;
    j->p++;
    return 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Append one byte if it fits (leaves room for NUL). */
static void put(char * out, size_t osz, size_t * o, char c) {
    if (out && *o + 1 < osz) out[(*o)++] = c;
}

/* Parse a JSON string at j->p into out (UTF-8, truncated on a character
 * boundary). out may be NULL to just skip. Returns 1 on success. */
static int jstring(jp_t * j, char * out, size_t osz) {
    if (!eat(j, '"')) return 0;
    size_t o = 0;
    while (j->p < j->end) {
        char c = *j->p++;
        if (c == '"') {
            if (out && osz) {
                /* Truncation may have cut the last multi-byte character:
                 * drop it if it is incomplete. */
                if (o > 0 && ((unsigned char)out[o - 1] & 0x80)) {
                    size_t s = o - 1;
                    while (s > 0 && o - s < 4 && ((unsigned char)out[s] & 0xC0) == 0x80) s--;
                    unsigned char lead = (unsigned char)out[s];
                    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                    if (o - s < need) o = s;
                }
                out[o] = 0;
            }
            return 1;
        }
        if (c != '\\') { put(out, osz, &o, c); continue; }
        if (j->p >= j->end) return 0;
        c = *j->p++;
        switch (c) {
        case 'n': put(out, osz, &o, '\n'); break;
        case 't': put(out, osz, &o, '\t'); break;
        case 'r': put(out, osz, &o, '\r'); break;
        case 'b': case 'f': break;
        case 'u': {
            if (j->end - j->p < 4) return 0;
            int v = 0;
            for (int i = 0; i < 4; i++) {
                int h = hexval(j->p[i]);
                if (h < 0) return 0;
                v = v * 16 + h;
            }
            j->p += 4;
            /* Surrogate pair. */
            if (v >= 0xD800 && v <= 0xDBFF && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                int w = 0, ok = 1;
                for (int i = 0; i < 4; i++) {
                    int h = hexval(j->p[2 + i]);
                    if (h < 0) { ok = 0; break; }
                    w = w * 16 + h;
                }
                if (ok && w >= 0xDC00 && w <= 0xDFFF) {
                    v = 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00);
                    j->p += 6;
                }
            }
            if (v < 0x80) put(out, osz, &o, (char)v);
            else if (v < 0x800) {
                put(out, osz, &o, (char)(0xC0 | (v >> 6)));
                put(out, osz, &o, (char)(0x80 | (v & 0x3F)));
            } else if (v < 0x10000) {
                put(out, osz, &o, (char)(0xE0 | (v >> 12)));
                put(out, osz, &o, (char)(0x80 | ((v >> 6) & 0x3F)));
                put(out, osz, &o, (char)(0x80 | (v & 0x3F)));
            } else {
                put(out, osz, &o, (char)(0xF0 | (v >> 18)));
                put(out, osz, &o, (char)(0x80 | ((v >> 12) & 0x3F)));
                put(out, osz, &o, (char)(0x80 | ((v >> 6) & 0x3F)));
                put(out, osz, &o, (char)(0x80 | (v & 0x3F)));
            }
            break;
        }
        default: put(out, osz, &o, c); break;    /* \" \\ \/ */
        }
    }
    return 0;
}

static int skip_value(jp_t * j, int depth);

/* Skip a literal or number. */
static int skip_scalar(jp_t * j) {
    const char * s = j->p;
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\r' || c == '\t') break;
        j->p++;
    }
    return j->p > s;
}

static int skip_value(jp_t * j, int depth) {
    if (depth > 64) return 0;
    int c = peek(j);
    if (c == '"') return jstring(j, NULL, 0);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        if (eat(j, close)) return 1;
        for (;;) {
            if (c == '{') {
                if (!jstring(j, NULL, 0) || !eat(j, ':')) return 0;
            }
            if (!skip_value(j, depth + 1)) return 0;
            if (eat(j, ',')) continue;
            return eat(j, close);
        }
    }
    if (c < 0) return 0;
    return skip_scalar(j);
}

/* Value kinds for a scalar read. */
typedef struct { int type; double num; char str[64]; } jval_t;

/* Read a scalar (string/number/bool/null) or skip a container (type NONE). */
static int read_value(jp_t * j, jval_t * v) {
    v->type = HY_V_NONE; v->num = 0; v->str[0] = 0;
    int c = peek(j);
    if (c == '"') { v->type = HY_V_STR; return jstring(j, v->str, sizeof v->str); }
    if (c == '{' || c == '[') return skip_value(j, 0);
    if (c == 't' && j->end - j->p >= 4 && !memcmp(j->p, "true", 4))  { j->p += 4; v->type = HY_V_BOOL; v->num = 1; return 1; }
    if (c == 'f' && j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) { j->p += 5; v->type = HY_V_BOOL; v->num = 0; return 1; }
    if (c == 'n' && j->end - j->p >= 4 && !memcmp(j->p, "null", 4))  { j->p += 4; return 1; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[40]; size_t n = 0;
        while (j->p < j->end && n + 1 < sizeof buf &&
               (strchr("+-.eE", *j->p) || (*j->p >= '0' && *j->p <= '9')))
            buf[n++] = *j->p++;
        buf[n] = 0;
        v->type = HY_V_NUM;
        v->num = strtod(buf, NULL);
        return n > 0;
    }
    return 0;
}

static void copy_str(char * dst, size_t dsz, const jval_t * v) {
    if (v->type == HY_V_STR) {
        size_t n = strlen(v->str);
        if (n >= dsz) {
            n = dsz - 1;
            while (n > 0 && ((unsigned char)v->str[n] & 0xC0) == 0x80) n--;   /* char boundary */
        }
        memcpy(dst, v->str, n);
        dst[n] = 0;
    } else {
        dst[0] = 0;
    }
}

/* Latin-1 trail byte (after 0xC3) -> plain ASCII letter, 0 if none. Same
 * mapping as calendar.c: the UI fonts hold ASCII, ° and icon symbols only. */
static char latin1_ascii(unsigned char n) {
    if (n >= 0xA0 && n <= 0xA5) return 'a';
    if (n == 0xA7) return 'c';
    if (n >= 0xA8 && n <= 0xAB) return 'e';
    if (n >= 0xAC && n <= 0xAF) return 'i';
    if (n == 0xB1) return 'n';
    if ((n >= 0xB2 && n <= 0xB6) || n == 0xB8) return 'o';
    if (n >= 0xB9 && n <= 0xBC) return 'u';
    if (n == 0xBD || n == 0xBF) return 'y';
    if (n >= 0x80 && n <= 0x85) return 'A';
    if (n == 0x87) return 'C';
    if (n >= 0x88 && n <= 0x8B) return 'E';
    if (n >= 0x8C && n <= 0x8F) return 'I';
    if (n == 0x91) return 'N';
    if ((n >= 0x92 && n <= 0x96) || n == 0x98) return 'O';
    if (n >= 0x99 && n <= 0x9C) return 'U';
    if (n == 0x9D) return 'Y';
    if (n == 0x9F) return 's';
    return 0;
}

void hy_fold_text(char * s) {
    char * w = s;
    const unsigned char * r = (const unsigned char *)s;
    while (*r) {
        unsigned char c = *r;
        char a = 0;
        if (c < 0x80) {
            a = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
            r++;
        } else {
            if (c == 0xC3 && r[1]) a = latin1_ascii(r[1]);
            else if (c == 0xC2 && r[1] == 0xA0) a = ' ';
            else if (c == 0xE2 && r[1] == 0x80 && r[2]) {
                unsigned char t = r[2];
                if (t == 0x98 || t == 0x99) a = '\'';
                else if (t == 0x9C || t == 0x9D) a = '"';
                else if (t == 0x93 || t == 0x94) a = '-';
                else if (t == 0xA6) a = '.';
            }
            r++;                                            /* lead byte */
            while ((*r & 0xC0) == 0x80) r++;                /* its continuation bytes */
            if (!a) continue;                               /* emoji etc.: dropped */
        }
        if (a == ' ' && (w == s || w[-1] == ' ')) continue; /* no leading/double spaces */
        *w++ = a;
    }
    while (w > s && w[-1] == ' ') w--;
    *w = 0;
}

/* Display name: folded to the font, never empty (an emoji-only name). */
static void copy_name(char * dst, size_t dsz, const jval_t * v) {
    copy_str(dst, dsz, v);
    if (!dst[0]) return;                 /* really absent: caller skips the entry */
    hy_fold_text(dst);
    if (!dst[0]) snprintf(dst, dsz, "Homey");
}

/* Iterate the top-level collection: object keyed by id, or array. Calls
 * cb(j, ctx) with j->p at each element value; cb must consume the value.
 * Returns 0 on success, -1 on a shape error. */
typedef int (*elem_cb)(jp_t * j, void * ctx);
static int for_each_elem(jp_t * j, elem_cb cb, void * ctx) {
    int c = peek(j);
    if (c == '{') {
        j->p++;
        if (eat(j, '}')) return 0;
        for (;;) {
            if (!jstring(j, NULL, 0) || !eat(j, ':')) return -1;
            if (peek(j) == '{') { if (!cb(j, ctx)) return -1; }
            else if (!skip_value(j, 0)) return -1;
            if (eat(j, ',')) continue;
            return eat(j, '}') ? 0 : -1;
        }
    }
    if (c == '[') {
        j->p++;
        if (eat(j, ']')) return 0;
        for (;;) {
            if (peek(j) == '{') { if (!cb(j, ctx)) return -1; }
            else if (!skip_value(j, 0)) return -1;
            if (eat(j, ',')) continue;
            return eat(j, ']') ? 0 : -1;
        }
    }
    return -1;
}

/* ------------------------------------------------------------ devices */

/* Capabilities FreeToon can show or control (base id, before any ".sub"). */
static const char * const CAP_KEEP[] = {
    "onoff", "dim", "light_temperature", "light_hue", "light_saturation", "light_mode",
    "windowcoverings_state", "windowcoverings_set", "windowcoverings_closed",
    "locked", "target_temperature", "thermostat_mode",
    "measure_temperature", "measure_humidity", "measure_power", "meter_power",
    "measure_luminance", "meter_gas",
    "alarm_contact", "alarm_motion", "alarm_smoke", "alarm_water", "alarm_presence",
    "measure_battery", "alarm_battery",
    "play_scene", "button",
    NULL
};

static int cap_wanted(const char * id) {
    size_t base = strcspn(id, ".");
    for (int i = 0; CAP_KEEP[i]; i++)
        if (strlen(CAP_KEEP[i]) == base && !strncmp(CAP_KEEP[i], id, base)) return 1;
    return 0;
}

const hy_cap_t * hy_dev_cap(const hy_dev_t * d, const char * cap_id) {
    for (int i = 0; i < d->ncap; i++)
        if (!strcmp(d->cap[i].id, cap_id)) return &d->cap[i];
    return NULL;
}

static int has_cap_prefix(const hy_dev_t * d, const char * pre) {
    size_t n = strlen(pre);
    for (int i = 0; i < d->ncap; i++)
        if (!strncmp(d->cap[i].id, pre, n)) return 1;
    return 0;
}

static int classify(const hy_dev_t * d) {
    const char * c = d->vcls[0] ? d->vcls : d->cls;
    if (!strcmp(c, "light")) return HY_K_LIGHT;
    if (!strcmp(c, "socket")) return HY_K_SOCKET;
    if (!strcmp(c, "windowcoverings") || !strcmp(c, "curtain") || !strcmp(c, "blinds") ||
        !strcmp(c, "sunshade") || !strcmp(c, "awning") || !strcmp(c, "garagedoor"))
        return HY_K_COVER;
    if (!strcmp(c, "lock")) return HY_K_LOCK;
    if (!strcmp(c, "thermostat") || !strcmp(c, "heater") || !strcmp(c, "airconditioning"))
        return HY_K_THERMOSTAT;
    if (!strcmp(c, "fan")) return HY_K_FAN;
    if (hy_dev_cap(d, "play_scene") || hy_dev_cap(d, "button")) return HY_K_BUTTON;
    if (!strcmp(c, "sensor") || !strcmp(c, "homealarm") || !strcmp(c, "doorbell"))
        return HY_K_SENSOR;
    if (has_cap_prefix(d, "windowcoverings_")) return HY_K_COVER;
    if (hy_dev_cap(d, "dim")) return HY_K_LIGHT;
    if (hy_dev_cap(d, "onoff")) return HY_K_SOCKET;
    if (has_cap_prefix(d, "measure_") || has_cap_prefix(d, "alarm_")) return HY_K_SENSOR;
    return HY_K_OTHER;
}

const char * hy_kind_name(int k) {
    static const char * const n[] = { "other", "light", "socket", "cover", "lock",
                                       "thermostat", "fan", "sensor", "button" };
    return (k >= 0 && k < (int)(sizeof n / sizeof n[0])) ? n[k] : "?";
}

/* A Toon app on the Homey (e.g. nl.toonwater.toonwater) models this Toon
 * itself; showing it would let FreeToon switch itself in a loop. */
static int is_toon_driver(const char * uri) {
    /* "homey:app:<appid>": look only at the app id. */
    const char * a = strstr(uri, "homey:app:");
    if (!a) return 0;
    a += 10;
    size_t n = strcspn(a, ":");
    for (size_t i = 0; i + 4 <= n; i++)
        if (!strncmp(a + i, "toon", 4)) return 1;
    return 0;
}

static int parse_caps_obj(jp_t * j, hy_dev_t * d) {
    if (!eat(j, '{')) return skip_value(j, 0);
    if (eat(j, '}')) return 1;
    for (;;) {
        char capid[sizeof ((hy_cap_t *)0)->id];
        if (!jstring(j, capid, sizeof capid) || !eat(j, ':')) return 0;
        hy_cap_t * c = NULL;
        if (cap_wanted(capid) && d->ncap < HY_MAX_CAP && peek(j) == '{') {
            c = &d->cap[d->ncap];
            memset(c, 0, sizeof *c);
            snprintf(c->id, sizeof c->id, "%s", capid);
        }
        if (!c) {
            if (!skip_value(j, 0)) return 0;
        } else {
            j->p++;                                   /* '{' */
            if (!eat(j, '}')) {
                for (;;) {
                    char key[24];
                    if (!jstring(j, key, sizeof key) || !eat(j, ':')) return 0;
                    jval_t v;
                    if (!strcmp(key, "value")) {
                        if (!read_value(j, &v)) return 0;
                        c->type = (unsigned char)v.type;
                        c->num = v.num;
                        if (v.type == HY_V_STR) copy_str(c->str, sizeof c->str, &v);
                    } else if (!strcmp(key, "units")) {
                        if (!read_value(j, &v)) return 0;
                        copy_str(c->units, sizeof c->units, &v);
                    } else if (!strcmp(key, "setable")) {
                        if (!read_value(j, &v)) return 0;
                        c->setable = v.type == HY_V_BOOL && v.num != 0;
                    } else if (!strcmp(key, "getable")) {
                        if (!read_value(j, &v)) return 0;
                        c->getable = v.type == HY_V_BOOL && v.num != 0;
                    } else if (!skip_value(j, 0)) {
                        return 0;
                    }
                    if (eat(j, ',')) continue;
                    if (!eat(j, '}')) return 0;
                    break;
                }
            }
            d->ncap++;
        }
        if (eat(j, ',')) continue;
        return eat(j, '}');
    }
}

typedef struct { hy_dev_t * out; int max; int n; } dev_ctx_t;

static int dev_cb(jp_t * j, void * vctx) {
    dev_ctx_t * ctx = vctx;
    hy_dev_t tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.available = 1;
    char driver[96] = "";
    int ncaps_listed = -1;      /* -1 = no "capabilities" array seen */

    if (!eat(j, '{')) return 0;
    if (!eat(j, '}')) {
        for (;;) {
            char key[32];
            if (!jstring(j, key, sizeof key) || !eat(j, ':')) return 0;
            jval_t v;
            if (!strcmp(key, "id"))                 { if (!read_value(j, &v)) return 0; copy_str(tmp.id, sizeof tmp.id, &v); }
            else if (!strcmp(key, "name"))          { if (!read_value(j, &v)) return 0; copy_name(tmp.name, sizeof tmp.name, &v); }
            else if (!strcmp(key, "zone"))          { if (!read_value(j, &v)) return 0; copy_str(tmp.zone, sizeof tmp.zone, &v); }
            else if (!strcmp(key, "class"))         { if (!read_value(j, &v)) return 0; copy_str(tmp.cls, sizeof tmp.cls, &v); }
            else if (!strcmp(key, "virtualClass"))  { if (!read_value(j, &v)) return 0; copy_str(tmp.vcls, sizeof tmp.vcls, &v); }
            else if (!strcmp(key, "driverUri") || (!strcmp(key, "driverId") && !driver[0])) {
                if (!read_value(j, &v)) return 0;
                if (v.type == HY_V_STR) snprintf(driver, sizeof driver, "%s", v.str);
            }
            else if (!strcmp(key, "available"))     { if (!read_value(j, &v)) return 0; tmp.available = !(v.type == HY_V_BOOL && v.num == 0); }
            else if (!strcmp(key, "capabilities")) {
                /* Count only: an empty list marks remotes and similar. */
                if (peek(j) == '[') {
                    ncaps_listed = 0;
                    j->p++;
                    if (!eat(j, ']')) {
                        for (;;) {
                            if (!skip_value(j, 0)) return 0;
                            ncaps_listed++;
                            if (eat(j, ',')) continue;
                            if (!eat(j, ']')) return 0;
                            break;
                        }
                    }
                } else if (!skip_value(j, 0)) return 0;
            }
            else if (!strcmp(key, "capabilitiesObj")) { if (!parse_caps_obj(j, &tmp)) return 0; }
            else if (!skip_value(j, 0)) return 0;   /* settings, data, ui, energy, images, ... */
            if (eat(j, ',')) continue;
            if (!eat(j, '}')) return 0;
            break;
        }
    }

    if (!tmp.id[0] || !tmp.name[0]) return 1;
    if (ncaps_listed == 0) return 1;                 /* remote / no capabilities */
    if (driver[0] && is_toon_driver(driver)) return 1;
    /* Unavailable apps send an empty capabilitiesObj: keep the device (shown
     * greyed) only if it has a usable class; otherwise there is nothing to show. */
    tmp.kind = (unsigned char)classify(&tmp);
    if (tmp.ncap == 0 && (tmp.available || tmp.kind == HY_K_OTHER)) return 1;
    if (ctx->n < ctx->max) ctx->out[ctx->n++] = tmp;
    return 1;
}

int hy_parse_devices(const char * body, size_t len, hy_dev_t * out, int max) {
    if (!body || !out || max <= 0) return -1;
    jp_t j = { body, body + len };
    dev_ctx_t ctx = { out, max, 0 };
    if (for_each_elem(&j, dev_cb, &ctx) != 0) return -1;
    return ctx.n;
}

/* -------------------------------------------------------------- zones */

typedef struct { hy_zone_t * out; int max; int n; } zone_ctx_t;

static int zone_cb(jp_t * j, void * vctx) {
    zone_ctx_t * ctx = vctx;
    hy_zone_t z;
    memset(&z, 0, sizeof z);
    if (!eat(j, '{')) return 0;
    if (!eat(j, '}')) {
        for (;;) {
            char key[24];
            if (!jstring(j, key, sizeof key) || !eat(j, ':')) return 0;
            jval_t v;
            if (!strcmp(key, "id"))        { if (!read_value(j, &v)) return 0; copy_str(z.id, sizeof z.id, &v); }
            else if (!strcmp(key, "name")) { if (!read_value(j, &v)) return 0; copy_name(z.name, sizeof z.name, &v); }
            else if (!skip_value(j, 0)) return 0;
            if (eat(j, ',')) continue;
            if (!eat(j, '}')) return 0;
            break;
        }
    }
    if (z.id[0] && z.name[0] && ctx->n < ctx->max) ctx->out[ctx->n++] = z;
    return 1;
}

int hy_parse_zones(const char * body, size_t len, hy_zone_t * out, int max) {
    if (!body || !out || max <= 0) return -1;
    jp_t j = { body, body + len };
    zone_ctx_t ctx = { out, max, 0 };
    if (for_each_elem(&j, zone_cb, &ctx) != 0) return -1;
    return ctx.n;
}

/* -------------------------------------------------------------- flows */

/* The "This flow is started" trigger card. The SAME id is used by the action
 * card "Start a flow" (seen in the real export), so for advanced flows the
 * card type must be checked too. */
#define PROG_TRIGGER "homey:manager:flow:programmatic_trigger"

typedef struct { hy_flow_t * out; int max; int n; int advanced; } flow_ctx_t;

static int ends_with_prog(const char * id) {
    static const char tail[] = "manager:flow:programmatic_trigger";
    size_t n = strlen(id), t = sizeof tail - 1;
    return n >= t && !strcmp(id + n - t, tail);
}

/* Advanced flow "cards": a map of card objects {type, id, ...}. Returns 1 if
 * one of them is a programmatic trigger, 0 if none, -1 on a shape error. */
static int cards_have_prog_trigger(jp_t * j) {
    int found = 0;
    if (peek(j) != '{') return skip_value(j, 0) ? 0 : -1;
    j->p++;
    if (eat(j, '}')) return 0;
    for (;;) {
        if (!jstring(j, NULL, 0) || !eat(j, ':')) return -1;
        if (peek(j) == '{') {
            char type[16] = "", id[96] = "";
            j->p++;
            if (!eat(j, '}')) {
                for (;;) {
                    char key[16];
                    if (!jstring(j, key, sizeof key) || !eat(j, ':')) return -1;
                    jval_t v;
                    if (!strcmp(key, "type"))    { if (!read_value(j, &v)) return -1; copy_str(type, sizeof type, &v); }
                    else if (!strcmp(key, "id")) { if (!read_value(j, &v)) return -1; copy_str(id, sizeof id, &v); }
                    else if (!skip_value(j, 0)) return -1;
                    if (eat(j, ',')) continue;
                    if (!eat(j, '}')) return -1;
                    break;
                }
            }
            if (!strcmp(type, "trigger") && ends_with_prog(id)) found = 1;
        } else if (!skip_value(j, 0)) {
            return -1;
        }
        if (eat(j, ',')) continue;
        return eat(j, '}') ? found : -1;
    }
}

static int flow_cb(jp_t * j, void * vctx) {
    flow_ctx_t * ctx = vctx;
    hy_flow_t f;
    memset(&f, 0, sizeof f);
    int enabled = 1, prog = 0;
    if (!eat(j, '{')) return 0;
    if (!eat(j, '}')) {
        for (;;) {
            char key[24];
            if (!jstring(j, key, sizeof key) || !eat(j, ':')) return 0;
            jval_t v;
            if (!strcmp(key, "id"))           { if (!read_value(j, &v)) return 0; copy_str(f.id, sizeof f.id, &v); }
            else if (!strcmp(key, "name"))    { if (!read_value(j, &v)) return 0; copy_name(f.name, sizeof f.name, &v); }
            else if (!strcmp(key, "enabled")) { if (!read_value(j, &v)) return 0; enabled = !(v.type == HY_V_BOOL && v.num == 0); }
            else if (!strcmp(key, "trigger") && !ctx->advanced) {
                /* Standard flow: exactly one trigger card; check its id. */
                const char * s = j->p;
                if (!skip_value(j, 0)) return 0;
                jp_t t = { s, j->p };
                if (eat(&t, '{')) {
                    while (!eat(&t, '}')) {
                        char tk[8];
                        if (!jstring(&t, tk, sizeof tk) || !eat(&t, ':')) break;
                        if (!strcmp(tk, "id")) {
                            jval_t tv;
                            if (read_value(&t, &tv) && tv.type == HY_V_STR && ends_with_prog(tv.str))
                                prog = 1;
                        } else if (!skip_value(&t, 0)) break;
                        eat(&t, ',');
                    }
                }
            }
            else if (!strcmp(key, "cards") && ctx->advanced) {
                int r = cards_have_prog_trigger(j);
                if (r < 0) return 0;
                if (r) prog = 1;
            }
            else if (!skip_value(j, 0)) return 0;
            if (eat(j, ',')) continue;
            if (!eat(j, '}')) return 0;
            break;
        }
    }
    if (enabled && prog && f.id[0] && f.name[0] && ctx->n < ctx->max) {
        f.advanced = (unsigned char)ctx->advanced;
        ctx->out[ctx->n++] = f;
    }
    return 1;
}

int hy_parse_flows(const char * body, size_t len, int advanced,
                   hy_flow_t * out, int already, int max) {
    if (!body || !out || already < 0 || max <= 0) return -1;
    jp_t j = { body, body + len };
    flow_ctx_t ctx = { out, max, already, advanced ? 1 : 0 };
    if (for_each_elem(&j, flow_cb, &ctx) != 0) return -1;
    return ctx.n;
}
