/* Zon statistics from FreeToon's own solar sources: see solar_rrd.h.
 *
 * Every 10 s each source's reading goes into one accumulator (solar_acc.c),
 * which adds only each source's own increase. At the end of every hour its
 * total (Wh) is written to elec_solar_quantity / 5yrhours, at the end of
 * every (UTC) day also to 10yrdays, and every 5 minutes the average summed
 * power to elec_solar_flow / 5min. Timestamps as for the P1 meters and the
 * HomeWizard sockets (#48): a bin is labelled with its END, and only closed
 * bins can be written to a logger without a device, so the hour is written
 * at the first tick after it closed (and once more 10 minutes later, in case
 * hcb_rrd closed the slot late).
 *
 * Loggers: created when missing (same layout as the stock ones); when they
 * sample a device (the meter adapter's solar meter) and FreeToon has its own
 * sources, they are pointed at "placeholder" (device saved, history kept)
 * and the meter adapter is read as one more source, so it still counts. With
 * no own source left the saved device is restored and nothing is written. */
#include "solar_rrd.h"
#include "solar_acc.h"
#include "solar_app.h"
#include "rrd_push.h"
#include "settings.h"
#include "homewizard.h"
#include "homeassistant.h"
#include "meteradapter.h"
#include "boxtalk.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ACC_PATH    "/mnt/data/freetoon_solar_acc.dat"
#define SAVED_PATH  "/mnt/data/freetoon_rrd_solardev.cfg"
#define TICK_S      10

static const char * const LOGGERS[2] = { "elec_solar_quantity", "elec_solar_flow" };

static const char * const XML_QUANTITY =
    "<rrdLogger><package>hcb_rrd</package><name>elec_solar_quantity</name>"
    "<internalAddress>elec_solar_quantity</internalAddress><visibility>0</visibility>"
    "<deviceUuid>placeholder</deviceUuid><deviceVar>CurrentElectricityQuantity</deviceVar>"
    "<deviceSvc>ElectricityQuantityMeter</deviceSvc><sampleType>integer</sampleType>"
    "<rra><binLength>3600</binLength><bins>43830</bins><futureBins>0</futureBins>"
    "<consolidator>last</consolidator><name>5yrhours</name><minSamplesPerBin>1</minSamplesPerBin>"
    "<saveInterval>1</saveInterval></rra>"
    "<rra><binLength>86400</binLength><bins>3653</bins><futureBins>0</futureBins>"
    "<consolidator>last</consolidator><name>10yrdays</name><minSamplesPerBin>1</minSamplesPerBin>"
    "<saveInterval>1</saveInterval></rra></rrdLogger>";

static const char * const XML_FLOW =
    "<rrdLogger><package>hcb_rrd</package><name>elec_solar_flow</name>"
    "<internalAddress>elec_solar_flow</internalAddress><visibility>0</visibility>"
    "<deviceUuid>placeholder</deviceUuid><deviceVar>CurrentElectricityFlow</deviceVar>"
    "<deviceSvc>ElectricityFlowMeter</deviceSvc><sampleType>double</sampleType>"
    "<rra><binLength>300</binLength><bins>8928</bins><futureBins>0</futureBins>"
    "<consolidator>avg</consolidator><name>5min</name><minSamplesPerBin>1</minSamplesPerBin>"
    "<saveInterval>1</saveInterval></rra></rrdLogger>";

static sacc_t g_acc;

/* ---- saved devices of the stock loggers ("name uuid" per line) ---- */
static void saved_get(const char * name, char * dev, size_t n) {
    dev[0] = 0;
    FILE * f = fopen(SAVED_PATH, "r");
    if (!f) return;
    char a[64], b[64];
    while (fscanf(f, "%63s %63s", a, b) == 2)
        if (!strcmp(a, name)) snprintf(dev, n, "%s", b);
    fclose(f);
}
static void saved_set(const char * name, const char * dev) {
    char other[2][64] = { "", "" };
    for (int i = 0; i < 2; i++)
        if (strcmp(LOGGERS[i], name)) saved_get(LOGGERS[i], other[i], sizeof other[i]);
        else snprintf(other[i], sizeof other[i], "%s", dev);
    if (!other[0][0] && !other[1][0]) { unlink(SAVED_PATH); return; }
    FILE * f = fopen(SAVED_PATH ".tmp", "w");
    if (!f) return;
    for (int i = 0; i < 2; i++) if (other[i][0]) fprintf(f, "%s %s\n", LOGGERS[i], other[i]);
    if (fclose(f) == 0) rename(SAVED_PATH ".tmp", SAVED_PATH);
}

