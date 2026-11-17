#ifndef TOON_SCREEN_ZWAVE_H
#define TOON_SCREEN_ZWAVE_H

/* Public bridge surface for the Z-Wave admin screen. The screen create
 * function itself stays declared via screens.h — this header only exposes
 * what the master↔slave bridge needs (g_dev list accessors + a slave-side
 * setter so the WASM client can mirror the master's controller device list). */

#define ZWAVE_DEV_MAX 24

typedef struct {
    char uuid[40];
    char name[64];
    char type[48];
    int  node_id;
    int  is_switch;
    int  state;        /* 0 off, 1 on, -1 unknown */
    float power_w;     /* Live power consumption from CurrentElectricityFlow */
} zwave_dev_t;

/* Read-only accessors used by pwa_server's render_state_json. */
int zwave_dev_count(void);
int zwave_dev_at(int i, zwave_dev_t * out);

/* Switch / plug helpers used by screen_home (Verlichting tile & Slimme stekkers tile) */
int  zwave_switch_count(void);
int  zwave_switch_count_cached(void);   /* no poll; 0 until the list is loaded */
int  zwave_get_switch(int idx, zwave_dev_t * out);
int  zwave_toggle_switch(int idx);
void zwave_poll(void);
int  zwave_is_switch_device(const char * type, const char * name, const char * cc, int node_id);
int  zwave_get_custom_name(const char * uuid, int node_id, char * out, size_t outsz);

/* Slave-side setter — replace the screen's internal device list with the
 * master's. Idempotent; the next refresh of the Z-Wave admin screen on the
 * WASM client reads the mirrored data. */
void zwave_set_devices_from_remote(int n, const zwave_dev_t * src);

#endif
