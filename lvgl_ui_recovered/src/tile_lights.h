/*
 * The places on the Verlichting home tile (up to TL_SLOTS; how many the tile
 * shows depends on its size, see tl_set_capacity()).
 *
 * Each slot holds a reference to one switchable light, from any source:
 *   "dz:<idx>"        Domoticz device
 *   "ha:<entity_id>"  Home Assistant light/switch
 *   "hy:<device id>"  Homey device with a settable onoff
 *   "zw:<uuid>"       Z-Wave plug/switch
 *   "hu:<light id>"   Philips Hue lamp
 *   "hg:<group id>"   Philips Hue room or zone
 *   "hs:<host>"       HomeWizard Energy Socket (by its IP / host)
 *   ""                empty
 *   "auto"            slots 1 and 2 only, not chosen yet: the pre-chooser
 *                     rule (Domoticz T1/T2, then HA devices flagged Home,
 *                     then Z-Wave switches).
 * Stored in settings.tile_light1..8. Chosen in the tile chooser
 * (screen_tile_lights.c, long-press on the tile), and also by the T1/T2
 * buttons on the Domoticz screen, the Home flag of HA devices and the
 * home button on the Homey screen: all of them write these slots.
 */
#ifndef TILE_LIGHTS_H
#define TILE_LIGHTS_H

#include <stddef.h>

#define TL_REF_LEN 72
#define TL_SLOTS   8

enum { TL_NONE = 0, TL_DZ, TL_HA, TL_ZW, TL_HY, TL_HUE, TL_HUEG, TL_HWS };

typedef struct {
    int  kind;              /* TL_* */
    int  dz_idx;            /* TL_DZ */
    int  ha_i;              /* TL_HA: index in ha_devices, -1 if not loaded (yet) */
    int  zw_i;              /* TL_ZW: n-th Z-Wave switch, -1 if not found */
    int  hue_i;             /* TL_HUE / TL_HUEG: index in hue_state lights / groups, -1 if not found;
                               TL_HWS: socket slot (settings.hw_socket_host[]) */
    char id[TL_REF_LEN - 4]; /* HA entity id / Homey id / Z-Wave uuid / Hue id */
} tl_src_t;

/* What slot 0..TL_SLOTS-1 shows right now. */
tl_src_t tl_resolve(int slot);
/* "dz:7" etc. for a resolved source ("" for TL_NONE). */
void tl_ref(const tl_src_t * s, char * out, size_t n);
/* Slot that currently shows ref, or -1. */
int  tl_slot_of(const char * ref);
/* Put ref ("" = empty) in a slot and save. Slots still on "auto" are fixed
 * to what they show first, and ref is removed from any other slot. */
void tl_set(int slot, const char * ref);
/* Pin buttons elsewhere: on = put ref in the first empty slot the tile shows
 * (when all are taken: drop_oldest moves the others up one and uses the
 * last one, else nothing happens); off = remove it. Returns the slot used,
 * or -1. */
int  tl_pin(const char * ref, int on, int drop_oldest);
/* How many slots the tile on the home screen shows (set by the home screen
 * from the tile's size; 2 when there is no tile). Pins go there. */
void tl_set_capacity(int n);
int  tl_capacity(void);
/* Name (folded for the UI fonts by the sources) and state. Returns 1 when the
 * device is known and available (on is then valid). */
int  tl_status(const tl_src_t * s, char * name, size_t n, int * on);
/* Brightness 0..100 of a dimmable light (its last level while off), or -1
 * when the light can't be dimmed. */
int  tl_level(const tl_src_t * s);
/* Toggle the device in a slot. */
void tl_toggle(int slot);
/* Dim the light in a slot to pct (1..100; switches it on). */
void tl_set_level(int slot, int pct);

/* Any ref, not only the tile's places (schedules, web page). */
tl_src_t tl_parse(const char * ref);
/* Switch the device behind ref on or off (not a toggle). 1 = sent. */
int  tl_set_on_ref(const char * ref, int on);

/* Every switchable device FreeToon knows, grouped by source: cb is called
 * once per device with the group's header ("Domoticz", "Hue - lampen", ...),
 * its ref, display name, whether it answers and whether it dims. UI thread
 * (reads the sources the way their screens do). */
typedef void (*tl_enum_cb)(void * ctx, const char * group, const char * ref,
                           const char * name, int available, int dimmable);
void tl_enumerate(tl_enum_cb cb, void * ctx);

#endif
