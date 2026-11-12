#ifndef TOON_HOMEWIZARD_H
#define TOON_HOMEWIZARD_H

/* Live state from the two HomeWizard devices on VLAN99.
   Updated by homewizard_thread (poll every 5s). */
typedef struct {
    volatile int    connected_p1;
    /* 1 once a poll has been attempted (success or fail) — lets the UI show
     * "Initializing" before the first poll vs "offline" after one fails. */
    volatile int    polled_p1;
    volatile int    polled_water;
    volatile float  power_w;          /* active_power_w (signed; negative = export) */
    volatile float  kwh_import_t1;    /* total_power_import_t1_kwh */
    volatile float  kwh_import_t2;    /* total_power_import_t2_kwh */
    volatile float  kwh_import_total; /* total_power_import_kwh */
    volatile float  kwh_export_total; /* total_power_export_kwh */
    volatile int    tariff;           /* active_tariff: 1 or 2 */
    volatile float  gas_m3;           /* total_gas_m3 (cumulative) */
    volatile float  gas_hour_m3;      /* consumption over the trailing ~60 min,
                                         derived from the cumulative counter */
    volatile float  voltage_l1_v;
    volatile float  current_l1_a;

    volatile int    connected_water;
    volatile float  water_total_m3;   /* total_liter_m3 (cumulative m³) */
    volatile float  water_lpm;        /* active_liter_lpm */
    /* Per-pour session — set by the poller when L/min rises from 0 to >0,
     * finalised when it returns to 0 and stays there for ~20 s. Lets the
     * UI show "+12.3 L" so the user gets immediate feedback after using
     * water, without waiting for the next billing-cycle aggregate. */
    volatile float  water_session_l;       /* litres in the current/last pour */
    volatile int    water_session_active;  /* 1 while pouring, 0 between */
    volatile int    water_session_age_s;   /* seconds since session ended (for fade) */

    /* kWh meter on the PV inverter (settings.hw_kwh_host) */
    volatile int    connected_kwh;
    volatile int    polled_kwh;
    volatile float  kwh_power_w;      /* production, always >= 0 (abs of active_power_w) */
} hw_state_t;

extern hw_state_t hw_state;

/* HomeWizard Energy Sockets (settings.hw_socket_host[]). */
typedef struct {
    volatile int   polled;      /* a poll was attempted for the current host */
    volatile int   connected;
    volatile int   on;          /* power_on */
    volatile float power_w;     /* active_power_w */
} hw_socket_t;
extern hw_socket_t hw_sockets[];   /* HW_SOCKET_MAX entries (settings.h) */

/* Switch socket i on/off in the background (PUT /api/v1/state). The new
 * state is shown at once; the next poll confirms it. */
void homewizard_socket_set_async(int i, int on);

/* Number of sockets with a host configured. */
int  homewizard_socket_count(void);

/* Start the poller if anything HomeWizard is configured and it isn't running
 * yet. Safe to call repeatedly (Settings calls it after a change, so a newly
 * enabled meter works without a UI restart). */
void homewizard_ensure_started(void);

/* Thread safety primitives */
void homewizard_lock(void);
void homewizard_unlock(void);
void homewizard_get_snapshot(hw_state_t * out);

int homewizard_start(void);

#endif
