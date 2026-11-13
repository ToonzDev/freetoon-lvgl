/*
 * Weather poller — Buienradar only. Current conditions + 5-day + hourly come
 * from Buienradar's per-place forecast (forecast.buienradar.nl/2.0/forecast/<id>),
 * the place name is looked up with Buienradar's location search, and the data
 * feed supplies the radar-map URL and the Dutch weather report (plus the
 * nearest station's temperature if the forecast is unreachable). The rain tile
 * (buienradar_rain.c) uses this module's coordinates: manual Lat/Lon, else the
 * place's own. Icon codes are Buienradar's own a..z letters.
 *
 * No JSON library: small strstr-based extractors plus a depth-aware key lookup
 * (obj_get) for the forecast's nested day objects.
 */
#include "weather.h"
#include "shell_util.h"
#include "http.h"
#include "settings.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <unistd.h>
#include <pthread.h>
#include <time.h>

weather_state_t weather_state = {0};
static pthread_mutex_t g_wx_mtx = PTHREAD_MUTEX_INITIALIZER;

void weather_lock(void) {
    pthread_mutex_lock(&g_wx_mtx);
}

void weather_unlock(void) {
    pthread_mutex_unlock(&g_wx_mtx);
}

void weather_get_snapshot(weather_state_t * out) {
    if (!out) return;
    pthread_mutex_lock(&g_wx_mtx);
    *out = weather_state;
    pthread_mutex_unlock(&g_wx_mtx);
}

float weather_display_temp(void) {
    if (settings.weather_use_feel)
        return weather_state.feel_temp;
    return weather_state.current_temp;
}

float weather_hour_display_temp(const weather_hour_t * h) {
    if (!h) return 0.0f;
    if (settings.weather_use_feel)
        return h->feel_temp;
    return h->temperature;
}

/* Coordinates for the rain tile: manual Lat/Lon, else the place found by the
 * location search, else the forecast's own location. 0 = unresolved. */
static double g_cfg_lat = 0.0, g_cfg_lon = 0.0;
static volatile int g_wx_wake = 0;
static volatile int g_wx_relocate = 0;   /* settings changed: resolve lat/lon again */

/* "51.83" or "51,83" (Dutch decimal comma) -> 51.83; atof stopped at the comma. */
static double coord_atof(const char * s) {
    char b[24]; size_t i = 0;
    for (; s && s[i] && i < sizeof b - 1; i++) b[i] = (s[i] == ',') ? '.' : s[i];
    b[i] = 0;
    return atof(b);
}

void weather_get_lat_lon(double * lat, double * lon) {
    if (lat) *lat = (g_cfg_lat != 0.0 ? g_cfg_lat : (settings.weather_lat[0] ? coord_atof(settings.weather_lat) : 0.0));
    if (lon) *lon = (g_cfg_lon != 0.0 ? g_cfg_lon : (settings.weather_lon[0] ? coord_atof(settings.weather_lon) : 0.0));
}

/* Case-insensitive strstr. Buienradar's data feed switched from lowercase keys
 * ("stationid","temperature","iconurl","actualradarurl"…) to PascalCase
 * ("StationId","Temperature","IconUrl","ActualRadarUrl"…), which silently broke
 * the current-weather + radar parse. Matching keys case-insensitively makes the
 * parser survive either casing (and any future flip). */
static const char * ci_strstr(const char * hay, const char * needle) {
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, nl) == 0) return hay;
    return NULL;
}

/* Scan a substring of json for "key":<number>; returns parsed double, dflt if missing. */
static double js_num(const char * begin, const char * end, const char * key, double dflt) {
    char n[64];
    snprintf(n, sizeof(n), "\"%s\":", key);
    const char * p = ci_strstr(begin, n);
    if (!p || p >= end) return dflt;
    p += strlen(n);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n' || *p == '"') {
        /* "null" or quoted-number like "13" — handle both */
        if (*p == '"') p++; else return dflt;
    }
    return strtod(p, NULL);
}

