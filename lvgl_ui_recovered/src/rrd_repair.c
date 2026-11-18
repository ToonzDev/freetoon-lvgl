/* Meter history repair after a meter-adapter -> HomeWizard P1 switch.
 * See rrd_repair.h for what is repaired and why. */
#include "rrd_repair.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================= pure core ======================= */

/* own rounding: round() is a new libm import the Toon ABI baseline lacks */
static double rnd(double x) {
    return x < 0 ? -(double)(long long)(-x + 0.5) : (double)(long long)(x + 0.5);
}

static int cmp_dbl(const void * a, const void * b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Per-day increase of daily point k over the previous valid point; NAN for
 * the first valid point. prev[k] = index of that previous point or -1. */
static void daily_deltas(const double * v, const rrd_pt_t * d, int nd, double * per_day, int * prev) {
    int p = -1;
    for (int k = 0; k < nd; k++) {
        per_day[k] = NAN; prev[k] = -1;
        if (isnan(v[k])) continue;
        if (p >= 0) {
            double days = (double)(d[k].ts - d[p].ts) / 86400.0;
            if (days < 1.0) days = 1.0;
            per_day[k] = (v[k] - v[p]) / days;
            prev[k] = p;
        }
        p = k;
    }
}

/* Median of up to 7 ordinary per-day increases on each side of k. */
static double neighbour_median(const double * per_day, int nd, int k, double min_step) {
    double buf[14]; int n = 0;
    for (int i = k - 1, got = 0; i >= 0 && got < 7; i--)
        if (!isnan(per_day[i]) && per_day[i] >= 0 && per_day[i] < min_step) { buf[n++] = per_day[i]; got++; }
    for (int i = k + 1, got = 0; i < nd && got < 7; i++)
        if (!isnan(per_day[i]) && per_day[i] >= 0 && per_day[i] < min_step) { buf[n++] = per_day[i]; got++; }
    if (n == 0) return 0.0;
    qsort(buf, (size_t)n, sizeof buf[0], cmp_dbl);
    return (n & 1) ? buf[n / 2] : (buf[n / 2 - 1] + buf[n / 2]) / 2.0;
}

void rrd_repair_compute(const rrd_pt_t * h, int nh, const rrd_pt_t * d, int nd,
                        long now, long window_s, const rrd_lim_t * lim,
                        double * new_h, double * new_d, rrd_fix_t * res) {
    memset(res, 0, sizeof *res);
    for (int i = 0; i < nh; i++) new_h[i] = h[i].v;
    for (int k = 0; k < nd; k++) new_d[k] = d[k].v;
    if (nd < 2) return;

    double * per_day = malloc(sizeof(double) * (size_t)nd);
    int    * prev    = malloc(sizeof(int) * (size_t)nd);
    if (!per_day || !prev) { free(per_day); free(prev); return; }
    long from = now - window_s;

    /* ---- 1. counter step in the daily archive ---- */
    daily_deltas(new_d, d, nd, per_day, prev);
    int cand = -1, ncand = 0;
    double cand_med = 0;
    for (int k = 0; k < nd; k++) {
        if (isnan(per_day[k]) || d[k].ts < from || d[k].ts > now) continue;
        double days = (double)(d[k].ts - d[prev[k]].ts) / 86400.0;
        if (days < 1.0) days = 1.0;
        double delta = per_day[k] * days;
        double med = neighbour_median(per_day, nd, k, lim->min_step);
        int is_step = delta < -lim->tol ||                       /* a counter can't go back */
                      (delta > lim->min_step * days && delta > 10.0 * med * days);
        if (is_step) { ncand++; cand = k; cand_med = med; }
    }
    if (ncand > 1) res->multi_step = ncand;
    if (ncand == 1) {
        int p = prev[cand];
        double days = (double)(d[cand].ts - d[p].ts) / 86400.0;
        if (days < 1.0) days = 1.0;
        double offset = rnd((new_d[cand] - new_d[p]) - cand_med * days);
        long cut = d[p].ts;
        /* refuse a shift that would push old readings below zero */
        int ok = 1;
        for (int k = 0; k <= p && ok; k++) if (!isnan(new_d[k]) && new_d[k] + offset < 0) ok = 0;
        if (ok && offset != 0) {
            res->step = 1; res->step_ts = d[cand].ts; res->offset = offset;
            for (int k = 0; k < nd; k++)
                if (d[k].ts <= cut && !isnan(new_d[k])) { new_d[k] += offset; res->n_shift_d++; }
            for (int i = 0; i < nh; i++)
                if (h[i].ts <= cut && !isnan(new_h[i])) { new_h[i] += offset; res->n_shift_h++; }
        }
    }

    /* ---- 2. hourly readings that don't fit their day ---- */
    int i = 0, p = -1;
    for (int k = 0; k < nd; k++) {
        if (isnan(new_d[k])) continue;
        if (p < 0) { p = k; continue; }
        long t0 = d[p].ts, t1 = d[k].ts;
        double v0 = new_d[p], v1 = new_d[k];
        p = k;
        if (t1 > now || t1 < from || v1 < v0 - lim->tol) continue;
        while (i < nh && h[i].ts <= t0) i++;
        int first = i, bad = 0;
        double last = v0;
        for (int j = first; j < nh && h[j].ts <= t1; j++) {
            double x = new_h[j];
            if (isnan(x)) continue;
            if (x < v0 - lim->tol || x > v1 + lim->tol || x < last - lim->tol) bad = 1;
            last = x;
        }
        if (!bad) continue;
        for (int j = first; j < nh && h[j].ts <= t1; j++) {
            if (isnan(new_h[j])) continue;   /* only overwrite, never fill gaps */
            double x = rnd(v0 + (v1 - v0) * (double)(h[j].ts - t0) / (double)(t1 - t0));
            if (fabs(x - new_h[j]) >= 0.5) { new_h[j] = x; res->n_rebuild_h++; }
        }
    }
    /* Hours of the day still running have no end reading to spread towards.
     * A frozen one (below the last daily reading) gets that reading: no usage
     * known, but no false step either when the next P1 value arrives. */
    int last = -1;
    for (int k = 0; k < nd; k++) if (!isnan(new_d[k]) && d[k].ts <= now) last = k;
    if (last >= 0 && d[last].ts >= from) {
        double v0 = new_d[last];
        for (int j = 0; j < nh; j++) {
            if (h[j].ts <= d[last].ts || h[j].ts > now || isnan(new_h[j])) continue;
            if (new_h[j] < v0 - lim->tol) { new_h[j] = v0; res->n_rebuild_h++; }
        }
    }
    free(per_day); free(prev);
}

#ifndef RRD_REPAIR_CORE_ONLY
/* ======================= on the Toon ======================= */
#include "i18n.h"
#include "rrd_push.h"
#include "settings.h"
#include <pthread.h>
#include <time.h>

#define RRD_VAR_DIR    "/mnt/data/qmf/var/hcb_rrd"
#define RRD_BACKUP_DIR "/mnt/data/freetoon_rrd_backup"
#define WINDOW_S       (120L * 86400L)

typedef struct {
    const char * logger;
    const char * nl, * en;      /* name in the summary */
    double unit;                 /* raw units per shown unit */
    const char * unit_txt;
    rrd_lim_t lim;
} repair_logger_t;

static const repair_logger_t g_loggers[] = {
    { "gas_quantity",     "Gas",                "Gas",                 1000.0, "m3",  { 30000.0, 500.0 } },
    { "elec_quantity_nt", "Stroom normaal",     "Electricity normal",  1000.0, "kWh", { 50000.0, 1000.0 } },
    { "elec_quantity_lt", "Stroom dal",         "Electricity low",     1000.0, "kWh", { 50000.0, 1000.0 } },
};
#define N_LOGGERS (int)(sizeof g_loggers / sizeof g_loggers[0])

typedef struct {
    rrd_pt_t * h, * d; int nh, nd;
    double * new_h, * new_d;
    rrd_fix_t fix;
    int ok;
} repair_data_t;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int  g_state = RRD_REPAIR_IDLE;
static char g_text[1024];
static int  g_have_fixes = 0;
static repair_data_t g_data[N_LOGGERS];

static void set_state(int st, const char * text) {
    pthread_mutex_lock(&g_mtx);
    g_state = st;
    if (text) snprintf(g_text, sizeof g_text, "%s", text);
    pthread_mutex_unlock(&g_mtx);
}

int rrd_repair_status(char * text, int textsz, int * have_fixes) {
    pthread_mutex_lock(&g_mtx);
    int st = g_state;
    if (text && textsz > 0) snprintf(text, (size_t)textsz, "%s", g_text);
    if (have_fixes) *have_fixes = g_have_fixes;
    pthread_mutex_unlock(&g_mtx);
    return st;
}

static void free_data(void) {
    for (int l = 0; l < N_LOGGERS; l++) {
        free(g_data[l].h); free(g_data[l].d); free(g_data[l].new_h); free(g_data[l].new_d);
        memset(&g_data[l], 0, sizeof g_data[l]);
    }
}

/* getRrdData with unix-time keys -> ascending points. */
static int fetch(const char * logger, const char * rra, long from, long to, rrd_pt_t ** out, int * n) {
    *out = NULL; *n = 0;
    char cmd[400];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -s --max-time 60 'http://127.0.0.1:10080/hcb_rrd?action=getRrdData"
        "&loggerName=%s&rra=%s&from=%ld&to=%ld&nullForNaN=1' 2>/dev/null", logger, rra, from, to);
    FILE * p = popen(cmd, "r");
    if (!p) return -1;
    size_t cap = 256 * 1024, len = 0;
    char * b = malloc(cap);
    while (b) {
        if (len + 1 >= cap) {
            if (cap >= 8u * 1024 * 1024) break;
            char * nb = realloc(b, cap * 2);
            if (!nb) break;
            b = nb; cap *= 2;
        }
        size_t r = fread(b + len, 1, cap - 1 - len, p);
        if (r == 0) break;
        len += r;
    }
    pclose(p);
    if (!b || len == 0) { free(b); return -1; }
    b[len] = 0;
    int ncap = 4096;
    rrd_pt_t * pts = malloc(sizeof *pts * (size_t)ncap);
    const char * s = b;
    while (pts && (s = strchr(s, '"'))) {
        char * e;
        long ts = strtol(s + 1, &e, 10);
        if (*e != '"') { s++; continue; }
        s = strchr(e, ':');
        if (!s) break;
        s++;
        while (*s == ' ') s++;
        double v = NAN;
        if (*s != 'n' && *s != 'N') v = strtod(s, NULL);
        if (*n == ncap) {
            rrd_pt_t * np = realloc(pts, sizeof *pts * (size_t)(ncap *= 2));
            if (!np) break;
            pts = np;
        }
        pts[(*n)++] = (rrd_pt_t){ ts, v };
    }
    free(b);
    if (!pts) return -1;
    *out = pts;
    return 0;
}

