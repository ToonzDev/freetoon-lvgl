#ifndef BUIENRADAR_RAIN_H
#define BUIENRADAR_RAIN_H

#include <stdint.h>
#include <time.h>

#define RAIN_MAX_POINTS_3H  48
#define RAIN_MAX_POINTS_24H 25

typedef struct {
    char time_str[8];   /* "HH:MM" */
    float mm;           /* mm/u precipitation */
    int value;          /* 0..100 intensity scale */
} rain_point_t;

typedef struct {
    int count_3h;
    rain_point_t points_3h[RAIN_MAX_POINTS_3H];
    char summary_3h[64];
    int has_rain_3h;
    float max_mm_3h;

    int count_24h;
    rain_point_t points_24h[RAIN_MAX_POINTS_24H];
    char summary_24h[64];
    int has_rain_24h;
    float max_mm_24h;

    time_t last_updated;
    int is_valid;
} buienradar_rain_snapshot_t;

int  buienradar_rain_start(void);
void buienradar_rain_refresh_async(void);
void buienradar_rain_get_snapshot(buienradar_rain_snapshot_t * out);

/* 0 none, 1 light (<2.5 mm/h), 2 moderate (<10), 3 heavy. */
int  buienradar_rain_level(float mm);
/* One plain sentence for the next hours into buf ("Droog tot 14:20",
 * "Over 10 min lichte regen", "Lichte regen, stopt rond 15:05").
 * Returns minutes until rain starts (0 = raining now), -1 = none expected
 * in the 3-hour data (or no data). */
int  buienradar_rain_outlook(const buienradar_rain_snapshot_t * s, char * buf, int len);

#endif /* BUIENRADAR_RAIN_H */

