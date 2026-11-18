#ifndef TOON_SCREENS_H
#define TOON_SCREENS_H

#include "lvgl/lvgl.h"

/* Each screen builds its widgets onto its own root lv_obj
   (created via lv_obj_create(NULL)). Use ui_push to navigate
   forward and ui_pop to go back. */

void ui_push(lv_obj_t * scr);
void ui_pop(void);
/* Push a screen that is built fresh on every open. The stack deletes it
 * (async) once it leaves the stack — back button, auto-home, anything — so
 * the screen must free its timers/data in an LV_EVENT_DELETE handler and
 * must not cache its root in a static it reuses later. */
void ui_push_owned(lv_obj_t * scr);
int  ui_is_dimmed(void);
/* Apply settings.home_theme live (swap dark<->stock home, no restart). */
void ui_apply_home_theme(void);
void ui_refresh_home(void);
void ui_refresh_theme(void);
/* Rebuild for a new colour theme. Call on the UI thread after theme_set();
 * from another thread use ui_request_theme_refresh() (applied on the next
 * idle tick, after settings.ui_theme was set). */
void ui_request_theme_refresh(void);
/* Swap a cached screen in the stack for a freshly built one (theme change). */
void ui_replace_screen(lv_obj_t * old_scr, lv_obj_t * new_scr);
/* Delete a cached screen for a rebuild, safe when it is the active one. */
void ui_del_screen(lv_obj_t * scr);
void ui_request_restart(void);   /* clean _exit(0) restart; drops the launcher crash-guard marker */
void ui_request_reboot(void);    /* full device reboot (sync + /sbin/reboot) */


/* Per-screen builders. Each returns a freshly created screen object. */
lv_obj_t * screen_home_create(void);
void screen_home_destroy(void);
void screen_home_stock_destroy(void);   /* theme change: rebuild on next show */
void screen_dim_destroy(void);
void screen_dim_grid_destroy(void);
lv_obj_t * screen_home_stock_create(void);   /* light tile-carousel "stock theme" home */
/* Auto-home: drop the home screen's swipe page back to page 1 (the main page)
 * when idle. No-op if already there. */
void screen_home_reset_to_main(void);
void screen_home_force_page(int n);   /* sim/testing: jump to a home page (page 2 is gesture-only) */
void screen_home_set_ticker_speed(int spd);
void screen_home_open_news(void);
lv_obj_t * screen_thermostat_create(void);
lv_obj_t * screen_dim_create(void);
lv_obj_t * screen_dim_grid_create(void);   /* custom grid dim (opt-in); used by screen_dim_create */
/* Dim screens: outside temperature in the big room-temp font. Picks the
 * longest of "8.4°C" / "8.4°" / "8°" that fits max_w with the label's font. */
void dim_set_outside_temp(lv_obj_t * lbl, float t, lv_coord_t max_w);
/* Dim screens, forecast strip (#49): one column per day = weather icon (in a
 * lighter tint) with the weekday and the day's maximum temperature next to
 * it, in a larger font. build: column i of width col_w and height h at x,y in
 * parent. paint: fill column i from weather_state.days[i] (hidden if none). */
void dim_fc_build(lv_obj_t * parent, int x, int y, int col_w, int h,
                  lv_obj_t ** icon, lv_obj_t ** day, lv_obj_t ** temp);
void dim_fc_paint(lv_obj_t * icon, lv_obj_t * day, lv_obj_t * temp, int i);
lv_obj_t * screen_black_create(void);      /* pure black standby screen when Slapen or Weg */
lv_obj_t * screen_settings_create(void);
lv_obj_t * screen_schedule_create(void);
lv_obj_t * screen_stats_create(void);
lv_obj_t * screen_forecast_create(void);
void screen_weather_hourly_show(int day_idx);

/* Inbox modal popup — opened from the envelope button on home. */
void screen_inbox_show(void);
/* Agenda modal — upcoming calendar events (HA + iCal). */
void screen_calendar_show(void);
void screen_calendar_close(void);
/* Waste calendar modal (CR3). */
void screen_waste_show(void);
void screen_waste_close(void);
void screen_home_dismiss_modals(void);
void screen_settings_close_modals(void);
/* Home-tile layout editor ("Indeling"). */
void screen_layout_editor_show(void);
lv_obj_t * screen_layout_editor_create(void);
/* Dim-screen block layout editor ("Dim indeling"). */
void screen_dim_layout_editor_show(void);
lv_obj_t * screen_dim_layout_editor_create(void);
/* Boot picker layout (headless-sim render only; device path is bootpick_run). */
lv_obj_t * screen_bootpick_create(void);
lv_obj_t * screen_crypto_picker_create(void);   /* Settings -> Crypto coin live-search */
lv_obj_t * screen_ha_picker_create(void);       /* Settings -> HA entity picker (domain-filtered list) */
void screen_ha_picker_open(const char * domain, lv_obj_t * target_ta);  /* sets domain+target, then ui_push */
void screen_ha_picker_open_add(const char * domain, int dev_type);      /* add-mode: pick -> ha_device_add */
lv_obj_t * screen_ha_devices_create(void);      /* Settings -> Devices manager (add/remove/pin) */
lv_obj_t * screen_crypto_create(void);           /* crypto tile-tap -> price-history graphs */
lv_obj_t * screen_forecast_create(void);
lv_obj_t * screen_stats_create(void);
lv_obj_t * screen_schedule_create(void);
lv_obj_t * screen_heater_advanced_create(void);
lv_obj_t * screen_vent_remote_create(void);
lv_obj_t * screen_vent_advanced_create(void);
lv_obj_t * screen_lights_create(void);
lv_obj_t * screen_devices_create(void);   /* Devices hub: all backends, by room (#53) */
int        devices_hub_available(void);   /* any device backend configured (home button) */
lv_obj_t * screen_marketplace_create(void);
lv_obj_t * screen_zwave_create(void);
lv_obj_t * screen_wifi_create(void);
lv_obj_t * screen_adapters_create(void);
lv_obj_t * screen_meteradapters_create(void);   /* add / configure / remove meter adapters */
lv_obj_t * screen_domoticz_create(void);
lv_obj_t * screen_hue_create(void);
lv_obj_t * screen_efanlamp_create(void);  /* BLE ceiling fan+light control */
struct integration_meta_s;
lv_obj_t * screen_chart_24h_create(const struct integration_meta_s * meta);
static inline lv_obj_t * screen_templogger_create(void) {
    return screen_chart_24h_create(NULL);
}

/* Open the Tile-slots picker modal — used both by the Settings → Tiles
 * tile and the long-press handler on the four right-column home tiles. */
void screen_settings_open_tile_slots_modal(void);
/* Open single-slot assignment modal for a specific tile slot (0..7). */
void screen_settings_open_tile_slot_single_modal(int slot);

/* Open detail screen or app for an integration ID (or local: ID). Returns 1 if handled. */
int open_integration_by_id(const char * b);

/* Called once at boot. Loads home as root. */
void ui_init(void);

/* Idle/wake controller — call ui_mark_activity() on every touch
   (already wired in main via indev read_cb wrapper). The idle ticker
   pushes the dim screen and lowers the backlight when timeout elapses. */
void ui_mark_activity(void);
void ui_idle_tick(void);   /* call from main loop, cheap */
void ui_wake_now(void);    /* explicit wake from dim screen tap */
void ui_dim_now(void);     /* explicit transition to dim screen (e.g. gear long-press) */
void ui_theme_install(void); /* theme tweaks (Toon 1: no button shadows); ui_init calls it */
int  ui_is_dimmed(void);    /* returns 1 if dim screen is currently active */

#endif
