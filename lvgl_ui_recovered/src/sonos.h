#ifndef TOON_SONOS_H
#define TOON_SONOS_H

/* Sonos HTTP API integration (node-sonos-http-api).
 * Talks to the HTTP API service on http://<host>:<port>/.
 * Polls /<zone>/state in the background and sends transport commands.
 */

#include "lvgl/lvgl.h"

#define SONOS_MAX_ZONES 16

typedef struct {
    volatile int connected;               /* 1 if last poll succeeded */
    volatile int playing;                 /* 1 = PLAYING, 0 = PAUSED/STOPPED */
    char         zone[48];                /* active room/zone name */
    char         artist[64];              /* current artist or radio station */
    char         title[96];               /* current track title */
    char         album[64];               /* album name */
    volatile int duration;                /* track duration in seconds */
    volatile int elapsed;                 /* elapsed seconds */
    volatile int volume;                  /* current zone volume 0..100 */
    volatile int is_group;                /* 1 if zone is group coordinator */
    volatile int is_radio;                /* 1 if radio stream, 0 if track */
    char         art_url[256];            /* album art URI if available */
} sonos_state_t;

extern sonos_state_t sonos_state;

/* Start background poller thread */
int  sonos_start(void);

/* Snapshot copy for thread-safe UI rendering */
void sonos_get_snapshot(sonos_state_t * out);

/* Returns pre-decoded & downscaled in-memory LVGL image descriptors, or NULL if none */
const lv_img_dsc_t * sonos_get_art_big(int * out_ver);
const lv_img_dsc_t * sonos_get_art_tile(int * out_ver);

/* Probe / discover zones from the Sonos HTTP API (/zones).
 * Fills `zones` with up to `max_zones` room names.
 * Returns count >= 0 on success, -1 on connection failure. */
int  sonos_probe(char zones[][48], int max_zones);

/* Transport control (async, runs on background worker thread) */
void sonos_play(void);
void sonos_pause(void);
void sonos_play_pause(void);
void sonos_next(void);
void sonos_prev(void);
void sonos_seek(int seconds);     /* timeseek/<seconds> */
void sonos_volume_up(void);       /* +2 */
void sonos_volume_down(void);     /* -2 */
void sonos_set_volume(int vol);
void sonos_set_zone(const char * zone);

#define SONOS_MAX_ITEMS 48

typedef enum {
    SONOS_SRC_FAVORITES = 0,
    SONOS_SRC_PLAYLISTS = 1,
    SONOS_SRC_SPOTIFY   = 2
} sonos_source_t;

typedef struct {
    char name[64];
    char uri[128];      /* For Spotify: spotify:playlist:... */
} sonos_item_t;

typedef struct {
    int  configured;
    char client_id[64];
    char client_secret[64];
    int  user_count;
    char user_names[8][32];
    char user_ids[8][64];
    int  selected_user;    /* 0 = Sonos Playlists, 1..N = Spotify user */
} sonos_spotify_cfg_t;

void sonos_get_spotify_config(sonos_spotify_cfg_t * cfg);
int  sonos_get_source_items(sonos_source_t src, int spotify_user_idx, sonos_item_t items[], int max_items);
int  sonos_get_favorites(char favs[][64], int max_favs);
void sonos_play_favorite(const char * name);
void sonos_play_playlist(const char * name);
void sonos_play_spotify(const char * uri);

#endif
