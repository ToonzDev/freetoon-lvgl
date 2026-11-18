#include "energy.h"
#include "settings.h"
#include "meteradapter.h"
#include "homewizard.h"
#include "p1esphome.h"
#include "homeassistant.h"
#include "i18n.h"
#include "http.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern meter_state_t meter_state;
extern hw_state_t    hw_state;

int energy_connected(void) {
    if (settings.client_mode) {
        return meter_state.connected || hw_state.connected_p1 || (settings.enable_ha && ha_state.have_power);
    }
    switch (settings.energy_source) {
        case 1:  return settings.enable_p1_elec && hw_state.connected_p1;
        case 2:  return p1esp_state.connected;
        case 3:  return settings.enable_ha && ha_state.have_power;
        default: return meter_state.connected;
    }
}

float energy_power_w(void) {
    if (settings.client_mode) {
        if (hw_state.connected_p1) return hw_state.power_w;
        if (meter_state.connected) return meter_state.power_w;
        if (settings.enable_ha && ha_state.have_power) return ha_state.energy_power_w;
    }
    switch (settings.energy_source) {
        case 1:  return hw_state.power_w;
        case 2:  return p1esp_state.power_w;
        case 3:  return ha_state.energy_power_w;
        default: return meter_state.power_w;
    }
}

/* Cumulative gas (<0 means "no gas reading available"). */
float energy_gas_m3(void) {
    if (settings.client_mode) {
        if (hw_state.connected_p1) return hw_state.gas_m3;
        if (meter_state.gas_m3 > 0) return meter_state.gas_m3;
        if (settings.enable_ha && ha_state.have_gas) return ha_state.energy_gas_m3;
    }
    switch (settings.energy_source) {
        case 1:  return hw_state.connected_p1 ? hw_state.gas_m3 : -1.0f;
        case 2:  return p1esp_state.connected ? p1esp_state.gas_m3 : -1.0f;
        case 3:  return ha_state.have_gas ? ha_state.energy_gas_m3 : -1.0f;
        default: return meter_state.gas_m3 > 0 ? meter_state.gas_m3 : -1.0f;
    }
}

/* Trailing-hour gas. */
float energy_gas_hour_m3(void) {
    if (settings.client_mode) {
        if (hw_state.connected_p1) return hw_state.gas_hour_m3;
        if (meter_state.gas_now_m3h > 0) return meter_state.gas_now_m3h;
        if (meter_state.gas_today_m3 >= 0) return meter_state.gas_today_m3;
    }
    switch (settings.energy_source) {
        case 1:  return hw_state.connected_p1 ? hw_state.gas_hour_m3 : -1.0f;
        case 2:  return p1esp_state.connected ? p1esp_state.gas_hour_m3 : -1.0f;
        default:
            if (meter_state.gas_now_m3h > 0) return meter_state.gas_now_m3h;
            return meter_state.gas_today_m3 >= 0 ? meter_state.gas_today_m3 : -1.0f;
    }
}

/* --- gas used since local midnight, for sources that only give a meter
 * reading (HomeWizard / ESPHome P1, a Home Assistant gas meter sensor) ---
 * today = reading now - reading at 00:00. The 00:00 reading comes from
 * hcb_rrd's hourly gas_quantity sample (litres; FreeToon pushes it for a
 * HomeWizard/ESPHome P1, happ_pwrusage for the Toon's own meter), looked up
 * on a background thread. Until that is known (or when it isn't there, e.g.
 * the first day after installing) the first reading seen today is the
 * baseline, i.e. "since start-up". */
static pthread_mutex_t g_gb_mtx = PTHREAD_MUTEX_INITIALIZER;
static int    g_gb_day = -1;           /* year*1000 + yday of the baseline */
static double g_gb_m3 = -1;            /* cumulative m3 at 00:00 */
static int    g_gb_exact = 0;          /* 1 = from hcb_rrd at 00:00 */
static int    g_gb_busy = 0;
static long   g_gb_last_try = 0;
static double g_gb_cur = -1;           /* latest reading, for the thread's sanity check */
static long   g_gb_last_seen = 0;      /* time of the previous gas_since_midnight call */

/* Most gas a household burns in one day; a 00:00 value further below the
 * current reading than this is not ours. */
