/* Schakelschema: see switch_schedule.h. */
#include "switch_schedule.h"
#include "tile_lights.h"
#include "settings.h"
#include "lvgl.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICK_MS   15000
#define CATCHUP_S (3 * 3600)
#define QML_PATH  "/mnt/data/tsc/plugSchedule_userSettings.json"

static ss_set_t        g_set;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static char            g_path[256];

/* Second send of each command on the next tick (plugs miss one now and then). */
static ss_action_t g_retry[SS_MAX];
static int         g_nretry;

static const char * path(void) {
    if (!g_path[0]) {
        const char * d = getenv("TOONUI_DATA_DIR");
        snprintf(g_path, sizeof g_path, "%s/freetoon_schedule.cfg", d && d[0] ? d : "/mnt/data");
    }
    return g_path;
}

void sched_location(double * lat, double * lon) {
    double a = atof(settings.weather_lat), o = atof(settings.weather_lon);
    if (a < -66 || a > 66 || o < -180 || o > 180 || (a == 0 && o == 0)) { a = 52.1; o = 5.1; }
    *lat = a; *lon = o;
}

static void save_locked(void) {
    if (ss_save(&g_set, path()) != 0) fprintf(stderr, "[schema] could not save %s\n", path());
}

static void tick(lv_timer_t * t) {
    (void)t;
    if (settings.client_mode) return;          /* the master runs the schedule */
    for (int i = 0; i < g_nretry; i++) tl_set_on_ref(g_retry[i].ref, g_retry[i].on);
    g_nretry = 0;

    double lat, lon;
    sched_location(&lat, &lon);
    ss_action_t act[SS_MAX];
    int changed = 0, n;
    pthread_mutex_lock(&g_mtx);
    n = ss_due(&g_set, (long)time(NULL), lat, lon, CATCHUP_S, act, SS_MAX, &changed);
    if (changed) save_locked();
    pthread_mutex_unlock(&g_mtx);
    for (int i = 0; i < n; i++) {
        char name[80], when[48];
        sched_device_name(act[i].ref, name, sizeof name);
        time_t tt = (time_t)act[i].t;
        struct tm lt;
        localtime_r(&tt, &lt);
        strftime(when, sizeof when, "%H:%M", &lt);
        int ok = tl_set_on_ref(act[i].ref, act[i].on);
        fprintf(stderr, "[schema] %s %s (%s)%s\n", name, act[i].on ? "aan" : "uit", when,
                ok ? "" : " - apparaat niet gevonden");
        if (g_nretry < SS_MAX) g_retry[g_nretry++] = act[i];
    }
}

void sched_init(void) {
    static int done;
    if (done) return;
    done = 1;
    pthread_mutex_lock(&g_mtx);
    if (ss_load(&g_set, path()) != 0) {
        /* First start: take over the QML app's rules once. */
        FILE * f = fopen(QML_PATH, "r");
        if (f) {
            static char buf[64 * 1024];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            int k = ss_import_qml(&g_set, buf, (long)time(NULL));
            fprintf(stderr, "[schema] %d rule(s) taken over from the Slimme stekkers app\n", k);
            save_locked();
        }
    }
    pthread_mutex_unlock(&g_mtx);
    lv_timer_create(tick, TICK_MS, NULL);
}

int sched_count(void) {
    pthread_mutex_lock(&g_mtx);
    int n = g_set.n;
    pthread_mutex_unlock(&g_mtx);
    return n;
}

int sched_get(int i, ss_rule_t * out) {
    pthread_mutex_lock(&g_mtx);
    int ok = i >= 0 && i < g_set.n;
    if (ok) *out = g_set.r[i];
    pthread_mutex_unlock(&g_mtx);
    return ok;
}

int sched_put(int i, const ss_rule_t * r) {
    ss_rule_t c = *r;
    if (!c.ref[0] || c.mode < SS_CLOCK || c.mode > SS_SUNSET || c.hhmm < 0 || c.hhmm > 2359 ||
        c.hhmm % 100 > 59 || c.offset < -180 || c.offset > 180) return -1;
    for (char * p = c.ref; *p; p++) if (*p == '\n' || *p == '\r' || *p == ' ') return -1;
    c.days &= SS_ALL_DAYS; c.on = c.on ? 1 : 0; c.active = c.active ? 1 : 0;
    c.last = (long)time(NULL);                /* only moments from now on */
    pthread_mutex_lock(&g_mtx);
    int rc = 0;
    if (i < 0) {
        if (g_set.n < SS_MAX) g_set.r[g_set.n++] = c; else rc = -1;
    } else if (i < g_set.n) g_set.r[i] = c;
    else rc = -1;
    if (rc == 0) save_locked();
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int sched_delete(int i) {
    pthread_mutex_lock(&g_mtx);
    int rc = -1;
    if (i >= 0 && i < g_set.n) {
        memmove(&g_set.r[i], &g_set.r[i + 1], (size_t)(g_set.n - i - 1) * sizeof g_set.r[0]);
        g_set.n--;
        save_locked();
        rc = 0;
    }
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

void sched_device_name(const char * ref, char * out, size_t n) {
    tl_src_t s = tl_parse(ref);
    if (s.kind == TL_NONE) { snprintf(out, n, "?"); return; }
    tl_status(&s, out, n, NULL);
    if (!out[0]) snprintf(out, n, "%s", ref);
}

void sched_rule_text(const ss_rule_t * r, char * out, size_t n) {
    char t[48], d[48];
    ss_time_text(r, t, sizeof t);
    ss_days_text(r->days, d, sizeof d);
    snprintf(out, n, "%s %s%s, %s", r->on ? "Aan" : "Uit", r->mode == SS_CLOCK ? "om " : "bij ", t, d);
}

long sched_next_text(char * out, size_t n) {
    double lat, lon;
    sched_location(&lat, &lon);
    long now = (long)time(NULL), best = -1;
    ss_rule_t br;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_set.n; i++) {
        if (!g_set.r[i].active) continue;
        long t = ss_next_occurrence(&g_set.r[i], now, lat, lon);
        if (t > 0 && (best < 0 || t < best)) { best = t; br = g_set.r[i]; }
    }
    pthread_mutex_unlock(&g_mtx);
    if (best < 0) { if (n) out[0] = 0; return -1; }
    char name[80], when[32];
    sched_device_name(br.ref, name, sizeof name);
    time_t tt = (time_t)best, nn = (time_t)now;
    struct tm lt, ln;
    localtime_r(&tt, &lt);
    localtime_r(&nn, &ln);
    const char * day = "";
    static const char * const wd[7] = { "zo", "ma", "di", "wo", "do", "vr", "za" };
    char dbuf[8];
    if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year) day = "vandaag";
    else {
        struct tm tm1 = ln;
        tm1.tm_mday += 1; tm1.tm_isdst = -1;
        mktime(&tm1);
        if (lt.tm_yday == tm1.tm_yday && lt.tm_year == tm1.tm_year) day = "morgen";
        else { snprintf(dbuf, sizeof dbuf, "%s", wd[lt.tm_wday]); day = dbuf; }
    }
    strftime(when, sizeof when, "%H:%M", &lt);
    snprintf(out, n, "%s %s, %s %s", name, br.on ? "aan" : "uit", day, when);
    return best;
}