/* Scan a substring for "key":"VALUE" — copies into out (max outsz-1). */
static int js_str(const char * begin, const char * end, const char * key,
                  char * out, size_t outsz) {
    char n[64];
    snprintf(n, sizeof(n), "\"%s\":\"", key);
    const char * p = ci_strstr(begin, n);
    if (!p || p >= end) { if (outsz) out[0] = 0; return 0; }
    p += strlen(n);
    const char * e = p;
    while (e < end && *e != '"') {
        if (*e == '\\' && e + 1 < end) e++;
        e++;
    }
    /* Decode the JSON string escapes while copying. Buienradar escapes URLs as
     * "https:\/\/…RadarMapNL?w=500&h=512" — copied raw, the literal \/ and
     * & make curl reject the radar/icon URL ("radar fetch failed"). */
    size_t o = 0;
    for (const char * q = p; q < e && o < outsz - 1; q++) {
        if (*q == '\\' && q + 1 < e) {
            char c = q[1];
            if (c == '/' || c == '"' || c == '\\') { out[o++] = c; q++; }
            else if (c == 'n' || c == 't' || c == 'r') { out[o++] = ' '; q++; }
            else if (c == 'u' && q + 5 < e) {
                unsigned v = 0; int ok = 1;
                for (int k = 2; k <= 5; k++) {
                    char h = q[k]; v <<= 4;
                    if      (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else { ok = 0; break; }
                }
                if (ok && v && v < 0x80) { out[o++] = (char)v; q += 5; }
                else                     { out[o++] = *q; }   /* non-ASCII \uXXXX: leave as-is */
            }
            else { out[o++] = *q; }       /* unknown escape: keep the backslash */
        } else {
            out[o++] = *q;
        }
    }
    out[o] = 0;
    return 1;
}

/* Compute Dutch short-day label "ma 13-5" from a "2026-05-14T00:00:00" string. */
static const char * dutch_dow[]  = {"zo","ma","di","wo","do","vr","za"};
static const char * french_dow[] = {"di","lu","ma","me","je","ve","sa"};   /* FR; EN keeps Dutch, as before */
static void format_day_label(const char * iso_date, char * out, size_t outsz) {
    if (strlen(iso_date) < 10) { snprintf(out, outsz, "?"); return; }
    int y = atoi(iso_date);
    int mo = atoi(iso_date + 5);
    int d = atoi(iso_date + 8);
    /* Zeller's congruence for day of week */
    int yy = (mo < 3) ? y - 1 : y;
    int mm = (mo < 3) ? mo + 12 : mo;
    int K = yy % 100, J = yy / 100;
    int h = (d + (13*(mm+1))/5 + K + K/4 + J/4 + 5*J) % 7;  /* 0=Sat,1=Sun,2=Mon */
    int dow = (h + 6) % 7;  /* 0=Sun..6=Sat */
    snprintf(out, outsz, "%s %d-%d",
             settings.language == LANG_FR ? french_dow[dow] : dutch_dow[dow], d, mo);
}

/* ---- Buienradar forecast: current + 5-day + hourly by location id ----------
 * forecast.buienradar.nl/2.0/forecast/<GeoNames id> is a per-place forecast
 * (not the ~52 KNMI stations of the data feed), with hourly temperature and
 * feel temperature for today and the next week. "Now" is the hourly slot
 * nearest the current time.
 *
 * Shape (keys we use):
 *   { "location":{"lat":52.03,"lon":5.66},
 *     "days":[ { …, "morning":{…}, "afternoon":{…}, …,        <- nested parts
 *                "date":"2026-11-12T00:00:00", "mintemperature":2.6,
 *                "maxtemperature":7.9, "iconcode":"j", "precipitation":20,
 *                "beaufort":1, "winddirection":"NW", "winddirectiondegrees":300,
 *                "hours":[ {"datetime":"2026-11-12T09:00:00", "temperature":2.9,
 *                           "feeltemperature":0.6, "iconcode":"nn",
 *                           "precipitation":27, "precipitationmm":0.2, …}, … ] },
 *              … ] }
 * The day-level keys also occur inside morning/afternoon/…, so day values are
 * read at the day object's own nesting level (obj_get). Today's hours start at
 * the coming hour; later days have 24 hours; days after the first week have
 * no hours. Icon codes are Buienradar's own letters, doubled at night ("aa"). */

/* Buienradar icon letter -> short NL/EN description (codes as documented by
 * python-buienradar's condition table). */
struct br_cond { char letter; const char * nl; const char * en; };
static const struct br_cond BR_COND[] = {
    {'a', "Onbewolkt",               "Clear"},
    {'b', "Half bewolkt",            "Partly cloudy"},
    {'j', "Licht bewolkt",           "Mostly clear"},
    {'o', "Half bewolkt",            "Partly cloudy"},
    {'r', "Half bewolkt",            "Partly cloudy"},
    {'c', "Zwaar bewolkt",           "Overcast"},
    {'p', "Bewolkt",                 "Cloudy"},
    {'d', "Bewolkt, lokaal mist",    "Cloudy, local fog"},
    {'n', "Mist",                    "Fog"},
    {'f', "Kans op lichte regen",    "Chance of light rain"},
    {'k', "Lichte buien",            "Light showers"},
    {'m', "Lichte regen",            "Light rain"},
    {'h', "Buien",                   "Showers"},
    {'l', "Regen",                   "Rain"},
    {'q', "Regen",                   "Rain"},
    {'w', "Natte sneeuw",            "Sleet"},
    {'u', "Lichte sneeuw",           "Light snow"},
    {'v', "Lichte sneeuw",           "Light snow"},
    {'i', "Sneeuwbuien",             "Snow showers"},
    {'t', "Zware sneeuw",            "Heavy snow"},
    {'g', "Kans op onweer",          "Chance of thunder"},
    {'s', "Onweersbuien",            "Thunderstorms"},
};
static const char * br_desc(const char * icon) {
    char l = (icon && icon[0]) ? icon[0] : 'c';
    for (size_t i = 0; i < sizeof(BR_COND)/sizeof(BR_COND[0]); i++)
        if (BR_COND[i].letter == l)
            return tr(BR_COND[i].nl, BR_COND[i].en);   /* FR via i18n_fr.c */
    return tr("Bewolkt", "Cloudy");
}

/* p at '{' or '[': returns the char after the matching close (strings skipped),
 * or end if unbalanced/truncated. */
static const char * json_skip(const char * p, const char * end) {
    int depth = 0;
    for (; p < end; p++) {
        char c = *p;
        if (c == '"') {
            for (p++; p < end && *p != '"'; p++)
                if (*p == '\\' && p + 1 < end) p++;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            if (--depth == 0) return p + 1;
        }
    }
    return end;
}

/* Value of "key" at the object's OWN level (not inside nested objects/arrays).
 * ob points at '{', oe past its '}'. Returns a pointer to the value, or NULL. */
static const char * obj_get(const char * ob, const char * oe, const char * key) {
    size_t kl = strlen(key);
    const char * p = ob + 1;
    while (p < oe) {
        char c = *p;
        if (c == '{' || c == '[') { p = json_skip(p, oe); continue; }
        if (c == '"') {
            const char * ks = p + 1, * ke = ks;
            while (ke < oe && *ke != '"') { if (*ke == '\\' && ke + 1 < oe) ke++; ke++; }
            const char * q = ke + 1;
            while (q < oe && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
            if (q < oe && *q == ':') {
                q++;
                while (q < oe && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
                if ((size_t)(ke - ks) == kl && strncasecmp(ks, key, kl) == 0) return q;
                p = q;          /* skip the value: nested values via the loop above */
                if (p < oe && *p == '"') {   /* string value: skip it whole */
                    for (p++; p < oe && *p != '"'; p++)
                        if (*p == '\\' && p + 1 < oe) p++;
                    p++;
                }
                continue;
            }
            p = ke + 1;
            continue;
        }
        p++;
    }
    return NULL;
}
static double obj_num(const char * ob, const char * oe, const char * key, double dflt) {
    const char * v = obj_get(ob, oe, key);
    if (!v) return dflt;
    if (*v == '"') v++;
    if (!((*v >= '0' && *v <= '9') || *v == '-' || *v == '.')) return dflt;   /* null */
    return strtod(v, NULL);
}
static void obj_str(const char * ob, const char * oe, const char * key, char * out, size_t outsz) {
    if (outsz) out[0] = 0;
    const char * v = obj_get(ob, oe, key);
    if (!v || *v != '"') return;
    v++;
    size_t o = 0;
    while (v < oe && *v != '"' && o + 1 < outsz) out[o++] = *v++;
    out[o] = 0;
}

/* Calls fn(ob, oe, ctx) for each object element of the array at arr ('['). */
typedef void (*obj_fn)(const char * ob, const char * oe, void * ctx);
static void each_obj(const char * arr, const char * end, obj_fn fn, void * ctx) {
    if (!arr || *arr != '[') return;
    const char * ae = json_skip(arr, end);
    for (const char * p = arr + 1; p < ae; ) {
        if (*p == '{') {
            const char * oe = json_skip(p, ae);
            if (oe >= ae && ae == end) break;          /* truncated last element */
            fn(p, oe, ctx);
            p = oe;
        } else p++;
    }
}

static void fill_hour(const char * ob, const char * oe, void * ctx) {
    weather_day_t * D = (weather_day_t *)ctx;
    if (D->hour_count >= 24) return;
    weather_hourly_slot_t * H = &D->hours[D->hour_count];
    memset(H, 0, sizeof *H);
    char iso[24];
    obj_str(ob, oe, "datetime", iso, sizeof iso);
    if (strlen(iso) < 16) return;                       /* no time: skip the slot */
    snprintf(H->label, sizeof H->label, "%.5s", iso + 11);
    H->temperature = (float)obj_num(ob, oe, "temperature", 0);
    H->feel_temp   = (float)obj_num(ob, oe, "feeltemperature", H->temperature);
    H->rain_chance = (int)obj_num(ob, oe, "precipitation", 0);
    H->rain_mm     = (float)obj_num(ob, oe, "precipitationmm", 0);
    H->wind_bft    = (int)obj_num(ob, oe, "beaufort", 0);
    H->wind_deg    = (int)obj_num(ob, oe, "winddirectiondegrees", 0);
    obj_str(ob, oe, "winddirection", H->wind_dir, sizeof H->wind_dir);
    obj_str(ob, oe, "iconcode", H->icon, sizeof H->icon);
    D->hour_count++;
}

static void fill_day(const char * ob, const char * oe, void * ctx) {
    (void)ctx;
    int dc = weather_state.day_count;
    if (dc >= WEATHER_FORECAST_DAYS) return;
    weather_day_t * D = &weather_state.days[dc];
    memset(D, 0, sizeof *D);
    char iso[24];
    obj_str(ob, oe, "date", iso, sizeof iso);
    if (strlen(iso) < 10) return;
    snprintf(D->date_iso, sizeof D->date_iso, "%.10s", iso);
    format_day_label(iso, D->day, sizeof D->day);
    D->min_temp    = (float)obj_num(ob, oe, "mintemperature", 0);
    D->max_temp    = (float)obj_num(ob, oe, "maxtemperature", 0);
    D->rain_chance = (int)obj_num(ob, oe, "precipitation", 0);
    D->wind_bft    = (int)obj_num(ob, oe, "beaufort", 0);
    D->wind_deg    = (int)obj_num(ob, oe, "winddirectiondegrees", 0);
    obj_str(ob, oe, "winddirection", D->wind_dir, sizeof D->wind_dir);
    obj_str(ob, oe, "iconcode", D->icon, sizeof D->icon);
    D->icon[1] = 0;                                     /* day overview: day variant */
    snprintf(D->desc, sizeof D->desc, "%s", br_desc(D->icon));
    each_obj(obj_get(ob, oe, "hours"), oe, fill_hour, D);
    weather_state.day_count = dc + 1;
}

/* Parses the forecast into weather_state (caller holds the lock). Fills
 * lat/lon with the forecast's own location. Returns 0 on success. */
static int parse_br_forecast(const char * body, double * lat, double * lon) {
    const char * end = body + strlen(body);
    const char * root = strchr(body, '{');
    if (!root) return -1;
    const char * re = json_skip(root, end);
    const char * days = obj_get(root, re, "days");
    if (!days || *days != '[') return -1;

    const char * loc = obj_get(root, re, "location");
    if (loc && *loc == '{') {
        const char * le = json_skip(loc, re);
        *lat = obj_num(loc, le, "lat", 0);
        *lon = obj_num(loc, le, "lon", 0);
    }

    static weather_day_t prev[WEATHER_FORECAST_DAYS];   /* keep the old forecast if this one is empty */
    memcpy(prev, weather_state.days, sizeof prev);
    int prev_dc = weather_state.day_count;
    weather_state.day_count = 0;
    each_obj(days, re, fill_day, NULL);
    if (weather_state.day_count == 0) {
        memcpy(weather_state.days, prev, sizeof prev);
        weather_state.day_count = prev_dc;
        return -1;
    }

    /* All hourly slots in time order, across days. */
    static const weather_hourly_slot_t * flat[WEATHER_FORECAST_DAYS * 24];
    int nf = 0;
    for (int d = 0; d < weather_state.day_count; d++)
        for (int h = 0; h < weather_state.days[d].hour_count; h++)
            flat[nf++] = &weather_state.days[d].hours[h];
    if (nf == 0) return -1;

    /* "Now" = the first-day slot nearest the current local time (today's hours
       start at the coming hour, so this is normally the first one). */
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    int now_min = lt.tm_hour * 60 + lt.tm_min, cur = 0, best = 1 << 30;
    for (int i = 0; i < weather_state.days[0].hour_count; i++) {
        const char * l = weather_state.days[0].hours[i].label;
        int diff = abs(atoi(l) * 60 + atoi(l + 3) - now_min);
        if (diff < best) { best = diff; cur = i; }
    }
    const weather_hourly_slot_t * C = flat[cur];
    weather_state.current_temp = C->temperature;
    weather_state.feel_temp    = C->feel_temp;
    snprintf(weather_state.current_icon, sizeof weather_state.current_icon, "%s", C->icon);
    snprintf(weather_state.current_desc, sizeof weather_state.current_desc, "%s", br_desc(C->icon));

    /* Home/dim strip: 3-hour steps from "now". */
    int hc = 0;
    for (int i = cur; i < nf && hc < WEATHER_FORECAST_HOURS; i += 3) {
        const weather_hourly_slot_t * S = flat[i];
        weather_hour_t * H = &weather_state.hours[hc++];
        memset(H, 0, sizeof *H);
        snprintf(H->label, sizeof H->label, "%s", S->label);
        H->temperature = S->temperature;
        H->feel_temp   = S->feel_temp;
        H->wind_bft    = S->wind_bft;
        snprintf(H->wind_dir, sizeof H->wind_dir, "%s", S->wind_dir);
        snprintf(H->icon, sizeof H->icon, "%s", S->icon);
    }
    weather_state.hour_count = hc;
    return 0;
}

static int fetch_br_forecast(int id) {
    if (id <= 0) return -1;
    char url[96];
    snprintf(url, sizeof url, "https://forecast.buienradar.nl/2.0/forecast/%d", id);
    static char body[160 * 1024];       /* ~80 KB today; room for growth */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) sleep(2);
        if (http_fetch(url, body, sizeof body) != 0) continue;
        double lat = 0, lon = 0;
        weather_lock();
        int rc = parse_br_forecast(body, &lat, &lon);
        weather_unlock();
        if (rc == 0) {
            /* No coordinates yet (name lookup failed, no manual Lat/Lon): use
               the forecast's own location for the rain tile. */
            if (g_cfg_lat == 0.0 && g_cfg_lon == 0.0 && (lat != 0.0 || lon != 0.0)) {
                g_cfg_lat = lat; g_cfg_lon = lon;
            }
            return 0;
        }
    }
    return -1;
}

/* Decode HTML entities, convert UTF-8 diacritics to clean ASCII for fonts,
 * strip HTML tags, and insert paragraph breaks between glued sentences. */
static void sanitize_weather_text(char * s, size_t maxlen) {
    if (!s || !*s || maxlen < 4) return;
    char * buf = (char *)malloc(maxlen + 512);
    if (!buf) return;
    size_t o = 0;
    const char * r = s;
    while (*r && o < maxlen - 4) {
        /* Fix missing space / paragraph break between sentence and next word:
         * e.g. "km/u.Vanochtend" -> "km/u.\n\nVanochtend" */
        if ((*r == '.' || *r == '!' || *r == '?') && (r[1] >= 'A' && r[1] <= 'Z')) {
            buf[o++] = *r++;
            buf[o++] = '\n';
            buf[o++] = '\n';
            continue;
        }

        /* HTML entities */
        if (*r == '&') {
            const char * semi = strchr(r, ';');
            if (semi && (semi - r) <= 12) {
                int len = (int)(semi - r + 1);
                char ent[16];
                memcpy(ent, r, len);
                ent[len] = '\0';

                char rep = 0;
                if (!strcasecmp(ent, "&nbsp;"))        rep = ' ';
                else if (!strcasecmp(ent, "&eacute;")) rep = 'e';
                else if (!strcasecmp(ent, "&egrave;")) rep = 'e';
                else if (!strcasecmp(ent, "&euml;"))   rep = 'e';
                else if (!strcasecmp(ent, "&ecirc;"))  rep = 'e';
                else if (!strcasecmp(ent, "&aacute;")) rep = 'a';
                else if (!strcasecmp(ent, "&agrave;")) rep = 'a';
                else if (!strcasecmp(ent, "&auml;"))   rep = 'a';
                else if (!strcasecmp(ent, "&acirc;"))  rep = 'a';
                else if (!strcasecmp(ent, "&oacute;")) rep = 'o';
                else if (!strcasecmp(ent, "&ograve;")) rep = 'o';
                else if (!strcasecmp(ent, "&ouml;"))   rep = 'o';
                else if (!strcasecmp(ent, "&ocirc;"))  rep = 'o';
                else if (!strcasecmp(ent, "&iacute;")) rep = 'i';
                else if (!strcasecmp(ent, "&igrave;")) rep = 'i';
                else if (!strcasecmp(ent, "&iuml;"))   rep = 'i';
                else if (!strcasecmp(ent, "&icirc;"))  rep = 'i';
                else if (!strcasecmp(ent, "&uacute;")) rep = 'u';
                else if (!strcasecmp(ent, "&ugrave;")) rep = 'u';
                else if (!strcasecmp(ent, "&uuml;"))   rep = 'u';
                else if (!strcasecmp(ent, "&ucirc;"))  rep = 'u';
                else if (!strcasecmp(ent, "&ccedil;")) rep = 'c';
                else if (!strcasecmp(ent, "&amp;"))    rep = '&';
                else if (!strcasecmp(ent, "&quot;"))   rep = '"';
                else if (!strcasecmp(ent, "&apos;"))   rep = '\'';
                else if (!strcasecmp(ent, "&lt;"))     rep = '<';
                else if (!strcasecmp(ent, "&gt;"))     rep = '>';
                else if (!strcasecmp(ent, "&deg;")) {
                    buf[o++] = (char)0xC2; buf[o++] = (char)0xB0; /* degree symbol */
                    r += len;
                    continue;
                } else if (ent[1] == '#') {
                    int val = atoi(ent + 2);
                    if (val > 0 && val < 128) rep = (char)val;
                }

                if (rep) {
                    if (r[1] >= 'A' && r[1] <= 'Z' && rep >= 'a' && rep <= 'z')
                        rep = rep - 'a' + 'A';
                    buf[o++] = rep;
                    r += len;
                    continue;
                } else {
                    /* Unknown entity: drop */
                    r += len;
                    continue;
                }
            }
        }

        /* HTML tags: <br>, <p>, etc. */
        if (*r == '<') {
            const char * endtag = strchr(r, '>');
            if (endtag && (endtag - r) <= 10) {
                int tlen = (int)(endtag - r + 1);
                if (!strncasecmp(r, "<br", 3) || !strncasecmp(r, "<p", 2)) {
                    buf[o++] = '\n';
                }
                r += tlen;
                continue;
            }
        }

        /* UTF-8 accented characters */
        unsigned char c0 = (unsigned char)r[0];
        unsigned char c1 = (unsigned char)r[1];
        if (c0 == 0xC3) {
            char rep = 0;
            switch (c1) {
                case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: rep = 'a'; break;
                case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: rep = 'A'; break;
                case 0xA8: case 0xA9: case 0xAA: case 0xAB: rep = 'e'; break;
                case 0x88: case 0x89: case 0x8A: case 0x8B: rep = 'E'; break;
                case 0xAC: case 0xAD: case 0xAE: case 0xAF: rep = 'i'; break;
                case 0x8C: case 0x8D: case 0x8E: case 0x8F: rep = 'I'; break;
                case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: rep = 'o'; break;
                case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: rep = 'O'; break;
                case 0xB9: case 0xBA: case 0xBB: case 0xBC: rep = 'u'; break;
                case 0x99: case 0x9A: case 0x9B: case 0x9C: rep = 'U'; break;
                case 0xA7: rep = 'c'; break;
                case 0x87: rep = 'C'; break;
                default: break;
            }
            if (rep) {
                buf[o++] = rep;
                r += 2;
                continue;
            }
        } else if (c0 == 0xE2 && c1 == 0x80) {
            unsigned char c2 = (unsigned char)r[2];
            char rep = 0;
            if (c2 == 0x98 || c2 == 0x99) rep = '\'';
            else if (c2 == 0x9C || c2 == 0x9D) rep = '"';
            else if (c2 == 0x93 || c2 == 0x94) rep = '-';
            else if (c2 == 0xA6) {
                buf[o++] = '.'; buf[o++] = '.'; buf[o++] = '.';
                r += 3;
                continue;
            }
            if (rep) {
                buf[o++] = rep;
                r += 3;
                continue;
            }
        }

        buf[o++] = *r++;
    }
    buf[o] = '\0';
    strncpy(s, buf, maxlen - 1);
    s[maxlen - 1] = '\0';
    free(buf);
}

/* buienradar feed — used for the radar-map URL + the NL weather-report
 * narrative, plus the nearest station's temperature if the forecast fetch failed. */
static int parse_buienradar_extras(const char * body) {

    /* --- radar image URL (lives in "actual" object) --- */
    js_str(body, body + strlen(body), "actualradarurl",
           weather_state.radar_url, sizeof(weather_state.radar_url));

    /* If the forecast fetch failed, pick the closest KNMI station so live real + feel
     * temperatures stay fresh instead of freezing at an old reading. */
    if (!weather_state.connected) {
        const char * sm = ci_strstr(body, "\"stationmeasurements\":");
        if (sm) {
            double ref_lat = (g_cfg_lat != 0.0) ? g_cfg_lat : 52.1009;
            double ref_lon = (g_cfg_lon != 0.0) ? g_cfg_lon : 5.1783;
            double best_d2 = 1e9, best_t = 0.0, best_f = 0.0;
            int found = 0;
            const char * p = sm;
            while ((p = strchr(p, '{')) != NULL) {
                const char * e = strchr(p, '}');
                if (!e) break;
                double slat = js_num(p, e, "lat", 0.0);
                double slon = js_num(p, e, "lon", 0.0);
                double stmp = js_num(p, e, "temperature", -999.0);
                if (slat != 0.0 && stmp > -100.0) {
                    double dlat = slat - ref_lat;
                    double dlon = (slon - ref_lon) * 0.62;
                    double d2 = dlat * dlat + dlon * dlon;
                    if (d2 < best_d2) {
                        best_d2 = d2;
                        best_t  = stmp;
                        best_f  = js_num(p, e, "feeltemperature", stmp);
                        found   = 1;
                    }
                }
                p = e + 1;
                if (*p == ']') break;
            }
            if (found) {
                weather_state.current_temp = (float)best_t;
                weather_state.feel_temp    = (float)best_f;
                weather_state.connected    = 1;
            }
        }
    }

    /* --- weatherreport title + text --- */
    const char * wr = ci_strstr(body, "\"weatherreport\":");
    if (wr) {
        const char * wr_end = ci_strstr(wr, "\"shortterm\"");
        if (!wr_end) wr_end = wr + 4096;
        js_str(wr, wr_end, "title", weather_state.weatherreport_title,
               sizeof(weather_state.weatherreport_title));
        js_str(wr, wr_end, "text", weather_state.weatherreport_text,
               sizeof(weather_state.weatherreport_text));
        sanitize_weather_text(weather_state.weatherreport_title, sizeof(weather_state.weatherreport_title));
        sanitize_weather_text(weather_state.weatherreport_text, sizeof(weather_state.weatherreport_text));
    }

    return 0;
}

/* Fetches the radar GIF to disk via curl. We don't write our own libcurl
   binding — popen out, redirect to file, atomic rename. The forecast
   screen reads /tmp/toonui_radar.gif via LVGL's stdio FS driver + lv_gif. */
static int fetch_radar_image(void) {
    if (!weather_state.radar_url[0]) return -1;
    /* The URL comes from Buienradar's JSON (fetched without certificate
       checks), and js_str decodes \' — so it is NOT quote-safe. Require an
       http(s) URL and shell-quote it. */
    char cmd[1024], qurl[700];
    if (!sh_is_http_url(weather_state.radar_url) ||
        !sh_quote(qurl, sizeof qurl, weather_state.radar_url)) return -1;
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s -k -L --max-time 10 -A 'toonui/1.0' "
        "-o /tmp/toonui_radar.gif.tmp %s && mv /tmp/toonui_radar.gif.tmp /tmp/toonui_radar.gif",
        qurl);
    int rc = system(cmd);
    return (rc == 0) ? 0 : -1;
}


