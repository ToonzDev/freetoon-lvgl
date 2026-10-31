/*
 * Schedule parser + writer for happ_thermstat's thermostatProgram config.
 * Communicates via BoxTalk (port 1337) with hcb_config, with direct file fallback
 * to /qmf/config/config_happ_thermstat.xml.
 *
 * NOTE on Day of Week:
 *   - hcb_config convention: 0 = Sun, 1 = Mon, 2 = Tue, 3 = Wed, 4 = Thu, 5 = Fri, 6 = Sat
 *   - UI convention:         0 = Mon, 1 = Tue, 2 = Wed, 3 = Thu, 4 = Fri, 5 = Sat, 6 = Sun
 *
 * Conversions:
 *   ui_day  = (hcb_day + 6) % 7
 *   hcb_day = (ui_day + 1) % 7
 */
#include "schedule.h"
#include "boxtalk.h"
#include "i18n.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

schedule_entry_t schedule_entries[SCHEDULE_MAX];
int              schedule_count = 0;

unsigned schedule_state_color(int state) {
    switch (state) {
        case 0: return 0xff8866;   /* Comfort  — warm orange */
        case 1: return 0x66cc88;   /* Home     — green */
        case 2: return 0x4466cc;   /* Sleep    — blue */
        case 3: return 0xaa66ff;   /* Away     — purple */
        default:return 0x666666;
    }
}

const char * schedule_state_name(int state) {
    switch (state) {
        case 0: return tr("Comfort", "Comfort");
        case 1: return tr("Thuis", "Home");
        case 2: return tr("Slapen", "Sleep");
        case 3: return tr("Weg", "Away");
        default:return "--";
    }
}

const char * schedule_day_short(int day) {
    static const char * names_nl[] = {"Ma", "Di", "Wo", "Do", "Vr", "Za", "Zo"};
    static const char * names_en[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    if (day < 0 || day > 6) return "--";
    return settings.language == LANG_EN ? names_en[day] : names_nl[day];
}

/* Which Comfort/Home/Sleep/Away program does the weekly schedule say is
 * "active right now" — i.e. the one with the most-recent start time that
 * is at or before this moment, wrapping around the week if every entry
 * is in the future. Returns -1 if there are no entries at all.
 *
 * schedule_entries uses the UI convention (0=Mon..6=Sun).
 * tm_wday is 0=Sun..6=Sat, so (tm_wday + 6) % 7 maps to 0=Mon..6=Sun. */
int schedule_program_now(void) {
    if (schedule_count <= 0) return -1;
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    int toon_day = (tm.tm_wday + 6) % 7;
    int now_mow  = toon_day * 1440 + tm.tm_hour * 60 + tm.tm_min;

    int best_state    = -1;
    int best_mow      = -1;     /* most-recent <= now */
    int latest_state  = -1;
    int latest_mow    = -1;     /* fallback: largest in week (wraps) */
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        int mow = e->start_day * 1440 + e->start_hour * 60 + e->start_min;
        if (mow <= now_mow && mow > best_mow) {
            best_mow   = mow;
            best_state = e->target_state;
        }
        if (mow > latest_mow) {
            latest_mow   = mow;
            latest_state = e->target_state;
        }
    }
    return (best_state >= 0) ? best_state : latest_state;
}

static int compare_entries(const void * a, const void * b) {
    const schedule_entry_t * x = (const schedule_entry_t *)a;
    const schedule_entry_t * y = (const schedule_entry_t *)b;
    if (x->start_day != y->start_day) return x->start_day - y->start_day;
    if (x->start_hour != y->start_hour) return x->start_hour - y->start_hour;
    return x->start_min - y->start_min;
}

int schedule_add(const schedule_entry_t * e) {
    if (schedule_count >= SCHEDULE_MAX) return -1;
    schedule_entries[schedule_count++] = *e;
    qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
    return 0;
}

int schedule_remove(int idx) {
    if (idx < 0 || idx >= schedule_count) return -1;
    memmove(&schedule_entries[idx], &schedule_entries[idx + 1],
            (schedule_count - idx - 1) * sizeof(schedule_entry_t));
    schedule_count--;
    return 0;
}

int schedule_replace(int idx, const schedule_entry_t * e) {
    if (idx < 0 || idx >= schedule_count) return -1;
    schedule_entries[idx] = *e;
    qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
    return 0;
}

/* Parse an integer from either XML <key>NNN</key> or JSON "key": NNN */
static int get_field_int(const char * s, const char * e, const char * key, int dflt) {
    char xml_tag[48];
    snprintf(xml_tag, sizeof xml_tag, "<%s>", key);
    const char * p = strstr(s, xml_tag);
    if (p && p < e) {
        p += strlen(xml_tag);
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        return atoi(p);
    }
    char json_key[48];
    snprintf(json_key, sizeof json_key, "\"%s\"", key);
    p = strstr(s, json_key);
    if (p && p < e) {
        p += strlen(json_key);
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p < e && *p == ':') p++;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p < e && *p == '"') p++;
        return atoi(p);
    }
    return dflt;
}

