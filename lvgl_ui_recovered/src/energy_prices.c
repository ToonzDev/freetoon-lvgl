/*
 * energy_prices.c — dynamic energy contracts (#44). See energy_prices.h.
 *
 * Threading: one worker thread does all network / hcb_rrd I/O. The two
 * caches (hourly prices H, per-day prices D) are shared under g_mtx; the
 * UI thread only does short lookups. New results set g_dirty, which
 * ep_tick() (UI thread) turns into listener calls.
 */
#include "energy_prices.h"
#include "settings.h"
#include "http.h"
#include "stats.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/time.h>

#define VAT        1.21
#define H_MAX      4400        /* hourly prices kept (~6 months) */
#define CHUNK_DAYS 60          /* days per API / hcb_rrd request */
#define MAX_DAYS   (6 * 366)   /* oldest day a request may reach back */
#define NODATA     (-999.0f)

/* ------------------------------------------------------------------------ */
/* Caches                                                                   */
/* ------------------------------------------------------------------------ */
typedef struct { int32_t hr; float p[2]; } ep_hr_t;          /* hr = epoch/3600 */
typedef struct { int32_t day; float v; uint8_t kind; uint8_t vol; int32_t ts; } ep_day_t;

static ep_hr_t *  H = NULL;  static int Hn = 0, Hcap = 0;
static ep_day_t * D = NULL;  static int Dn = 0, Dcap = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv  = PTHREAD_COND_INITIALIZER;
static volatile int g_dirty = 0;
static int g_unsaved = 0;          /* worker: something worth writing to flash */
static int g_started = 0;

/* jobs */
enum { J_PRICES = 0, J_WEIGHT };
typedef struct { int type, kind; char l1[48], l2[48]; int32_t d0, d1; } ep_job_t;
#define QMAX 12
static ep_job_t Q[QMAX];
static int Qn = 0;
static ep_job_t g_running;
static int g_running_valid = 0;
static long g_fail_ts = 0;

static void (*g_listener[4])(void);

static long now_s(void) { return (long)time(NULL); }
static long today_utc(void) { return now_s() / 86400; }

static const char * data_dir(void) {
    const char * d = getenv("TOONUI_DATA_DIR");
    if (d && *d) return d;
    if (access("/mnt/data", W_OK) == 0) return "/mnt/data";
    return ".";
}

