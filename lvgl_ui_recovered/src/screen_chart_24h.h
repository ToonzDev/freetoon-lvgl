#ifndef TOON_SCREEN_CHART_24H_H
#define TOON_SCREEN_CHART_24H_H

#include "lvgl.h"
#include "tile_slots.h"

/* Opens the generic 24-hour chart screen for an integration.
 * If meta is NULL, default TemperatureLogger parameters are used. */
lv_obj_t * screen_chart_24h_create(const integration_meta_t * meta);

#endif /* TOON_SCREEN_CHART_24H_H */

