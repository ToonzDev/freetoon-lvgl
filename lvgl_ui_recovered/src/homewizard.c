/*
 * Background poller for two HomeWizard P1 devices. Their LAN addresses come
 * from settings (settings.p1_elec_host / p1_water_host) so no personal IP is
 * baked into the binary; a poller is skipped when its host is empty.
 *
 * Both expose GET /api/v1/data returning a flat JSON object.
 * We parse the handful of fields we need with strstr/strtod; no JSON
 * library dependency.
 */
#include "homewizard.h"
#include "meteradapter.h"
#include "rrd_push.h"
#include "settings.h"
#include "canonical_state.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>   /* struct timeval — not transitively pulled in by sys/socket.h under musl/emscripten */
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

hw_state_t hw_state = {0};

static pthread_mutex_t g_hw_state_mtx = PTHREAD_MUTEX_INITIALIZER;

void homewizard_lock(void) {
    pthread_mutex_lock(&g_hw_state_mtx);
}

void homewizard_unlock(void) {
    pthread_mutex_unlock(&g_hw_state_mtx);
}

void homewizard_get_snapshot(hw_state_t * out) {
    if (!out) return;
    pthread_mutex_lock(&g_hw_state_mtx);
    *out = hw_state;
    pthread_mutex_unlock(&g_hw_state_mtx);
}

hw_socket_t hw_sockets[HW_SOCKET_MAX];

/* Plain HTTP/1.0 to a HomeWizard device (local API v1, port 80). `ip` must be
 * an IPv4 address. `body` NULL = GET, otherwise PUT with a JSON body. */
static int http_req(const char * ip, const char * path, const char * body,
                    char * out, size_t outsz) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(80);
#ifdef SIM
    { const char * p = getenv("HW_SIM_PORT");   /* fake devices need no root */
      if (p && atoi(p) > 0) a.sin_port = htons((uint16_t)atoi(p)); }
#endif
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); return -1; }
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { close(s); return -1; }
    char req[384];
    int n = body
        ? snprintf(req, sizeof(req),
            "PUT %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\n"
            "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
            path, ip, (unsigned)strlen(body), body)
        : snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, ip);
    if (n <= 0 || n >= (int)sizeof req) { close(s); return -1; }
    if (send(s, req, n, 0) != n) { close(s); return -1; }
    size_t got = 0;
    while (got < outsz - 1) {
        ssize_t k = recv(s, out + got, outsz - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k;
    }
    out[got] = 0;
    close(s);
    return 0;
}

static int http_get(const char * ip, const char * path, char * out, size_t outsz) {
    return http_req(ip, path, NULL, out, outsz);
}

/* Find "key": <number> and return parsed double. Returns dflt if missing. */
static double parse_num(const char * json, const char * key, double dflt) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return dflt;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n') return dflt;   /* null */
    return strtod(p, NULL);
}

/* --- trailing-60-min gas usage, derived from the cumulative total_gas_m3 ---
 * The P1 only exposes a cumulative counter, so keep ~1-minute snapshots in a
 * ring and report the consumption over the trailing hour. While the ring hasn't
 * yet spanned a full hour we report the delta over whatever window we have (an
 * under-estimate that self-corrects as the ring fills) — deliberately NOT
 * extrapolated, so an early short window can't spike a consumer's auto-scale. */
#define GAS_RING_N 64
static struct { long t; float m3; } gas_ring[GAS_RING_N];
static int  gas_ring_head = 0, gas_ring_count = 0;
static long gas_ring_last_t = 0;

