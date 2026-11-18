#ifndef TOON_RRD_REPAIR_H
#define TOON_RRD_REPAIR_H
/*
 * Repair of the cumulative meter history in hcb_rrd after a switch from the
 * Toon meter adapter to a HomeWizard P1 (gas_quantity, elec_quantity_nt/_lt).
 *
 * Two faults are repaired:
 *  1. Counter step: the adapter's counter and the P1's differ, so the daily
 *     archive jumps on the day of the switch (a one-day "190 m3" bar). The
 *     excess is a counter offset: every reading before the step is raised by
 *     it, so all differences (= the bars) stay as they were and the step day
 *     gets an ordinary value. Only one step in the window is fixed; with more
 *     than one nothing is shifted (it can't be told which is which).
 *  2. Hourly readings that don't fit the day: hcb_rrd kept sampling the
 *     adapter's frozen counter into the hourly archive, overwriting the P1
 *     pushes (hours view: 0.0 m3). Where an hour lies outside its day's start
 *     and end reading, or goes backwards, the day's hours are rebuilt by
 *     spreading the day's usage evenly.
 * Values can only be overwritten (a NaN write stores 0), never cleared.
 */

/* ---- pure core (host-testable) ---- */
typedef struct { long ts; double v; } rrd_pt_t;   /* v = NAN: empty slot */

typedef struct {
    double min_step;   /* smallest one-day jump that can be a counter step (raw units) */
    double tol;        /* slack between hourly and daily readings (raw units) */
} rrd_lim_t;

typedef struct {
    int    step;        /* 1 = one counter step found, history shifted */
    int    multi_step;  /* >1 step candidates in the window: not shifted */
    long   step_ts;     /* daily point right after the step */
    double offset;      /* added to all readings at or before the step */
    int    n_shift_h, n_shift_d, n_rebuild_h;
} rrd_fix_t;

/* h/d: hourly and daily archive, ascending ts. new_h/new_d receive the
 * repaired values (copies of v where unchanged). Only daily points in
 * [now - window_s, now] are examined; the shift reaches all older history. */
void rrd_repair_compute(const rrd_pt_t * h, int nh, const rrd_pt_t * d, int nd,
                        long now, long window_s, const rrd_lim_t * lim,
                        double * new_h, double * new_d, rrd_fix_t * res);

/* ---- on the Toon ---- */
/* Background scan (dry run) / apply; poll rrd_repair_status() from the UI.
 * The text is a ready-to-show summary (nl/en via tr()). */
enum { RRD_REPAIR_IDLE, RRD_REPAIR_SCANNING, RRD_REPAIR_SCANNED,
       RRD_REPAIR_APPLYING, RRD_REPAIR_DONE, RRD_REPAIR_FAILED };
int  rrd_repair_scan_start(void);
int  rrd_repair_apply_start(void);       /* only after a scan with fixes */
int  rrd_repair_status(char * text, int textsz, int * have_fixes);

#endif
