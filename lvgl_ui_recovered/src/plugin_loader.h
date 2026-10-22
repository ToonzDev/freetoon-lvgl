#pragma once
#include "lvgl.h"
#include "tile_slots.h"

/* Opens the detail screen for a marketplace integration via dynamic .so plugin.
 * Returns 1 on success, 0 on failure. */
int plugin_screen_open(const integration_meta_t * meta);