/* Loggers ready for FreeToon's own writes. Returns 1 when one was created
 * or taken over just now (hcb_rrd needs a moment before setRrdData). */
static int loggers_take(void) {
    int changed = 0;
    for (int i = 0; i < 2; i++) {
        char dev[64];
        int st = rrd_logger_device(LOGGERS[i], dev, sizeof dev);
        if (st == -2) continue;                /* config not readable: try again later */
        if (st != 0) {
            int rc = boxtalk_set_object_config(i == 0 ? XML_QUANTITY : XML_FLOW);
            fprintf(stderr, "[solar-rrd] created logger %s (rc=%d)\n", LOGGERS[i], rc);
            changed = 1;
        } else if (dev[0] && strcmp(dev, "placeholder")) {
            saved_set(LOGGERS[i], dev);
            int rc = rrd_logger_set_device(LOGGERS[i], "placeholder");
            fprintf(stderr, "[solar-rrd] %s: FreeToon sums the solar sources now, stop sampling "
                            "device %s (rc=%d)\n", LOGGERS[i], dev, rc);
            changed = 1;
        }
    }
    return changed;
}

/* No own source any more: give the loggers back to the device they had. */
static void loggers_give_back(void) {
    for (int i = 0; i < 2; i++) {
        char saved[64], dev[64];
        saved_get(LOGGERS[i], saved, sizeof saved);
        if (!saved[0]) continue;
        if (rrd_logger_device(LOGGERS[i], dev, sizeof dev) == 0 && !strcmp(dev, "placeholder")) {
            int rc = rrd_logger_set_device(LOGGERS[i], saved);
            fprintf(stderr, "[solar-rrd] %s: no FreeToon solar source, sample device %s again (rc=%d)\n",
                    LOGGERS[i], saved, rc);
        }
        saved_set(LOGGERS[i], "");
    }
}

/* FreeToon's own sources (configured, not only answering right now). */
static int have_own_source(void) {
    return settings.hw_kwh_host[0] || settings.hw_kwh2_host[0] || solar_app_active() ||
           (settings.enable_ha && settings.ha_solar_entity[0]);
}

/* Feed every source's reading; returns the summed power now (W). */
static double feed_sources(long now) {
    double w = 0;
    char id[SACC_ID_LEN];
    const char * hosts[2] = { settings.hw_kwh_host, settings.hw_kwh2_host };
    double totals[2] = { hw_state.kwh1_total_wh, hw_state.kwh2_total_wh };
    for (int i = 0; i < 2; i++) {
        if (!hosts[i][0]) continue;
        snprintf(id, sizeof id, "hw:%s", hosts[i]);
        if (totals[i] > 0) sacc_feed(&g_acc, id, SACC_LIFETIME, totals[i], now);
    }
    if (hw_state.connected_kwh) w += hw_state.kwh_power_w;

    if (solar_app_active()) {
        solar_app_state_t st;
        solar_app_get(&st);
        if (st.total_wh > 0) {
            snprintf(id, sizeof id, "app:%s:%s", st.app, st.brand);
            sacc_feed(&g_acc, id, SACC_LIFETIME, st.total_wh, now);
        } else if (st.today_wh >= 0) {
            snprintf(id, sizeof id, "app:%s:%s:today", st.app, st.brand);
            sacc_feed(&g_acc, id, SACC_DAILY, st.today_wh, now);
        }
        w += solar_app_power_w();
    }
    if (settings.enable_ha && settings.ha_solar_entity[0] && ha_state.have_solar) {
        snprintf(id, sizeof id, "ha:%s", settings.ha_solar_entity);
        sacc_feed(&g_acc, id, SACC_POWER, ha_state.solar_w, now);
        w += ha_state.solar_w > 0 ? ha_state.solar_w : 0;
    }
    if (meteradapter_have_solar()) {
        sacc_feed(&g_acc, "meteradapter", SACC_POWER, meter_state.solar_w, now);
        w += meter_state.solar_w > 0 ? meter_state.solar_w : 0;
    }
    return w;
}