/* ---- Place name -> Buienradar location id + coordinates --------------------
 * location.buienradar.nl/1.1/location/search?query=<name> returns
 *   [ {"id":2756429,"name":"Ede","countrycode":"NL",
 *      "location":{"lat":52.03333,"lon":5.65833}, …}, … ]
 * ranked Dutch-first, but worldwide (e.g. "Bergen" also has Norway), so a
 * Dutch match wins, then a Belgian one, then the first result. The id is the
 * GeoNames id the forecast endpoint takes. */
struct br_pick { int rank, id; double lat, lon; char cc[4]; };
static void pick_place(const char * ob, const char * oe, void * ctx) {
    struct br_pick * P = (struct br_pick *)ctx;
    if (P->rank == 0) return;                           /* already have a Dutch one */
    int id = (int)obj_num(ob, oe, "id", 0);
    if (id <= 0) return;
    char cc[4];
    obj_str(ob, oe, "countrycode", cc, sizeof cc);
    int rank = !strcasecmp(cc, "NL") ? 0 : !strcasecmp(cc, "BE") ? 1 : 2;
    if (rank >= P->rank) return;
    P->rank = rank; P->id = id; P->lat = P->lon = 0;
    snprintf(P->cc, sizeof P->cc, "%s", cc);
    const char * loc = obj_get(ob, oe, "location");
    if (loc && *loc == '{') {
        const char * le = json_skip(loc, oe);
        P->lat = obj_num(loc, le, "lat", 0);
        P->lon = obj_num(loc, le, "lon", 0);
    }
}

