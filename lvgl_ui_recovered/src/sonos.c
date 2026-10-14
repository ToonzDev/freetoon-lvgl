/*
 * sonos.c — Background poller and transport controller for Sonos HTTP API.
 * Compatible with node-sonos-http-api (same API used by Toon's legacy Sonos QML app).
 */
#include "sonos.h"
#include "settings.h"
#include "http.h"
#include "display.h"
#define LV_USE_SJPG 1
#include "lvgl/src/extra/libs/sjpg/tjpgd.h"
#include "lvgl/src/extra/libs/png/lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

sonos_state_t sonos_state = {0};

static pthread_mutex_t g_sonos_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_sonos_tid = 0;
static volatile int    g_force_poll = 0;

static char            g_cur_art_url[256] = "";
static volatile int    g_art_version = 0;
static volatile int    g_art_downloading = 0;

#define ART_BIG_DIM   SUNI(130)
#define ART_TILE_DIM  SUNI(46)

static lv_color_t g_art_big_buf[ART_BIG_DIM * ART_BIG_DIM];
static lv_color_t g_art_tile_buf[ART_TILE_DIM * ART_TILE_DIM];
static int        g_art_has_image = 0;

static const lv_img_dsc_t g_art_big_dsc = {
    .header.always_zero = 0,
    .header.w = ART_BIG_DIM,
    .header.h = ART_BIG_DIM,
    .data_size = sizeof(g_art_big_buf),
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data = (const uint8_t *)g_art_big_buf,
};

static const lv_img_dsc_t g_art_tile_dsc = {
    .header.always_zero = 0,
    .header.w = ART_TILE_DIM,
    .header.h = ART_TILE_DIM,
    .data_size = sizeof(g_art_tile_buf),
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data = (const uint8_t *)g_art_tile_buf,
};

void sonos_get_snapshot(sonos_state_t * out) {
    if (!out) return;
    pthread_mutex_lock(&g_sonos_mtx);
    *out = sonos_state;
    pthread_mutex_unlock(&g_sonos_mtx);
}

const lv_img_dsc_t * sonos_get_art_big(int * out_ver) {
    pthread_mutex_lock(&g_sonos_mtx);
    if (out_ver) *out_ver = g_art_version;
    const lv_img_dsc_t * ret = g_art_has_image ? &g_art_big_dsc : NULL;
    pthread_mutex_unlock(&g_sonos_mtx);
    return ret;
}

const lv_img_dsc_t * sonos_get_art_tile(int * out_ver) {
    pthread_mutex_lock(&g_sonos_mtx);
    if (out_ver) *out_ver = g_art_version;
    const lv_img_dsc_t * ret = g_art_has_image ? &g_art_tile_dsc : NULL;
    pthread_mutex_unlock(&g_sonos_mtx);
    return ret;
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

typedef struct {
    char url[256];
} art_req_t;

typedef struct {
    const uint8_t * data;
    size_t size;
    size_t pos;
    uint8_t * rgb_buf;
    int w, h;
} jdec_session_t;

static size_t jdec_infunc(JDEC * jd, uint8_t * buff, size_t ndata) {
    jdec_session_t * s = (jdec_session_t *)jd->device;
    size_t left = s->size > s->pos ? s->size - s->pos : 0;
    size_t to_read = ndata < left ? ndata : left;
    if (buff && to_read > 0) {
        memcpy(buff, s->data + s->pos, to_read);
    }
    s->pos += to_read;
    return to_read;
}

static int jdec_outfunc(JDEC * jd, void * bitmap, JRECT * rect) {
    jdec_session_t * s = (jdec_session_t *)jd->device;
    uint8_t * src = (uint8_t *)bitmap;
    int rw = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; y++) {
        uint8_t * dst = s->rgb_buf + (y * s->w + rect->left) * 3;
        memcpy(dst, src, rw * 3);
        src += rw * 3;
    }
    return 1;
}

