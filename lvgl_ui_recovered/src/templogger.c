/*
 * templogger.c — Background continuous temperature logger service.
 * Records 3 temperatures every 10 minutes (144 points across a 24-hour day):
 *   1. Room temperature (toon_state.indoor_temp)
 *   2. Room setpoint / program temperature (toon_state.setpoint)
 *   3. Outside temperature (weather_display_temp() / buienradar fallback)
 *
 * Persists data to /HCBv2/var/ (symlink to /mnt/data/qmf/var/) in the exact
 * format used by the legacy QML temperaturelogger app, ensuring 100% data
 * compatibility across restarts and firmware switches.
 */

#include "templogger.h"
#include "boxtalk.h"     /* toon_state */
#include "weather.h"     /* weather_display_temp() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

static float g_room[TEMPLOGGER_POINTS];
static float g_sp[TEMPLOGGER_POINTS];
static float g_out[TEMPLOGGER_POINTS];

static int   g_cur_idx = 0;
static float g_last_room = TEMPLOGGER_INVALID_VAL;
static float g_last_sp = TEMPLOGGER_INVALID_VAL;
static float g_last_out = TEMPLOGGER_INVALID_VAL;
static long  g_last_time = 0;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static const char * g_data_dir = NULL;

static const char * resolve_data_dir(void) {
    if (g_data_dir) return g_data_dir;
    if (access("/HCBv2/var", W_OK) == 0) {
        g_data_dir = "/HCBv2/var";
    } else if (access("/mnt/data/qmf/var", W_OK) == 0) {
        g_data_dir = "/mnt/data/qmf/var";
    } else {
        g_data_dir = "/mnt/data";
    }
    return g_data_dir;
}

/* Parse comma-separated float values into array */
static int read_csv_floats(const char * path, float * arr, int max_count, int offset) {
    FILE * f = fopen(path, "r");
    if (!f) return 0;

    char line[4096];
    size_t rd = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    if (rd == 0) return 0;
    line[rd] = '\0';

    int count = 0;
    char * ptr = line;
    while (*ptr && count < max_count) {
        while (*ptr == ' ' || *ptr == '\n' || *ptr == '\r' || *ptr == '\t') ptr++;
        if (!*ptr) break;

        char * endp = NULL;
        float v = strtof(ptr, &endp);
        if (endp == ptr) break;

        if (v <= -25.0f) {
            arr[count] = TEMPLOGGER_INVALID_VAL;
        } else {
            /* If legacy file had a non-zero offset: real_temp = stored - offset */
            arr[count] = v - (float)offset;
        }
        count++;

        ptr = endp;
        while (*ptr == ' ' || *ptr == ',') ptr++;
    }
    return count;
}

static void save_csv_floats(const char * path, const float * arr, int count) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE * f = fopen(tmp, "w");
    if (!f) return;

    for (int i = 0; i < count; i++) {
        if (arr[i] <= -25.0f || arr[i] == TEMPLOGGER_INVALID_VAL) {
            fprintf(f, "%s-30", i > 0 ? "," : "");
        } else {
            fprintf(f, "%s%.2f", i > 0 ? "," : "", arr[i]);
        }
    }
    fprintf(f, "\n");
    fclose(f);
    rename(tmp, path);
}

static float read_buienradar_file_temp(void) {
    const char * cand[] = {
        "/var/volatile/tmp/actualBuienradarTemp.txt",
        "/tmp/actualBuienradarTemp.txt",
        NULL
    };
    for (int i = 0; cand[i]; i++) {
        FILE * f = fopen(cand[i], "r");
        if (f) {
            char buf[64];
            if (fgets(buf, sizeof(buf), f)) {
                fclose(f);
                float val = 0.0f;
                if (sscanf(buf, "%f", &val) == 1 && val > -50.0f && val < 60.0f) {
                    return val;
                }
            } else {
                fclose(f);
            }
        }
    }
    return TEMPLOGGER_INVALID_VAL;
}

static void load_history(void) {
    const char * dir = resolve_data_dir();
    char path[256];

    /* Initialize with sentinel */
    for (int i = 0; i < TEMPLOGGER_POINTS; i++) {
        g_room[i] = TEMPLOGGER_INVALID_VAL;
        g_sp[i]   = TEMPLOGGER_INVALID_VAL;
        g_out[i]  = TEMPLOGGER_INVALID_VAL;
    }

    /* Check legacy offset */
    int offset = 0;
    snprintf(path, sizeof(path), "%s/actualTempOffset.txt", dir);
    FILE * foff = fopen(path, "r");
    if (foff) {
        char buf[32];
        if (fgets(buf, sizeof(buf), foff)) {
            offset = atoi(buf);
        }
        fclose(foff);
    }

    snprintf(path, sizeof(path), "%s/actualTempReading24H.txt", dir);
    read_csv_floats(path, g_room, TEMPLOGGER_POINTS, offset);

    snprintf(path, sizeof(path), "%s/actualTempSetpoint24H.txt", dir);
    read_csv_floats(path, g_sp, TEMPLOGGER_POINTS, offset);

    snprintf(path, sizeof(path), "%s/actualTempBuienradar24H.txt", dir);
    read_csv_floats(path, g_out, TEMPLOGGER_POINTS, offset);

    fprintf(stderr, "[templogger] loaded 24h history from %s (legacy offset=%d)\n", dir, offset);
}