/* days <-> civil (Howard Hinnant), no timegm() in the Toon ABI baseline */
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(long z, int * y, int * m, int * d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static int cmp_hr(const void * a, const void * b) {
    const ep_hr_t * x = a, * y = b;
    return (x->hr > y->hr) - (x->hr < y->hr);
}
static int cmp_day(const void * a, const void * b) {
    const ep_day_t * x = a, * y = b;
    if (x->kind != y->kind) return (int)x->kind - (int)y->kind;
    return (x->day > y->day) - (x->day < y->day);
}

/* lock held */
static ep_hr_t * h_find(int32_t hr) {
    ep_hr_t key = { hr, { 0, 0 } };
    return Hn ? bsearch(&key, H, (size_t)Hn, sizeof *H, cmp_hr) : NULL;
}
static ep_day_t * d_find(int kind, int32_t day) {
    ep_day_t key = { day, 0, (uint8_t)kind, 0, 0 };
    return Dn ? bsearch(&key, D, (size_t)Dn, sizeof *D, cmp_day) : NULL;
}

/* Merge hourly prices of one commodity. lock NOT held. */
static void h_merge(int com, const int32_t * hr, const float * p, int n) {
    if (n <= 0) return;
    pthread_mutex_lock(&g_mtx);
    int sorted = Hn;
    int changed = 0;
    for (int i = 0; i < n; i++) {
        ep_hr_t key = { hr[i], { 0, 0 } };
        ep_hr_t * e = sorted ? bsearch(&key, H, (size_t)sorted, sizeof *H, cmp_hr) : NULL;
        if (e) { if (!(e->p[com] == p[i])) { e->p[com] = p[i]; changed = 1; } continue; }
        changed = 1;
        if (Hn == Hcap) {
            int nc = Hcap ? Hcap * 2 : 1024;
            ep_hr_t * nh = realloc(H, sizeof *H * (size_t)nc);
            if (!nh) break;
            H = nh; Hcap = nc;
        }
        H[Hn].hr = hr[i];
        H[Hn].p[com] = p[i];
        H[Hn].p[!com] = NAN;
        Hn++;
    }
    qsort(H, (size_t)Hn, sizeof *H, cmp_hr);
    /* the same hour can now appear twice (new for both commodities) */
    int w = 0;
    for (int i = 0; i < Hn; i++) {
        if (w > 0 && H[w - 1].hr == H[i].hr) {
            for (int c = 0; c < 2; c++) if (!isnan(H[i].p[c])) H[w - 1].p[c] = H[i].p[c];
        } else H[w++] = H[i];
    }
    Hn = w;
    if (Hn > H_MAX) {                         /* keep the newest */
        memmove(H, H + (Hn - H_MAX), sizeof *H * H_MAX);
        Hn = H_MAX;
    }
    if (changed) { g_dirty = 1; g_unsaved = 1; }
    pthread_mutex_unlock(&g_mtx);
}

/* lock NOT held */
static void d_merge(const ep_day_t * in, int n) {
    if (n <= 0) return;
    pthread_mutex_lock(&g_mtx);
    int sorted = Dn;
    for (int i = 0; i < n; i++) {
        ep_day_t * e = sorted ? bsearch(&in[i], D, (size_t)sorted, sizeof *D, cmp_day) : NULL;
        if (e) { *e = in[i]; continue; }
        if (Dn == Dcap) {
            int nc = Dcap ? Dcap * 2 : 1024;
            ep_day_t * nd = realloc(D, sizeof *D * (size_t)nc);
            if (!nd) break;
            D = nd; Dcap = nc;
        }
        D[Dn++] = in[i];
    }
    qsort(D, (size_t)Dn, sizeof *D, cmp_day);
    int w = 0;
    for (int i = 0; i < Dn; i++) {
        if (w > 0 && D[w - 1].kind == D[i].kind && D[w - 1].day == D[i].day) D[w - 1] = D[i];
        else D[w++] = D[i];
    }
    Dn = w;
    g_dirty = 1;
    for (int i = 0; i < n; i++) if (!in[i].vol) { g_unsaved = 1; break; }
    pthread_mutex_unlock(&g_mtx);
}

/* ------------------------------------------------------------------------ */
/* Persistence                                                              */
/* ------------------------------------------------------------------------ */
static void path_of(char * buf, size_t n, const char * name) {
    snprintf(buf, n, "%s/%s", data_dir(), name);
}

static void save_all(void) {
    char p[256], t[264];
    pthread_mutex_lock(&g_mtx);
    if (!g_unsaved) { pthread_mutex_unlock(&g_mtx); return; }
    g_unsaved = 0;
    path_of(p, sizeof p, "toonui_prices.dat");
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE * f = fopen(t, "w");
    if (f) {
        for (int i = 0; i < Hn; i++)
            fprintf(f, "%d %.5f %.5f\n", (int)H[i].hr, (double)H[i].p[0], (double)H[i].p[1]);
        fclose(f);
        rename(t, p);
    }
    path_of(p, sizeof p, "toonui_price_days.dat");
    snprintf(t, sizeof t, "%s.tmp", p);
    f = fopen(t, "w");
    if (f) {
        for (int i = 0; i < Dn; i++)
            if (!D[i].vol)
                fprintf(f, "%d %d %.5f\n", D[i].kind, (int)D[i].day, (double)D[i].v);
        fclose(f);
        rename(t, p);
    }
    pthread_mutex_unlock(&g_mtx);
}

static void load_all(void) {
    char p[256], line[96];
    path_of(p, sizeof p, "toonui_prices.dat");
    FILE * f = fopen(p, "r");
    if (f) {
        int cap = 512, n = 0;
        int32_t * hr = malloc(sizeof *hr * (size_t)cap);
        float * pe = malloc(sizeof *pe * (size_t)cap), * pg = malloc(sizeof *pg * (size_t)cap);
        while (hr && pe && pg && fgets(line, sizeof line, f)) {
            int h; char a[24], b[24];
            if (sscanf(line, "%d %23s %23s", &h, a, b) != 3) continue;
            if (n == cap) {
                cap *= 2;
                int32_t * nh = realloc(hr, sizeof *hr * (size_t)cap);
                float * ne = realloc(pe, sizeof *pe * (size_t)cap), * ng = realloc(pg, sizeof *pg * (size_t)cap);
                if (nh) hr = nh;
                if (ne) pe = ne;
                if (ng) pg = ng;
                if (!nh || !ne || !ng) break;
            }
            hr[n] = h; pe[n] = strtof(a, NULL); pg[n] = strtof(b, NULL);
            n++;
        }
        fclose(f);
        if (hr && pe && pg) {
            /* split by commodity, skipping NaN (unknown) */
            int32_t * h2 = malloc(sizeof *h2 * (size_t)(n ? n : 1));
            float * p2 = malloc(sizeof *p2 * (size_t)(n ? n : 1));
            for (int c = 0; c < 2 && h2 && p2; c++) {
                int k = 0;
                for (int i = 0; i < n; i++) {
                    float v = c ? pg[i] : pe[i];
                    if (!isnan(v)) { h2[k] = hr[i]; p2[k] = v; k++; }
                }
                h_merge(c, h2, p2, k);
            }
            free(h2); free(p2);
        }
        free(hr); free(pe); free(pg);
    }
    path_of(p, sizeof p, "toonui_price_days.dat");
    f = fopen(p, "r");
    if (f) {
        int cap = 1024, n = 0;
        ep_day_t * a = malloc(sizeof *a * (size_t)cap);
        while (a && fgets(line, sizeof line, f)) {
            int k, d; float v;
            if (sscanf(line, "%d %d %f", &k, &d, &v) != 3 || k < 0 || k >= EP_K_COUNT) continue;
            if (n == cap) {
                ep_day_t * na = realloc(a, sizeof *a * (size_t)(cap * 2));
                if (!na) break;
                a = na; cap *= 2;
            }
            a[n++] = (ep_day_t){ d, v, (uint8_t)k, 0, 0 };
        }
        fclose(f);
        d_merge(a, n);
        free(a);
    }
}

/* ------------------------------------------------------------------------ */
/* Fetching                                                                 */
/* ------------------------------------------------------------------------ */
/* EnergyZero prices for UTC days [d0, d1] (excl. VAT). Arrays get the hour
 * numbers and prices; returns the count, -1 on failure. */
static int fetch_prices(int com, long d0, long d1, int32_t ** ohr, float ** op) {
    *ohr = NULL; *op = NULL;
    int y0, m0, dd0, y1, m1, dd1;
    civil_from_days(d0, &y0, &m0, &dd0);
    civil_from_days(d1, &y1, &m1, &dd1);
    char url[320];
    snprintf(url, sizeof url,
             "https://api.energyzero.nl/v1/energyprices?fromDate=%04d-%02d-%02dT00:00:00.000Z"
             "&tillDate=%04d-%02d-%02dT23:59:59.999Z&interval=4&usageType=%d&inclBtw=false",
             y0, m0, dd0, y1, m1, dd1, com == EP_GAS ? 3 : 1);
    size_t cap = (size_t)(d1 - d0 + 1) * 24 * 80 + 4096;
    char * body = malloc(cap);
    if (!body) return -1;
    body[0] = 0;
    if (http_fetch_secure(url, body, cap, 40) != 0 || !strstr(body, "\"Prices\"")) {
        fprintf(stderr, "[prices] fetch %s %04d-%02d-%02d..%04d-%02d-%02d failed\n",
                com ? "gas" : "elec", y0, m0, dd0, y1, m1, dd1);
        free(body);
        return -1;
    }
    int ncap = (int)(d1 - d0 + 2) * 24, n = 0;
    int32_t * hr = malloc(sizeof *hr * (size_t)ncap);
    float * p = malloc(sizeof *p * (size_t)ncap);
    const char * s = body;
    while (hr && p && n < ncap && (s = strstr(s, "\"readingDate\""))) {
        s = strchr(s + 13, '"');
        if (!s) break;
        int Y, M, Dd, hh, mi;
        if (sscanf(s + 1, "%d-%d-%dT%d:%d", &Y, &M, &Dd, &hh, &mi) != 5) { s++; continue; }
        const char * q = strstr(s, "\"price\"");
        if (!q) break;
        q = strchr(q, ':');
        if (!q) break;
        double v = strtod(q + 1, NULL);
        long t = days_from_civil(Y, M, Dd) * 86400L + hh * 3600L;
        hr[n] = (int32_t)(t / 3600);
        p[n] = (float)v;
        n++;
        s = q;
    }
    free(body);
    if (!hr || !p) { free(hr); free(p); return -1; }
    *ohr = hr; *op = p;
    return n;
}

/* Day averages for UTC days [d0, d1] from px[]. A past day without enough
 * prices gets a NODATA marker (retried after 6 h) so it is not re-requested
 * on every redraw. Only stores what changed. */
static void day_averages(int com, long d0, long d1, const float * px) {
    long today = today_utc();
    int kind = com ? EP_K_AVG_G : EP_K_AVG_E;
    ep_day_t out[CHUNK_DAYS + 4];
    int k = 0;
    for (long d = d0; d <= d1 && d <= today && k < (int)(sizeof out / sizeof out[0]); d++) {
        double sum = 0; int cnt = 0;
        for (int h = 0; h < 24; h++) {
            float p = px[(d - d0) * 24 + h];
            if (!isnan(p)) { sum += p; cnt++; }
        }
        float v = (cnt >= 20) ? (float)(sum / cnt) : NODATA;
        int vol = (d >= today) || v == NODATA;
        pthread_mutex_lock(&g_mtx);
        ep_day_t * e = d_find(kind, (int32_t)d);
        int same = e && e->v == v && e->vol == vol && (!vol || now_s() - e->ts < 900);
        pthread_mutex_unlock(&g_mtx);
        if (!same) out[k++] = (ep_day_t){ (int32_t)d, v, (uint8_t)kind, (uint8_t)vol, (int32_t)now_s() };
    }
    d_merge(out, k);
}

/* Get hourly prices for UTC days [d0, d1] into px[(hr - d0*24)], from the
 * cache when complete, else from the API (also feeding the caches). */
static int prices_for(int com, long d0, long d1, float * px, int want_future) {
    int nh = (int)(d1 - d0 + 1) * 24, have = 0;
    for (int i = 0; i < nh; i++) px[i] = NAN;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < nh; i++) {
        ep_hr_t * e = h_find((int32_t)(d0 * 24 + i));
        if (e && !isnan(e->p[com])) { px[i] = e->p[com]; have++; }
    }
    pthread_mutex_unlock(&g_mtx);
    long today = today_utc();          /* future hours may simply not exist yet */
    long lim = (want_future || d1 < today) ? d1 : today;
    int need = (int)(lim - d0 + 1) * 24;
    if (need <= 0 || have >= need - 2) { day_averages(com, d0, d1, px); return have; }
    int32_t * hr; float * p;
    int n = fetch_prices(com, d0, d1, &hr, &p);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        long off = (long)hr[i] - d0 * 24;
        if (off >= 0 && off < nh) px[off] = p[i];
    }
    /* recent hours go into the hourly cache; all days into the day averages */
    day_averages(com, d0, d1, px);
    long recent = (now_s() / 3600) - H_MAX + 48;
    int k = 0;
    for (int i = 0; i < n; i++) if (hr[i] >= recent) { hr[k] = hr[i]; p[k] = p[i]; k++; }
    h_merge(com, hr, p, k);
    free(hr); free(p);
    return n;
}