static void resample_to_lv_color(const uint8_t * src, int src_w, int src_h, int channels,
                                 lv_color_t * dst, int dst_w, int dst_h) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    int src_stride = src_w * channels;

    int crop_x = 0, crop_y = 0, crop_w = src_w, crop_h = src_h;
    if (src_w > src_h) {
        crop_w = src_h;
        crop_x = (src_w - src_h) / 2;
    } else if (src_h > src_w) {
        crop_h = src_w;
        crop_y = (src_h - src_w) / 2;
    }

    for (int dy = 0; dy < dst_h; dy++) {
        int sy16 = (dy * crop_h * 65536) / dst_h;
        int y0 = crop_y + (sy16 >> 16);
        if (y0 >= src_h) y0 = src_h - 1;
        int y1 = (y0 + 1 < crop_y + crop_h && y0 + 1 < src_h) ? y0 + 1 : y0;
        int y_frac = (sy16 >> 8) & 0xff;

        for (int dx = 0; dx < dst_w; dx++) {
            int sx16 = (dx * crop_w * 65536) / dst_w;
            int x0 = crop_x + (sx16 >> 16);
            if (x0 >= src_w) x0 = src_w - 1;
            int x1 = (x0 + 1 < crop_x + crop_w && x0 + 1 < src_w) ? x0 + 1 : x0;
            int x_frac = (sx16 >> 8) & 0xff;

            int w00 = (256 - x_frac) * (256 - y_frac);
            int w10 = x_frac * (256 - y_frac);
            int w01 = (256 - x_frac) * y_frac;
            int w11 = x_frac * y_frac;

            const uint8_t * p00 = src + y0 * src_stride + x0 * channels;
            const uint8_t * p10 = src + y0 * src_stride + x1 * channels;
            const uint8_t * p01 = src + y1 * src_stride + x0 * channels;
            const uint8_t * p11 = src + y1 * src_stride + x1 * channels;

            uint8_t r = (p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11 + 32768) >> 16;
            uint8_t g = (p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11 + 32768) >> 16;
            uint8_t b = (p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11 + 32768) >> 16;

            dst[dy * dst_w + dx] = lv_color_make(r, g, b);
        }
    }
}

static void * art_download_worker(void * arg) {
    art_req_t * req = (art_req_t *)arg;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "/tmp/sonos_art_dl.tmp");
    char safe_url[256];
    size_t si = 0;
    for (size_t i = 0; req->url[i] && si + 4 < sizeof safe_url; i++) {
        if (req->url[i] == '\'') {
            safe_url[si++] = '%'; safe_url[si++] = '2'; safe_url[si++] = '7';
        } else {
            safe_url[si++] = req->url[i];
        }
    }
    safe_url[si] = 0;

    char cmd[512];
    snprintf(cmd, sizeof cmd, "curl -s -L --connect-timeout 3 --max-time 6 -o %s '%s' 2>/dev/null", tmp, safe_url);
    int rc = system(cmd);
    bool ok = false;
    lv_color_t * tmp_big = malloc(sizeof(g_art_big_buf));
    lv_color_t * tmp_tile = malloc(sizeof(g_art_tile_buf));

    if (rc == 0 && tmp_big && tmp_tile) {
        FILE * f = fopen(tmp, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fsz > 4 && fsz < 5 * 1024 * 1024) {
                uint8_t * fbuf = malloc(fsz);
                if (fbuf) {
                    if (fread(fbuf, 1, fsz, f) == (size_t)fsz) {
                        if (fbuf[0] == 0xFF && fbuf[1] == 0xD8) {
                            /* JPEG */
                            void * work = malloc(4096);
                            if (work) {
                                jdec_session_t s = {
                                    .data = fbuf,
                                    .size = (size_t)fsz,
                                    .pos = 0,
                                    .rgb_buf = NULL,
                                    .w = 0, .h = 0
                                };
                                JDEC jd;
                                if (jd_prepare(&jd, jdec_infunc, work, 4096, &s) == JDR_OK) {
                                    int min_dim = (jd.width < jd.height) ? jd.width : jd.height;
                                    uint8_t scale = 0;
                                    if ((min_dim >> 2) >= ART_BIG_DIM) scale = 2;
                                    else if ((min_dim >> 1) >= ART_BIG_DIM) scale = 1;

                                    int out_w = jd.width >> scale;
                                    int out_h = jd.height >> scale;
                                    s.w = out_w;
                                    s.h = out_h;
                                    s.rgb_buf = malloc(out_w * out_h * 3);
                                    if (s.rgb_buf) {
                                        if (jd_decomp(&jd, jdec_outfunc, scale) == JDR_OK) {
                                            resample_to_lv_color(s.rgb_buf, out_w, out_h, 3, tmp_big, ART_BIG_DIM, ART_BIG_DIM);
                                            resample_to_lv_color(s.rgb_buf, out_w, out_h, 3, tmp_tile, ART_TILE_DIM, ART_TILE_DIM);
                                            ok = true;
                                        }
                                        free(s.rgb_buf);
                                    }
                                }
                                free(work);
                            }
                        } else if (fbuf[0] == 0x89 && fbuf[1] == 'P' && fbuf[2] == 'N' && fbuf[3] == 'G') {
                            /* PNG */
                            unsigned char * png_rgba = NULL;
                            unsigned int pw = 0, ph = 0;
                            if (lodepng_decode32(&png_rgba, &pw, &ph, fbuf, fsz) == 0 && png_rgba && pw > 0 && ph > 0) {
                                resample_to_lv_color(png_rgba, pw, ph, 4, tmp_big, ART_BIG_DIM, ART_BIG_DIM);
                                resample_to_lv_color(png_rgba, pw, ph, 4, tmp_tile, ART_TILE_DIM, ART_TILE_DIM);
                                ok = true;
                                free(png_rgba);
                            }
                        }
                    }
                    free(fbuf);
                }
            }
            fclose(f);
        }
    }
    unlink(tmp);

    pthread_mutex_lock(&g_sonos_mtx);
    if (ok) {
        memcpy(g_art_big_buf, tmp_big, sizeof(g_art_big_buf));
        memcpy(g_art_tile_buf, tmp_tile, sizeof(g_art_tile_buf));
        g_art_has_image = 1;
        g_art_version++;
    } else {
        g_art_has_image = 0;
        g_cur_art_url[0] = 0;
        g_art_version++;
    }
    pthread_mutex_unlock(&g_sonos_mtx);

    free(tmp_big);
    free(tmp_tile);
    free(req);
    g_art_downloading = 0;
    return NULL;
}

