#ifndef TOON_HOMEY_H
#define TOON_HOMEY_H

/* Athom Homey Pro (2023) client over the local Web API (BACKLOG #28).
 *
 * Config: settings.enable_homey, settings.homey_host ("ip[:port]" or
 * http(s)://...), API key in /mnt/data/homey.key (0600; created in the Homey
 * web app under Settings -> API keys, scopes: devices read + control, flows
 * start).
 *
 * A background thread polls the device list every HOMEY_POLL_S seconds and
 * zones + flows every HOMEY_META_S seconds into homey_state. Readers take
 * homey_lock()/homey_unlock() around access; the poller swaps in a fully
 * parsed table under the lock, so readers never see a half-parsed list.
 *
 * Control: the homey_* setters below never block. They check the request
 * against the known device list (capability present, setable, device
 * available), update homey_state at once (optimistic) and queue the HTTP
 * call for a worker thread; a device poll right after confirms or corrects
 * the value. A newer value for the same device + capability replaces a
 * queued one (dim slider drags send the last position). */

#include "homey_parse.h"

#define HOMEY_POLL_S  10
#define HOMEY_META_S  300

enum {
    HOMEY_ST_OFF = 0,     /* disabled or not configured */
    HOMEY_ST_OK,
    HOMEY_ST_AUTH,        /* HTTP 401/403: API key wrong or missing scope */
    HOMEY_ST_NOCONN,      /* host unreachable */
    HOMEY_ST_BADDATA,     /* reached, but the body did not parse */
};

typedef struct {
    volatile int status;          /* HOMEY_ST_* */
    volatile long last_ok;        /* time() of the last good device poll */
    int       ndev;
    hy_dev_t  dev[HY_MAX_DEV];
    int       nzone;
    hy_zone_t zone[HY_MAX_ZONE];
    int       nflow;
    hy_flow_t flow[HY_MAX_FLOW];
    volatile unsigned gen;        /* bumped on every device update (poll or local set) */
    volatile int      last_action_code;   /* HTTP status of the last control request, 0 = no connection */
    volatile unsigned action_fail_gen;    /* bumped when a control request fails (for a toast) */
} homey_state_t;

extern homey_state_t homey_state;

void homey_lock(void);
void homey_unlock(void);

/* Starts the poller once (it re-reads the settings every round). */
int  homey_start(void);
/* Wake the poller now (after a settings change). Resets status to
 * HOMEY_ST_OFF ("connecting" while enabled) until the new host/key is tried;
 * a poll still running with the old values does not overwrite that. */
void homey_kick(void);

/* Zone name for a zone id, "" if unknown. Caller holds the lock. */
const char * homey_zone_name(const char * zone_id);

/* API key file. get: copies the key ("" if none). set: validates and writes
 * it (0600), returns 0, or -1 for characters that cannot be a Homey key. */
void homey_get_key(char * out, size_t n);
int  homey_set_key(const char * key);

/* Settings "test": one synchronous device fetch. Returns the device count
 * (>= 0), or -HOMEY_ST_AUTH / -HOMEY_ST_NOCONN / -HOMEY_ST_BADDATA. */
int  homey_probe(void);

/* ---- control (non-blocking). All return 0 when queued, -1 when refused
 * (unknown device/flow, capability missing or read-only, device
 * unavailable, invalid value, queue full). ---- */
int homey_set_bool(const char * dev_id, const char * cap, int on);
int homey_set_num(const char * dev_id, const char * cap, double v);   /* 0..1 caps clamped */
int homey_set_enum(const char * dev_id, const char * cap, const char * value);

int homey_onoff(const char * dev_id, int on);
int homey_dim(const char * dev_id, double level);           /* 0..1 */
int homey_cover(const char * dev_id, const char * state);   /* "up" / "idle" / "down" */
int homey_press(const char * dev_id);                       /* play_scene or button */
int homey_start_flow(const char * flow_id);                 /* manual flows only */

/* Shared UI rules (hub, Homey screen, remote page). read_only: locks and
 * garage doors, which FreeToon shows but never operates (no one-tap
 * unlock/open). switchable: a plain on/off device (setable onoff, not
 * read-only, not a cover). */
int homey_read_only(const hy_dev_t * d);
int homey_switchable(const hy_dev_t * d);

#endif
