#include "buienradar_rain.h"
#include "settings.h"
#include "weather.h"
#include "i18n.h"
#include <time.h>
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
            const char * colon = strchr(dt, ':');
            if (colon && colon < end) {
                const char * t = strchr(colon, 'T');
                if (t && t < end && strlen(t) >= 6) {
                    snprintf(pt.time_str, sizeof(pt.time_str), "%.5s", t + 1);
                }
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
            const char * colon = strchr(dt, ':');
            if (colon && colon < end) {
                const char * t = strchr(colon, 'T');
                if (t && t < end && strlen(t) >= 6) {
                    snprintf(pt.time_str, sizeof(pt.time_str), "%.5s", t + 1);
                }
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

        /* Only calculate statistics over the 24-hour forecast window (first 24 bars) */
        if (count < 24) {
            if (pt.value > max_val) max_val = pt.value;
            if (pt.mm > max_mm) max_mm = pt.mm;
        }

        snap->points_24h[count++] = pt;
        p = end + 1;
    }

    snap->count_24h = (count > 24) ? 24 : count;
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
        /* Manual Lat/Lon (decimal comma accepted), else the configured place's
           coordinates, else De Bilt — the same fallback as the weather. */
        double lat = 0.0, lon = 0.0;
        weather_get_lat_lon(&lat, &lon);
        if (lat == 0.0 && lon == 0.0) {
            lat = 52.1009;
            lon = 5.1783;
        }

        char url_3h[256];
        snprintf(url_3h, sizeof(url_3h),
                 "https://graphdata.buienradar.nl/2.0/forecast/geo/RainHistoryForecast?lat=%.4f&lon=%.4f",
                 lat, lon);

        buienradar_rain_snapshot_t temp_snap = {0};

        /* Fetch 3-hour forecast */
        fprintf(stderr, "[rain] fetching 3h forecast (%.4f, %.4f)\n", lat, lon);
        if (http_fetch_to(url_3h, body_3h, sizeof(body_3h), 15) == 0) {
            if (parse_3h(body_3h, &temp_snap) == 0) {
                fprintf(stderr, "[rain] 3h forecast OK (%d points, '%s')\n",
                        temp_snap.count_3h, temp_snap.summary_3h);
            } else {
                fprintf(stderr, "[rain] 3h forecast parse failed\n");
            }
        } else {
            fprintf(stderr, "[rain] 3h forecast fetch failed\n");
        }

        /* Fetch 24-hour forecast every 30 mins or on first run */
        if (tick_24h == 0 || s_rain_data.count_24h == 0) {
            char url_24h[256];
            snprintf(url_24h, sizeof(url_24h),
                     "https://graphdata.buienradar.nl/3.0/forecast/geo/Rain24Hour?lat=%.4f&lon=%.4f",
                     lat, lon);
            fprintf(stderr, "[rain] fetching 24h forecast (%.4f, %.4f)\n", lat, lon);
            if (http_fetch_to(url_24h, body_24h, sizeof(body_24h), 15) == 0) {
                if (parse_24h(body_24h, &temp_snap) == 0) {
                    fprintf(stderr, "[rain] 24h forecast OK (%d points, '%s')\n",
                            temp_snap.count_24h, temp_snap.summary_24h);
                } else {
                    fprintf(stderr, "[rain] 24h forecast parse failed\n");
                }
            } else {
                fprintf(stderr, "[rain] 24h forecast fetch failed\n");
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

        /* If initial fetch failed, retry quickly (every 10s) until we have data,
         * rather than making the user stare at 'Laden...' for 5 minutes! */
        if (!s_rain_data.is_valid) {
            fprintf(stderr, "[rain] no rain forecast data yet — retrying in 10s\n");
            for (int i = 0; i < 10 && !s_rain_wake; i++) {
                sleep(1);
            }
            s_rain_wake = 0;
            continue;
        }

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


/* ---- plain-language outlook ------------------------------------------------
 * The 3-hour curve as one sentence ("Droog tot 14:20", "Over 10 min lichte
 * regen", "Regen stopt rond 15:05"), computed against the clock each time it
 * is shown, so "over 10 min" counts down between fetches. */
int buienradar_rain_level(float mm) {
    if (mm < 0.1f)  return 0;
    if (mm < 2.5f)  return 1;     /* light (KNMI) */
    if (mm < 10.0f) return 2;     /* moderate */
    return 3;                     /* heavy */
}

/* minutes from now to "HH:MM" (today), wrapped into -720..719 */
static int delta_min(const char * hhmm, int now_min) {
    int h = 0, m = 0;
    if (!hhmm || sscanf(hhmm, "%d:%d", &h, &m) != 2) return 9999;
    int d = h * 60 + m - now_min;
    while (d < -720) d += 1440;
    while (d >= 720) d -= 1440;
    return d;
}

int buienradar_rain_outlook(const buienradar_rain_snapshot_t * s, char * buf, int len) {
    if (buf && len) buf[0] = 0;
    if (!s || !s->is_valid || s->count_3h <= 0) return -1;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int now_min = tm.tm_hour * 60 + tm.tm_min;

    int now_i = -1, start_i = -1, stop_i = -1, last_d = -9999, peak = 0;
    for (int i = 0; i < s->count_3h; i++) {
        int d = delta_min(s->points_3h[i].time_str, now_min);
        if (d == 9999 || d < -4) continue;          /* history */
        if (now_i < 0) now_i = i;
        last_d = d;
        int lv = buienradar_rain_level(s->points_3h[i].mm);
        if (i == now_i && lv > 0) start_i = i;
        if (start_i < 0 && lv > 0) start_i = i;
        if (start_i >= 0 && stop_i < 0 && i > start_i && lv == 0) stop_i = i;
        if (start_i >= 0 && stop_i < 0 && lv > peak) peak = lv;
    }
    if (now_i < 0) return -1;                       /* data too old */
    static const char * adj_nl[4] = { "", "lichte", "matige", "zware" };
    static const char * adj_en[4] = { "", "light", "moderate", "heavy" };
    static const char * cap_nl[4] = { "", "Lichte", "Matige", "Zware" };
    static const char * cap_en[4] = { "", "Light", "Moderate", "Heavy" };
    int en = settings.language == LANG_EN;

    if (start_i < 0) {
        if (buf) snprintf(buf, len, en ? "Dry for the next %d hours" : "Droog de komende %d uur",
                          last_d >= 150 ? 3 : 2);
        return -1;
    }
    int ds = delta_min(s->points_3h[start_i].time_str, now_min);
    if (start_i == now_i || ds <= 0) {
        if (buf) {
            if (stop_i >= 0)
                snprintf(buf, len, en ? "%s rain, stops around %s" : "%s regen, stopt rond %s",
                         en ? cap_en[peak] : cap_nl[peak], s->points_3h[stop_i].time_str);
            else
                snprintf(buf, len, en ? "%s rain for a while" : "%s regen, houdt aan",
                         en ? cap_en[peak] : cap_nl[peak]);
        }
        return 0;
    }
    if (buf) {
        if (ds <= 60) {
            int m5 = ((ds + 2) / 5) * 5;
            if (m5 < 5) m5 = 5;
            if (en) snprintf(buf, len, "%s rain in %d min", cap_en[peak], m5);
            else    snprintf(buf, len, "Over %d min %s regen", m5, adj_nl[peak]);
        } else {
            snprintf(buf, len, en ? "Dry until %s" : "Droog tot %s", s->points_3h[start_i].time_str);
        }
    }
    (void)adj_en;
    return ds;
}

#ifdef SIM
/* Headless sim: install a rain snapshot (SIM_RAIN in sim.c). */
void buienradar_rain_sim_set(const buienradar_rain_snapshot_t * in) {
    pthread_mutex_lock(&s_rain_mtx);
    s_rain_data = *in;
    pthread_mutex_unlock(&s_rain_mtx);
}
#endif
