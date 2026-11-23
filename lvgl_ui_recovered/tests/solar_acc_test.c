/* Host test for solar_acc.c (BACKLOG #60). Synthetic values only.
 *   make test-solar   (in src/) */
#include "../src/solar_acc.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int fails = 0, checks = 0;
#define NEAR(a, b) (fabs((a) - (b)) < 0.01)
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    sacc_t a;
    long t = 1700000000;

    /* Two kWh meters on two PV systems: both increases add up; the first
     * reading of each is only a baseline (no lifetime jump). */
    memset(&a, 0, sizeof a);
    CHECK(sacc_feed(&a, "hw:m1", SACC_LIFETIME, 1000000, t) == 0, "baseline m1");
    CHECK(sacc_feed(&a, "hw:m2", SACC_LIFETIME, 5000000, t) == 0, "baseline m2");
    sacc_feed(&a, "hw:m1", SACC_LIFETIME, 1000300, t + 600);
    sacc_feed(&a, "hw:m2", SACC_LIFETIME, 5000200, t + 600);
    CHECK(NEAR(a.total_wh, 500), "sum of two meters: %.1f", a.total_wh);

    /* 0 = not read yet: ignored, no baseline at 0 (else the next reading
     * would add the whole lifetime). */
    CHECK(sacc_feed(&a, "hw:m3", SACC_LIFETIME, 0, t + 600) == 0, "zero ignored");
    sacc_feed(&a, "hw:m3", SACC_LIFETIME, 800000, t + 610);
    CHECK(NEAR(a.total_wh, 500), "new source only a baseline: %.1f", a.total_wh);

    /* Offline for two hours, then back: the production meanwhile counts. */
    sacc_feed(&a, "hw:m1", SACC_LIFETIME, 1004300, t + 600 + 7200);
    CHECK(NEAR(a.total_wh, 4500), "catch up after offline: %.1f", a.total_wh);

    /* Meter swapped (counter drops) and an impossible jump: re-base only. */
    sacc_feed(&a, "hw:m2", SACC_LIFETIME, 100, t + 8000);
    CHECK(NEAR(a.total_wh, 4500), "drop re-bases: %.1f", a.total_wh);
    sacc_feed(&a, "hw:m2", SACC_LIFETIME, 9000000, t + 8010);
    CHECK(NEAR(a.total_wh, 4500), "impossible jump re-bases: %.1f", a.total_wh);
    sacc_feed(&a, "hw:m2", SACC_LIFETIME, 9000050, t + 8070);
    CHECK(NEAR(a.total_wh, 4550), "counts again after re-base: %.1f", a.total_wh);

    /* Daily counter: increases, a new day adds that day's value so far, a
     * small dip is a glitch. */
    memset(&a, 0, sizeof a);
    sacc_feed(&a, "app:inv", SACC_DAILY, 12000, t);
    sacc_feed(&a, "app:inv", SACC_DAILY, 12500, t + 300);
    CHECK(NEAR(a.total_wh, 500), "daily increase: %.1f", a.total_wh);
    sacc_feed(&a, "app:inv", SACC_DAILY, 12450, t + 600);
    CHECK(NEAR(a.total_wh, 500), "small dip ignored: %.1f", a.total_wh);
    sacc_feed(&a, "app:inv", SACC_DAILY, 12500, t + 900);
    CHECK(NEAR(a.total_wh, 550), "after the dip it counts from the dip: %.1f", a.total_wh);
    sacc_feed(&a, "app:inv", SACC_DAILY, 40, t + 36000);
    CHECK(NEAR(a.total_wh, 590), "new day adds its value: %.1f", a.total_wh);

    /* Power: trapezium over time; a gap > 3 min is not bridged. */
    memset(&a, 0, sizeof a);
    sacc_feed(&a, "ha:sensor.pv", SACC_POWER, 1000, t);
    sacc_feed(&a, "ha:sensor.pv", SACC_POWER, 3000, t + 36);       /* avg 2000 W x 36 s = 20 Wh */
    CHECK(NEAR(a.total_wh, 20), "power integration: %.2f", a.total_wh);
    sacc_feed(&a, "ha:sensor.pv", SACC_POWER, 3000, t + 36 + 600);  /* gap */
    CHECK(NEAR(a.total_wh, 20), "gap not bridged: %.2f", a.total_wh);
    sacc_feed(&a, "ha:sensor.pv", SACC_POWER, -50, t + 36 + 636);   /* negative = 0 W */
    CHECK(NEAR(a.total_wh, 35), "negative power as 0: %.2f", a.total_wh);

    /* A source changing kind re-bases. */
    sacc_feed(&a, "ha:sensor.pv", SACC_LIFETIME, 777000, t + 700);
    CHECK(NEAR(a.total_wh, 35), "kind change re-bases: %.2f", a.total_wh);

    /* Floor: the total continues from what the statistics already hold. */
    sacc_floor(&a, 123456);
    CHECK(NEAR(a.total_wh, 123456), "floor raises");
    sacc_floor(&a, 10);
    CHECK(NEAR(a.total_wh, 123456), "floor never lowers");

    /* Save / load round trip, incl. an id with spaces. */
    sacc_feed(&a, "app:inverters:solis cloud", SACC_DAILY, 100, t);
    const char * path = "../build-test/solar_acc_test.dat";
    CHECK(sacc_save(&a, path) == 0, "save");
    sacc_t b;
    CHECK(sacc_load(&b, path) == 0, "load");
    CHECK(NEAR(b.total_wh, a.total_wh) && b.n == a.n, "round trip total %.1f n %d", b.total_wh, b.n);
    int same = 1;
    for (int i = 0; i < a.n; i++)
        if (strcmp(a.src[i].id, b.src[i].id) || a.src[i].kind != b.src[i].kind ||
            !NEAR(a.src[i].last, b.src[i].last) || a.src[i].t != b.src[i].t) same = 0;
    CHECK(same, "sources round trip");
    remove(path);
    CHECK(sacc_load(&b, "../build-test/does-not-exist") != 0 && b.total_wh == 0 && b.n == 0, "missing file");

    /* More sources than slots: the one not heard of longest is reused. */
    memset(&a, 0, sizeof a);
    char id[32];
    for (int i = 0; i < SACC_MAX + 2; i++) {
        snprintf(id, sizeof id, "s%d", i);
        sacc_feed(&a, id, SACC_LIFETIME, 1000, t + i);
    }
    CHECK(a.n == SACC_MAX, "slots capped at %d", a.n);

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