/* Parse entries from XML or JSON text. */
static int parse_schedule_buffer(const char * buf) {
    if (!buf || !buf[0]) return -1;

    /* Find the schedule section */
    const char * sched_start = strstr(buf, "<schedule>");
    const char * sched_end   = sched_start ? strstr(sched_start, "</schedule>") : NULL;
    int is_xml = (sched_start && sched_end);

    if (!is_xml) {
        sched_start = strstr(buf, "\"schedule\"");
        if (!sched_start) sched_start = buf;
        sched_end = buf + strlen(buf);
    }

    schedule_count = 0;
    const char * p = sched_start;
    while (p < sched_end && schedule_count < SCHEDULE_MAX) {
        const char * entry_start = NULL;
        const char * entry_end   = NULL;

        if (is_xml) {
            entry_start = strstr(p, "<entry>");
            if (!entry_start || entry_start >= sched_end) break;
            entry_end = strstr(entry_start, "</entry>");
            if (!entry_end || entry_end > sched_end) break;
            p = entry_end + 8;
        } else {
            entry_start = strstr(p, "\"entry\"");
            if (!entry_start || entry_start >= sched_end) break;
            entry_start = strchr(entry_start, '[');
            if (!entry_start || entry_start >= sched_end) break;
            entry_end = strchr(entry_start, ']');
            if (!entry_end || entry_end > sched_end) break;
            p = entry_end + 1;
        }

        schedule_entry_t * e = &schedule_entries[schedule_count++];
        int hcb_sday = get_field_int(entry_start, entry_end, "startDayOfWeek", 0);
        int hcb_eday = get_field_int(entry_start, entry_end, "endDayOfWeek",   0);

        /* Map hcb_config day (0=Sun..6=Sat) to UI (0=Mon..6=Sun) */
        e->start_day    = (hcb_sday + 6) % 7;
        e->end_day      = (hcb_eday + 6) % 7;
        e->start_min    = get_field_int(entry_start, entry_end, "startMin",    0);
        e->start_hour   = get_field_int(entry_start, entry_end, "startHour",   0);
        e->end_min      = get_field_int(entry_start, entry_end, "endMin",      0);
        e->end_hour     = get_field_int(entry_start, entry_end, "endHour",     0);
        e->target_state = get_field_int(entry_start, entry_end, "targetState", 0);
    }

    qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
    fprintf(stderr, "[sched] parsed %d entries\n", schedule_count);
    return schedule_count > 0 ? 0 : -1;
}

static void seed_default_schedule(void) {
    schedule_count = 0;
    /* Standard weekly program: 07:00 Home (1), 23:00 Sleep (2) */
    for (int d = 0; d < 7; d++) {
        schedule_entry_t * e1 = &schedule_entries[schedule_count++];
        e1->start_day = d;
        e1->start_hour = 7;
        e1->start_min = 0;
        e1->target_state = 1;

        schedule_entry_t * e2 = &schedule_entries[schedule_count++];
        e2->start_day = d;
        e2->start_hour = 23;
        e2->start_min = 0;
        e2->target_state = 2;
    }
    for (int i = 0; i < schedule_count; i++) {
        int nxt = (i + 1) % schedule_count;
        schedule_entries[i].end_day  = schedule_entries[nxt].start_day;
        schedule_entries[i].end_hour = schedule_entries[nxt].start_hour;
        schedule_entries[i].end_min  = schedule_entries[nxt].start_min;
    }
    fprintf(stderr, "[sched] seeded default weekly schedule (%d entries)\n", schedule_count);
}

/* Fallback: read /qmf/config/config_happ_thermstat.xml (or /HCBv2/config/) directly */
static int read_file_config(void) {
    const char * paths[] = {
        "/qmf/config/config_happ_thermstat.xml",
        "/HCBv2/config/config_happ_thermstat.xml",
        "/mnt/data/qmf/config/config_happ_thermstat.xml",
        "/mnt/data/qmf/config/toonsim_hcb_config.xml",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE * f = fopen(paths[i], "r");
        if (!f) continue;
        static char file_buf[64 * 1024];
        size_t n = fread(file_buf, 1, sizeof(file_buf) - 1, f);
        fclose(f);
        if (n > 0) {
            file_buf[n] = '\0';
            const char * tp = strstr(file_buf, "thermostatProgram");
            if (tp && parse_schedule_buffer(tp - 50 > file_buf ? tp - 50 : file_buf) == 0) {
                fprintf(stderr, "[sched] loaded %d entries from %s\n", schedule_count, paths[i]);
                return 0;
            }
        }
    }
    return -1;
}

