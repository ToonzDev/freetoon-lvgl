/*
 * Round thermostat home tile (LT_THERMO_DIAL): a dial (lv_arc) over the
 * setpoint range with a knob to drag, the setpoint large in the middle,
 * the room temperature and the boiler state, and -/+ buttons in the dial's
 * open bottom. The setpoint is sent when the knob is released (one request,
 * not one per step), through the PIN gate like the other thermostat
 * controls. A tap in the middle opens the thermostat screen.
 */
#ifndef TILE_DIAL_H
#define TILE_DIAL_H

#include "lvgl.h"

/* Fill a tile made by the home screen (make_tile). on_open: tap in the
 * middle (the thermostat screen). Update once a second. */
void td_tile_build(lv_obj_t * tile, lv_obj_t * title, lv_event_cb_t on_open);
void td_tile_update(void);

#endif
