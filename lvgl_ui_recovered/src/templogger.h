#ifndef TOON_TEMPLOGGER_H
#define TOON_TEMPLOGGER_H

#define TEMPLOGGER_POINTS       144       /* 24 hours * 6 samples/hour (every 10 min) */
#define TEMPLOGGER_INVALID_VAL  (-999.0f) /* Sentinel for unrecorded/empty slot */

typedef struct {
    float room[TEMPLOGGER_POINTS];
    float setpoint[TEMPLOGGER_POINTS];
    float outside[TEMPLOGGER_POINTS];
    int   cur_idx;                        /* Current 10-minute slot (0..143) */
    float latest_room;
    float latest_setpoint;
    float latest_outside;
    long  last_sample_time;
} templogger_snapshot_t;

/* Start background recorder thread & load history files */
void templogger_start(void);

/* Get a thread-safe snapshot of all 144 points across the 24 hours */
int  templogger_get_snapshot(templogger_snapshot_t * out);

/* Get latest instantaneous values (for home tiles) */
void templogger_get_latest(float * room, float * sp, float * out);

#endif /* TOON_TEMPLOGGER_H */