/* ---- hourly meter readings (hcb_rrd, unix-time keys) ------------------- */
typedef struct { long ts; double v; } rpt_t;

static int fetch_usage(const char * logger, long from, long to, rpt_t ** out) {
    *out = NULL;
    char qs[320];
    snprintf(qs, sizeof qs, "hcb_rrd?action=getRrdData&loggerName=%s&rra=5yrhours&from=%ld&to=%ld&nullForNaN=1",
             logger, from, to);
    size_t cap = (size_t)((to - from) / 3600 + 4) * 40 + 4096;
    char * b = malloc(cap);
    if (!b) return -1;
    int n = 0;
    if (stats_fetch_raw(qs, b, cap) == 0) {
        char * s = strstr(b, "\r\n\r\n");
        s = s ? s + 4 : b;
        if (!strstr(s, "\"error\"")) {
            int ncap = (int)((to - from) / 3600 + 4);
            rpt_t * pts = malloc(sizeof *pts * (size_t)ncap);
            while (pts && n < ncap && (s = strchr(s, '"'))) {
                char * e;
                long ts = strtol(s + 1, &e, 10);
                if (*e != '"' || ts < 1000000000L) { s++; continue; }
                s = strchr(e, ':');
                if (!s) break;
                s++;
                while (*s == ' ') s++;
                double v = NAN;
                if (*s != 'n' && *s != 'N') v = strtod(s, NULL);
                pts[n++] = (rpt_t){ ts, v };
            }
            *out = pts;
            free(b);
            return pts ? n : -1;
        }
    }
    free(b);
#ifdef SIM
    /* No hcb_rrd in the simulator: a plausible synthetic meter (Wh / L). */
    {
        int ncap = (int)((to - from) / 3600 + 2);
        rpt_t * pts = malloc(sizeof *pts * (size_t)ncap);
        if (!pts) return -1;
        int gas = strstr(logger, "gas") != NULL;
        double cum = 1e6;
        for (long t = (from / 3600) * 3600; t <= to && n < ncap; t += 3600) {
            struct tm tm; time_t tt = (time_t)t; localtime_r(&tt, &tm);
            int h = tm.tm_hour;
            double u = gas ? ((h >= 6 && h <= 9) || (h >= 17 && h <= 22) ? 180 : 40)
                           : ((h >= 17 && h <= 21) ? 900 : (h >= 7 && h <= 16) ? 350 : 220);
            pts[n++] = (rpt_t){ t, cum };
            cum += u;
        }
        *out = pts;
        return n;
    }
#endif
    return -1;
}

