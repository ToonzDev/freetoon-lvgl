/*
 * sonos.c — Background poller and transport controller for Sonos HTTP API.
 * Compatible with node-sonos-http-api (same API used by Toon's legacy Sonos QML app).
 */
#define _GNU_SOURCE
#include "sonos.h"
#include "settings.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

sonos_state_t sonos_state = {0};

static pthread_mutex_t g_sonos_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_sonos_tid = 0;
static volatile int    g_force_poll = 0;

void sonos_get_snapshot(sonos_state_t * out) {
    if (!out) return;
    pthread_mutex_lock(&g_sonos_mtx);
    *out = sonos_state;
    pthread_mutex_unlock(&g_sonos_mtx);
}

/* Encode string for URL path (e.g. spaces to %20) */
static void url_encode(const char * src, char * dst, size_t dst_sz) {
    if (!src || !dst || dst_sz == 0) return;
    size_t o = 0;
    for (const char * p = src; *p && o + 4 < dst_sz; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = c;
        } else {
            o += snprintf(dst + o, dst_sz - o, "%%%02X", c);
        }
    }
    dst[o] = 0;
}

static int extract_json_str(const char * src, const char * key, char * out, size_t outsz) {
    if (!src || !key || !out || outsz == 0) return 0;
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char * p = strstr(src, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

static int extract_json_int(const char * src, const char * key, int * out) {
    if (!src || !key || !out) return 0;
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char * p = strstr(src, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '"') p++;
    *out = atoi(p);
    return 1;
}

int sonos_probe(char zones[][48], int max_zones) {
    if (!settings.sonos_host[0]) return -1;
    int port = settings.sonos_port > 0 ? settings.sonos_port : 5005;
    char url[160];
    snprintf(url, sizeof url, "http://%s:%d/zones", settings.sonos_host, port);

    char * buf = malloc(32768);
    if (!buf) return -1;
    buf[0] = 0;

    int rc = http_fetch_to(url, buf, 32768, 3);
    if (rc != 0 || buf[0] == 0) {
        free(buf);
        return -1;
    }

    int count = 0;
    const char * cur = buf;
    while ((cur = strstr(cur, "\"coordinator\"")) != NULL && count < max_zones) {
        const char * rn = strstr(cur, "\"roomName\"");
        if (!rn) break;
        rn = strchr(rn, ':');
        if (!rn) break;
        rn++;
        while (*rn == ' ' || *rn == '\t') rn++;
        if (*rn == '"') {
            rn++;
            char room[48];
            size_t o = 0;
            while (*rn && *rn != '"' && o + 1 < sizeof room) {
                if (*rn == '\\' && rn[1]) rn++;
                room[o++] = *rn++;
            }
            room[o] = 0;
            if (room[0]) {
                /* Deduplicate */
                int dup = 0;
                for (int i = 0; i < count; i++) {
                    if (strcmp(zones[i], room) == 0) { dup = 1; break; }
                }
                if (!dup) {
                    snprintf(zones[count], 48, "%s", room);
                    count++;
                }
            }
        }
        cur = rn;
    }

    free(buf);
    return count;
}

static void poll_state(void) {
    if (!settings.sonos_enabled || !settings.sonos_host[0]) {
        pthread_mutex_lock(&g_sonos_mtx);
        sonos_state.connected = 0;
        pthread_mutex_unlock(&g_sonos_mtx);
        return;
    }

    /* Auto-select first zone if none configured */
    if (!settings.sonos_zone[0]) {
        char rooms[SONOS_MAX_ZONES][48];
        int n = sonos_probe(rooms, SONOS_MAX_ZONES);
        if (n > 0) {
            snprintf(settings.sonos_zone, sizeof settings.sonos_zone, "%s", rooms[0]);
            settings_save();
        } else {
            pthread_mutex_lock(&g_sonos_mtx);
            sonos_state.connected = 0;
            pthread_mutex_unlock(&g_sonos_mtx);
            return;
        }
    }

    int port = settings.sonos_port > 0 ? settings.sonos_port : 5005;
    char enc_zone[96];
    url_encode(settings.sonos_zone, enc_zone, sizeof enc_zone);

    char url[256];
    snprintf(url, sizeof url, "http://%s:%d/%s/state", settings.sonos_host, port, enc_zone);

    char buf[8192];
    buf[0] = 0;
    int rc = http_fetch_to(url, buf, sizeof buf, 2);
    if (rc != 0 || buf[0] == 0) {
        pthread_mutex_lock(&g_sonos_mtx);
        sonos_state.connected = 0;
        pthread_mutex_unlock(&g_sonos_mtx);
        return;
    }

    sonos_state_t st;
    memset(&st, 0, sizeof st);
    st.connected = 1;
    snprintf(st.zone, sizeof st.zone, "%s", settings.sonos_zone);

    char playstate[32] = "";
    if (extract_json_str(buf, "playbackState", playstate, sizeof playstate)) {
        st.playing = (strcmp(playstate, "PLAYING") == 0);
    }

    int vol = 0;
    if (extract_json_int(buf, "volume", &vol)) {
        st.volume = vol;
    }

    int elapsed = 0;
    if (extract_json_int(buf, "elapsedTime", &elapsed)) {
        st.elapsed = elapsed;
    }

    const char * track = strstr(buf, "\"currentTrack\"");
    if (track) {
        char type[24] = "";
        extract_json_str(track, "type", type, sizeof type);
        if (strcmp(type, "radio") == 0) {
            st.is_radio = 1;
            char st_name[64] = "";
            extract_json_str(track, "stationName", st_name, sizeof st_name);
            snprintf(st.artist, sizeof st.artist, "%s", st_name);
            extract_json_str(track, "title", st.title, sizeof st.title);
        } else {
            st.is_radio = 0;
            extract_json_str(track, "artist", st.artist, sizeof st.artist);
            extract_json_str(track, "title", st.title, sizeof st.title);
            extract_json_str(track, "album", st.album, sizeof st.album);
            int dur = 0;
            if (extract_json_int(track, "duration", &dur)) st.duration = dur;
        }
        /* Filter internal URI scheme string if shown as title */
        if (strncmp(st.title, "x-sonosapi", 10) == 0) st.title[0] = 0;
    }

    /* Check group */
    const char * members = strstr(buf, "\"members\"");
    if (members) {
        int cnt = 0;
        const char * m = members;
        while ((m = strstr(m, "\"roomName\"")) != NULL) { cnt++; m += 10; }
        st.is_group = (cnt > 1);
    }

    pthread_mutex_lock(&g_sonos_mtx);
    sonos_state = st;
    pthread_mutex_unlock(&g_sonos_mtx);
}

static void * sonos_worker(void * arg) {
    (void)arg;
    while (1) {
        poll_state();
        for (int i = 0; i < 20; i++) {
            if (g_force_poll) {
                g_force_poll = 0;
                break;
            }
            usleep(100000); /* 100 ms slices = 2 s total */
        }
    }
    return NULL;
}

int sonos_start(void) {
    if (g_sonos_tid) return 0;
    if (pthread_create(&g_sonos_tid, NULL, sonos_worker, NULL) != 0) {
        g_sonos_tid = 0;
        return -1;
    }
    pthread_detach(g_sonos_tid);
    return 0;
}

/* Fire-and-forget command */
typedef struct {
    char url[256];
} cmd_arg_t;

static void * sonos_cmd_worker(void * arg) {
    cmd_arg_t * ca = (cmd_arg_t *)arg;
    char dummy[512];
    http_fetch_to(ca->url, dummy, sizeof dummy, 2);
    free(ca);
    usleep(150000); /* wait 150ms for Sonos to apply */
    g_force_poll = 1; /* trigger immediate state update */
    return NULL;
}

static void send_cmd(const char * cmd) {
    if (!settings.sonos_enabled || !settings.sonos_host[0] || !settings.sonos_zone[0]) return;
    int port = settings.sonos_port > 0 ? settings.sonos_port : 5005;
    char enc_zone[96];
    url_encode(settings.sonos_zone, enc_zone, sizeof enc_zone);

    cmd_arg_t * ca = malloc(sizeof *ca);
    if (!ca) return;
    snprintf(ca->url, sizeof ca->url, "http://%s:%d/%s/%s", settings.sonos_host, port, enc_zone, cmd);

    pthread_t t;
    if (pthread_create(&t, NULL, sonos_cmd_worker, ca) == 0) {
        pthread_detach(t);
    } else {
        free(ca);
    }
}

void sonos_play(void) {
    pthread_mutex_lock(&g_sonos_mtx);
    sonos_state.playing = 1;
    pthread_mutex_unlock(&g_sonos_mtx);
    send_cmd("play");
}

void sonos_pause(void) {
    pthread_mutex_lock(&g_sonos_mtx);
    sonos_state.playing = 0;
    pthread_mutex_unlock(&g_sonos_mtx);
    send_cmd("pause");
}

void sonos_play_pause(void) {
    int p;
    pthread_mutex_lock(&g_sonos_mtx);
    p = sonos_state.playing;
    sonos_state.playing = !p;
    pthread_mutex_unlock(&g_sonos_mtx);
    send_cmd(p ? "pause" : "play");
}

void sonos_next(void) {
    send_cmd("next");
}

void sonos_prev(void) {
    send_cmd("previous");
}

void sonos_volume_up(void) {
    pthread_mutex_lock(&g_sonos_mtx);
    int grp = sonos_state.is_group;
    if (sonos_state.volume <= 98) sonos_state.volume += 2;
    pthread_mutex_unlock(&g_sonos_mtx);
    send_cmd(grp ? "groupVolume/+2" : "volume/+2");
}

void sonos_volume_down(void) {
    pthread_mutex_lock(&g_sonos_mtx);
    int grp = sonos_state.is_group;
    if (sonos_state.volume >= 2) sonos_state.volume -= 2;
    pthread_mutex_unlock(&g_sonos_mtx);
    send_cmd(grp ? "groupVolume/-2" : "volume/-2");
}

void sonos_set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    pthread_mutex_lock(&g_sonos_mtx);
    int grp = sonos_state.is_group;
    sonos_state.volume = vol;
    pthread_mutex_unlock(&g_sonos_mtx);
    char cmd[32];
    snprintf(cmd, sizeof cmd, "%s/%d", grp ? "groupVolume" : "volume", vol);
    send_cmd(cmd);
}

void sonos_set_zone(const char * zone) {
    if (!zone || !zone[0]) return;
    snprintf(settings.sonos_zone, sizeof settings.sonos_zone, "%s", zone);
    settings_save();
    g_force_poll = 1;
}

int sonos_get_favorites(char favs[][64], int max_favs) {
    if (!settings.sonos_host[0] || max_favs <= 0) return 0;
    int port = settings.sonos_port > 0 ? settings.sonos_port : 5005;
    char enc_zone[96] = "";
    if (settings.sonos_zone[0]) url_encode(settings.sonos_zone, enc_zone, sizeof enc_zone);
    char url[256];
    if (enc_zone[0])
        snprintf(url, sizeof url, "http://%s:%d/%s/favorites", settings.sonos_host, port, enc_zone);
    else
        snprintf(url, sizeof url, "http://%s:%d/favorites", settings.sonos_host, port);

    char buf[16384];
    buf[0] = 0;
    int rc = http_fetch_to(url, buf, sizeof buf, 3);
    if (rc != 0 || buf[0] == 0) {
        snprintf(url, sizeof url, "http://%s:%d/favorites", settings.sonos_host, port);
        rc = http_fetch_to(url, buf, sizeof buf, 3);
        if (rc != 0 || buf[0] == 0) return 0;
    }

    int count = 0;
    char * p = strchr(buf, '[');
    if (!p) return 0;
    p++;
    while (*p && *p != ']' && count < max_favs) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == ']') break;
        if (*p == '"') {
            p++;
            char * q = strchr(p, '"');
            if (!q) break;
            size_t len = q - p;
            if (len >= 64) len = 63;
            memcpy(favs[count], p, len);
            favs[count][len] = 0;
            count++;
            p = q + 1;
        } else if (*p == '{') {
            char * end_obj = strchr(p, '}');
            if (!end_obj) break;
            char * t = strstr(p, "\"title\"");
            if (t && t < end_obj) {
                t = strchr(t, ':');
                if (t && t < end_obj) {
                    t = strchr(t, '"');
                    if (t && t < end_obj) {
                        t++;
                        char * q = strchr(t, '"');
                        if (q && q <= end_obj) {
                            size_t len = q - t;
                            if (len >= 64) len = 63;
                            memcpy(favs[count], t, len);
                            favs[count][len] = 0;
                            count++;
                        }
                    }
                }
            }
            p = end_obj + 1;
        } else {
            p++;
        }
    }
    return count;
}

void sonos_play_favorite(const char * name) {
    if (!name || !name[0]) return;
    char enc[128];
    url_encode(name, enc, sizeof enc);
    char cmd[160];
    snprintf(cmd, sizeof cmd, "favorite/%s", enc);
    send_cmd(cmd);
}

void sonos_play_playlist(const char * name) {
    if (!name || !name[0]) return;
    char enc[128];
    url_encode(name, enc, sizeof enc);
    char cmd[160];
    snprintf(cmd, sizeof cmd, "playlist/%s", enc);
    send_cmd(cmd);
}

