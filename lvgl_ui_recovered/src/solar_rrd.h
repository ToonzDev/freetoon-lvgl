#ifndef TOON_SOLAR_RRD_H
#define TOON_SOLAR_RRD_H

/* Zon statistics from FreeToon's own solar sources (BACKLOG #60): the sum of
 * every source (HomeWizard kWh meters, inverter app, Home Assistant sensor,
 * and the meter adapter's solar meter while FreeToon owns the loggers) goes
 * into the stock loggers elec_solar_quantity (Wh: 5yrhours, 10yrdays) and
 * elec_solar_flow (W: 5min), which the Statistics Zon tab reads.
 *
 * With only the meter adapter's solar meter, hcb_rrd keeps sampling it into
 * those loggers itself and nothing here writes. Master only. */
int solar_rrd_start(void);

#endif
