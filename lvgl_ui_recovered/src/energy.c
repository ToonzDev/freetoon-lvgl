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
    if (v > 0 && day == g_gb_day) {
        g_gb_m3 = v / 1000.0;
        g_gb_exact = 1;
        fprintf(stderr, "[energy] gas at 00:00 from hcb_rrd: %.3f m3\n", g_gb_m3);
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
        g_gb_day = day;
        g_gb_m3 = cum_m3;
        g_gb_exact = 0;
        g_gb_last_try = 0;
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
 * otherwise the HA production sensor. */
int   energy_have_solar(void) {
    if (settings.hw_kwh_host[0]) return hw_state.connected_kwh;
    return settings.enable_ha && ha_state.have_solar;
}
float energy_solar_w(void) {
    if (settings.hw_kwh_host[0]) return hw_state.kwh_power_w;
    return ha_state.solar_w;
}
