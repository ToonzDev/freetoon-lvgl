#ifndef TOON_CALENDAR_H
#define TOON_CALENDAR_H

/* Upcoming-events agenda. Merges two optional sources (settings):
 *   - calendar_ha_entity : a Home Assistant calendar.* entity (REST, Bearer)
 *   - calendar_ics_url   : a public/private .ics URL (CalDAV/Google/etc.)
 * Events from both are parsed, de-duplicated, sorted by date+time, and the
 * next CAL_MAX upcoming ones are kept. Refreshed on a slow background loop
 * (calendars don't change minute-to-minute). */

#define CAL_MAX 64

typedef struct {
    char date[12];      /* "YYYY-MM-DD" */
    char time[8];       /* "HH:MM", empty for all-day */
    char end_time[8];   /* "HH:MM", empty if none */
    char summary[80];
} calendar_event_t;

typedef struct {
    volatile int     connected;     /* 1 after a successful fetch from a source */
    volatile int     count;         /* number of upcoming events in ev[] */
    calendar_event_t ev[CAL_MAX];
} calendar_state_t;

extern calendar_state_t calendar_state;

int  calendar_start(void);          /* background poller; 0 on success */
void calendar_refresh_now(void);    /* on-demand refresh (runs in caller thread) */
void calendar_refresh_async(void);  /* on-demand refresh on a detached thread (UI-safe) */

#endif
