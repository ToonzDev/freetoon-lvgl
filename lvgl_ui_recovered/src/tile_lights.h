/*
 * The two switches on the Verlichting home tile.
 *
 * Each slot holds a reference to one switchable light, from any source:
 *   "dz:<idx>"        Domoticz device
 *   "ha:<entity_id>"  Home Assistant light/switch
 *   "hy:<device id>"  Homey device with a settable onoff
 *   "zw:<uuid>"       Z-Wave plug/switch
 *   ""                empty
 *   "auto"            not chosen yet: the pre-chooser rule (Domoticz T1/T2,
 *                     then HA devices flagged Home, then Z-Wave switches).
 * Stored in settings.tile_light1/2. Chosen in the tile chooser
 * (screen_tile_lights.c, long-press on the tile), and also by the T1/T2
 * buttons on the Domoticz screen, the Home flag of HA devices and the
 * home button on the Homey screen: all of them write these two slots.
 */
#ifndef TILE_LIGHTS_H
#define TILE_LIGHTS_H

#include <stddef.h>

#define TL_REF_LEN 72

enum { TL_NONE = 0, TL_DZ, TL_HA, TL_ZW, TL_HY };

typedef struct {
    int  kind;              /* TL_* */
    int  dz_idx;            /* TL_DZ */
    int  ha_i;              /* TL_HA: index in ha_devices, -1 if not loaded (yet) */
    int  zw_i;              /* TL_ZW: n-th Z-Wave switch, -1 if not found */
    char id[TL_REF_LEN - 4]; /* HA entity id / Homey id / Z-Wave uuid */
} tl_src_t;

/* What slot 0/1 shows right now. */
tl_src_t tl_resolve(int slot);
/* "dz:7" etc. for a resolved source ("" for TL_NONE). */
void tl_ref(const tl_src_t * s, char * out, size_t n);
/* Slot (0/1) that currently shows ref, or -1. */
int  tl_slot_of(const char * ref);
/* Put ref ("" = empty) in a slot and save. A slot still on "auto" is fixed
 * to what it shows first, and ref is removed from the other slot. */
void tl_set(int slot, const char * ref);
/* Pin buttons elsewhere: on = put ref in the first empty slot (when both are
 * taken: drop_oldest moves slot 2 up and uses slot 2, else nothing happens);
 * off = remove it. Returns the slot used, or -1. */
int  tl_pin(const char * ref, int on, int drop_oldest);
/* Name (folded for the UI fonts by the sources) and state. Returns 1 when the
 * device is known and available (on is then valid). */
int  tl_status(const tl_src_t * s, char * name, size_t n, int * on);
/* Toggle the device in a slot. */
void tl_toggle(int slot);

#endif
