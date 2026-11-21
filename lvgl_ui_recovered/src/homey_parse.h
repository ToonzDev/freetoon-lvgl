#ifndef TOON_HOMEY_PARSE_H
#define TOON_HOMEY_PARSE_H

/* Parser for the Homey Pro (2023) local Web API (BACKLOG #28).
 *
 * Pure C, no dependencies on toonui (no settings, no http, no LVGL), so it
 * can be unit-tested on the host (tests/homey_parse_test.c).
 *
 * Input shapes (verified against a real 2026-11 export):
 *   GET /api/manager/devices/device/       object keyed by device id
 *   GET /api/manager/zones/zone/           object keyed by zone id
 *   GET /api/manager/flow/flow/            object keyed by flow id
 *   GET /api/manager/flow/advancedflow/    object keyed by flow id
 * An array of the same objects is accepted too.
 *
 * Only what FreeToon shows is kept: id, name, zone, class, availability and a
 * whitelist of capabilities. Device `settings` (which hold third-party
 * passwords and API keys in real installs) are skipped and never copied.
 */

#include <stddef.h>

#define HY_MAX_DEV   160
#define HY_MAX_CAP   10
#define HY_MAX_ZONE  64
#define HY_MAX_FLOW  64

#define HY_ID_LEN    40     /* uuid (36) + NUL, with slack */
#define HY_NAME_LEN  48

/* What FreeToon does with a device, from virtualClass (or class). */
enum {
    HY_K_OTHER = 0,
    HY_K_LIGHT,        /* light, or socket with virtualClass light */
    HY_K_SOCKET,
    HY_K_COVER,        /* windowcoverings, curtain, blinds, sunshade, ... */
    HY_K_LOCK,
    HY_K_THERMOSTAT,   /* thermostat, heater, airconditioning */
    HY_K_FAN,
    HY_K_SENSOR,       /* sensor, homealarm, doorbell */
    HY_K_BUTTON,       /* momentary: button class / play_scene */
};

enum { HY_V_NONE = 0, HY_V_BOOL, HY_V_NUM, HY_V_STR };

typedef struct {
    char   id[32];          /* "onoff", "dim", "measure_power.phase1", ... */
    char   units[8];        /* as reported ("W", "°C", "%"), may be empty */
    unsigned char type;     /* HY_V_* of the current value */
    unsigned char setable;
    unsigned char getable;
    double num;             /* HY_V_NUM / HY_V_BOOL (0/1) */
    char   str[24];         /* HY_V_STR (enum id such as "up"/"idle"/"down") */
} hy_cap_t;

typedef struct {
    char id[HY_ID_LEN];
    char name[HY_NAME_LEN];
    char zone[HY_ID_LEN];   /* zone id; name via hy_zone_t */
    char cls[24];           /* class */
    char vcls[24];          /* virtualClass, "" if null */
    unsigned char kind;     /* HY_K_* */
    unsigned char available;
    unsigned char ncap;
    hy_cap_t cap[HY_MAX_CAP];
} hy_dev_t;

typedef struct {
    char id[HY_ID_LEN];
    char name[HY_NAME_LEN];
} hy_zone_t;

typedef struct {
    char id[HY_ID_LEN];
    char name[HY_NAME_LEN];
    unsigned char advanced;  /* 1 = advancedflow endpoint */
} hy_flow_t;

/* Each returns the number of entries stored (0..max), or -1 if `body` is not
 * the expected JSON shape. `len` is the body length (body need not be NUL-
 * terminated). Devices that are the Toon itself (a Toon app on the Homey),
 * devices without any capability FreeToon knows, and remotes are skipped. */
int hy_parse_devices(const char * body, size_t len, hy_dev_t * out, int max);
int hy_parse_zones(const char * body, size_t len, hy_zone_t * out, int max);
/* Only enabled flows that can be started by hand ("This flow is started"
 * trigger card, homey:manager:flow:programmatic_trigger). Appends after
 * `already` entries (call once per endpoint); returns the new total. */
int hy_parse_flows(const char * body, size_t len, int advanced,
                   hy_flow_t * out, int already, int max);

/* In place: UTF-8 -> what the UI fonts can draw (ASCII). Accented Latin-1
 * letters fold to their base letter, typographic quotes/dashes to ASCII,
 * anything else (emoji) is dropped; spaces are trimmed and collapsed.
 * Applied to every device, zone and flow name by the parsers. */
void hy_fold_text(char * s);

/* Lookup helpers. */
const hy_cap_t * hy_dev_cap(const hy_dev_t * d, const char * cap_id);
const char * hy_kind_name(int kind);

#endif