static void check_trigger_art_download(const char * new_url) {
    if (!new_url || !new_url[0]) {
        pthread_mutex_lock(&g_sonos_mtx);
        if (g_art_has_image || g_cur_art_url[0]) {
            g_art_has_image = 0;
            g_cur_art_url[0] = 0;
            g_art_version++;
        }
        pthread_mutex_unlock(&g_sonos_mtx);
        return;
    }
    if (strcmp(new_url, g_cur_art_url) == 0) return; /* already fetched or fetching */

    if (g_art_downloading) return; /* busy */
    g_art_downloading = 1;
    snprintf(g_cur_art_url, sizeof g_cur_art_url, "%s", new_url);

    art_req_t * req = malloc(sizeof *req);
    if (!req) { g_art_downloading = 0; return; }
    snprintf(req->url, sizeof req->url, "%s", new_url);
    pthread_t tid;
    if (pthread_create(&tid, NULL, art_download_worker, req) == 0) {
        pthread_detach(tid);
    } else {
        free(req);
        g_art_downloading = 0;
    }
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

        /* Extract album art URL */
        char art[256] = "";
        if (extract_json_str(track, "absoluteAlbumArtUri", art, sizeof art) && art[0]) {
            snprintf(st.art_url, sizeof st.art_url, "%s", art);
        } else if (extract_json_str(track, "albumArtUri", art, sizeof art) && art[0]) {
            if (strncmp(art, "http", 4) == 0) {
                snprintf(st.art_url, sizeof st.art_url, "%s", art);
            } else {
                int port = settings.sonos_port > 0 ? settings.sonos_port : 5005;
                snprintf(st.art_url, sizeof st.art_url, "http://%s:%d%s", settings.sonos_host, port, art);
            }
        }
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

    check_trigger_art_download(st.art_url);
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

void sonos_seek(int seconds) {
    if (seconds < 0) seconds = 0;
    pthread_mutex_lock(&g_sonos_mtx);
    sonos_state.elapsed = seconds;
    pthread_mutex_unlock(&g_sonos_mtx);
    char cmd[32];
    snprintf(cmd, sizeof cmd, "timeseek/%d", seconds);
    send_cmd(cmd);
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

static int get_sonos_playlists(sonos_item_t items[], int max_items) {
    if (!settings.sonos_host[0] || max_items <= 0) return 0;
    int port = settings.sonos_port > 0 ? settings.sonos_port : 5005;
    char enc_zone[96] = "";
    if (settings.sonos_zone[0]) url_encode(settings.sonos_zone, enc_zone, sizeof enc_zone);
    char url[256];
    if (enc_zone[0])
        snprintf(url, sizeof url, "http://%s:%d/%s/playlists", settings.sonos_host, port, enc_zone);
    else
        snprintf(url, sizeof url, "http://%s:%d/playlists", settings.sonos_host, port);

    char buf[16384];
    buf[0] = 0;
    int rc = http_fetch_to(url, buf, sizeof buf, 3);
    if (rc != 0 || buf[0] == 0) {
        snprintf(url, sizeof url, "http://%s:%d/playlists", settings.sonos_host, port);
        rc = http_fetch_to(url, buf, sizeof buf, 3);
        if (rc != 0 || buf[0] == 0) return 0;
    }

    int count = 0;
    char * p = strchr(buf, '[');
    if (!p) return 0;
    p++;
    while (*p && *p != ']' && count < max_items) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == ']') break;
        if (*p == '"') {
            p++;
            char * q = strchr(p, '"');
            if (!q) break;
            size_t len = (size_t)(q - p);
            if (len >= sizeof items[0].name) len = sizeof items[0].name - 1;
            memcpy(items[count].name, p, len);
            items[count].name[len] = 0;
            items[count].uri[0] = 0;
            count++;
            p = q + 1;
        } else {
            p++;
        }
    }
    return count;
}