/* Hourly usage deltas for the window into u[(hour - base_hr)], summed over
 * the loggers. Returns hours with a value. */
static int usage_for(const char * l1, const char * l2, long d0, long d1, float * u) {
    int nh = (int)(d1 - d0 + 1) * 24, got = 0;
    for (int i = 0; i < nh; i++) u[i] = NAN;
    const char * L[2] = { l1, l2 };
    for (int k = 0; k < 2; k++) {
        if (!L[k] || !L[k][0]) continue;
        rpt_t * pts;
        int n = fetch_usage(L[k], d0 * 86400L - 3600, (d1 + 1) * 86400L, &pts);
        if (n < 0) { if (k == 0) return -1; continue; }
        for (int i = 1; i < n; i++) {
            if (isnan(pts[i].v) || isnan(pts[i - 1].v)) continue;
            if (pts[i].ts - pts[i - 1].ts > 2 * 3600) continue;
            double d = pts[i].v - pts[i - 1].v;
            if (d < 0 || d > 30000) continue;          /* meter swap / tariff artefact */
            long off = pts[i - 1].ts / 3600 - d0 * 24;  /* interval start */
            if (off < 0 || off >= nh) continue;
            if (isnan(u[off])) { u[off] = 0; got++; }
            u[off] += (float)d;
        }
        free(pts);
    }
    return got;
}