static void gas_hour_update(float gas_now) {
    if (gas_now <= 0) { hw_state.gas_hour_m3 = 0; return; }  /* no gas meter read */
    long now = time(NULL);
    if (gas_ring_last_t == 0 || now - gas_ring_last_t >= 55) {  /* ~1/min */
        gas_ring[gas_ring_head].t  = now;
        gas_ring[gas_ring_head].m3 = gas_now;
        gas_ring_head = (gas_ring_head + 1) % GAS_RING_N;
        if (gas_ring_count < GAS_RING_N) gas_ring_count++;
        gas_ring_last_t = now;
    }
    if (gas_ring_count < 2) { hw_state.gas_hour_m3 = 0; return; }
    long cutoff = now - 3600;
    int oldest = (gas_ring_head - gas_ring_count + GAS_RING_N) % GAS_RING_N;
    /* newest sample that is still at/older than 1h ago; else the oldest we have */
    int ref = oldest;
    for (int k = 0; k < gas_ring_count; k++) {
        int i = (oldest + k) % GAS_RING_N;
        if (gas_ring[i].t <= cutoff) ref = i; else break;
    }
    float delta = gas_now - gas_ring[ref].m3;
    if (delta < 0) delta = 0;          /* counter reset / rollover guard */
    hw_state.gas_hour_m3 = delta;
}

/* Gas from the P1's "external" device list:
 *   "external":[{"unique_id":"..","type":"gas_meter","timestamp":251113101500,
 *                "value":2569.646,"unit":"m3"}, {...water_meter...}]
 * HomeWizard calls the flat total_gas_m3 deprecated in favour of this list,
 * and it can be null while the list has the reading. Returns the gas meter
 * with the newest timestamp (a replaced meter can leave an old entry), or
 * -1 when there is none. */
static double parse_external_gas(const char * json) {
    double best = -1, best_ts = -1;
    const char * p = json;
    while ((p = strstr(p, "\"gas_meter\"")) != NULL) {
        /* the enclosing {...} of this entry (entries are flat objects) */
        const char * obj = p;
        while (obj > json && *obj != '{') obj--;
        const char * end = strchr(p, '}');
        p += 11;
        if (*obj != '{' || !end) continue;
        char entry[320];
        size_t n = (size_t)(end - obj);
        if (n >= sizeof entry) continue;
        memcpy(entry, obj, n);
        entry[n] = 0;
        /* must be the type field, not e.g. a unique_id that happens to match */
        if (!strstr(entry, "\"type\":\"gas_meter\"") && !strstr(entry, "\"type\": \"gas_meter\"")) continue;
        double v  = parse_num(entry, "value", -1);
        double ts = parse_num(entry, "timestamp", 0);
        if (v > 0 && ts >= best_ts) { best = v; best_ts = ts; }
    }
    return best;
}

static void poll_p1(void) {
    if (!settings.enable_p1_elec || !settings.p1_elec_host[0]) { hw_state.connected_p1 = 0; return; }
    hw_state.polled_p1 = 1;             /* a poll is being attempted */
    static char body[4096];
    if (http_get(settings.p1_elec_host, "/api/v1/data", body, sizeof(body)) != 0) {
        hw_state.connected_p1 = 0;
        return;
    }
    const char * j = strstr(body, "\r\n\r\n");
    j = j ? j + 4 : body;
    if (!strstr(j, "active_power_w")) {
        hw_state.connected_p1 = 0;
        return;
    }
    homewizard_lock();
    hw_state.power_w          = (float)parse_num(j, "active_power_w",         0);
    hw_state.kwh_import_t1    = (float)parse_num(j, "total_power_import_t1_kwh", 0);
    hw_state.kwh_import_t2    = (float)parse_num(j, "total_power_import_t2_kwh", 0);
    hw_state.kwh_import_total = (float)parse_num(j, "total_power_import_kwh", 0);
    hw_state.kwh_export_total = (float)parse_num(j, "total_power_export_kwh", 0);
    hw_state.kwh_export_t1    = (float)parse_num(j, "total_power_export_t1_kwh", 0);
    hw_state.kwh_export_t2    = (float)parse_num(j, "total_power_export_t2_kwh", 0);
    hw_state.tariff           = (int)  parse_num(j, "active_tariff",          1);
    double gas_m3 = parse_num(j, "total_gas_m3", 0);
    if (gas_m3 <= 0) {                       /* missing / null: use the external list */
        double ext = parse_external_gas(j);
        if (ext > 0) gas_m3 = ext;
    }
    hw_state.gas_m3           = (float)gas_m3;
    gas_hour_update(hw_state.gas_m3);
    hw_state.voltage_l1_v     = (float)parse_num(j, "active_voltage_l1_v",    0);
    hw_state.current_l1_a     = (float)parse_num(j, "active_current_a",       0);
    hw_state.connected_p1     = 1;
    homewizard_unlock();
    canonical_state_notify(CSTATE_EVT_ENERGY);
}

