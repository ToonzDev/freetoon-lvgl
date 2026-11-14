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

#endif
