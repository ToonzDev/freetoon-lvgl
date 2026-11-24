#ifndef TOON_SWITCH_SCHEDULE_H
#define TOON_SWITCH_SCHEDULE_H
/*
 * Schakelschema: switch any device FreeToon can switch (tile_lights.h refs)
 * on or off at set times, as the ToonStore app "Slimme stekkers" did for
 * Z-Wave plugs. Rules and their timing: switch_sched_core.h. Runs on the
 * master (an LVGL timer, every 15 s); every command is sent twice (again on
 * the next tick), as plugs sometimes miss one. Rules from the QML app are
 * imported once. Edited on the Toon (screen_switch_schedule.c) and on
 * :10081/schema (pwa_server.c); both go through these functions, which are
 * thread-safe.
 */
#include "switch_sched_core.h"
#include <stddef.h>

void sched_init(void);                         /* UI thread, after lv_init */
int  sched_count(void);
int  sched_get(int i, ss_rule_t * out);        /* 1 = ok */
/* i = -1 adds; the rule starts handled up to now. 0 = saved. */
int  sched_put(int i, const ss_rule_t * r);
int  sched_delete(int i);
/* Device name of a rule's ref ("?" when the source does not know it). */
void sched_device_name(const char * ref, char * out, size_t n);
/* "Aan om 07:00, ma-vr" */
void sched_rule_text(const ss_rule_t * r, char * out, size_t n);
/* Next action over all active rules: "Lamp aan, vandaag 07:00"; "" if none.
 * Returns its time (unix s) or -1. */
long sched_next_text(char * out, size_t n);
/* Location used for sunrise / sunset (weather Lat/Lon, else mid-NL). */
void sched_location(double * lat, double * lon);

#endif