static long bin(long t, long len) { return t - (t % len); }

static void * solar_rrd_thread(void * arg) {
    (void)arg;
    sleep(120);                                /* BoxTalk / hcb_config / sources up */
    sacc_load(&g_acc, ACC_PATH);
    int owned = 0, floored = 0;
    long hold_until = 0, last_take = 0, last_save = 0;
    long last_h = 0, last_d = 0, last_5 = 0, fix_t = 0, fix_h = 0, fix_d = 0;
    double fix_v = 0, flow_sum = 0;
    int flow_n = 0;

    for (;;) {
        sleep(TICK_S);
        if (settings.client_mode) continue;
        long now = (long)time(NULL);
        if (now < 1600000000L) continue;       /* clock not set yet */

        if (!have_own_source()) {
            if (owned) { loggers_give_back(); owned = 0; floored = 0; }
            continue;
        }
        if (!owned || now - last_take >= 600) {  /* also undoes happ_pwrusage re-pointing them */
            if (loggers_take()) hold_until = now + 20;
            last_take = now;
            if (!owned) {
                owned = 1;
                last_h = bin(now, 3600); last_d = bin(now, 86400); last_5 = bin(now, 300);
            }
        }

        double w = feed_sources(now);
        flow_sum += w; flow_n++;
        if (now < hold_until) continue;        /* let hcb_rrd load a new logger */

        if (!floored) {                        /* continue from what the statistics hold */
            double h = 0, d = 0;
            int okh = rrd_last_value(LOGGERS[0], "5yrhours", now - 3 * 86400, now, &h) == 0;
            int okd = rrd_last_value(LOGGERS[0], "10yrdays", now - 400 * 86400L, now, &d) == 0;
            double fl = (okh && h > d) ? h : (okd ? d : (okh ? h : 0));
            if (fl > g_acc.total_wh)
                fprintf(stderr, "[solar-rrd] continue from %.0f Wh in elec_solar_quantity (was %.0f)\n",
                        fl, g_acc.total_wh);
            sacc_floor(&g_acc, fl);
            floored = 1;
        }

        long b5 = bin(now, 300), bh = bin(now, 3600), bd = bin(now, 86400);
        if (b5 != last_5) {                    /* the 5 minutes that just closed */
            if (flow_n > 0) rrd_push(LOGGERS[1], "5min", b5, flow_sum / flow_n);
            flow_sum = 0; flow_n = 0; last_5 = b5;
        }
        if (bh != last_h) {                    /* the hour (and day) that just closed */
            double v = (double)(long long)(g_acc.total_wh + 0.5);
            rrd_push(LOGGERS[0], "5yrhours", bh, v);
            fix_h = bh; fix_d = 0; fix_v = v; fix_t = now + 600;
            if (bd != last_d) { rrd_push(LOGGERS[0], "10yrdays", bd, v); fix_d = bd; last_d = bd; }
            fprintf(stderr, "[solar-rrd] hour %ld: total %.0f Wh\n", bh, v);
            last_h = bh;
            sacc_save(&g_acc, ACC_PATH);
            last_save = now;
        } else if (fix_t && now >= fix_t) {    /* once more, in case hcb_rrd closed the slot late */
            rrd_push(LOGGERS[0], "5yrhours", fix_h, fix_v);
            if (fix_d) rrd_push(LOGGERS[0], "10yrdays", fix_d, fix_v);
            fix_t = 0;
        }
        if (now - last_save >= 900) {          /* counters catch up after a reboot; power sources lose < 15 min */
            sacc_save(&g_acc, ACC_PATH);
            last_save = now;
        }
    }
    return NULL;
}

int solar_rrd_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, solar_rrd_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
