/* Host test for switch_sched_core.c (schakelschema). Synthetic data only.
 *   make test-schedule   (in src/) */
#include "../src/switch_sched_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static long at(int y, int m, int d, int hh, int mm) {
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = hh; t.tm_min = mm; t.tm_isdst = -1;
    return (long)mktime(&t);
}
static int hm(long t) { time_t tt = t; struct tm l; localtime_r(&tt, &l); return l.tm_hour * 100 + l.tm_min; }
static int near_hm(long t, int want, int tol_min) {
    int got = hm(t);
    int a = got / 100 * 60 + got % 100, b = want / 100 * 60 + want % 100;
    return abs(a - b) <= tol_min;
}
static ss_rule_t rule(const char * ref, int on, int mode, int hhmm, int offset, int days) {
    ss_rule_t r;
    memset(&r, 0, sizeof r);
    r.active = 1; r.on = on; r.mode = mode; r.hhmm = hhmm; r.offset = offset; r.days = days;
    snprintf(r.ref, sizeof r.ref, "%s", ref);
    return r;
}

int main(void) {
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    const double LAT = 52.37, LON = 4.90;    /* Amsterdam */

    /* Sun: Amsterdam, published times within a few minutes. */
    long rise, set;
    CHECK(ss_sun(2026, 6, 21, LAT, LON, &rise, &set) == 0, "sun June");
    CHECK(near_hm(rise, 520, 6) && near_hm(set, 2206, 6), "June rise %04d set %04d", hm(rise), hm(set));
    CHECK(ss_sun(2026, 12, 21, LAT, LON, &rise, &set) == 0, "sun December");
    CHECK(near_hm(rise, 848, 6) && near_hm(set, 1629, 6), "Dec rise %04d set %04d", hm(rise), hm(set));
    CHECK(ss_sun(2026, 6, 21, 80.0, LON, &rise, &set) != 0, "midnight sun: no rise/set");

    /* Clock occurrence keeps local time over the DST change (29 March 2026). */
    ss_rule_t r7 = rule("zw:a", 1, SS_CLOCK, 700, 0, SS_ALL_DAYS);
    CHECK(hm(ss_occurrence(&r7, 2026, 3, 28, LAT, LON)) == 700, "07:00 before DST");
    CHECK(hm(ss_occurrence(&r7, 2026, 3, 29, LAT, LON)) == 700, "07:00 on DST day");
    CHECK(hm(ss_occurrence(&r7, 2026, 3, 30, LAT, LON)) == 700, "07:00 after DST");

    /* Weekdays: Mon-Fri rule on Saturday 28 Nov 2026 -> none; Friday -> yes. */
    ss_rule_t wk = rule("zw:a", 1, SS_CLOCK, 700, 0, 0x1f);
    CHECK(ss_occurrence(&wk, 2026, 11, 28, LAT, LON) == -1, "Saturday not in ma-vr");
    CHECK(ss_occurrence(&wk, 2026, 11, 27, LAT, LON) == at(2026, 11, 27, 7, 0), "Friday in ma-vr");
    CHECK(ss_next_occurrence(&wk, at(2026, 11, 28, 9, 0), LAT, LON) == at(2026, 11, 30, 7, 0), "next = Monday");

    /* Sunset with an offset. */
    ss_rule_t ss = rule("hu:1", 1, SS_SUNSET, 0, -15, SS_ALL_DAYS);
    ss_sun(2026, 12, 21, LAT, LON, &rise, &set);
    CHECK(ss_occurrence(&ss, 2026, 12, 21, LAT, LON) == set - 900, "sunset -15 min");

    /* Due: on 07:00 + off 08:00 for one plug, restart at 09:00 -> only "off". */
    ss_set_t s;
    memset(&s, 0, sizeof s);
    s.r[s.n++] = rule("zw:p1", 1, SS_CLOCK, 700, 0, SS_ALL_DAYS);
    s.r[s.n++] = rule("zw:p1", 0, SS_CLOCK, 800, 0, SS_ALL_DAYS);
    s.r[s.n++] = rule("hs:10.0.0.9", 1, SS_CLOCK, 830, 0, SS_ALL_DAYS);
    long day0 = at(2026, 11, 24, 0, 0);
    for (int i = 0; i < s.n; i++) s.r[i].last = day0;
    ss_action_t act[8];
    int changed;
    int n = ss_due(&s, at(2026, 11, 24, 9, 0), LAT, LON, 3 * 3600, act, 8, &changed);
    CHECK(n == 2 && changed, "two devices due: %d", n);
    int off_p1 = 0, on_hs = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(act[i].ref, "zw:p1") && act[i].on == 0) off_p1 = 1;
        if (!strcmp(act[i].ref, "hs:10.0.0.9") && act[i].on == 1) on_hs = 1;
    }
    CHECK(off_p1 && on_hs, "latest per device wins (p1 off, socket on)");
    n = ss_due(&s, at(2026, 11, 24, 9, 0) + 15, LAT, LON, 3 * 3600, act, 8, &changed);
    CHECK(n == 0 && !changed, "handled: not again (%d)", n);

    /* Normal tick: at 07:00:10 the 07:00 rule fires. */
    memset(&s, 0, sizeof s);
    s.r[s.n++] = rule("zw:p1", 1, SS_CLOCK, 700, 0, SS_ALL_DAYS);
    s.r[0].last = at(2026, 11, 24, 6, 0);
    n = ss_due(&s, at(2026, 11, 24, 7, 0) + 10, LAT, LON, 3 * 3600, act, 8, &changed);
    CHECK(n == 1 && act[0].on == 1, "fires just after its time");
    CHECK(ss_due(&s, at(2026, 11, 24, 7, 0) + 25, LAT, LON, 3 * 3600, act, 8, &changed) == 0, "once");

    /* Older than the catch-up window: skipped. A rule saved after its time
     * (last = now) does not fire for that time. Inactive rules never fire. */
    s.r[0].last = at(2026, 11, 24, 6, 0);
    CHECK(ss_due(&s, at(2026, 11, 24, 10, 30), LAT, LON, 3 * 3600, act, 8, &changed) == 0, "too old");
    s.r[0].last = at(2026, 11, 24, 9, 0);
    CHECK(ss_due(&s, at(2026, 11, 24, 9, 0) + 30, LAT, LON, 3 * 3600, act, 8, &changed) == 0, "saved after its time");
    s.r[0].active = 0; s.r[0].last = 0;
    CHECK(ss_due(&s, at(2026, 11, 24, 7, 1), LAT, LON, 3 * 3600, act, 8, &changed) == 0, "inactive");

    /* Texts. */
    char b[64];
    ss_days_text(SS_ALL_DAYS, b, sizeof b); CHECK(!strcmp(b, "elke dag"), "%s", b);
    ss_days_text(0x1f, b, sizeof b);        CHECK(!strcmp(b, "ma-vr"), "%s", b);
    ss_days_text(0x60, b, sizeof b);        CHECK(!strcmp(b, "za, zo"), "%s", b);
    ss_days_text(0x15, b, sizeof b);        CHECK(!strcmp(b, "ma, wo, vr"), "%s", b);
    ss_time_text(&r7, b, sizeof b);         CHECK(!strcmp(b, "07:00"), "%s", b);
    ss_time_text(&ss, b, sizeof b);         CHECK(!strcmp(b, "zonsondergang -15 min"), "%s", b);

    /* Save / load round trip; malformed lines skipped. */
    memset(&s, 0, sizeof s);
    s.r[s.n++] = rule("zw:p1", 1, SS_CLOCK, 2359, 0, 0x41);
    s.r[s.n++] = rule("ha:switch.pomp", 0, SS_SUNRISE, 0, 30, SS_ALL_DAYS);
    s.r[1].last = 1795500000; s.r[1].active = 0;
    const char * path = "../build-test/switch_sched_test.cfg";
    CHECK(ss_save(&s, path) == 0, "save");
    FILE * f = fopen(path, "a");
    fputs("r 1 1 0 2400 0 127 0 zw:bad\nr 1 1 9 700 0 127 0 zw:bad\ngarbage\n", f);
    fclose(f);
    ss_set_t l;
    CHECK(ss_load(&l, path) == 0 && l.n == 2, "load n=%d", l.n);
    CHECK(!memcmp(&l.r[0], &s.r[0], sizeof s.r[0]) && !memcmp(&l.r[1], &s.r[1], sizeof s.r[1]), "round trip");
    remove(path);

    /* Import of the QML app's settings (synthetic). */
    const char * qml =
        "{\"scheduleitems\":["
        "{\"pluguuid\":\"aaaa-1111\",\"starttime\":730,\"action\":\"Aan\",\"active\":1,"
        "\"mo\":1,\"tu\":1,\"we\":1,\"th\":1,\"fr\":1,\"sa\":0,\"su\":0},"
        "{\"pluguuid\":\"-\",\"starttime\":900,\"action\":\"Aan\",\"active\":0,\"mo\":1},"
        "{\"pluguuid\":\"bbbb-2222\",\"starttime\":2215,\"action\":\"Uit\",\"active\":0,"
        "\"mo\":0,\"tu\":0,\"we\":0,\"th\":0,\"fr\":0,\"sa\":1,\"su\":1}]}";
    memset(&s, 0, sizeof s);
    CHECK(ss_import_qml(&s, qml, 1234) == 2 && s.n == 2, "import 2 of 3");
    CHECK(!strcmp(s.r[0].ref, "zw:aaaa-1111") && s.r[0].on == 1 && s.r[0].hhmm == 730 &&
          s.r[0].days == 0x1f && s.r[0].active == 1 && s.r[0].last == 1234, "import rule 1");
    CHECK(!strcmp(s.r[1].ref, "zw:bbbb-2222") && s.r[1].on == 0 && s.r[1].hhmm == 2215 &&
          s.r[1].days == 0x60 && s.r[1].active == 0, "import rule 2");

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
