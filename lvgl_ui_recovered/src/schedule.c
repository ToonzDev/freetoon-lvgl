/*
 * Schedule parser + writer for happ_thermstat's thermostatProgram config.
 * Communicates via HTTP (port 10080) with hcb_config, with direct file fallback
 * to /qmf/config/config_happ_thermstat.xml (or /mnt/data/qmf/config/).
 *
 * NOTE on Day of Week:
 *   Both hcb_config and this UI use the same convention:
 *   0 = Mon, 1 = Tue, 2 = Wed, 3 = Thu, 4 = Fri, 5 = Sat, 6 = Sun
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

/* Parse entries from XML or JSON text.
 * NOTE: Parses into a temporary array first. Never wipes in-memory entries if
 * no entries were found in the buffer. */
static int parse_schedule_buffer(const char * buf) {
    if (!buf || !buf[0]) return -1;

    const char * sched_start = strstr(buf, "<schedule>");
    const char * sched_end   = sched_start ? strstr(sched_start, "</schedule>") : NULL;
    int is_xml = (sched_start && sched_end);

    if (!is_xml) {
        sched_start = strstr(buf, "\"schedule\"");
        if (!sched_start) sched_start = buf;
        sched_end = buf + strlen(buf);
    }

    schedule_entry_t tmp[SCHEDULE_MAX];
    int tmp_count = 0;
    const char * p = sched_start;
    while (p < sched_end && tmp_count < SCHEDULE_MAX) {
        const char * entry_start = NULL;
        const char * entry_end   = NULL;

        if (is_xml) {
            entry_start = strstr(p, "<entry");
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

        schedule_entry_t * e = &tmp[tmp_count++];
        e->start_day    = get_field_int(entry_start, entry_end, "startDayOfWeek", 0);
        e->end_day      = get_field_int(entry_start, entry_end, "endDayOfWeek",   0);
        e->start_min    = get_field_int(entry_start, entry_end, "startMin",    0);
        e->start_hour   = get_field_int(entry_start, entry_end, "startHour",   0);
        e->end_min      = get_field_int(entry_start, entry_end, "endMin",      0);
        e->end_hour     = get_field_int(entry_start, entry_end, "endHour",     0);
        e->target_state = get_field_int(entry_start, entry_end, "targetState", 0);
    }

    if (tmp_count > 0) {
        memcpy(schedule_entries, tmp, tmp_count * sizeof(schedule_entry_t));
        schedule_count = tmp_count;
        qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
        fprintf(stderr, "[sched] parsed %d entries\n", schedule_count);
        return 0;
    }
    return -1;
}

/* Low-level HTTP for hcb_config on port 10080 with 300ms socket timeout */
static int http_get_body(const char* host_path, const char* qs, char* out, size_t outsz) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 }; /* 300ms timeout */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(10080);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        close(s);
        return -1;
    }
    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET /%s?%s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        host_path, qs);
    if (send(s, req, n, 0) != n) {
        close(s);
        return -1;
    }
    char accum[32768];
    size_t acclen = 0;
    while (acclen < sizeof(accum) - 1) {
        ssize_t k = recv(s, accum + acclen, sizeof(accum) - 1 - acclen, 0);
        if (k <= 0) break;
        acclen += (size_t)k;
    }
    accum[acclen] = 0;
    close(s);
    const char* body = strstr(accum, "\r\n\r\n");
    if (body) body += 4; else body = accum;
    size_t blen = strlen(body);
    if (blen >= outsz) blen = outsz - 1;
    memcpy(out, body, blen);
    out[blen] = 0;
    return 0;
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
        "/mnt/data/qmf/config/config_happ_thermstat.xml",
        "/HCBv2/config/config_happ_thermstat.xml",
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
            if (tp) {
                const char * dev_start = tp;
                while (dev_start > file_buf && strncmp(dev_start, "<device", 7) != 0) dev_start--;
                if (strncmp(dev_start, "<device", 7) == 0) {
                    if (parse_schedule_buffer(dev_start) == 0) {
                        fprintf(stderr, "[sched] loaded %d entries from %s\n", schedule_count, paths[i]);
                        return 0;
                    }
                }
            }
        }
    }
    return -1;
}

