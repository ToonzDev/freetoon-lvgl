/*
 * Schakelschema core (pure, host-tested by tests/switch_sched_test.c): the
 * rules that switch a device on or off at a time of day, when they are due,
 * and where they are stored. The device side (tile_lights.h refs, timer,
 * screens) is in switch_schedule.c.
 *
 * A rule: device ref ("zw:<uuid>", "hs:<ip>", "hu:<id>", ...; see
 * tile_lights.h), on or off, a time (a clock time, or sunrise / sunset plus
 * an offset in minutes), the weekdays it applies to and whether it is active.
 *
 * Due: every tick the most recent occurrence T <= now of each active rule
 * counts when it was not handled yet (T > last) and is at most `catchup`
 * seconds old, so a Toon that was off or restarting at T still switches
 * afterwards. Per device only the latest due occurrence is executed (after
 * a restart at 09:00 "on 07:00" + "off 08:00" gives "off"); every rule that
 * was due is marked handled. A rule that is saved (new or edited) starts
 * with last = now: it does not switch for a moment that already passed.
 */
#ifndef TOON_SWITCH_SCHED_CORE_H
#define TOON_SWITCH_SCHED_CORE_H

#include <stddef.h>

#define SS_MAX      40
#define SS_REF_LEN  72

enum { SS_CLOCK = 0, SS_SUNRISE = 1, SS_SUNSET = 2 };

/* days: bit 0 = Monday ... bit 6 = Sunday */
#define SS_ALL_DAYS 0x7f

typedef struct {
    int  active;
    int  on;              /* 1 = switch on, 0 = off */
    int  mode;            /* SS_CLOCK / SS_SUNRISE / SS_SUNSET */
    int  hhmm;            /* SS_CLOCK: 0..2359 */
    int  offset;          /* sun modes: minutes, -180..180 */
    int  days;            /* SS_ALL_DAYS mask */
    long last;            /* handled up to this time (unix s) */
    char ref[SS_REF_LEN];
} ss_rule_t;

typedef struct {
    ss_rule_t r[SS_MAX];
    int       n;
} ss_set_t;

typedef struct {
    char ref[SS_REF_LEN];
    int  on;
    int  rule;            /* index of the rule that won */
    long t;               /* its occurrence */
} ss_action_t;

/* Sunrise / sunset (unix s) of a local date at lat/lon. 0 = ok, -1 = the
 * sun does not rise or set that day. */
int  ss_sun(int y, int m, int d, double lat, double lon, long * rise, long * set);
/* Occurrence of a rule on a local date (unix s), -1 when the date's weekday
 * is not in the rule or the sun does not rise/set. */
long ss_occurrence(const ss_rule_t * r, int y, int m, int d, double lat, double lon);
/* Latest occurrence <= now within the last 3 days, -1 if none. */
long ss_last_occurrence(const ss_rule_t * r, long now, double lat, double lon);
/* First occurrence > now within the next 8 days, -1 if none. */
long ss_next_occurrence(const ss_rule_t * r, long now, double lat, double lon);
/* The due actions (see above), at most max; marks the due rules handled.
 * Returns the number of actions; *changed = 1 when any rule was marked. */
int  ss_due(ss_set_t * s, long now, double lat, double lon, long catchup,
            ss_action_t * out, int max, int * changed);

/* "07:00" or "zonsondergang -15 min" / "zonsopkomst" */
void ss_time_text(const ss_rule_t * r, char * out, size_t n);
/* "elke dag", "ma-vr", "za, zo", "ma, wo, vr" */
void ss_days_text(int days, char * out, size_t n);

/* Text file, one rule per line: "r <active> <on> <mode> <hhmm> <offset>
 * <days> <last> <ref>". Written atomically. 0 = ok. */
int  ss_save(const ss_set_t * s, const char * path);
int  ss_load(ss_set_t * s, const char * path);
/* Rules of the ToonStore app "Slimme stekkers" (plugSchedule, its
 * userSettings JSON) added to s as Z-Wave rules, handled up to now.
 * Returns the number added. */
int  ss_import_qml(ss_set_t * s, const char * json, long now);

#endif
