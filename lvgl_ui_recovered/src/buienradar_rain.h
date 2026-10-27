#ifndef BUIENRADAR_RAIN_H
#define BUIENRADAR_RAIN_H

#include <stdint.h>
#include <time.h>

#define RAIN_MAX_POINTS_3H  48
#define RAIN_MAX_POINTS_24H 48

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

#endif /* BUIENRADAR_RAIN_H */