void sonos_get_spotify_config(sonos_spotify_cfg_t * cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    FILE * f = fopen("/mnt/data/tsc/sonos.userSettings.json", "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;
    buf[n] = 0;

    extract_json_str(buf, "spotifyClientId", cfg->client_id, sizeof cfg->client_id);
    extract_json_str(buf, "spotifyClientSecret", cfg->client_secret, sizeof cfg->client_secret);
    extract_json_int(buf, "selectedPlaylistUser", &cfg->selected_user);

    if (cfg->client_id[0] && cfg->client_secret[0]) {
        cfg->configured = 1;
    }

    /* Extract user names */
    const char * un = strstr(buf, "\"spotifyUserNames\"");
    if (un) {
        const char * p = strchr(un, '[');
        if (p) {
            p++;
            while (*p && *p != ']' && cfg->user_count < 8) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
                if (*p == '"') {
                    p++;
                    const char * q = strchr(p, '"');
                    if (!q) break;
                    size_t len = (size_t)(q - p);
                    if (len >= sizeof cfg->user_names[0]) len = sizeof cfg->user_names[0] - 1;
                    memcpy(cfg->user_names[cfg->user_count], p, len);
                    cfg->user_names[cfg->user_count][len] = 0;
                    cfg->user_count++;
                    p = q + 1;
                } else {
                    p++;
                }
            }
        }
    }

    /* Extract user IDs */
    const char * ui = strstr(buf, "\"spotifyUserIDs\"");
    if (ui) {
        const char * p = strchr(ui, '[');
        if (p) {
            p++;
            int idx = 0;
            while (*p && *p != ']' && idx < 8) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
                if (*p == '"') {
                    p++;
                    const char * q = strchr(p, '"');
                    if (!q) break;
                    size_t len = (size_t)(q - p);
                    if (len >= sizeof cfg->user_ids[0]) len = sizeof cfg->user_ids[0] - 1;
                    memcpy(cfg->user_ids[idx], p, len);
                    cfg->user_ids[idx][len] = 0;
                    idx++;
                    p = q + 1;
                } else {
                    p++;
                }
            }
        }
    }
}

static char   s_spot_token[300] = {0};
static time_t s_spot_token_expiry = 0;

static int get_spotify_token(const sonos_spotify_cfg_t * cfg, char * out, size_t outsz) {
    time_t now = time(NULL);
    if (s_spot_token[0] && now < s_spot_token_expiry - 60) {
        snprintf(out, outsz, "%s", s_spot_token);
        return 0;
    }
    if (!cfg || !cfg->configured) return -1;

    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -s -4 -k --max-time 6 --connect-timeout 4 "
        "-X POST 'https://accounts.spotify.com/api/token' "
        "-d 'grant_type=client_credentials' -u '%s:%s'",
        cfg->client_id, cfg->client_secret);

    FILE * fp = popen(cmd, "r");
    if (!fp) return -1;
    char resp[1024];
    size_t got = fread(resp, 1, sizeof(resp) - 1, fp);
    pclose(fp);
    resp[got] = 0;

    char tok[300] = "";
    if (extract_json_str(resp, "access_token", tok, sizeof tok) && tok[0]) {
        int exp = 3600;
        extract_json_int(resp, "expires_in", &exp);
        snprintf(s_spot_token, sizeof s_spot_token, "%s", tok);
        s_spot_token_expiry = now + exp;
        snprintf(out, outsz, "%s", tok);
        return 0;
    }
    return -1;
}