#define GAS_MAX_DAY_M3 150.0

static void * gas_baseline_thread(void * arg) {
    int day = (int)(intptr_t)arg;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
    long midnight = (long)mktime(&lt);
    char url[256], want[40];
    snprintf(url, sizeof url,
        "http://127.0.0.1:10080/hcb_rrd?action=getRrdData&loggerName=gas_quantity&rra=5yrhours"
        "&from=%ld&to=%ld&readableTime=1&nullForNaN=1", midnight - 3600, midnight + 60);
    snprintf(want, sizeof want, "%02d-%02d-%04d 00:00:00", lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900);
    static char body[2048];   /* one thread at a time (g_gb_busy) */
    double v = -1;
    if (http_fetch(url, body, sizeof body) == 0) {
        const char * p = strstr(body, want);
        if (p && (p = strchr(p + strlen(want), ':'))) {
            p++;
            while (*p == ' ') p++;
            if (*p != 'n') v = strtod(p, NULL);
        }
    }
    pthread_mutex_lock(&g_gb_mtx);
    /* gas_quantity is the stock happ_pwrusage logger: hcb_rrd keeps sampling
     * the meter adapter's (last known) counter into it next to our pushes.
     * After a switch from the Toon meter adapter to a P1 that frozen old
     * reading can be the 00:00 sample every day — "today" then shows the
     * whole gap between the two counters. Only accept a plausible value. */
    if (v > 0 && day == g_gb_day && !g_gb_exact) {   /* not after a rebase */
        double m3 = v / 1000.0;
        if (g_gb_cur > 0 && (m3 > g_gb_cur + 0.001 || g_gb_cur - m3 > GAS_MAX_DAY_M3)) {
            fprintf(stderr, "[energy] gas at 00:00 from hcb_rrd %.3f m3 does not fit current "
                            "reading %.3f m3 (old meter adapter data?): ignored\n", m3, g_gb_cur);
            g_gb_exact = 1;                    /* don't keep asking today */
        } else {
            g_gb_m3 = m3;
            g_gb_exact = 1;
            fprintf(stderr, "[energy] gas at 00:00 from hcb_rrd: %.3f m3\n", g_gb_m3);
        }
    }
    g_gb_busy = 0;
    pthread_mutex_unlock(&g_gb_mtx);
    return NULL;
}

static float gas_since_midnight(float cum_m3) {
    if (cum_m3 <= 0) return -1.0f;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int day = (lt.tm_year + 1900) * 1000 + lt.tm_yday;
    float out;
    pthread_mutex_lock(&g_gb_mtx);
    if (day != g_gb_day) {                     /* new day (or first call) */
        /* Running across midnight with a fresh reading: our own first value
         * of the day IS the 00:00 reading — no need for hcb_rrd. */
        int rolled = g_gb_day >= 0 && g_gb_last_seen > 0 && (long)now - g_gb_last_seen < 900;
        g_gb_day = day;
        g_gb_m3 = cum_m3;
        g_gb_exact = rolled;
        g_gb_last_try = 0;
    }
    g_gb_cur = cum_m3;
    g_gb_last_seen = (long)now;
    /* Baseline above the current reading: the meter was replaced (a new one
     * restarts near 0) and the 00:00 value, or the first one seen today, is
     * the old meter's. Count from the new meter's reading instead of
     * clamping "today" to 0 for the rest of the day. The usage between
     * 00:00 and the swap is lost — the two counters can't be joined. */
    if (cum_m3 < g_gb_m3 - 0.001) {
        fprintf(stderr, "[energy] gas reading %.3f below today's baseline %.3f m3 (new meter?): rebased\n",
                cum_m3, g_gb_m3);
        g_gb_m3 = cum_m3;
        g_gb_exact = 1;                        /* don't fetch the old 00:00 value again */
    }
    /* look the 00:00 reading up: at once, then every 10 min until found */
    if (!g_gb_exact && !g_gb_busy && !settings.client_mode && (long)now - g_gb_last_try >= 600) {
        g_gb_busy = 1;
        g_gb_last_try = (long)now;
        pthread_t t;
        if (pthread_create(&t, NULL, gas_baseline_thread, (void *)(intptr_t)day) == 0) pthread_detach(t);
        else g_gb_busy = 0;
    }
    out = (float)(cum_m3 - g_gb_m3);
    pthread_mutex_unlock(&g_gb_mtx);
    return out < 0 ? 0.0f : out;               /* meter swap / bad sample */
}