/* ------------------------------------------------------------------------ */
/* Jobs                                                                     */
/* ------------------------------------------------------------------------ */
static int day_fresh(int kind, long day) {
    ep_day_t * e = d_find(kind, (int32_t)day);
    if (!e) return 0;
    if (!e->vol) return 1;
    long age = now_s() - e->ts;
    return age < (e->v == NODATA ? 6 * 3600 : 900);
}

static void run_prices(const ep_job_t * j) {
    float * px = malloc(sizeof *px * (size_t)(CHUNK_DAYS + 1) * 24);
    if (!px) return;
    for (long c0 = j->d0; c0 <= j->d1; c0 += CHUNK_DAYS) {
        long c1 = c0 + CHUNK_DAYS - 1;
        if (c1 > j->d1) c1 = j->d1;
        for (int com = 0; com < 2; com++)
            if (prices_for(com, c0, c1, px, j->kind == 1) < 0) g_fail_ts = now_s();
    }
    free(px);
}

static void run_weight(const ep_job_t * j) {
    int com = (j->kind == EP_K_GAS) ? EP_GAS : EP_ELEC;
    int avgk = com ? EP_K_AVG_G : EP_K_AVG_E;
    size_t nh = (size_t)(CHUNK_DAYS + 1) * 24;
    float * px = malloc(sizeof *px * nh), * u = malloc(sizeof *u * nh);
    if (!px || !u) { free(px); free(u); return; }
    long today = today_utc();
    for (long c0 = j->d0; c0 <= j->d1; c0 += CHUNK_DAYS) {
        long c1 = c0 + CHUNK_DAYS - 1;
        if (c1 > j->d1) c1 = j->d1;
        /* skip chunks that are already done */
        int todo = 0;
        pthread_mutex_lock(&g_mtx);
        for (long d = c0; d <= c1 && !todo; d++)
            if (!day_fresh(j->kind, d) || !day_fresh(avgk, d)) todo = 1;
        pthread_mutex_unlock(&g_mtx);
        if (!todo) continue;

        if (prices_for(com, c0, c1, px, 0) < 0) { g_fail_ts = now_s(); break; }
        int got = usage_for(j->l1, j->l2, c0, c1, u);
        if (got < 0) { g_fail_ts = now_s(); break; }

        ep_day_t out[CHUNK_DAYS + 1];
        int k = 0;
        for (long d = c0; d <= c1; d++) {
            double M = 0, U = 0, ps = 0; int pc = 0;
            for (int h = 0; h < 24; h++) {
                long off = (d - c0) * 24 + h;
                float p = px[off];
                if (isnan(p)) continue;
                ps += p; pc++;
                if (!isnan(u[off]) && u[off] > 0) { M += (double)u[off] * p; U += u[off]; }
            }
            float v = NODATA;
            if (U > 0) v = (float)(M / U);
            else if (pc >= 20) v = (float)(ps / pc);
            /* the running day, and days without any data that are recent
             * (data may still arrive), stay volatile */
            int vol = (d >= today) || (v == NODATA && d >= today - 7);
            out[k++] = (ep_day_t){ (int32_t)d, v, (uint8_t)j->kind, (uint8_t)vol, (int32_t)now_s() };
        }
        d_merge(out, k);
    }
    free(px); free(u);
}