/* Track per-pour session totals. session_start_m3 is captured the moment
 * flow rises above zero; session_l keeps the running delta. When flow stays
 * zero for the full grace window the session is finalised but kept visible
 * for an additional fade window so the UI can display "+X.X L" briefly. */
static float session_start_m3 = 0;
static int   session_zero_seconds = 0;
#define WATER_SESSION_GRACE_S 20   /* zero-flow seconds before we close   */
#define WATER_SESSION_FADE_S  60   /* keep "+X L" visible this long after */
#define WATER_POLL_INTERVAL_S 2    /* HomeWizard HWE-WTR */
/* oepi-loepi's ToonWater sensor is an ESP8266: like the HWE-P1 (see hw_thread)
 * it has a tiny lwIP socket table, so a fresh connection every 2 s risks
 * hanging it. ToonWater itself polls every 10 s; 6 s keeps the tap readout
 * responsive. Must be a multiple of the 2 s loop. */
#define TOONWATER_POLL_INTERVAL_S 6

/* Previous-poll total. Used to detect flow by diffing the cumulative
 * counter (which ticks per litre and is reliable down to 1 L pours) —
 * the device's "active_liter_lpm" field needs the flow to stay above
 * ~0.5 L/min for ~10 s before it registers, so trickle pours and a
 * just-opened tap stay invisible if we trust it alone. */
static float prev_total_m3 = -1.0f;

static void water_disconnected(void) {
    hw_state.connected_water = 0;
    /* Zero the live flow too — otherwise the home tile keeps the stale L/min
     * reading (and its spinner) up indefinitely until the meter comes back. */
    hw_state.water_lpm = 0;
}

/* A new water reading (cumulative m³ + the device's own L/min), polled every
 * interval_s seconds: derive the flow, update the tap session. Shared by the
 * HomeWizard HWE-WTR and the ToonWater sensor. */
static void water_update(float new_total_m3, float device_lpm, int interval_s);

static void poll_water(void) {
    if (!settings.enable_p1_water || !settings.p1_water_host[0]) {
        water_disconnected();
        return;
    }
    hw_state.polled_water = 1;          /* a poll is being attempted */
    static char body[2048];
    if (http_get(settings.p1_water_host, "/api/v1/data", body, sizeof(body)) != 0) {
        water_disconnected();
        return;
    }
    const char * j = strstr(body, "\r\n\r\n");
    j = j ? j + 4 : body;
    if (!strstr(j, "total_liter_m3")) {
        hw_state.connected_water = 0;
        return;
    }
    water_update((float)parse_num(j, "total_liter_m3",   0),
                 (float)parse_num(j, "active_liter_lpm", 0), WATER_POLL_INTERVAL_S);
}

/* kWh meter (HWE-KWH1/3, SDM230/630) on the PV inverter. Its active_power_w
 * sign depends on how it is wired (production is usually negative), so the
 * Solar tile gets the magnitude. */
static void poll_kwh(void) {
    if (!settings.hw_kwh_host[0]) { hw_state.connected_kwh = 0; hw_state.polled_kwh = 0; return; }
    hw_state.polled_kwh = 1;
    static char body[2048];
    if (http_get(settings.hw_kwh_host, "/api/v1/data", body, sizeof(body)) != 0) {
        hw_state.connected_kwh = 0;
        return;
    }
    const char * j = strstr(body, "\r\n\r\n");
    j = j ? j + 4 : body;
    if (!strstr(j, "active_power_w")) { hw_state.connected_kwh = 0; return; }
    double w = parse_num(j, "active_power_w", 0);
    hw_state.kwh_power_w   = (float)(w < 0 ? -w : w);
    hw_state.connected_kwh = 1;
    canonical_state_notify(CSTATE_EVT_ENERGY);
}

