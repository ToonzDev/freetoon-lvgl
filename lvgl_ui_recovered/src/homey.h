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
 * Control (set capability, start flow) comes in the next step. */

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
    volatile unsigned gen;        /* bumped on every successful device update */
} homey_state_t;

extern homey_state_t homey_state;

void homey_lock(void);
void homey_unlock(void);

/* Starts the poller once (it re-reads the settings every round). */
int  homey_start(void);
/* Wake the poller now (after a settings change). */
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

#endif
