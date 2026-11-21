/* Host-side test for homey_parse.c (BACKLOG #28).
 *
 *   make -C lvgl_ui_recovered/src test-homey
 *
 * Fixtures in tests/fixtures/ are SYNTHETIC: invented names, ids and values
 * that only copy the JSON shape of a real Homey Pro (2023). Real user exports
 * may be dropped into tests/local/ (git-ignored) and parsed with
 *   homey_parse_test --dump tests/local/devices.json
 */
#include "../src/homey_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char * slurp(const char * path, size_t * len) {
    FILE * f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char * b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(2); }
    b[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return b;
}

static const hy_dev_t * find(const hy_dev_t * d, int n, const char * name) {
    for (int i = 0; i < n; i++) if (!strcmp(d[i].name, name)) return &d[i];
    return NULL;
}

static void dump(const char * path) {
    size_t len;
    char * b = slurp(path, &len);
    static hy_dev_t d[HY_MAX_DEV];
    int n = hy_parse_devices(b, len, d, HY_MAX_DEV);
    printf("%d devices\n", n);
    for (int i = 0; i < n; i++) {
        printf("%-10s %s %-32s", hy_kind_name(d[i].kind), d[i].available ? " " : "x", d[i].name);
        for (int c = 0; c < d[i].ncap; c++) {
            const hy_cap_t * k = &d[i].cap[c];
            if (k->type == HY_V_STR) printf(" %s=%s", k->id, k->str);
            else if (k->type == HY_V_NONE) printf(" %s=-", k->id);
            else printf(" %s=%g%s", k->id, k->num, k->units);
        }
        printf("\n");
    }
    free(b);
}

