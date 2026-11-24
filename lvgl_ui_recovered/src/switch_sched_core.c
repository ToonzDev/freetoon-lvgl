/* Schakelschema core: see switch_sched_core.h. */
#include "switch_sched_core.h"
#include "minijson.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Days since 1970-01-01 of a civil date (proleptic Gregorian). */
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Local date `back` days before the date of t. */
static void local_date(long t, int back, int * y, int * m, int * d, int * wday) {
    time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    lt.tm_mday -= back;
    lt.tm_hour = 12; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
    mktime(&lt);                              /* normalises the date */
    *y = lt.tm_year + 1900; *m = lt.tm_mon + 1; *d = lt.tm_mday;
    if (wday) *wday = lt.tm_wday;
}

/* Standard sunrise equation (as icons.c), official zenith 90.833°. */
int ss_sun(int y, int m, int d, double lat, double lon, long * rise, long * set) {
    const double D2R = M_PI / 180.0, R2D = 180.0 / M_PI, ZENITH = 90.833;
    double lng_hour = lon / 15.0;
    long base = days_from_civil(y, m, d);
    int N = (int)(base - days_from_civil(y, 1, 1)) + 1;
    long out[2];
    for (int w = 0; w < 2; w++) {
        double t = N + (((w == 0) ? 6.0 : 18.0) - lng_hour) / 24.0;
        double M = 0.9856 * t - 3.289;
        double L = M + 1.916 * sin(M * D2R) + 0.020 * sin(2 * M * D2R) + 282.634;
        L = fmod(L + 720.0, 360.0);
        double RA = fmod(R2D * atan(0.91764 * tan(L * D2R)) + 720.0, 360.0);
        RA += floor(L / 90.0) * 90.0 - floor(RA / 90.0) * 90.0;
        RA /= 15.0;
        double sin_dec = 0.39782 * sin(L * D2R);
        double cos_dec = cos(asin(sin_dec));
        double cos_h = (cos(ZENITH * D2R) - sin_dec * sin(lat * D2R)) / (cos_dec * cos(lat * D2R));
        if (cos_h > 1.0 || cos_h < -1.0) return -1;
        double H = (w == 0) ? 360.0 - R2D * acos(cos_h) : R2D * acos(cos_h);
        H /= 15.0;
        double T = H + RA - 0.06571 * t - 6.622;
        double UT = fmod(T - lng_hour + 48.0, 24.0);
        out[w] = base * 86400L + (long)(UT * 3600.0 + 0.5);
    }
    if (rise) *rise = out[0];
    if (set)  *set  = out[1];
    return 0;
}

static int date_in_days(const ss_rule_t * r, int wday) {
    int bit = (wday + 6) % 7;                 /* tm_wday 0 = Sunday -> bit 6 */
    return (r->days >> bit) & 1;
}

long ss_occurrence(const ss_rule_t * r, int y, int m, int d, double lat, double lon) {
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12; t.tm_isdst = -1;
    mktime(&t);
    if (!date_in_days(r, t.tm_wday)) return -1;
    if (r->mode == SS_CLOCK) {
        t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
        t.tm_hour = r->hhmm / 100; t.tm_min = r->hhmm % 100; t.tm_sec = 0; t.tm_isdst = -1;
        return (long)mktime(&t);
    }
    long rise, set;
    if (ss_sun(y, m, d, lat, lon, &rise, &set) != 0) return -1;
    return (r->mode == SS_SUNRISE ? rise : set) + (long)r->offset * 60;
}

long ss_last_occurrence(const ss_rule_t * r, long now, double lat, double lon) {
    for (int back = 0; back <= 3; back++) {
        int y, m, d;
        local_date(now, back, &y, &m, &d, NULL);
        long t = ss_occurrence(r, y, m, d, lat, lon);
        if (t >= 0 && t <= now) return t;
    }
    return -1;
}

long ss_next_occurrence(const ss_rule_t * r, long now, double lat, double lon) {
    for (int ahead = 0; ahead <= 8; ahead++) {
        int y, m, d;
        local_date(now, -ahead, &y, &m, &d, NULL);
        long t = ss_occurrence(r, y, m, d, lat, lon);
        if (t > now) return t;
    }
    return -1;
}

int ss_due(ss_set_t * s, long now, double lat, double lon, long catchup,
           ss_action_t * out, int max, int * changed) {
    int n = 0;
    if (changed) *changed = 0;
    long when[SS_MAX];
    for (int i = 0; i < s->n; i++) {
        ss_rule_t * r = &s->r[i];
        when[i] = -1;
        if (!r->active || !r->ref[0]) continue;
        long t = ss_last_occurrence(r, now, lat, lon);
        if (t < 0 || t <= r->last || now - t > catchup) continue;
        when[i] = t;
    }
    for (int i = 0; i < s->n; i++) {
        if (when[i] < 0) continue;
        /* the latest due occurrence for this device wins */
        int best = i;
        for (int j = 0; j < s->n; j++)
            if (when[j] >= 0 && !strcmp(s->r[j].ref, s->r[i].ref) &&
                (when[j] > when[best] || (when[j] == when[best] && j > best))) best = j;
        if (best == i && n < max) {
            snprintf(out[n].ref, sizeof out[n].ref, "%s", s->r[i].ref);
            out[n].on = s->r[i].on;
            out[n].rule = i;
            out[n].t = when[i];
            n++;
        }
    }
    for (int i = 0; i < s->n; i++)
        if (when[i] >= 0) { s->r[i].last = now; if (changed) *changed = 1; }
    return n;
}