static long g_last_periodic = 0;

static void * worker(void * arg) {
    (void)arg;
    for (;;) {
        ep_job_t j;
        int have = 0, periodic = 0;
        pthread_mutex_lock(&g_mtx);
        for (;;) {
            if (Qn > 0) {
                j = Q[0];
                memmove(Q, Q + 1, sizeof Q[0] * (size_t)(Qn - 1));
                Qn--;
                g_running = j; g_running_valid = 1;
                have = 1;
                break;
            }
            if (now_s() - g_last_periodic >= 1800) { periodic = 1; break; }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 60;
            pthread_cond_timedwait(&g_cv, &g_mtx, &ts);
        }
        pthread_mutex_unlock(&g_mtx);

        if (periodic) {
            g_last_periodic = now_s();
            /* local today and tomorrow span UTC days today-1 .. today+1;
             * tomorrow's prices appear around 13:00 */
            ep_job_t pj = { J_PRICES, 1, "", "", (int32_t)(today_utc() - 1), (int32_t)(today_utc() + 1) };
            /* force a refetch of the future part: drop "fresh enough" checks
             * by asking prices_for directly through run_prices */
            run_prices(&pj);
            save_all();
            continue;
        }
        if (have) {
            if (j.type == J_PRICES) run_prices(&j);
            else run_weight(&j);
            save_all();
            pthread_mutex_lock(&g_mtx);
            g_running_valid = 0;
            pthread_mutex_unlock(&g_mtx);
        }
    }
    return NULL;
}