static int parse_spotify_playlists(const char * json, sonos_item_t items[], int max_items) {
    if (!json || !items || max_items <= 0) return 0;
    int count = 0;
    const char * cur = json;
    while ((cur = strstr(cur, "\"uri\":\"spotify:playlist:")) != NULL && count < max_items) {
        cur += 7; /* skip "uri":" */
        const char * q = strchr(cur, '"');
        if (!q) break;
        size_t ulen = (size_t)(q - cur);
        char uri[128];
        if (ulen >= sizeof uri) ulen = sizeof uri - 1;
        memcpy(uri, cur, ulen);
        uri[ulen] = 0;

        /* Look backwards before cur for "name":" */
        const char * search_start = (cur - 1500 > json) ? cur - 1500 : json;
        const char * nm = cur;
        while (nm > search_start) {
            if (strncmp(nm, "\"name\":\"", 8) == 0) break;
            nm--;
        }
        char name[64] = "";
        if (strncmp(nm, "\"name\":\"", 8) == 0) {
            nm += 8;
            const char * nq = strchr(nm, '"');
            if (nq && nq < cur) {
                size_t nlen = (size_t)(nq - nm);
                if (nlen >= sizeof name) nlen = sizeof name - 1;
                memcpy(name, nm, nlen);
                name[nlen] = 0;
            }
        }
        if (name[0] && uri[0]) {
            snprintf(items[count].name, sizeof items[count].name, "%s", name);
            snprintf(items[count].uri, sizeof items[count].uri, "%s", uri);
            count++;
        }
        cur = q + 1;
    }
    return count;
}

int sonos_get_source_items(sonos_source_t src, int spotify_user_idx, sonos_item_t items[], int max_items) {
    if (!items || max_items <= 0) return 0;
    if (src == SONOS_SRC_FAVORITES) {
        char favs[SONOS_MAX_ITEMS][64];
        int n = sonos_get_favorites(favs, max_items);
        for (int i = 0; i < n; i++) {
            snprintf(items[i].name, sizeof items[i].name, "%s", favs[i]);
            items[i].uri[0] = 0;
        }
        return n;
    } else if (src == SONOS_SRC_PLAYLISTS) {
        return get_sonos_playlists(items, max_items);
    } else if (src == SONOS_SRC_SPOTIFY) {
        sonos_spotify_cfg_t cfg;
        sonos_get_spotify_config(&cfg);
        if (!cfg.configured || spotify_user_idx < 0 || spotify_user_idx >= cfg.user_count)
            return 0;
        char token[300] = "";
        if (get_spotify_token(&cfg, token, sizeof token) != 0 || !token[0])
            return 0;

        char cmd[512];
        snprintf(cmd, sizeof cmd,
            "/usr/bin/curl -s -4 -k --max-time 6 --connect-timeout 4 "
            "-H 'Authorization: Bearer %s' "
            "'https://api.spotify.com/v1/users/%s/playlists'",
            token, cfg.user_ids[spotify_user_idx]);

        FILE * fp = popen(cmd, "r");
        if (!fp) return 0;
        char * resp = malloc(32768);
        if (!resp) { pclose(fp); return 0; }
        size_t got = fread(resp, 1, 32767, fp);
        pclose(fp);
        resp[got] = 0;
        int count = parse_spotify_playlists(resp, items, max_items);
        free(resp);
        return count;
    }
    return 0;
}

void sonos_play_favorite(const char * name) {
    if (!name || !name[0]) return;
    char enc[128];
    url_encode(name, enc, sizeof enc);
    char cmd[160];
    snprintf(cmd, sizeof cmd, "favorite/%s", enc);
    send_cmd("clearqueue");
    usleep(80000);
    send_cmd(cmd);
}

void sonos_play_playlist(const char * name) {
    if (!name || !name[0]) return;
    char enc[128];
    url_encode(name, enc, sizeof enc);
    char cmd[160];
    snprintf(cmd, sizeof cmd, "playlist/%s", enc);
    send_cmd("clearqueue");
    usleep(80000);
    send_cmd(cmd);
}

void sonos_play_spotify(const char * uri) {
    if (!uri || !uri[0]) return;
    char cmd[200];
    snprintf(cmd, sizeof cmd, "spotify/now/%s", uri);
    send_cmd("clearqueue");
    usleep(80000);
    send_cmd(cmd);
}

