#ifndef TOON_TILE_WIFIQR_H
#define TOON_TILE_WIFIQR_H
/* WiFi QR home tile (LT_WIFIQR), after the ToonStore app "wifiqr": a QR code
 * that phones scan to join a network - the Toon's own WiFi or a guest
 * network entered in its settings (settings.wifiqr_*). Tap = the QR code
 * full screen with the name and password; long press = settings. */
#include "lvgl/lvgl.h"
#include <stddef.h>

/* "WIFI:S:<ssid>;T:WPA;P:<pass>;H:true;;" for the current settings, and
 * the network name / password shown next to it. 0 = no network known. */
int  wifiqr_payload(char * qr, size_t qn, char * ssid, size_t sn, char * pass, size_t pn);

void wq_tile_build(lv_obj_t * tile, lv_obj_t * title);   /* make_tile() result */
void wq_tile_update(void);                               /* from the home refresh */
void wq_tile_forget(void);                               /* home screen torn down */

void wq_show_big(void);      /* full-screen QR (top layer) */
void wq_close_big(void);

#endif
