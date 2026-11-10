#ifndef TOON_HUE_H
#define TOON_HUE_H

#include <stddef.h>

#define HUE_MAX_LIGHTS 32

typedef struct {
    char id[16];           /* Hue light id, e.g. "1", "2" */
    char name[48];         /* e.g. "Living Room" */
    char type[32];         /* e.g. "Dimmable light", "Extended color light" */
    volatile int on;       /* 0 = off, 1 = on */
    volatile int bri;      /* 1..254 */
    volatile int bri_pct;  /* 0..100% */
    volatile int reachable;/* 1 = reachable, 0 = unreachable */
    int is_dimmable;       /* 1 if brightness can be set */
} hue_light_t;

/* Rooms and zones as defined in the Hue app (bridge /groups, type "Room"
 * or "Zone"). A zone can hold any set of lamps, e.g. "living room without
 * the reading lamp" — that is how "everything on except ..." is done. */
#define HUE_MAX_GROUPS 24
typedef struct {
    char id[16];           /* Hue group id, e.g. "3" */
    char name[48];
    int  is_zone;          /* 1 = Zone, 0 = Room */
    int  n_lights;         /* lamps in the group */
    volatile int any_on;   /* at least one lamp on */
    volatile int all_on;
    volatile int bri_pct;  /* last group brightness 0..100 */
} hue_group_t;

typedef struct {
    volatile int connected;      /* 1 if connected, 0 if error */
    volatile int count;          /* number of lights */
    hue_light_t  lights[HUE_MAX_LIGHTS];
    volatile int group_count;    /* rooms + zones */
    hue_group_t  groups[HUE_MAX_GROUPS];
} hue_state_t;

extern hue_state_t hue_state;

int  hue_start(void);
void hue_stop(void);

/* Probe bridge connection: returns light count (>= 0), or:
 *   -1: unauthorized / invalid username
 *   -2: host unreachable / connection failed */
int  hue_probe(const char * host, const char * user);

/* Pair with Hue Bridge (press link button).
 * Returns:
 *   0: success, out_user filled with token
 *   1: link button not pressed
 *  -1: connection error */
int  hue_pair(const char * host, char * out_user, size_t out_sz, char * err_msg, size_t err_sz);

/* Asynchronous actions */
void hue_light_toggle_async(const char * id);
void hue_light_set_bri_async(const char * id, int pct);
void hue_all_off_async(void);
void hue_all_on_async(void);
/* Rooms / zones */
void hue_group_set_on_async(const char * id, int on);
void hue_group_set_bri_async(const char * id, int pct);   /* 1..100, switches the group on */
/* Group shown on the home "Hue" tile (settings.hue_tile_group, "" = all
 * lamps). Returns its index in hue_state.groups or -1 (= all lamps). */
int  hue_tile_group_index(void);

#endif