/* Energy Socket: GET /api/v1/data (active_power_w) + GET /api/v1/state
 * ({"power_on":true,"switch_lock":false,"brightness":255}). */
static volatile int s_socket_pending[HW_SOCKET_MAX];   /* a PUT is in flight */

static void poll_socket(int i) {
    hw_socket_t * S = &hw_sockets[i];
    const char * ip = settings.hw_socket_host[i];
    if (!ip[0]) { S->polled = 0; S->connected = 0; return; }
    S->polled = 1;
    char body[1536];
    if (http_get(ip, "/api/v1/data", body, sizeof body) != 0 || !strstr(body, "active_power_w")) {
        S->connected = 0;
        return;
    }
    const char * j = strstr(body, "\r\n\r\n");
    S->power_w = (float)parse_num(j ? j + 4 : body, "active_power_w", 0);
    if (s_socket_pending[i]) { S->connected = 1; return; }   /* don't undo a switch */
    if (http_get(ip, "/api/v1/state", body, sizeof body) != 0) { S->connected = 0; return; }
    const char * p = strstr(body, "\"power_on\":");
    if (p) {
        p += 11;
        while (*p == ' ') p++;
        S->on = (*p == 't');
    }
    S->connected = 1;
}

typedef struct { int i; int on; } sock_cmd_t;

static void * socket_set_thread(void * arg) {
    sock_cmd_t c = *(sock_cmd_t *)arg;
    free(arg);
    char ip[sizeof settings.hw_socket_host[0]];
    snprintf(ip, sizeof ip, "%s", settings.hw_socket_host[c.i]);
    char out[512];
    int rc = http_req(ip, "/api/v1/state", c.on ? "{\"power_on\":true}" : "{\"power_on\":false}",
                      out, sizeof out);
    if (rc != 0 || !strstr(out, "power_on")) {
        fprintf(stderr, "[hw] socket %d switch failed\n", c.i);
        hw_sockets[c.i].on = !c.on;          /* show the real state again */
    }
    s_socket_pending[c.i] = 0;
    return NULL;
}

void homewizard_socket_set_async(int i, int on) {
    if (i < 0 || i >= HW_SOCKET_MAX) return;
    if (settings.client_mode) {
        /* A client switches through its master (POST /api/hw/socket). */
        extern int client_link_socket_set(int i, int on);
        if (!hw_sockets[i].remote_present) return;
        hw_sockets[i].on = on ? 1 : 0;           /* optimistic; next state frame confirms */
        client_link_socket_set(i, on);
        return;
    }
    if (!settings.hw_socket_host[i][0]) return;
    if (s_socket_pending[i]) return;
    sock_cmd_t * c = malloc(sizeof *c);
    if (!c) return;
    c->i = i; c->on = on ? 1 : 0;
    s_socket_pending[i] = 1;
    hw_sockets[i].on = c->on;                /* optimistic */
    pthread_t t;
    if (pthread_create(&t, NULL, socket_set_thread, c) != 0) {
        free(c);
        s_socket_pending[i] = 0;
        return;
    }
    pthread_detach(t);
}

int homewizard_socket_present(int i) {
    if (i < 0 || i >= HW_SOCKET_MAX) return 0;
    return settings.client_mode ? hw_sockets[i].remote_present : settings.hw_socket_host[i][0] != 0;
}

const char * homewizard_socket_name(int i) {
    if (i < 0 || i >= HW_SOCKET_MAX) return "";
    if (settings.client_mode) return hw_sockets[i].remote_name;
    return settings.hw_socket_name[i][0] ? settings.hw_socket_name[i] : settings.hw_socket_host[i];
}

