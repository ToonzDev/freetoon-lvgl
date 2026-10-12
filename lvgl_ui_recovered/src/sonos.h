#ifndef TOON_SONOS_H
#define TOON_SONOS_H

/* Sonos HTTP API integration (node-sonos-http-api).
 * Talks to the HTTP API service on http://<host>:<port>/.
 * Polls /<zone>/state in the background and sends transport commands.
 */

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
} sonos_state_t;

extern sonos_state_t sonos_state;

/* Start background poller thread */
int  sonos_start(void);

/* Snapshot copy for thread-safe UI rendering */
void sonos_get_snapshot(sonos_state_t * out);

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
void sonos_volume_up(void);       /* +2 */
void sonos_volume_down(void);     /* -2 */
void sonos_set_volume(int vol);
void sonos_set_zone(const char * zone);

#endif
