#include "buienradar_rain.h"
#include "settings.h"
#include "weather.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>

static buienradar_rain_snapshot_t s_rain_data = {0};
static pthread_mutex_t s_rain_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int s_rain_wake = 0;

static const char * ci_strstr(const char * hay, const char * needle) {
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    if (!nl) return hay;
    while (*hay) {
        if (strncasecmp(hay, needle, nl) == 0) return hay;
        hay++;
    }
    return NULL;
}

static int parse_3h(const char * body, buienradar_rain_snapshot_t * snap) {
    if (!body) return -1;
    const char * f_arr = ci_strstr(body, "\"forecasts\":");
    if (!f_arr) return -1;
    f_arr = strchr(f_arr, '[');
    if (!f_arr) return -1;

    /* Extract emptytext if present: "emptytext":"..." */
    const char * et = ci_strstr(body, "\"emptytext\":");
    if (et) {
        const char * col = strchr(et, ':');
        if (col) {
            const char * q1 = strchr(col, '"');
            if (q1) {
                const char * q2 = strchr(q1 + 1, '"');
                if (q2 && (q2 - (q1 + 1)) < (int)sizeof(snap->summary_3h)) {
                    size_t len = q2 - (q1 + 1);
                    memcpy(snap->summary_3h, q1 + 1, len);
                    snap->summary_3h[len] = '\0';
                }
            }
        }
    }

    int count = 0;
    int first_rain_idx = -1;
    int rain_stop_idx = -1;
    float max_mm = 0.0f;
    int max_val = 0;

    const char * p = f_arr;
    while ((p = strchr(p, '{')) != NULL && count < RAIN_MAX_POINTS_3H) {
        const char * end = strchr(p, '}');
        if (!end) break;

        rain_point_t pt = {0};

        /* Extract time */
        const char * dt = ci_strstr(p, "\"datetime\":");
        if (dt && dt < end) {
            const char * t = strchr(dt, 'T');
            if (t && t < end && strlen(t) >= 6) {
                snprintf(pt.time_str, sizeof(pt.time_str), "%.5s", t + 1);
            }
        }

        /* Extract value (0..100) */
        const char * val_p = ci_strstr(p, "\"value\":");
        if (val_p && val_p < end) {
            pt.value = atoi(val_p + 8);
        }

        /* Extract precipitation (mm/h) */
        const char * pr_p = ci_strstr(p, "\"precipitation\":");
        if (pr_p && pr_p < end) {
            pt.mm = (float)atof(pr_p + 16);
        }

        if (pt.value > max_val) max_val = pt.value;
        if (pt.mm > max_mm) max_mm = pt.mm;

        if (pt.value > 0 && first_rain_idx < 0) {
            first_rain_idx = count;
        } else if (first_rain_idx >= 0 && rain_stop_idx < 0 && pt.value == 0) {
            rain_stop_idx = count;
        }

        snap->points_3h[count++] = pt;
        p = end + 1;
    }

    snap->count_3h = count;
    snap->max_mm_3h = max_mm;
    snap->has_rain_3h = (max_val > 0);

    if (max_val == 0) {
        if (!snap->summary_3h[0]) {
            snprintf(snap->summary_3h, sizeof(snap->summary_3h), "Geen neerslag verwacht");
        }
    } else {
        if (first_rain_idx == 0) {
            if (rain_stop_idx > 0 && rain_stop_idx < count) {
                snprintf(snap->summary_3h, sizeof(snap->summary_3h), "Regen tot %s",
                         snap->points_3h[rain_stop_idx].time_str);
            } else {
                snprintf(snap->summary_3h, sizeof(snap->summary_3h), "Regen komende 3 uur");
            }
        } else if (first_rain_idx > 0) {
            snprintf(snap->summary_3h, sizeof(snap->summary_3h), "Regen om %s",
                     snap->points_3h[first_rain_idx].time_str);
        }
    }

    return (count > 0) ? 0 : -1;
}

