/*
 * Z-Wave smart plugs ("Stekkers") for Statistics.
 *
 * When a Z-Wave smart plug is added (stock UI), hcb_rrd creates a logger for
 * its kWh counter by itself: an <rrdLogger> in config_hcb_rrd.xml with
 * deviceSvc ElectricityQuantityMeter / deviceVar CurrentElectricityQuantity
 * (Wh, cumulative) and the archives "1monthhour" (hourly, one month) and
 * "1yearday" (daily, one year). The stock UI never shows that data; FreeToon
 * reads it with the normal getRrdData by logger name (see screen_stats.c).
 * Names and live watts come from hdrv_zwave's getDevices.json.
 */
#ifndef PLUGS_H
#define PLUGS_H

#define PLUG_MAX 8

typedef struct {
    char  logger[64];     /* hcb_rrd logger name */
    char  logger_q[192];  /* same, URL-encoded for getRrdData loggerName= */
    char  dev_uuid[48];   /* Z-Wave device uuid */
    char  name[40];       /* Z-Wave device name, e.g. "Tuin" */
    float power_w;        /* CurrentElectricityFlow */
    int   have_power;
    int   hw_slot;        /* HomeWizard socket index, -1 = Z-Wave plug */
} plug_t;

/* Find the plug loggers in the local hcb_rrd config and fill in name + live
 * power. Returns the number of plugs (0 on a slave / no plugs). */
int plugs_scan(plug_t * out, int max);

/* Re-read name + live power for the given plugs from hdrv_zwave. */
void plugs_refresh_power(plug_t * p, int n);

#endif