/* Looks the place up; returns its id (0 = not found / no answer) and its
 * coordinates in lat/lon. No side effects: runs on the UI thread (settings
 * save) as well as on wx_thread. The name is percent-encoded (http_fetch
 * rejects spaces/quotes), so "Sint Pancras" is looked up safely. */
static int br_find_place(const char * city, double * lat, double * lon) {
    *lat = *lon = 0;
    if (!city || !city[0]) return 0;
    char enc[160]; size_t o = 0;
    for (const char * p = city; *p && o + 4 < sizeof enc; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            enc[o++] = (char)c;
        } else {
            snprintf(enc + o, 4, "%%%02X", c);
            o += 3;
        }
    }
    enc[o] = 0;
    char url[256];
    snprintf(url, sizeof url, "https://location.buienradar.nl/1.1/location/search?query=%s", enc);
    char body[24 * 1024];
    /* 15 s, not 8: at start-up the Toon 1's CPU is busy and curl's TLS
       handshake alone can take most of 8 s. */
    if (http_fetch_to(url, body, sizeof body, 15) != 0) {
        fprintf(stderr, "[wx] place '%s': no response from location.buienradar.nl\n", city);
        return 0;
    }
    struct br_pick P = { .rank = 99 };
    const char * arr = strchr(body, '[');
    each_obj(arr, body + strlen(body), pick_place, &P);
    if (P.id <= 0) {
        fprintf(stderr, "[wx] place '%s': no match — check the spelling in "
                        "Settings > Weather\n", city);
        return 0;
    }
    *lat = P.lat; *lon = P.lon;
    fprintf(stderr, "[wx] place '%s' -> id %d (%.4f,%.4f, %s)\n",
            city, P.id, P.lat, P.lon, P.cc[0] ? P.cc : "?");
    return P.id;
}