int homewizard_socket_count(void) {
    int n = 0;
    for (int i = 0; i < HW_SOCKET_MAX; i++) if (homewizard_socket_present(i)) n++;
    return n;
}

/* "key":"123" or "key":123 → number; dflt if missing. The ToonWater sensor
 * quotes its numbers, and one firmware drops the opening quote of a value
 * ("pulselength":13693") — skipping an optional quote copes with both. */
static double parse_num_q(const char * json, const char * key, double dflt) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return dflt;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    char * e;
    double v = strtod(p, &e);
    return (e == p) ? dflt : v;
}

/* oepi-loepi's ToonWater sensor: GET http://<host>/water.html →
 *   {"waterflow":"0","waterquantity":"1294748","today":"48",...,"leakdetect":"0"}
 * waterflow in L/min, waterquantity = meter reading in litres. */
static void poll_toonwater(void) {
    if (!settings.enable_p1_water) { water_disconnected(); return; }
    hw_state.polled_water = 1;
    char url[96];
    snprintf(url, sizeof url, "http://%s/water.html", settings.toonwater_host);
    static char body[1024];
    if (http_fetch_to(url, body, sizeof body, 4) != 0 || !strstr(body, "\"waterquantity\"")) {
        water_disconnected();
        return;
    }
    double litres = parse_num_q(body, "waterquantity", -1);
    if (litres < 0) { water_disconnected(); return; }
    water_update((float)(litres / 1000.0), (float)parse_num_q(body, "waterflow", 0),
                 TOONWATER_POLL_INTERVAL_S);
}

static void water_update(float new_total_m3, float device_lpm, int interval_s) {
    /* Delta-derived flow: litres since last poll → L/min. Picks up tiny
     * pours the device's smoothed lpm misses entirely. The device's own
     * lpm wins when it's larger (heavy flow ramping up faster than the
     * poll tick resolves). */
    float derived_lpm = 0;
    if (prev_total_m3 >= 0 && new_total_m3 >= prev_total_m3) {
        float dl = (new_total_m3 - prev_total_m3) * 1000.0f;   /* litres */
        derived_lpm = dl * (60.0f / (float)interval_s);
    }
    prev_total_m3 = new_total_m3;

    float shown_lpm = (device_lpm > derived_lpm) ? device_lpm : derived_lpm;

    /* Demo override: if /tmp/demo_water exists with a number, treat it as the
     * live L/min. Used only to record marketing GIFs without needing a tap
     * physically open. Delete the file to return to real readings. */
    {
        FILE * df = fopen("/tmp/demo_water", "r");
        if (df) {
            float v;
            if (fscanf(df, "%f", &v) == 1) shown_lpm = v;
            fclose(df);
        }
    }

    homewizard_lock();
    hw_state.water_total_m3  = new_total_m3;
    hw_state.connected_water = 1;
    hw_state.water_lpm = shown_lpm;

    /* Session bookkeeping. The "flowing now?" test is shown_lpm > 0.05
     * rather than device_lpm, so a single litre poured between two polls
     * starts a session and the per-pour litre count starts ticking
     * immediately instead of after the device's lpm field catches up. */
    if (shown_lpm > 0.05f) {
        if (!hw_state.water_session_active) {
            session_start_m3 = hw_state.water_total_m3;
            hw_state.water_session_active = 1;
            hw_state.water_session_age_s = 0;
        }
        hw_state.water_session_l =
            (hw_state.water_total_m3 - session_start_m3) * 1000.0f;
        if (hw_state.water_session_l < 0) hw_state.water_session_l = 0;
        session_zero_seconds = 0;
    } else if (hw_state.water_session_active) {
        /* Flow stopped — wait the grace window before closing the session. */
        session_zero_seconds += interval_s;
        if (session_zero_seconds >= WATER_SESSION_GRACE_S) {
            hw_state.water_session_active = 0;
            hw_state.water_session_age_s = 0;
        }
    } else if (hw_state.water_session_l > 0) {
        /* Session closed; fade out the "+X L" display. */
        hw_state.water_session_age_s += interval_s;
        if (hw_state.water_session_age_s >=
            WATER_SESSION_GRACE_S + WATER_SESSION_FADE_S) {
            hw_state.water_session_l = 0;
            hw_state.water_session_age_s = 0;
        }
    }
    homewizard_unlock();
}