int schedule_load(void) {
    /* Try BoxTalk (port 1337) first */
    if (boxtalk_get_object_config("thermostatProgram") == 0) {
        for (int i = 0; i < 100; i++) {
            if (config_response_ready) break;
            usleep(10000);  /* 10ms * 100 = 1s max */
        }
        if (config_response_ready && strstr(config_response_buf, "schedule")) {
            if (parse_schedule_buffer(config_response_buf) == 0) {
                fprintf(stderr, "[sched] loaded %d entries via BoxTalk\n", schedule_count);
                return 0;
            }
        }
    }

    /* Fallback to local config file */
    if (read_file_config() == 0) return 0;

    /* If no config loaded, seed defaults so UI displays a clean initial schedule */
    if (schedule_count == 0) {
        seed_default_schedule();
        return 0;
    }
    return 0;
}

/* Build XML payload for SetObjectConfig */
static size_t build_schedule_xml(char * out, size_t outsz) {
    size_t off = 0;
    int n = snprintf(out + off, outsz - off,
        "<device>"
        "<package>happ_thermstat</package>"
        "<internalAddress>thermostatProgram</internalAddress>"
        "<type>program</type>"
        "<visibility>1</visibility>"
        "<schedule>");
    if (n < 0) return 0;
    off += (size_t)n;

    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        /* Convert UI day (0=Mon..6=Sun) to hcb_config day (0=Sun..6=Sat) */
        int hcb_sday = (e->start_day + 1) % 7;
        int hcb_eday = (e->end_day + 1) % 7;

        n = snprintf(out + off, outsz - off,
            "<entry>"
            "<type>weekly_recurring</type>"
            "<startMin>%d</startMin>"
            "<startHour>%d</startHour>"
            "<startDayOfWeek>%d</startDayOfWeek>"
            "<targetState>%d</targetState>"
            "<endMin>%d</endMin>"
            "<endHour>%d</endHour>"
            "<endDayOfWeek>%d</endDayOfWeek>"
            "</entry>",
            e->start_min, e->start_hour, hcb_sday,
            e->target_state,
            e->end_min, e->end_hour, hcb_eday);
        if (n < 0) return 0;
        off += (size_t)n;
        if (off >= outsz - 128) break;
    }

    n = snprintf(out + off, outsz - off, "</schedule></device>");
    if (n < 0) return 0;
    off += (size_t)n;
    return off;
}

/* Directly update /qmf/config/config_happ_thermstat.xml on disk as backup */
static void update_file_config(const char * new_dev_xml) {
    const char * paths[] = {
        "/qmf/config/config_happ_thermstat.xml",
        "/HCBv2/config/config_happ_thermstat.xml",
        "/mnt/data/qmf/config/config_happ_thermstat.xml",
        "/mnt/data/qmf/config/toonsim_hcb_config.xml",
        NULL
    };
    const char * path = NULL;
    FILE * f = NULL;
    for (int i = 0; paths[i]; i++) {
        f = fopen(paths[i], "r");
        if (f) { path = paths[i]; break; }
    }
    if (!f || !path) return;
    static char fbuf[128 * 1024];
    size_t n = fread(fbuf, 1, sizeof(fbuf) - 1, f);
    fclose(f);
    if (n == 0) return;
    fbuf[n] = '\0';

    /* Locate thermostatProgram device block */
    const char * tp = strstr(fbuf, "thermostatProgram");
    if (!tp) return;
    const char * dev_start = tp;
    while (dev_start > fbuf && strncmp(dev_start, "<device", 7) != 0) dev_start--;
    if (strncmp(dev_start, "<device", 7) != 0) return;
    const char * dev_end = strstr(tp, "</device>");
    if (!dev_end) return;
    dev_end += 9; /* include </device> */

    char tmp_path[64];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
    FILE * out = fopen(tmp_path, "w");
    if (!out) return;

    /* Write up to dev_start */
    fwrite(fbuf, 1, (size_t)(dev_start - fbuf), out);
    /* Write new device XML */
    fputs(new_dev_xml, out);
    /* Write remainder of file */
    fputs(dev_end, out);
    fclose(out);

    rename(tmp_path, path);
    fprintf(stderr, "[sched] updated %s directly\n", path);
}

int schedule_save(void) {
    if (schedule_count <= 0) return -1;

    /* Ensure chronological order */
    qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);

    /* Recalculate end times to match the next switch point */
    for (int i = 0; i < schedule_count; i++) {
        int nxt = (i + 1) % schedule_count;
        schedule_entries[i].end_day  = schedule_entries[nxt].start_day;
        schedule_entries[i].end_hour = schedule_entries[nxt].start_hour;
        schedule_entries[i].end_min  = schedule_entries[nxt].start_min;
    }

    static char xml[32 * 1024];
    size_t xn = build_schedule_xml(xml, sizeof(xml));
    if (!xn) return -1;

    /* 1. Send via BoxTalk to hcb_config */
    int rc = boxtalk_set_object_config(xml);
    fprintf(stderr, "[sched] boxtalk_set_object_config sent (rc=%d)\n", rc);

    /* 2. Also update config file on disk for immediate persistence */
    update_file_config(xml);

    return 0;
}