void ss_time_text(const ss_rule_t * r, char * out, size_t n) {
    if (r->mode == SS_CLOCK) { snprintf(out, n, "%02d:%02d", r->hhmm / 100 % 24, r->hhmm % 100 % 60); return; }
    const char * what = r->mode == SS_SUNRISE ? "zonsopkomst" : "zonsondergang";
    if (r->offset) snprintf(out, n, "%s %+d min", what, r->offset);
    else snprintf(out, n, "%s", what);
}

void ss_days_text(int days, char * out, size_t n) {
    static const char * const nm[7] = { "ma", "di", "wo", "do", "vr", "za", "zo" };
    days &= SS_ALL_DAYS;
    if (days == SS_ALL_DAYS) { snprintf(out, n, "elke dag"); return; }
    if (days == 0x1f)        { snprintf(out, n, "ma-vr"); return; }
    if (days == 0x60)        { snprintf(out, n, "za, zo"); return; }
    if (!days)               { snprintf(out, n, "geen dag"); return; }
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < 7 && o < n; i++)
        if ((days >> i) & 1) o += (size_t)snprintf(out + o, n - o, "%s%s", o ? ", " : "", nm[i]);
}

int ss_save(const ss_set_t * s, const char * path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE * f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# FreeToon schakelschema: r <active> <on> <mode> <hhmm> <offset> <days> <last> <ref>\n");
    for (int i = 0; i < s->n; i++) {
        const ss_rule_t * r = &s->r[i];
        fprintf(f, "r %d %d %d %d %d %d %ld %s\n", r->active, r->on, r->mode, r->hhmm,
                r->offset, r->days, r->last, r->ref);
    }
    if (fclose(f) != 0) { remove(tmp); return -1; }
    return rename(tmp, path) == 0 ? 0 : -1;
}

int ss_load(ss_set_t * s, const char * path) {
    memset(s, 0, sizeof *s);
    FILE * f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof line, f) && s->n < SS_MAX) {
        line[strcspn(line, "\r\n")] = 0;
        ss_rule_t r;
        memset(&r, 0, sizeof r);
        int off = 0;
        if (sscanf(line, "r %d %d %d %d %d %d %ld %n", &r.active, &r.on, &r.mode, &r.hhmm,
                   &r.offset, &r.days, &r.last, &off) != 7 || off <= 0 || !line[off]) continue;
        if (r.mode < SS_CLOCK || r.mode > SS_SUNSET || r.hhmm < 0 || r.hhmm > 2359 ||
            r.hhmm % 100 > 59 || r.offset < -180 || r.offset > 180) continue;
        snprintf(r.ref, sizeof r.ref, "%s", line + off);
        r.active = r.active ? 1 : 0; r.on = r.on ? 1 : 0; r.days &= SS_ALL_DAYS;
        s->r[s->n++] = r;
    }
    fclose(f);
    return 0;
}

int ss_import_qml(ss_set_t * s, const char * json, long now) {
    static const char * const dk[7] = { "mo", "tu", "we", "th", "fr", "sa", "su" };
    int added = 0;
    for (int i = 0; s->n < SS_MAX; i++) {
        char path[48];
        snprintf(path, sizeof path, "scheduleitems[%d]", i);
        const char * it = mj_find(json, path, NULL);
        if (!it) break;
        char uuid[SS_REF_LEN - 4] = "", action[16] = "";
        double st = -1, act = 0, v;
        mj_str(it, "pluguuid", uuid, sizeof uuid);
        mj_num(it, "starttime", &st);
        mj_str(it, "action", action, sizeof action);
        mj_num(it, "active", &act);
        int hhmm = (int)st;
        if (!uuid[0] || !strcmp(uuid, "-") || hhmm < 0 || hhmm > 2359 || hhmm % 100 > 59) continue;
        ss_rule_t * r = &s->r[s->n];
        memset(r, 0, sizeof *r);
        r->active = act != 0;
        r->on = !strcmp(action, "Aan");
        r->mode = SS_CLOCK;
        r->hhmm = hhmm;
        for (int k = 0; k < 7; k++)
            if (mj_num(it, dk[k], &v) && v != 0) r->days |= 1 << k;
        r->last = now;
        snprintf(r->ref, sizeof r->ref, "zw:%s", uuid);
        s->n++;
        added++;
    }
    return added;
}