/* setRrdData in batches of up to 500 samples. Returns samples written or -1. */
static int write_changed(const char * logger, const char * rra, const rrd_pt_t * pts,
                         const double * nv, int n) {
    size_t cap = 500 * 40 + 512;
    char * url = malloc(cap);
    if (!url) return -1;
    int written = 0, i = 0;
    while (i < n) {
        int len = snprintf(url, cap, "/usr/bin/curl -s --max-time 30 'http://127.0.0.1:10080/hcb_rrd"
                           "?action=setRrdData&loggerName=%s&rra=%s&samples=%%7B", logger, rra);
        int in_batch = 0;
        for (; i < n && in_batch < 500; i++) {
            if (isnan(nv[i]) || isnan(pts[i].v) || fabs(nv[i] - pts[i].v) < 0.5) continue;
            len += snprintf(url + len, cap - (size_t)len, "%s%%22%ld%%22%%3A%.0f",
                            in_batch ? "%2C" : "", pts[i].ts, nv[i]);
            in_batch++;
        }
        if (!in_batch) break;
        snprintf(url + len, cap - (size_t)len, "%%7D' 2>/dev/null");
        FILE * p = popen(url, "r");
        char resp[128] = "";
        if (p) { size_t r = fread(resp, 1, sizeof resp - 1, p); resp[r] = 0; pclose(p); }
        if (!strstr(resp, "\"ok\"")) {
            fprintf(stderr, "[rrdfix] %s/%s write failed: %s\n", logger, rra, resp);
            free(url);
            return -1;
        }
        written += in_batch;
    }
    free(url);
    return written;
}