static void enqueue(const ep_job_t * j) {
    pthread_mutex_lock(&g_mtx);
    int dup = 0;
    for (int i = 0; i < Qn && !dup; i++)
        if (Q[i].type == j->type && Q[i].kind == j->kind && Q[i].d0 <= j->d0 && Q[i].d1 >= j->d1) dup = 1;
    if (g_running_valid && g_running.type == j->type && g_running.kind == j->kind &&
        g_running.d0 <= j->d0 && g_running.d1 >= j->d1) dup = 1;
    if (!dup && Qn < QMAX) {
        Q[Qn++] = *j;
        pthread_cond_broadcast(&g_cv);   /* _signal is not in the Toon ABI baseline */
    }
    pthread_mutex_unlock(&g_mtx);
}

/* ------------------------------------------------------------------------ */
/* Public API (UI thread)                                                   */
/* ------------------------------------------------------------------------ */
void ep_start(void) {
    if (g_started) return;
    g_started = 1;
    load_all();
    g_unsaved = 0;
    g_dirty = 0;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&th, &at, worker, NULL) != 0)
        fprintf(stderr, "[prices] worker thread failed\n");
    pthread_attr_destroy(&at);
}

int ep_dynamic(int com) { return com == EP_GAS ? settings.dyn_gas : settings.dyn_elec; }
int ep_dynamic_any(void) { return settings.dyn_elec || settings.dyn_gas; }

double ep_allin(int com, double market) {
    return market * VAT + (com == EP_GAS ? settings.dyn_gas_surcharge : settings.dyn_elec_surcharge);
}

int ep_market_at(int com, long t, double * market) {
    ep_start();
    int ok = 0;
    pthread_mutex_lock(&g_mtx);
    ep_hr_t * e = h_find((int32_t)(t / 3600));
    if (e && !isnan(e->p[com])) { *market = e->p[com]; ok = 1; }
    pthread_mutex_unlock(&g_mtx);
    /* past and today: fetch on demand (once per 15 min per day); tomorrow
     * comes with the periodic refresh once it is published */
    long d = t / 86400;
    if (!ok && now_s() - g_fail_ts > 300 && d <= today_utc()) {
        static long tried_day[16], tried_ts[16];
        static int tried_i = 0;
        int recent = 0;
        for (int i = 0; i < 16; i++) if (tried_day[i] == d && now_s() - tried_ts[i] < 900) recent = 1;
        if (!recent) {
            tried_day[tried_i] = d; tried_ts[tried_i] = now_s(); tried_i = (tried_i + 1) % 16;
            ep_job_t j = { J_PRICES, 0, "", "", (int32_t)d, (int32_t)d };
            enqueue(&j);
        }
    }
    return ok;
}

int ep_day_market(int kind, long day, double * market) {
    ep_start();
    int r = 0;
    pthread_mutex_lock(&g_mtx);
    ep_day_t * e = d_find(kind, (int32_t)day);
    if (e && e->v != NODATA) { *market = e->v; r = (kind <= EP_K_AVG_G) ? 2 : 1; }
    else {
        int avgk = (kind == EP_K_GAS || kind == EP_K_AVG_G) ? EP_K_AVG_G : EP_K_AVG_E;
        e = d_find(avgk, (int32_t)day);
        if (e && e->v != NODATA) { *market = e->v; r = 2; }
    }
    pthread_mutex_unlock(&g_mtx);
    return r;
}

