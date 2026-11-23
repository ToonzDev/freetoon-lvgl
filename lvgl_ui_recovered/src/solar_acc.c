/* Solar production accumulator: see solar_acc.h. */
#include "solar_acc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_KW      100.0     /* more than this from one source is not a home system */
#define POWER_GAP   180       /* s: longer gaps in a power reading are not bridged */

static sacc_src_t * find(sacc_t * a, const char * id, int kind, long now, int * fresh) {
    *fresh = 0;
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->src[i].id, id)) {
            if (a->src[i].kind != kind) { a->src[i].kind = kind; *fresh = 1; }
            return &a->src[i];
        }
    sacc_src_t * s;
    if (a->n < SACC_MAX) s = &a->src[a->n++];
    else {                                   /* full: reuse the one not heard of longest */
        s = &a->src[0];
        for (int i = 1; i < a->n; i++) if (a->src[i].t < s->t) s = &a->src[i];
    }
    memset(s, 0, sizeof *s);
    snprintf(s->id, sizeof s->id, "%s", id);
    s->kind = kind;
    s->t = now;
    *fresh = 1;
    return s;
}

/* Most a source can add in dt seconds (with a minute's slack). */
static double cap_wh(long dt) {
    if (dt < 60) dt = 60;
    return MAX_KW * 1000.0 * (double)dt / 3600.0;
}

double sacc_feed(sacc_t * a, const char * id, int kind, double v, long now) {
    if (!a || !id || !id[0]) return 0;
    if (kind == SACC_LIFETIME && v <= 0) return 0;        /* 0 = not read yet */
    if (kind == SACC_DAILY && v < 0) return 0;
    if (kind == SACC_POWER && v < 0) v = 0;
    int fresh;
    sacc_src_t * s = find(a, id, kind, now, &fresh);
    long dt = now - s->t;
    double add = 0;
    if (!fresh && dt >= 0) {
        if (kind == SACC_LIFETIME) {
            double d = v - s->last;
            if (d > 0 && d <= cap_wh(dt)) add = d;
        } else if (kind == SACC_DAILY) {
            double d = v - s->last;
            if (d >= 0) { if (d <= cap_wh(dt)) add = d; }
            else if (v < s->last / 2 && v <= cap_wh(dt)) add = v;   /* a new day */
            /* a small dip is a glitch: re-base below without adding */
        } else if (kind == SACC_POWER) {
            if (dt > 0 && dt <= POWER_GAP) add = (s->last + v) / 2.0 * (double)dt / 3600.0;
        }
    }
    s->last = v;
    s->t = now;
    a->total_wh += add;
    return add;
}

void sacc_floor(sacc_t * a, double floor_wh) {
    if (a && floor_wh > a->total_wh) a->total_wh = floor_wh;
}

int sacc_save(const sacc_t * a, const char * path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "total %.3f\n", a->total_wh);
    for (int i = 0; i < a->n; i++)
        fprintf(f, "src %d %.3f %ld %s\n", a->src[i].kind, a->src[i].last, a->src[i].t, a->src[i].id);
    if (fclose(f) != 0) { remove(tmp); return -1; }
    return rename(tmp, path) == 0 ? 0 : -1;
}

int sacc_load(sacc_t * a, const char * path) {
    memset(a, 0, sizeof *a);
    FILE * f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int ok = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        double v;
        int kind, off = 0;
        long t;
        if (sscanf(line, "total %lf", &v) == 1 && v >= 0) { a->total_wh = v; ok = 1; continue; }
        if (sscanf(line, "src %d %lf %ld %n", &kind, &v, &t, &off) == 3 && off > 0 &&
            kind >= SACC_LIFETIME && kind <= SACC_POWER && line[off] && a->n < SACC_MAX) {
            sacc_src_t * s = &a->src[a->n++];
            snprintf(s->id, sizeof s->id, "%s", line + off);
            s->kind = kind; s->last = v; s->t = t;
        }
    }
    fclose(f);
    return ok ? 0 : -1;
}