/* Bin start helpers — round a timestamp down to the start of the current
   5-min / hour / day window. hcb_rrd expects the sample timestamp to land
   on a bin boundary. */
static long bin_5min(long ts) { return ts - (ts % 300); }
static long bin_hour(long ts) { return ts - (ts % 3600); }
static long bin_day(long ts)  { return ts - (ts % 86400); }

static void push_to_rrd(void) {
    long now = (long)time(NULL);
    static long last_5min = 0, last_hour = 0, last_day = 0;

    long b5 = bin_5min(now);
    long bh = bin_hour(now);
    long bd = bin_day(now);

    if (b5 != last_5min) {
        last_5min = b5;
        /* Flow archives — 5-minute live values from HomeWizard. */
        if (hw_state.connected_p1)
            rrd_push("elec_flow",  "5min", b5, hw_state.power_w);
        if (hw_state.connected_water)
            rrd_push("water_flow", "5min", b5, hw_state.water_lpm);
    }
    if (bh != last_hour) {
        last_hour = bh;
        /* Cumulative meters in their hourly archive. Toon stores the
           cumulative reading as integer in milli-units (litres for water,
           Wh for electricity, m³x1000 for gas). */
        if (hw_state.connected_p1) {
            rrd_push("elec_quantity_nt", "5yrhours", bh,
                     hw_state.kwh_import_t1 * 1000.0);
            rrd_push("elec_quantity_lt", "5yrhours", bh,
                     hw_state.kwh_import_t2 * 1000.0);
            rrd_push("gas_quantity",     "5yrhours", bh,
                     hw_state.gas_m3 * 1000.0);
        }
        if (hw_state.connected_water)
            rrd_push("water_quantity", "10yrhours", bh,
                     hw_state.water_total_m3 * 1000.0);
    }
    if (bd != last_day) {
        last_day = bd;
        /* Daily cumulative — same value rolled into the daily archive
           for the long-term graphs. */
        if (hw_state.connected_p1) {
            rrd_push("elec_quantity_nt", "10yrdays", bd,
                     hw_state.kwh_import_t1 * 1000.0);
            rrd_push("elec_quantity_lt", "10yrdays", bd,
                     hw_state.kwh_import_t2 * 1000.0);
            rrd_push("gas_quantity",     "10yrdays", bd,
                     hw_state.gas_m3 * 1000.0);
        }
    }
}

static void * hw_thread(void * arg) {
    (void)arg;
    /* 2 s loop for WATER: pours produce a 1-second update on the HWE-WTR side;
     * at 5 s the live L/min lagged so much that the user couldn't see they had
     * even turned the tap on.
     *
     * But the P1 meter (HWE-P1) is an ESP32 with a tiny TCP socket table. Each
     * poll is a fresh HTTP/1.0 "Connection: close" request, so the P1 actively
     * closes and holds the socket in TIME_WAIT for ~1-2 min. At one new
     * connection every 2 s (plus HA + the HomeWizard app polling the same
     * meter) the TIME_WAIT sockets exhaust its lwIP PCBs and the meter hangs /
     * reboots — i.e. the "P1 keeps crashing" since this UI started polling it.
     * So poll the P1 only every 5th tick (~10 s); water stays at 2 s. Push-to-
     * RRD is rate-limited by its own 5-min bucket, so the extra ticks are free. */
    unsigned tick = 0;
    time_t last_nilm_s = 0;
    while (1) {
        if (tick % 5 == 0) poll_p1();
        /* kWh meter and sockets are ESPs too: ~10 s each, on different ticks. */
        if (tick % 5 == 2) poll_kwh();
        for (int i = 0; i < HW_SOCKET_MAX; i++)
            if (tick % 5 == (unsigned)(i + 1) % 5) poll_socket(i);
        if (settings.toonwater_host[0]) {
            if (tick % (TOONWATER_POLL_INTERVAL_S / 2) == 0) poll_toonwater();
        } else {
            poll_water();
        }
        push_to_rrd();
        tick++;
        /* Feed NILM every 5 s from the cached P1 reading (P1 HTTP poll stays at
         * 10 s to avoid exhausting its lwIP TIME_WAIT PCBs). */
        if (settings.energy_source == 1 && hw_state.connected_p1) {
            time_t now = time(NULL);
            if (now - last_nilm_s >= 5) {
                meteradapter_on_flow(hw_state.power_w);
                last_nilm_s = now;
            }
        }
        sleep(2);
    }
    return NULL;
}