/* Copy the logger's .dat/.rra files into the backup dir. */
static int backup_logger(const char * logger, const char * dir) {
    char uuid[64];
    if (rrd_logger_uuid(logger, uuid, sizeof uuid) != 0) return -1;
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' && cp -p " RRD_VAR_DIR "/%s* '%s'/", dir, uuid, dir);
    return system(cmd) == 0 ? 0 : -1;
}

static void * scan_thread(void * arg) {
    (void)arg;
    free_data();
    long now = (long)time(NULL);
    char txt[1024]; int o = 0, fixes = 0;
    o += snprintf(txt + o, sizeof txt - (size_t)o, "%s",
                  tr("Laatste 120 dagen gecontroleerd:\n", "Checked the last 120 days:\n"));
    for (int l = 0; l < N_LOGGERS; l++) {
        const repair_logger_t * L = &g_loggers[l];
        repair_data_t * D = &g_data[l];
        if (!rrd_logger_exists(L->logger) ||
            fetch(L->logger, "5yrhours", now - 5L * 366 * 86400, now + 3600, &D->h, &D->nh) != 0 ||
            fetch(L->logger, "10yrdays", now - 10L * 366 * 86400, now + 86400, &D->d, &D->nd) != 0) {
            o += snprintf(txt + o, sizeof txt - (size_t)o, "- %s: %s\n", tr(L->nl, L->en),
                          tr("niet te lezen", "could not be read"));
            continue;
        }
        D->new_h = malloc(sizeof(double) * (size_t)(D->nh ? D->nh : 1));
        D->new_d = malloc(sizeof(double) * (size_t)(D->nd ? D->nd : 1));
        if (!D->new_h || !D->new_d) continue;
        rrd_repair_compute(D->h, D->nh, D->d, D->nd, now, WINDOW_S, &L->lim, D->new_h, D->new_d, &D->fix);
        D->ok = 1;
        rrd_fix_t * F = &D->fix;
        fprintf(stderr, "[rrdfix] %s: step=%d multi=%d offset=%.0f shift h=%d d=%d rebuild h=%d\n",
                L->logger, F->step, F->multi_step, F->offset, F->n_shift_h, F->n_shift_d, F->n_rebuild_h);
        o += snprintf(txt + o, sizeof txt - (size_t)o, "- %s: ", tr(L->nl, L->en));
        if (!F->step && !F->n_rebuild_h) {
            o += snprintf(txt + o, sizeof txt - (size_t)o, "%s\n",
                          F->multi_step ? tr("meerdere sprongen, niet automatisch te herstellen",
                                             "several jumps, can't be repaired automatically")
                                        : tr("in orde", "OK"));
            continue;
        }
        fixes = 1;
        if (F->step) {
            time_t st = (time_t)(F->step_ts - 86400);
            struct tm tm; localtime_r(&st, &tm);
            o += snprintf(txt + o, sizeof txt - (size_t)o,
                          tr("sprong van %.0f %s op %02d-%02d weghalen", "remove jump of %.0f %s on %02d-%02d"),
                          F->offset / L->unit, L->unit_txt, tm.tm_mday, tm.tm_mon + 1);
        }
        if (F->n_rebuild_h)
            o += snprintf(txt + o, sizeof txt - (size_t)o, tr("%s%d uurwaarden herstellen", "%s%d hourly values to rebuild"),
                          F->step ? ", " : "", F->n_rebuild_h);
        o += snprintf(txt + o, sizeof txt - (size_t)o, "\n");
        if (F->multi_step)
            o += snprintf(txt + o, sizeof txt - (size_t)o, "%s",
                          tr("  (meerdere sprongen: alleen uurwaarden)\n", "  (several jumps: hourly values only)\n"));
    }
    if (fixes)
        snprintf(txt + o, sizeof txt - (size_t)o, "%s",
                 tr("\nEr wordt eerst een reservekopie gemaakt.", "\nA backup is made first."));
    pthread_mutex_lock(&g_mtx);
    g_have_fixes = fixes;
    pthread_mutex_unlock(&g_mtx);
    set_state(RRD_REPAIR_SCANNED, txt);
    return NULL;
}