/* Today's total gas in m³ (<0 when unavailable). Was the trailing hour for a
 * HomeWizard/ESPHome P1 and the meter total for Home Assistant. */
float energy_gas_today_m3(void) {
    if (settings.client_mode) {
        if (meter_state.gas_today_m3 >= 0) return meter_state.gas_today_m3;
        if (hw_state.connected_p1 && hw_state.gas_m3 > 0) return gas_since_midnight(hw_state.gas_m3);
        if (settings.enable_ha && ha_state.have_gas) return gas_since_midnight(ha_state.energy_gas_m3);
    }
    switch (settings.energy_source) {
        case 1:  return hw_state.connected_p1 ? gas_since_midnight(hw_state.gas_m3) : -1.0f;
        case 2:  return p1esp_state.connected ? gas_since_midnight(p1esp_state.gas_m3) : -1.0f;
        case 3:  return ha_state.have_gas ? gas_since_midnight(ha_state.energy_gas_m3) : -1.0f;
        default: return meter_state.gas_today_m3 >= 0 ? meter_state.gas_today_m3 : -1.0f;
    }
}

/* --- electricity used since local midnight (kWh) --- */
static pthread_mutex_t g_eb_mtx = PTHREAD_MUTEX_INITIALIZER;
static int    g_eb_day = -1;
static double g_eb_kwh = -1;
static int    g_eb_exact = 0;
static int    g_eb_busy = 0;
static long   g_eb_last_try = 0;
static double g_eb_cur = -1;
static long   g_eb_last_seen = 0;
#define ELEC_MAX_DAY_KWH 300.0

static void * elec_baseline_thread(void * arg) {
    int day = (int)(intptr_t)arg;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
    long midnight = (long)mktime(&lt);
    char url[256], want[40];
    snprintf(url, sizeof url,
        "http://127.0.0.1:10080/hcb_rrd?action=getRrdData&loggerName=elec_quantity_nt&rra=5yrhours"
        "&from=%ld&to=%ld&readableTime=1&nullForNaN=1", midnight - 3600, midnight + 60);
    snprintf(want, sizeof want, "%02d-%02d-%04d 00:00:00", lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900);
    static char body[2048];
    double v_nt = -1, v_lt = -1;
    if (http_fetch(url, body, sizeof body) == 0) {
        const char * p = strstr(body, want);
        if (p && (p = strchr(p + strlen(want), ':'))) {
            p++; while (*p == ' ') p++;
            if (*p != 'n') v_nt = strtod(p, NULL);
        }
    }
    snprintf(url, sizeof url,
        "http://127.0.0.1:10080/hcb_rrd?action=getRrdData&loggerName=elec_quantity_lt&rra=5yrhours"
        "&from=%ld&to=%ld&readableTime=1&nullForNaN=1", midnight - 3600, midnight + 60);
    if (http_fetch(url, body, sizeof body) == 0) {
        const char * p = strstr(body, want);
        if (p && (p = strchr(p + strlen(want), ':'))) {
            p++; while (*p == ' ') p++;
            if (*p != 'n') v_lt = strtod(p, NULL);
        }
    }
    pthread_mutex_lock(&g_eb_mtx);
    if ((v_nt > 0 || v_lt > 0) && day == g_eb_day && !g_eb_exact) {
        double kwh = ((v_nt > 0 ? v_nt : 0) + (v_lt > 0 ? v_lt : 0)) / 1000.0;
        /* Same stale-meter-adapter guard as the gas baseline. */
        if (g_eb_cur > 0 && (kwh > g_eb_cur + 0.001 || g_eb_cur - kwh > ELEC_MAX_DAY_KWH)) {
            fprintf(stderr, "[energy] elec at 00:00 from hcb_rrd %.3f kWh does not fit current "
                            "reading %.3f kWh (old meter adapter data?): ignored\n", kwh, g_eb_cur);
        } else {
            g_eb_kwh = kwh;
            fprintf(stderr, "[energy] elec at 00:00 from hcb_rrd: %.3f kWh\n", g_eb_kwh);
        }
        g_eb_exact = 1;
    }
    g_eb_busy = 0;
    pthread_mutex_unlock(&g_eb_mtx);
    return NULL;
}