/* A Toon that ran the ToonWater QML app has its sensor in
 * /mnt/data/tsc/water_userSettings.json ({"urlString":"192.168.1.50",
 * "domMode":"ESP",...}). Adopt it once when FreeToon has no water source yet,
 * so the water meter keeps working after switching to FreeToon. The Domoticz
 * mode of ToonWater is not adopted (FreeToon reads the sensor directly). */
static void toonwater_adopt_config(void) {
    if (settings.toonwater_host[0] || settings.p1_water_host[0]) return;
    FILE * f = fopen("/mnt/data/tsc/water_userSettings.json", "r");
    if (!f) return;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (strstr(buf, "\"Domoticz\"")) return;
    const char * p = strstr(buf, "\"urlString\"");
    if (!p || !(p = strchr(p + 11, '"'))) return;
    p++;
    char host[64]; size_t o = 0;
    for (; *p && *p != '"' && o + 1 < sizeof host; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':')) return;   /* not a host */
        host[o++] = c;
    }
    host[o] = 0;
    if (!host[0]) return;
    snprintf(settings.toonwater_host, sizeof settings.toonwater_host, "%s", host);
    settings.enable_p1_water = 1;
    settings_save();
    fprintf(stderr, "[hw] adopted the ToonWater sensor %s from the ToonWater app\n", host);
}

static int hw_anything_configured(void) {
    return settings.enable_p1_elec || settings.enable_p1_water ||
           settings.hw_kwh_host[0] || homewizard_socket_count() > 0;
}

static pthread_mutex_t s_start_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_started = 0;

/* Skip the polling thread while nothing is configured — keeps "basic"
 * installs from spawning a thread that does nothing every 2 s and from
 * contaminating connected_* flags on the home tile. Started later from
 * Settings the moment something is switched on (no restart needed). */
static int start_locked(void) {
    if (s_started || !hw_anything_configured()) return 0;
    pthread_t th;
    if (pthread_create(&th, NULL, hw_thread, NULL) != 0) return -1;
    pthread_detach(th);
    s_started = 1;
    fprintf(stderr, "[hw] poller started (elec=%d water=%d kwh=%d sockets=%d)\n",
            settings.enable_p1_elec, settings.enable_p1_water,
            settings.hw_kwh_host[0] != 0, homewizard_socket_count());
    return 0;
}

void homewizard_ensure_started(void) {
    if (settings.client_mode) return;   /* a client reads the master, not devices */
    pthread_mutex_lock(&s_start_mtx);
    start_locked();
    pthread_mutex_unlock(&s_start_mtx);
}

int homewizard_start(void) {
    toonwater_adopt_config();
    pthread_mutex_lock(&s_start_mtx);
    if (!hw_anything_configured())
        fprintf(stderr, "[hw] no HomeWizard device configured — poller not started yet\n");
    int rc = start_locked();
    pthread_mutex_unlock(&s_start_mtx);
    return rc;
}