int weather_geocode(const char * city) {
    double lat, lon;
    return br_find_place(city, &lat, &lon);
}

/* The location in use: Buienradar id for the forecast, coordinates for the
 * rain tile (manual Lat/Lon win over the place's own).
 *
 * Start-up uses the stored id straight away and checks the place name
 * afterwards: on a Toon 1 the name lookup, run while the UI is still starting,
 * timed out (curl exit 28 — the TLS handshake doesn't finish on the busy
 * 400 MHz CPU) although the same request takes 1.5 s later on. The check is
 * retried at every radar refresh (5 min) until it gets an answer; a name that
 * maps to a different id — e.g. one saved during the old geocoder's
 * "Ede = Nigeria" bug — switches the forecast to it. */
static int g_loc_id = 0;
static int g_loc_by_name = 0;      /* id confirmed by the name lookup */

static int manual_coords(double * lat, double * lon) {
    if (!settings.weather_lat[0] || !settings.weather_lon[0]) return 0;
    *lat = coord_atof(settings.weather_lat);
    *lon = coord_atof(settings.weather_lon);
    return (*lat != 0.0 || *lon != 0.0);
}

/* (Re)start on the configured location: the stored id, or — without one —
 * the name lookup now, or De Bilt. */
static void weather_locate(void) {
    double lat = 0, lon = 0;
    int id = settings.weather_location_id;
    g_loc_by_name = 0;
    if (id <= 0 && settings.weather_location[0]) {
        id = br_find_place(settings.weather_location, &lat, &lon);
        g_loc_by_name = (id > 0);
    }
    if (id <= 0) {
        id = 2757783;                        /* De Bilt */
        fprintf(stderr, "[wx] no location found — using De Bilt (%d)\n", id);
    } else if (!g_loc_by_name) {
        fprintf(stderr, "[wx] using location id %d from settings\n", id);
    }
    g_loc_id = id;
    if (manual_coords(&lat, &lon))
        fprintf(stderr, "[wx] using manual location (%.4f,%.4f) for the rain tile\n", lat, lon);
    g_cfg_lat = lat; g_cfg_lon = lon;        /* 0,0: filled from the forecast */
}

