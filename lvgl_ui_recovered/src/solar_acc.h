/*
 * Solar production accumulator (BACKLOG #60): one running total in Wh, fed
 * by every solar source FreeToon reads. The Zon statistics get this total
 * (solar_rrd.c), so all sources add up, as decided 2026-11-23.
 *
 * Each source adds only its own increase since its last reading, so a source
 * that appears, disappears, goes offline or resets its counter never makes
 * the total jump or go backwards:
 *   SACC_LIFETIME  a lifetime counter in Wh (HomeWizard kWh meter, inverter
 *                  total): the increase is added; a drop (meter swapped) or an
 *                  impossible jump only re-bases.
 *   SACC_DAILY     a counter that restarts every day (inverter "today"): a
 *                  clear drop is the new day, whose value so far is added.
 *   SACC_POWER     watts (Home Assistant sensor, meter adapter): integrated
 *                  over time; a gap of more than 3 minutes is not bridged.
 * The first reading of a source only sets its baseline.
 *
 * Pure code (no I/O but the save/load helpers), tested on the host by
 * tests/solar_acc_test.c.
 */
#ifndef TOON_SOLAR_ACC_H
#define TOON_SOLAR_ACC_H

#define SACC_MAX     8
#define SACC_ID_LEN  80

enum { SACC_LIFETIME = 1, SACC_DAILY = 2, SACC_POWER = 3 };

typedef struct {
    char   id[SACC_ID_LEN];   /* e.g. "hw:192.168.1.40", "app:inverters:enphase" */
    int    kind;              /* SACC_* */
    double last;              /* last reading (Wh, or W for SACC_POWER) */
    long   t;                 /* time of the last reading */
} sacc_src_t;

typedef struct {
    double     total_wh;
    sacc_src_t src[SACC_MAX];
    int        n;
} sacc_t;

/* Feed one reading; returns the Wh added to the total (0 for a baseline). */
double sacc_feed(sacc_t * a, const char * id, int kind, double v, long now);
/* Never let the total be below `floor_wh` (the last value already in the
 * statistics): history stays monotonic when FreeToon takes over a logger. */
void   sacc_floor(sacc_t * a, double floor_wh);
/* Text file: "total <wh>" then "src <kind> <last> <t> <id>" lines.
 * Written atomically (tmp + rename). 0 = ok. */
int    sacc_save(const sacc_t * a, const char * path);
int    sacc_load(sacc_t * a, const char * path);

#endif
