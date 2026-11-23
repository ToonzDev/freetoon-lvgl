#ifndef TOON_RRD_PUSH_H
#define TOON_RRD_PUSH_H

/* Pushes a single sample into hcb_rrd via setRrdData.
   logger_name = "elec_flow" / "gas_quantity" / etc.
   rra         = "5min" / "5yrhours" / "10yrdays" / ...
   ts          = unix timestamp the sample belongs to
   value       = numeric value (printed as double; hcb_rrd will cast)
   Returns 0 on success. Uses curl in a child process. */
int rrd_push(const char * logger_name, const char * rra,
             long ts, double value);

/* 1 if hcb_rrd's config has a logger with this internalAddress (= name). */
int rrd_logger_exists(const char * name);

/* Create a counter logger (Wh, integer; archives 1monthhour + 1yearday, as
 * hcb_rrd keeps for Z-Wave plugs) through hcb_config SetObjectConfig. It has
 * no device behind it: fed with rrd_push only. 0 = sent. */
int rrd_create_counter_logger(const char * name);

/* With a HomeWizard/ESPHome P1 as energy source, stop the stock meter
 * loggers (gas_quantity, gas_flow, elec_quantity_nt/_lt, elec_flow) from
 * sampling the meter adapter (deviceUuid -> placeholder, original saved);
 * back on the meter adapter, restore the saved device. Master only. */
void rrd_meter_loggers_sync(void);
/* hcb_rrd's uuid of logger `name` (= its data file names). 0 = found. */
#include <stddef.h>
int  rrd_logger_uuid(const char * name, char * out, size_t outsz);
int  rrd_meter_loggers_start(void);   /* background: after 90 s, then every 10 min */

/* Device a logger samples: "" (none), "placeholder" or a device uuid.
 * 0 = logger found, -1 = no such logger, -2 = config not readable. */
int  rrd_logger_device(const char * name, char * dev, size_t devsz);
/* Point logger `name` at device `dev` (uuid characters or "placeholder"),
 * keeping its uuid and history (SetObjectConfig, verified 2026-11-18). */
int  rrd_logger_set_device(const char * name, const char * dev);
/* Newest non-NaN sample of `rra` between from and to (unix s). 0 = found. */
int  rrd_last_value(const char * name, const char * rra, long from, long to, double * out);

#endif