static float elec_since_midnight(double cum_kwh) {
    if (cum_kwh <= 0) return -1.0f;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int day = (lt.tm_year + 1900) * 1000 + lt.tm_yday;
    float out;
    pthread_mutex_lock(&g_eb_mtx);
    if (day != g_eb_day) {
        int rolled = g_eb_day >= 0 && g_eb_last_seen > 0 && (long)now - g_eb_last_seen < 900;
        g_eb_day = day;
        g_eb_kwh = cum_kwh;
        g_eb_exact = rolled;               /* see gas_since_midnight */
        g_eb_last_try = 0;
    }
    g_eb_cur = cum_kwh;
    g_eb_last_seen = (long)now;
    /* New meter (reading below the baseline): rebase, see gas_since_midnight. */
    if (cum_kwh < g_eb_kwh - 0.001) {
        fprintf(stderr, "[energy] elec reading %.3f below today's baseline %.3f kWh (new meter?): rebased\n",
                cum_kwh, g_eb_kwh);
        g_eb_kwh = cum_kwh;
        g_eb_exact = 1;
    }
    if (!g_eb_exact && !g_eb_busy && !settings.client_mode && (long)now - g_eb_last_try >= 600) {
        g_eb_busy = 1;
        g_eb_last_try = (long)now;
        pthread_t t;
        if (pthread_create(&t, NULL, elec_baseline_thread, (void *)(intptr_t)day) == 0) pthread_detach(t);
        else g_eb_busy = 0;
    }
    out = (float)(cum_kwh - g_eb_kwh);
    pthread_mutex_unlock(&g_eb_mtx);
    return out < 0 ? 0.0f : out;
}

/* Today's total electricity import in kWh (<0 when unavailable). */
float energy_elec_today_kwh(void) {
    if (settings.client_mode) {
        if (meter_state.elec_today_kwh >= 0) return meter_state.elec_today_kwh;
        if (hw_state.connected_p1) {
            double c = hw_state.kwh_import_total > 0 ? hw_state.kwh_import_total
                                                     : (hw_state.kwh_import_t1 + hw_state.kwh_import_t2);
            if (c > 0) return elec_since_midnight(c);
        }
    }
    switch (settings.energy_source) {
        case 1: { /* HomeWizard P1 */
            if (!hw_state.connected_p1) return -1.0f;
            double c = hw_state.kwh_import_total > 0 ? hw_state.kwh_import_total
                                                     : (hw_state.kwh_import_t1 + hw_state.kwh_import_t2);
            return elec_since_midnight(c);
        }
        case 2: { /* ESPHome P1 */
            if (!p1esp_state.connected) return -1.0f;
            double c = p1esp_state.kwh_import_t1 + p1esp_state.kwh_import_t2;
            return elec_since_midnight(c);
        }
        case 3: { /* Home Assistant */
            if (meter_state.elec_today_kwh >= 0) return meter_state.elec_today_kwh;
            return -1.0f;
        }
        default: { /* meteradapter / Toon smart meter */
            if (meter_state.elec_today_kwh >= 0) return meter_state.elec_today_kwh;
            if (meter_state.reading_elec_normal_wh > 0 || meter_state.reading_elec_low_wh > 0) {
                double c = ((meter_state.reading_elec_normal_wh > 0 ? meter_state.reading_elec_normal_wh : 0) +
                            (meter_state.reading_elec_low_wh > 0 ? meter_state.reading_elec_low_wh : 0)) / 1000.0;
                return elec_since_midnight(c);
            }
            return -1.0f;
        }
    }
}

const char * energy_offline_label(void) {
    if (settings.client_mode)
        return tr("Master offline", "Master offline");
    switch (settings.energy_source) {
        case 1:
            return hw_state.polled_p1 ? tr("P1 offline", "P1 offline")
                                      : tr("Initialiseren...", "Initializing...");
        case 2:
            return p1esp_state.polled ? tr("P1 offline", "P1 offline")
                                      : tr("Initialiseren...", "Initializing...");
        case 3:
            if (!settings.ha_power_entity[0])
                return tr("Geen sensor gekozen", "No sensor selected");
            return tr("HA offline", "HA offline");
        default:
            /* meteradapter: last_flow_s == 0 means no notify has arrived yet */
            return meter_state.last_flow_s ? tr("meter offline", "meter offline")
                                           : tr("Initialiseren...", "Initializing...");
    }
}