/* Confirms the place name (once per location). Returns 1 if it maps to a
 * different id than the one in use — the caller then refetches. */
static int weather_check_name(void) {
    if (g_loc_by_name || !settings.weather_location[0]) return 0;
    double lat, lon;
    int id = br_find_place(settings.weather_location, &lat, &lon);
    if (id <= 0) return 0;                   /* no answer / not found: try again later */
    g_loc_by_name = 1;
    double mlat, mlon;
    if (!manual_coords(&mlat, &mlon) && (lat != 0.0 || lon != 0.0)) {
        g_cfg_lat = lat; g_cfg_lon = lon;    /* the place's own, finer than the forecast's */
    }
    if (id == g_loc_id) return 0;
    fprintf(stderr, "[wx] '%s' is id %d, not %d — switching\n",
            settings.weather_location, id, g_loc_id);
    g_loc_id = id;
    return 1;
}

static void * wx_thread(void * arg) {
    (void)arg;
    sleep(5);                            /* initial settling delay for WiFi at boot */
    static char body[64 * 1024];
    int radar_tick = 0;
    while (1) {
        /* Weather settings saved: start over on the new location now. Without
           this, a changed city or Lat/Lon only took effect after a UI restart. */
        if (g_wx_relocate) {
            g_wx_relocate = 0;
            g_loc_id = 0;
        }
        if (g_loc_id == 0) weather_locate();

        /* Current conditions + 5-day + hourly — Buienradar forecast for the place. */
        if (fetch_br_forecast(g_loc_id) == 0) {
            weather_state.connected = 1;
            fprintf(stderr, "[wx] %s %.1fC @ id %d, %d-day %d-hour\n",
                    weather_state.current_desc, weather_state.current_temp,
                    g_loc_id, weather_state.day_count, weather_state.hour_count);
        } else {
            weather_state.connected = 0;
            fprintf(stderr, "[wx] forecast fetch failed (id %d)\n", g_loc_id);
        }

        /* buienradar feed — radar-map URL + NL weather report, and the nearest
           station's temperature if the forecast fetch failed. */
        if (http_fetch("https://data.buienradar.nl/2.0/feed/json",
                       body, sizeof(body)) == 0) {
            weather_lock();
            parse_buienradar_extras(body);
            weather_unlock();
        }

        /* No data at all (e.g. WiFi still connecting on boot): retry every 10 s
         * until the first successful fetch, rather than sleeping 15 minutes. */
        if (!weather_state.connected) {
            for (int s = 0; s < 10 && !g_wx_wake; s++)
                sleep(1);
            g_wx_wake = 0;
            continue;
        }

        /* Fetch the radar image every 5 minutes, waking early on
           weather_refresh_async(); confirm the place name meanwhile. */
        int refetch = weather_check_name();
        for (int i = 0; i < 3 && !g_wx_wake && !refetch; i++) {
            if (fetch_radar_image() == 0)
                fprintf(stderr, "[wx] radar refreshed (tick %d)\n", radar_tick++);
            else
                fprintf(stderr, "[wx] radar fetch failed\n");
            for (int s = 0; s < 5 * 60 && !g_wx_wake; s++)
                sleep(1);
            if (!g_wx_wake) refetch = weather_check_name();
        }
        g_wx_wake = 0;
    }
    return NULL;
}
void weather_refresh_async(void) {
    g_wx_wake = 1;
}