static int parse_24h(const char * body, buienradar_rain_snapshot_t * snap) {
    if (!body) return -1;
    const char * f_arr = ci_strstr(body, "\"forecasts\":");
    if (!f_arr) return -1;
    f_arr = strchr(f_arr, '[');
    if (!f_arr) return -1;

    int count = 0;
    float max_mm = 0.0f;
    int max_val = 0;

    const char * p = f_arr;
    while ((p = strchr(p, '{')) != NULL && count < RAIN_MAX_POINTS_24H) {
        const char * end = strchr(p, '}');
        if (!end) break;

        rain_point_t pt = {0};

        /* Extract dateTime */
        const char * dt = ci_strstr(p, "\"dateTime\":");
        if (dt && dt < end) {
            const char * t = strchr(dt, 'T');
            if (t && t < end && strlen(t) >= 6) {
                snprintf(pt.time_str, sizeof(pt.time_str), "%.5s", t + 1);
            }
        }

        /* Extract percentageValue (0..100) */
        const char * val_p = ci_strstr(p, "\"percentageValue\":");
        if (val_p && val_p < end) {
            pt.value = (int)atof(val_p + 18);
        }

        /* Extract dataValue (mm/u) */
        const char * pr_p = ci_strstr(p, "\"dataValue\":");
        if (pr_p && pr_p < end) {
            pt.mm = (float)atof(pr_p + 12);
        }

        if (pt.value > max_val) max_val = pt.value;
        if (pt.mm > max_mm) max_mm = pt.mm;

        snap->points_24h[count++] = pt;
        p = end + 1;
    }

    snap->count_24h = count;
    snap->max_mm_24h = max_mm;
    snap->has_rain_24h = (max_val > 0);

    if (max_val == 0) {
        snprintf(snap->summary_24h, sizeof(snap->summary_24h), "Geen neerslag (24u)");
    } else {
        snprintf(snap->summary_24h, sizeof(snap->summary_24h), "Neerslag komende 24u (max %.1f mm/u)", max_mm);
    }

    return (count > 0) ? 0 : -1;
}

static void * rain_thread(void * arg) {
    (void)arg;
    sleep(6); /* settle WiFi on boot */

    static char body_3h[32 * 1024];
    static char body_24h[32 * 1024];
    int tick_24h = 0;

    while (1) {
        double lat = 0.0, lon = 0.0;
        if (settings.weather_lat[0] && settings.weather_lon[0]) {
            lat = atof(settings.weather_lat);
            lon = atof(settings.weather_lon);
        }
        if (lat == 0.0 && lon == 0.0) {
            weather_get_lat_lon(&lat, &lon);
        }
        if (lat == 0.0 && lon == 0.0) {
            lat = 52.01;
            lon = 4.55;
        }

        char url_3h[256];
        snprintf(url_3h, sizeof(url_3h),
                 "https://graphdata.buienradar.nl/2.0/forecast/geo/RainHistoryForecast?lat=%.4f&lon=%.4f",
                 lat, lon);

        buienradar_rain_snapshot_t temp_snap = {0};

        /* Fetch 3-hour forecast */
        if (http_fetch_to(url_3h, body_3h, sizeof(body_3h), 10) == 0) {
            parse_3h(body_3h, &temp_snap);
        }

        /* Fetch 24-hour forecast every 30 mins or on first run */
        if (tick_24h == 0 || s_rain_data.count_24h == 0) {
            char url_24h[256];
            snprintf(url_24h, sizeof(url_24h),
                     "https://graphdata.buienradar.nl/3.0/forecast/geo/Rain24Hour?lat=%.4f&lon=%.4f",
                     lat, lon);
            if (http_fetch_to(url_24h, body_24h, sizeof(body_24h), 10) == 0) {
                parse_24h(body_24h, &temp_snap);
            }
        }

        pthread_mutex_lock(&s_rain_mtx);
        if (temp_snap.count_3h > 0) {
            s_rain_data.count_3h = temp_snap.count_3h;
            memcpy(s_rain_data.points_3h, temp_snap.points_3h, sizeof(temp_snap.points_3h));
            snprintf(s_rain_data.summary_3h, sizeof(s_rain_data.summary_3h), "%s", temp_snap.summary_3h);
            s_rain_data.has_rain_3h = temp_snap.has_rain_3h;
            s_rain_data.max_mm_3h = temp_snap.max_mm_3h;
            s_rain_data.last_updated = time(NULL);
            s_rain_data.is_valid = 1;
        }
        if (temp_snap.count_24h > 0) {
            s_rain_data.count_24h = temp_snap.count_24h;
            memcpy(s_rain_data.points_24h, temp_snap.points_24h, sizeof(temp_snap.points_24h));
            snprintf(s_rain_data.summary_24h, sizeof(s_rain_data.summary_24h), "%s", temp_snap.summary_24h);
            s_rain_data.has_rain_24h = temp_snap.has_rain_24h;
            s_rain_data.max_mm_24h = temp_snap.max_mm_24h;
        }
        pthread_mutex_unlock(&s_rain_mtx);

        tick_24h = (tick_24h + 1) % 6; /* 6 * 5 min = 30 min */

        /* Sleep 5 minutes (300 s) unless woken up */
        for (int i = 0; i < 300 && !s_rain_wake; i++) {
            sleep(1);
        }
        s_rain_wake = 0;
    }
    return NULL;
}

int buienradar_rain_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, rain_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}

void buienradar_rain_refresh_async(void) {
    s_rain_wake = 1;
}

void buienradar_rain_get_snapshot(buienradar_rain_snapshot_t * out) {
    if (!out) return;
    pthread_mutex_lock(&s_rain_mtx);
    *out = s_rain_data;
    pthread_mutex_unlock(&s_rain_mtx);
}