static void save_history(void) {
    const char * dir = resolve_data_dir();
    char path[256];

    snprintf(path, sizeof(path), "%s/actualTempReading24H.txt", dir);
    save_csv_floats(path, g_room, TEMPLOGGER_POINTS);

    snprintf(path, sizeof(path), "%s/actualTempSetpoint24H.txt", dir);
    save_csv_floats(path, g_sp, TEMPLOGGER_POINTS);

    snprintf(path, sizeof(path), "%s/actualTempBuienradar24H.txt", dir);
    save_csv_floats(path, g_out, TEMPLOGGER_POINTS);

    /* We write clean true Celsius degrees, so offset is 0 */
    snprintf(path, sizeof(path), "%s/actualTempOffset.txt", dir);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * fo = fopen(tmp, "w");
    if (fo) {
        fprintf(fo, "0\n");
        fclose(fo);
        rename(tmp, path);
    }
}

static void do_sample(int idx) {
    if (idx < 0 || idx >= TEMPLOGGER_POINTS) return;

    float r = toon_state.indoor_temp;
    float sp = toon_state.setpoint;
    float out = weather_display_temp();

    if (!weather_state.connected || out <= -50.0f || out >= 60.0f) {
        float f_out = read_buienradar_file_temp();
        if (f_out != TEMPLOGGER_INVALID_VAL) out = f_out;
    }

    pthread_mutex_lock(&g_mtx);
    if (r > 0.0f && r < 50.0f) {
        g_room[idx] = r;
        g_last_room = r;
    }
    if (sp > 0.0f && sp < 40.0f) {
        g_sp[idx] = sp;
        g_last_sp = sp;
    }
    if (out > -50.0f && out < 60.0f) {
        g_out[idx] = out;
        g_last_out = out;
    }
    g_cur_idx = idx;
    g_last_time = (long)time(NULL);

    save_history();
    pthread_mutex_unlock(&g_mtx);

    fprintf(stderr, "[templogger] sample slot %d: room=%.1f sp=%.1f out=%.1f\n",
            idx, g_last_room, g_last_sp, g_last_out);
}

static void * templogger_thread(void * arg) {
    (void)arg;
    load_history();

    /* Warm-up sleep to let BoxTalk and weather threads populate */
    sleep(5);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int last_idx = (tm.tm_hour * 6) + (tm.tm_min / 10);
    do_sample(last_idx);

    for (;;) {
        sleep(10);
        now = time(NULL);
        localtime_r(&now, &tm);
        int cur_idx = (tm.tm_hour * 6) + (tm.tm_min / 10);

        if (cur_idx != last_idx) {
            do_sample(cur_idx);
            last_idx = cur_idx;
        } else {
            /* Update instantaneous values */
            pthread_mutex_lock(&g_mtx);
            if (toon_state.indoor_temp > 0.0f) g_last_room = toon_state.indoor_temp;
            if (toon_state.setpoint > 0.0f)    g_last_sp = toon_state.setpoint;
            float out = weather_display_temp();
            if (out > -50.0f && out < 60.0f)   g_last_out = out;
            g_cur_idx = cur_idx;
            pthread_mutex_unlock(&g_mtx);
        }
    }
    return NULL;
}

void templogger_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, templogger_thread, NULL) == 0) {
        pthread_detach(th);
    }
}

int templogger_get_snapshot(templogger_snapshot_t * out) {
    if (!out) return -1;
    pthread_mutex_lock(&g_mtx);
    memcpy(out->room,     g_room, sizeof(g_room));
    memcpy(out->setpoint, g_sp,   sizeof(g_sp));
    memcpy(out->outside,  g_out,  sizeof(g_out));
    out->cur_idx          = g_cur_idx;
    out->latest_room      = g_last_room;
    out->latest_setpoint  = g_last_sp;
    out->latest_outside   = g_last_out;
    out->last_sample_time = g_last_time;
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

void templogger_get_latest(float * room, float * sp, float * out) {
    pthread_mutex_lock(&g_mtx);
    if (room) *room = (g_last_room != TEMPLOGGER_INVALID_VAL) ? g_last_room : toon_state.indoor_temp;
    if (sp)   *sp   = (g_last_sp   != TEMPLOGGER_INVALID_VAL) ? g_last_sp   : toon_state.setpoint;
    if (out)  *out  = (g_last_out  != TEMPLOGGER_INVALID_VAL) ? g_last_out  : weather_display_temp();
    pthread_mutex_unlock(&g_mtx);
}