static void * apply_thread(void * arg) {
    (void)arg;
    /* the adapter must not overwrite the repair again: P1 source -> loggers off */
    rrd_meter_loggers_sync();
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    char dir[128];
    snprintf(dir, sizeof dir, RRD_BACKUP_DIR "/%04d%02d%02d-%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    for (int l = 0; l < N_LOGGERS; l++) {
        repair_data_t * D = &g_data[l];
        if (!D->ok || (!D->fix.step && !D->fix.n_rebuild_h)) continue;
        if (backup_logger(g_loggers[l].logger, dir) != 0) {
            set_state(RRD_REPAIR_FAILED, tr("Reservekopie mislukt - niets gewijzigd.",
                                            "Backup failed - nothing changed."));
            return NULL;
        }
    }
    char txt[512]; int o = 0, failed = 0;
    for (int l = 0; l < N_LOGGERS; l++) {
        repair_data_t * D = &g_data[l];
        const repair_logger_t * L = &g_loggers[l];
        if (!D->ok || (!D->fix.step && !D->fix.n_rebuild_h)) continue;
        int wd = write_changed(L->logger, "10yrdays", D->d, D->new_d, D->nd);
        int wh = wd < 0 ? -1 : write_changed(L->logger, "5yrhours", D->h, D->new_h, D->nh);
        /* read back what was written */
        int bad = 0;
        if (wh >= 0) {
            rrd_pt_t * chk = NULL; int nc = 0;
            if (fetch(L->logger, "5yrhours", D->h[0].ts, D->h[D->nh - 1].ts, &chk, &nc) == 0) {
                for (int i = 0, j = 0; i < D->nh; i++) {
                    if (isnan(D->new_h[i]) || fabs(D->new_h[i] - D->h[i].v) < 0.5) continue;
                    while (j < nc && chk[j].ts < D->h[i].ts) j++;
                    if (j >= nc || chk[j].ts != D->h[i].ts || fabs(chk[j].v - D->new_h[i]) >= 1.0) bad++;
                }
                free(chk);
            } else bad = -1;
        }
        fprintf(stderr, "[rrdfix] %s: wrote daily %d hourly %d, read-back mismatches %d\n",
                L->logger, wd, wh, bad);
        if (wd < 0 || wh < 0 || bad) failed = 1;
        o += snprintf(txt + o, sizeof txt - (size_t)o, "- %s: %s\n", tr(L->nl, L->en),
                      (wd < 0 || wh < 0 || bad) ? tr("MISLUKT", "FAILED") : tr("hersteld", "repaired"));
    }
    snprintf(txt + o, sizeof txt - (size_t)o, tr("\nReservekopie: %s", "\nBackup: %s"), dir);
    free_data();
    pthread_mutex_lock(&g_mtx);
    g_have_fixes = 0;
    pthread_mutex_unlock(&g_mtx);
    set_state(failed ? RRD_REPAIR_FAILED : RRD_REPAIR_DONE, txt);
    return NULL;
}

static int start(void * (*fn)(void *), int need_state, int new_state, const char * text) {
    pthread_mutex_lock(&g_mtx);
    int busy = g_state == RRD_REPAIR_SCANNING || g_state == RRD_REPAIR_APPLYING;
    int ok = !busy && (need_state < 0 || (g_state == need_state && g_have_fixes));
    if (ok) { g_state = new_state; snprintf(g_text, sizeof g_text, "%s", text); }
    pthread_mutex_unlock(&g_mtx);
    if (!ok) return -1;
    pthread_t t;
    if (pthread_create(&t, NULL, fn, NULL) != 0) {
        set_state(RRD_REPAIR_FAILED, tr("Kon niet starten.", "Could not start."));
        return -1;
    }
    pthread_detach(t);
    return 0;
}

int rrd_repair_scan_start(void) {
    if (settings.client_mode) return -1;
    return start(scan_thread, -1, RRD_REPAIR_SCANNING,
                 tr("Geschiedenis controleren...", "Checking history..."));
}

int rrd_repair_apply_start(void) {
    return start(apply_thread, RRD_REPAIR_SCANNED, RRD_REPAIR_APPLYING,
                 tr("Bezig met herstellen...", "Repairing..."));
}
#endif
