#include "backlight.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>

static char g_bl_path[320] = "";
static int g_bl_max = 1000;
static int g_bl_inited = 0;

static void bl_init_paths(void) {
    if (g_bl_inited) return;
    g_bl_inited = 1;

    DIR * d = opendir("/sys/class/backlight");
    if (d) {
        struct dirent * de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char path[320];
            snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", de->d_name);
            if (access(path, F_OK) == 0) {
                snprintf(g_bl_path, sizeof(g_bl_path), "%s", path);
                char max_path[320];
                snprintf(max_path, sizeof(max_path), "/sys/class/backlight/%s/max_brightness", de->d_name);
                FILE * mf = fopen(max_path, "r");
                if (mf) {
                    int m = 0;
                    if (fscanf(mf, "%d", &m) == 1 && m > 0) {
                        g_bl_max = m;
                    }
                    fclose(mf);
                }
                break;
            }
        }
        closedir(d);
    }
    if (!g_bl_path[0]) {
        snprintf(g_bl_path, sizeof(g_bl_path), "/sys/class/backlight/mp3309-bl/brightness");
    }
}

/* The LTR-303 read takes ~0.5 s (sensor integration time), so it must NEVER run
   on the UI thread — a background poller caches it and the UI reads the cache. */
static volatile int g_als_cache = -1;

/* The actual (slow, blocking) sensor read. Background thread only. */
static int als_read_slow(void) {
    for (int i = 0; i < 8; i++) {
        char pn[96]; snprintf(pn, sizeof pn, "/sys/bus/iio/devices/iio:device%d/name", i);
        FILE * f = fopen(pn, "r"); if (!f) continue;
        char nm[32] = ""; if (fscanf(f, "%31s", nm) != 1) nm[0] = 0; fclose(f);
        if (strcmp(nm, "ltr303") != 0) continue;
        char rp[120]; snprintf(rp, sizeof rp, "/sys/bus/iio/devices/iio:device%d/in_intensity_both_raw", i);
        FILE * g = fopen(rp, "r"); if (!g) return -1;
        int v = -1; if (fscanf(g, "%d", &v) != 1) v = -1; fclose(g);
        return v;
    }
    return -1;
}

/* Background poller: does the slow read off the UI thread, caches the result. */
static void * als_thread(void * arg) {
    (void)arg;
    for (;;) { g_als_cache = als_read_slow(); sleep(3); }
    return NULL;
}
void backlight_als_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, als_thread, NULL) == 0) pthread_detach(t);
}

/* Non-blocking: returns the cached value (-1 until the poller's first read). */
int backlight_als_raw(void) { return g_als_cache; }

/* Map ambient light to a backlight level between the user's dim/active bounds.
   Returns -1 when there's no sensor yet (caller falls back to the fixed value). */
int backlight_auto_level(int dim, int active) {
    int raw = backlight_als_raw();
    if (raw < 0) return -1;
    /* Gentler curve: a normally-lit room (raw ~120+) already reaches full
       brightness; only a genuinely dark room drops toward the dim level. */
    const int RAW_FULL = 130;          /* raw at/above which we go full-bright */
    if (raw > RAW_FULL) raw = RAW_FULL;
    if (active < dim) { int t = active; active = dim; dim = t; }
    return dim + (active - dim) * raw / RAW_FULL;
}

void backlight_set(int level) {
    bl_init_paths();
    if (level < 0)    level = 0;
    if (level > 1000) level = 1000;
    int hw_val = (int)(((long long)level * g_bl_max) / 1000);
    if (level > 0 && hw_val == 0) hw_val = 1;

    FILE * f = fopen(g_bl_path, "w");
    if (!f) return;
    fprintf(f, "%d\n", hw_val);
    fclose(f);
}

int backlight_get(void) {
    bl_init_paths();
    FILE * f = fopen(g_bl_path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    if (v < 0) return -1;
    if (g_bl_max <= 0) return v;
    int level = (int)(((long long)v * 1000 + (g_bl_max / 2)) / g_bl_max);
    if (level > 1000) level = 1000;
    return level;
}