int schedule_load(void) {
    /* 1. Try HTTP getObjectConfig from hcb_config (port 10080) */
    static char body[32 * 1024];
    if (http_get_body("hcb_config",
        "action=getObjectConfig&package=happ_thermstat&internalAddress=thermostatProgram",
        body, sizeof(body)) == 0) {
        if (parse_schedule_buffer(body) == 0) {
            fprintf(stderr, "[sched] loaded %d entries via HTTP hcb_config\n", schedule_count);
            return 0;
        }
    }

    /* 2. Fallback to local config file */
    if (read_file_config() == 0) return 0;

    /* 3. If no config loaded but entries already exist in memory, preserve them */
    if (schedule_count > 0) {
        return 0;
    }

    /* 4. First boot with no config found: seed clean defaults */
    seed_default_schedule();
    return 0;
}

/* Build the JSON payload for hcb_config setObjectConfig endpoint */
static size_t build_json(char * out, size_t outsz) {
    size_t off = 0;
    int n = snprintf(out + off, outsz - off, "{\"device\":[{\"schedule\":[");
    if (n < 0) return 0;
    off += (size_t)n;
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        n = snprintf(out + off, outsz - off,
            "%s{\"entry\":["
            "{\"type\":\"weekly_recurring\"},"
            "{\"startMin\":\"%d\"},{\"startHour\":\"%d\"},{\"startDayOfWeek\":\"%d\"},"
            "{\"targetState\":\"%d\"},"
            "{\"endMin\":\"%d\"},{\"endHour\":\"%d\"},{\"endDayOfWeek\":\"%d\"}"
            "]}",
            i ? "," : "",
            e->start_min, e->start_hour, e->start_day,
            e->target_state,
            e->end_min, e->end_hour, e->end_day);
        if (n < 0) return 0;
        off += (size_t)n;
        if (off >= outsz - 64) break;
    }
    n = snprintf(out + off, outsz - off, "]}]}");
    if (n < 0) return 0;
    off += (size_t)n;
    return off;
}

/* URL-encode a buffer into another. Out must be at least 3*in_len+1. */
static void url_encode(const char * in, size_t in_len, char * out) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%'; out[j++] = hex[c >> 4]; out[j++] = hex[c & 0xf];
        }
    }
    out[j] = 0;
}

/* Build XML payload for file persistence */
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
        n = snprintf(out + off, outsz - off,
            "<entry type=\"weekly_recurring\">"
            "<type>weekly_recurring</type>"
            "<startMin>%d</startMin>"
            "<startHour>%d</startHour>"
            "<startDayOfWeek>%d</startDayOfWeek>"
            "<targetState>%d</targetState>"
            "<endMin>%d</endMin>"
            "<endHour>%d</endHour>"
            "<endDayOfWeek>%d</endDayOfWeek>"
            "</entry>",
            e->start_min, e->start_hour, e->start_day,
            e->target_state,
            e->end_min, e->end_hour, e->end_day);
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
        "/mnt/data/qmf/config/config_happ_thermstat.xml",
        "/HCBv2/config/config_happ_thermstat.xml",
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

    char tmp_path[128];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
    FILE * out = fopen(tmp_path, "w");
    if (!out) {
        out = fopen(path, "w");
        if (!out) return;
        fwrite(fbuf, 1, (size_t)(dev_start - fbuf), out);
        fputs(new_dev_xml, out);
        fputs(dev_end, out);
        fclose(out);
        fprintf(stderr, "[sched] updated %s directly\n", path);
        return;
    }

    /* Write up to dev_start */
    fwrite(fbuf, 1, (size_t)(dev_start - fbuf), out);
    /* Write new device XML */
    fputs(new_dev_xml, out);
    /* Write remainder of file */
    fputs(dev_end, out);
    fclose(out);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
    } else {
        fprintf(stderr, "[sched] updated %s via rename\n", path);
    }
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

    /* 1. Send via HTTP to hcb_config on port 10080 */
    static char json[16 * 1024];
    size_t jn = build_json(json, sizeof(json));
    if (jn > 0) {
        static char encoded[48 * 1024];
        url_encode(json, jn, encoded);

        static char qs[52 * 1024];
        snprintf(qs, sizeof(qs),
            "action=setObjectConfig&package=happ_thermstat&internalAddress=thermostatProgram&config=%s",
            encoded);

        static char body[1024];
        int rc = http_get_body("hcb_config", qs, body, sizeof(body));
        fprintf(stderr, "[sched] http setObjectConfig rc=%d\n", rc);
    }

    /* 2. Also update config file on disk for immediate persistence */
    static char xml[32 * 1024];
    size_t xn = build_schedule_xml(xml, sizeof(xml));
    if (xn > 0) {
        update_file_config(xml);
    }

    return 0;
}