void ep_request_days(int kind, const char * l1, const char * l2, long day0, long day1) {
    ep_start();
    long today = today_utc();
    if (day1 > today) day1 = today;
    if (day0 < today - MAX_DAYS) day0 = today - MAX_DAYS;
    if (day0 > day1) return;
    if (now_s() - g_fail_ts < 300) return;          /* back off after a failure */
    int weighted = (kind == EP_K_ELEC || kind == EP_K_GAS || kind == EP_K_SOLAR) && l1 && l1[0];
    int avgk = (kind == EP_K_GAS || kind == EP_K_AVG_G) ? EP_K_AVG_G : EP_K_AVG_E;
    /* the first and last day that still need work */
    long a = -1, b = -1;
    pthread_mutex_lock(&g_mtx);
    for (long d = day0; d <= day1; d++) {
        int ok = day_fresh(avgk, d) && (!weighted || day_fresh(kind, d));
        if (!ok) { if (a < 0) a = d; b = d; }
    }
    pthread_mutex_unlock(&g_mtx);
    if (a < 0) return;
    ep_job_t j;
    memset(&j, 0, sizeof j);
    j.type = weighted ? J_WEIGHT : J_PRICES;
    j.kind = kind;
    if (l1) snprintf(j.l1, sizeof j.l1, "%s", l1);
    if (l2) snprintf(j.l2, sizeof j.l2, "%s", l2);
    j.d0 = (int32_t)a; j.d1 = (int32_t)b;
    enqueue(&j);
}

int ep_cheapest(int hours, long * start, double * avg_allin) {
    ep_start();
    long h0 = now_s() / 3600;
    double best = 1e9; long best_h = -1;
    pthread_mutex_lock(&g_mtx);
    double win[48];
    int n = 0;
    for (long h = h0; h < h0 + 40 && n < 48; h++) {
        ep_hr_t * e = h_find((int32_t)h);
        if (!e || isnan(e->p[EP_ELEC])) break;
        win[n++] = e->p[EP_ELEC];
    }
    pthread_mutex_unlock(&g_mtx);
    for (int i = 0; i + hours <= n; i++) {
        double s = 0;
        for (int k = 0; k < hours; k++) s += win[i + k];
        s /= hours;
        if (s < best) { best = s; best_h = h0 + i; }
    }
    if (best_h < 0) return 0;
    *start = best_h * 3600;
    *avg_allin = ep_allin(EP_ELEC, best);
    return 1;
}

static int cmp_d(const void * a, const void * b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int ep_today_quartiles(double * q1, double * q3) {
    ep_start();
    time_t t = (time_t)now_s();
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_isdst = -1;
    long mid = (long)mktime(&tm);
    double v[25];
    int n = 0;
    pthread_mutex_lock(&g_mtx);
    for (int h = 0; h < 25; h++) {
        ep_hr_t * e = h_find((int32_t)(mid / 3600 + h));
        if (e && !isnan(e->p[EP_ELEC])) v[n++] = e->p[EP_ELEC];
    }
    pthread_mutex_unlock(&g_mtx);
    if (n < 20) return 0;
    qsort(v, (size_t)n, sizeof v[0], cmp_d);
    *q1 = ep_allin(EP_ELEC, v[n / 4]);
    *q3 = ep_allin(EP_ELEC, v[(3 * n) / 4]);
    return 1;
}

int ep_busy(void) {
    pthread_mutex_lock(&g_mtx);
    int b = Qn > 0 || g_running_valid;
    pthread_mutex_unlock(&g_mtx);
    return b;
}

void ep_add_listener(void (*cb)(void)) {
    for (int i = 0; i < 4; i++) if (g_listener[i] == cb) return;
    for (int i = 0; i < 4; i++) if (!g_listener[i]) { g_listener[i] = cb; return; }
}
void ep_remove_listener(void (*cb)(void)) {
    for (int i = 0; i < 4; i++) if (g_listener[i] == cb) g_listener[i] = NULL;
}

void ep_tick(void) {
    if (!g_dirty) return;
    g_dirty = 0;
    for (int i = 0; i < 4; i++) if (g_listener[i]) g_listener[i]();
}