int main(int argc, char ** argv) {
    if (argc == 3 && !strcmp(argv[1], "--dump")) { dump(argv[2]); return 0; }
    const char * dir = argc > 1 ? argv[1] : "../tests/fixtures";
    char path[512];
    size_t len;

    /* ---- name folding (UI fonts are ASCII only) ---- */
    {
        static const struct { const char * in, * out; } T[] = {
            { "Caf\xc3\xa9 \xc3\x84rger \xc3\x9f",         "Cafe Arger s" },
            { "\xf0\x9f\x92\xa1 Lamp \xf0\x9f\x92\xa1",       "Lamp" },
            { "\xe2\x80\x9cTV\xe2\x80\x9d \xe2\x80\x93 hal",  "\"TV\" - hal" },
            { "a\xc2\xa0\xc2\xa0" "b\t c",                    "a b c" },
            { "\xf0\x9f\x92\xa1",                             "" },
            { "trunc\xe2\x80",                                "trunc" },   /* cut mid-sequence */
            { "\xe4\xb8\xad\xe6\x96\x87x",                    "x" },
        };
        for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
            char s[64];
            snprintf(s, sizeof s, "%s", T[i].in);
            hy_fold_text(s);
            if (strcmp(s, T[i].out)) { fprintf(stderr, "FAIL fold #%zu: '%s'\n", i, s); fails++; }
        }
    }

    /* ---- devices ---- */
    snprintf(path, sizeof path, "%s/homey_devices.json", dir);
    char * b = slurp(path, &len);
    static hy_dev_t d[HY_MAX_DEV];
    int n = hy_parse_devices(b, len, d, HY_MAX_DEV);
    CHECK(n == 11);

    const hy_dev_t * x;
    CHECK((x = find(d, n, "Lamp Hall")) && x->kind == HY_K_LIGHT && x->ncap == 2 && x->available);
    if (x) {
        const hy_cap_t * c = hy_dev_cap(x, "dim");
        CHECK(c && c->type == HY_V_NUM && fabs(c->num - 0.5) < 1e-9 && c->setable && !strcmp(c->units, "%"));
        c = hy_dev_cap(x, "onoff");
        CHECK(c && c->type == HY_V_BOOL && c->num == 1);
        CHECK(!strcmp(x->zone, "10000000-0000-4000-8000-000000000001"));
        CHECK(!strcmp(x->cls, "socket") && !strcmp(x->vcls, "light"));
    }
    /* \u escapes incl. a surrogate pair decode to UTF-8. */
    CHECK((x = find(d, n, "Kitchen Strip e")) && x->kind == HY_K_LIGHT && x->ncap == 6);   /* é + emoji folded */
    if (x) {
        const hy_cap_t * c = hy_dev_cap(x, "light_mode");
        CHECK(c && c->type == HY_V_STR && !strcmp(c->str, "color"));
        c = hy_dev_cap(x, "light_temperature");
        CHECK(c && c->type == HY_V_NONE);                     /* null value */
    }
    CHECK((x = find(d, n, "Shutter Front")) && x->kind == HY_K_COVER && x->ncap == 1);
    if (x) { const hy_cap_t * c = hy_dev_cap(x, "windowcoverings_state"); CHECK(c && !strcmp(c->str, "down")); }
    CHECK((x = find(d, n, "Curtain Garden")) && x->kind == HY_K_COVER && x->ncap == 3);
    CHECK((x = find(d, n, "Front Door")) && x->kind == HY_K_LOCK);
    CHECK((x = find(d, n, "Temp & Hum Attic")) && x->kind == HY_K_SENSOR);
    if (x) { const hy_cap_t * c = hy_dev_cap(x, "measure_temperature"); CHECK(c && fabs(c->num - 21.4) < 1e-9 && !strcmp(c->units, "\xc2\xb0" "C")); }
    CHECK((x = find(d, n, "Scene Evening")) && x->kind == HY_K_BUTTON);
    CHECK((x = find(d, n, "Cloud Lamp")) && !x->available && x->ncap == 0 && x->kind == HY_K_LIGHT);
    CHECK(!find(d, n, "Gone App Device"));                    /* unavailable + nothing usable */
    CHECK(!find(d, n, "Remote Living"));                      /* no capabilities */
    CHECK(!find(d, n, "Toon 2"));                             /* the Toon itself */
    CHECK((x = find(d, n, "Airco Living")) && x->kind == HY_K_THERMOSTAT && x->ncap == 7);
    if (x) CHECK(hy_dev_cap(x, "meter_power.daily") && !hy_dev_cap(x, "fan_speed"));
    CHECK((x = find(d, n, "Wake Desktop")) && x->kind == HY_K_BUTTON);
    CHECK((x = find(d, n, "Fan Bathroom")) && x->kind == HY_K_FAN && x->ncap == 7);

    /* No settings content may leak into any stored string. */
    for (int i = 0; i < n; i++) {
        CHECK(!strstr(d[i].name, "secret"));
        for (int c = 0; c < d[i].ncap; c++) CHECK(!strstr(d[i].cap[c].str, "secret"));
    }

    /* Table limit honoured. */
    CHECK(hy_parse_devices(b, len, d, 3) == 3);

    /* Truncated bodies: never crash, never over-read (run under ASan). */
    int bad = 0;
    for (size_t cut = 0; cut < len; cut++) {
        char * t = malloc(cut ? cut : 1);
        memcpy(t, b, cut);
        int r = hy_parse_devices(t, cut, d, HY_MAX_DEV);
        if (r != -1 && r > 11) bad++;
        free(t);
    }
    CHECK(bad == 0);
    CHECK(hy_parse_devices("not json", 8, d, HY_MAX_DEV) == -1);
    CHECK(hy_parse_devices("{}", 2, d, HY_MAX_DEV) == 0);
    CHECK(hy_parse_devices("[]", 2, d, HY_MAX_DEV) == 0);
    free(b);

    /* ---- zones ---- */
    snprintf(path, sizeof path, "%s/homey_zones.json", dir);
    b = slurp(path, &len);
    hy_zone_t z[HY_MAX_ZONE];
    n = hy_parse_zones(b, len, z, HY_MAX_ZONE);
    CHECK(n == 4);
    CHECK(n == 4 && !strcmp(z[2].name, "Living Room") && !strcmp(z[2].id, "10000000-0000-4000-8000-000000000002"));
    free(b);

    /* ---- flows ---- */
    hy_flow_t f[HY_MAX_FLOW];
    snprintf(path, sizeof path, "%s/homey_flows.json", dir);
    b = slurp(path, &len);
    n = hy_parse_flows(b, len, 0, f, 0, HY_MAX_FLOW);
    CHECK(n == 2);
    CHECK(n >= 2 && !strcmp(f[0].name, "Movie Evening") && !strcmp(f[1].name, "\"We Go To Bed\"") && !f[0].advanced);
    free(b);
    snprintf(path, sizeof path, "%s/homey_advancedflows.json", dir);
    b = slurp(path, &len);
    n = hy_parse_flows(b, len, 1, f, n, HY_MAX_FLOW);
    CHECK(n == 3);
    CHECK(n == 3 && !strcmp(f[2].name, "Start Office Time") && f[2].advanced);
    free(b);

    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("homey_parse_test: all checks passed\n");
    return 0;
}