void weather_relocate_async(void) {
    g_wx_relocate = 1;
    g_wx_wake = 1;
}

int weather_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, wx_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}

#ifdef SIM
/* One synchronous lookup + forecast fetch, printing the result (sim `wxfetch`). */
int weather_fetch_now(void) {
    g_loc_id = 0;
    weather_locate();
    if (fetch_br_forecast(g_loc_id) != 0) { fprintf(stderr, "forecast fetch failed\n"); return -1; }
    if (weather_check_name() && fetch_br_forecast(g_loc_id) != 0) {
        fprintf(stderr, "forecast fetch failed\n"); return -1;
    }
    double lat, lon; weather_get_lat_lon(&lat, &lon);
    printf("id %d  rain-tile coords %.4f,%.4f\n", g_loc_id, lat, lon);
    printf("now: %.1fC (feel %.1fC) icon '%s' %s\n", weather_state.current_temp,
           weather_state.feel_temp, weather_state.current_icon, weather_state.current_desc);
    printf("strip:");
    for (int i = 0; i < weather_state.hour_count; i++)
        printf("  %s %.1fC %s %d%s", weather_state.hours[i].label, weather_state.hours[i].temperature,
               weather_state.hours[i].icon, weather_state.hours[i].wind_bft, weather_state.hours[i].wind_dir);
    printf("\n");
    for (int d = 0; d < weather_state.day_count; d++) {
        const weather_day_t * D = &weather_state.days[d];
        printf("%-8s %s %4.1f..%4.1f rain %3d%% bft %d %-3s icon %-2s %-22s %2d h",
               D->day, D->date_iso, D->min_temp, D->max_temp, D->rain_chance, D->wind_bft,
               D->wind_dir, D->icon, D->desc, D->hour_count);
        if (D->hour_count)
            printf(" [%s %.1fC/%.1fC %d%% %.1fmm %s .. %s]", D->hours[0].label, D->hours[0].temperature,
                   D->hours[0].feel_temp, D->hours[0].rain_chance, D->hours[0].rain_mm, D->hours[0].icon,
                   D->hours[D->hour_count - 1].label);
        printf("\n");
    }
    return 0;
}
#endif