/* Solar: a HomeWizard kWh meter on the inverter wins when its IP is set;
 * then the HA production sensor; then a solar meter (S0 pulse sensor) on
 * the Toon's own meter adapter. */
int   energy_have_solar(void) {
    if (settings.hw_kwh_host[0] || settings.hw_kwh2_host[0]) return hw_state.connected_kwh;
    if (settings.enable_ha && ha_state.have_solar) return 1;
    return meteradapter_have_solar();
}
float energy_solar_w(void) {
    if (settings.hw_kwh_host[0] || settings.hw_kwh2_host[0]) return hw_state.kwh_power_w;
    if (settings.enable_ha && ha_state.have_solar) return ha_state.solar_w;
    return meteradapter_have_solar() ? meter_state.solar_w : 0.0f;
}

/* ---- cost today / yesterday ---------------------------------------------
 * Fixed tariffs only (dynamic tariffs: BACKLOG #44). Yesterday's total is the
 * last value computed before midnight; it survives a restart through a tiny
 * file in the data dir, rewritten at most every 30 min:
 *   y <yyyymmdd> <euro>   yesterday's final total
 *   t <yyyymmdd> <euro>   today's running total (becomes "y" after midnight) */
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
static float  s_cost_yday = -1.0f, s_cost_last = -1.0f;
static int    s_cost_ymd = 0;           /* day s_cost_last belongs to */
static time_t s_cost_saved = 0;
static int    s_cost_loaded = 0;

static int ymd_of(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}
static const char * cost_path(void) {
    static char buf[256];
    const char * d = getenv("TOONUI_DATA_DIR");
    snprintf(buf, sizeof buf, "%s/toonui_cost.txt", (d && *d) ? d : "/mnt/data");
    return buf;
}
static void cost_save(void) {
    FILE * f = fopen(cost_path(), "w");
    if (!f) return;
    int yday = ymd_of(time(NULL) - 86400);
    if (s_cost_yday >= 0) fprintf(f, "y %d %.3f\n", yday, s_cost_yday);
    if (s_cost_last >= 0) fprintf(f, "t %d %.3f\n", s_cost_ymd, s_cost_last);
    fclose(f);
}
static void cost_load(void) {
    s_cost_loaded = 1;
    FILE * f = fopen(cost_path(), "r");
    if (!f) return;
    int today = ymd_of(time(NULL)), yday = ymd_of(time(NULL) - 86400);
    char k; int d; float v;
    while (fscanf(f, " %c %d %f", &k, &d, &v) == 3) {
        if (k == 'y' && d == yday) s_cost_yday = v;
        if (k == 't' && d == yday && s_cost_yday < 0) s_cost_yday = v;   /* last save before midnight */
        if (k == 't' && d == today) { s_cost_last = v; s_cost_ymd = d; }
    }
    fclose(f);
}

float energy_cost_today(void) {
    if (!s_cost_loaded) cost_load();
    time_t now = time(NULL);
    int today = ymd_of(now);
    if (s_cost_ymd && s_cost_ymd != today) {
        /* midnight passed: the last total of the previous day is yesterday */
        s_cost_yday = (s_cost_ymd == ymd_of(now - 86400)) ? s_cost_last : -1.0f;
        s_cost_last = -1.0f;
        s_cost_ymd = today;
        cost_save();
        s_cost_saved = now;
    }
    float e = energy_elec_today_kwh(), g = energy_gas_today_m3();
    if (e < 0 && g < 0) return -1.0f;
    float c = (e > 0 ? e * settings.rate_elec_kwh : 0.0f) + (g > 0 ? g * settings.rate_gas_m3 : 0.0f);
    s_cost_last = c;
    s_cost_ymd = today;
    if (now - s_cost_saved > 1800) { cost_save(); s_cost_saved = now; }
    return c;
}

float energy_cost_yesterday(void) {
    if (!s_cost_loaded) cost_load();
    return s_cost_yday;
}
