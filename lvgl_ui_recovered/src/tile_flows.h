/*
 * "Homey flows" home tile (LT_HOMEY_FLOWS): buttons that start Homey flows
 * (the ones that can be started by hand, homey_start_flow()).
 *
 * Up to TF_SLOTS places; how many the tile shows depends on its size. Which
 * flow sits on which place is chosen in the chooser (long press on the
 * tile) and stored in settings.tile_flows: "auto" (not chosen yet: the first
 * flows Homey lists) or a comma-separated list of flow ids, one per place,
 * empty for an empty place.
 */
#ifndef TILE_FLOWS_H
#define TILE_FLOWS_H

#include "lvgl.h"
#include <stddef.h>

#define TF_SLOTS 8

/* Flow id on a place ("" = empty). */
void tf_get(int slot, char * id, size_t n);
/* Put a flow on a place ("" = empty) and save; it leaves any other place.
 * The first call after "auto" fixes the places to what they showed. */
void tf_set(int slot, const char * id);
/* Place that holds id, or -1. */
int  tf_slot_of(const char * id);
/* Display name of a flow; 1 = Homey still lists it. */
int  tf_name(const char * id, char * name, size_t n);
/* How many places the tile on the home screen shows (0 without a tile). */
int  tf_capacity(void);

/* Home tile: fill a tile made by the home screen (make_tile) with the
 * buttons; long press anywhere opens the chooser. Update once a second. */
void tf_tile_build(lv_obj_t * tile, lv_obj_t * title);
void tf_tile_update(void);

/* The chooser screen (ui_push_owned). */
lv_obj_t * screen_tile_flows_create(void);

#endif
