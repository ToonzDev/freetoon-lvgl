#ifndef TOON_SOLAR_APP_H
#define TOON_SOLAR_APP_H
#include <stddef.h>
#include <time.h>

/* Solar production measured by a marketplace app (BACKLOG #60): the app's
 * daemon reads an inverter and POSTs every poll to 127.0.0.1:10081
 * /api/v1/solar/source
 *   {"app":"inverters","brand":"enphase","ok":1,"power_w":1024,
 *    "today_wh":21674,"total_wh":4111344,"msg":""}
 * (today_wh / total_wh -1 = not reported; ok 0 + msg = why there is no
 * reading). The core decides what to do with it (energy_solar_w()). */

typedef struct {
    char   app[32];
    char   brand[24];
    int    ok;            /* last post had a reading */
    double power_w;
    double today_wh;      /* < 0 = unknown */
    double total_wh;      /* < 0 = unknown */
    char   msg[160];      /* last error from the app, "" when ok */
    time_t last_post;     /* any post */
    time_t last_ok;       /* last post with a reading */
} solar_app_state_t;

/* Parse and store one post. 0 = accepted, -1 = malformed. */
int  solar_app_post(const char * json);
/* Copy of the current state (thread-safe). */
void solar_app_get(solar_app_state_t * out);
/* 1 while an app is the solar source: it posts (last post < 10 min), has an
 * inverter type set and gave a reading in the last 24 h (an inverter or its
 * WiFi stick sleeps at night, the source stays). */
int  solar_app_active(void);
/* Production now: the last reading while fresh (< 5 min), else 0. */
float solar_app_power_w(void);

#endif
