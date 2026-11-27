/* See vent_homey.h. */
#include "vent_homey.h"
#include "ventilation.h"
#include "homey.h"
#include "settings.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static lv_timer_t * g_timer;
static time_t g_timer_end;          /* 0 = no timer */
static int    g_timer_back = 0;     /* relay mask to return to */
static double g_watts = -1;
static int    g_mask = -1;          /* last relay mask seen (-1 unknown) */
static int    g_logged_state = -2;

int vent_homey_active(void) {
    return settings.vent_homey && settings.enable_homey &&
           (settings.vent_hy_r1[0] || settings.vent_hy_r2[0]);
}

/* The on/off capability of relay r (0/1): "onoff", or one channel of a
 * multi-channel device ("onoff.1", ...). */
static const char * relay_cap(int r) {
    const char * c = r ? settings.vent_hy_c2 : settings.vent_hy_c1;
    return c[0] ? c : "onoff";
}

/* Relay state from Homey. Caller holds the lock. 1 on, 0 off, -1 not found. */
static int relay_on(const char * id, const char * cap, double * watts) {
    if (!id[0]) return 0;                       /* no such relay: counts as off */
    for (int i = 0; i < homey_state.ndev; i++) {
        const hy_dev_t * d = &homey_state.dev[i];
        if (strcmp(d->id, id)) continue;
        int on = -1;
        for (int c = 0; c < d->ncap; c++) {
            const hy_cap_t * k = &d->cap[c];
            if (!strcmp(k->id, cap) && k->type != HY_V_NONE) on = k->num != 0;
            else if (!strncmp(k->id, "measure_power", 13) && k->type == HY_V_NUM) {
                if (*watts < 0) *watts = 0;
                *watts += k->num;
            }
        }
        return d->available ? on : -1;
    }
    return -1;
}

static const char * mode_of(int mask) {
    if (mask == 0) return "off";
    if (mask == (settings.vent_hy_low & 3)) return "low";
    if (mask == (settings.vent_hy_high & 3)) return "high";
    return "?";
}

static void apply_mask(int mask) {
    const char * id[2] = { settings.vent_hy_r1, settings.vent_hy_r2 };
    /* off first, then on: never two windings at once */
    for (int pass = 0; pass < 2; pass++)
        for (int r = 0; r < 2; r++) {
            if (!id[r][0]) continue;
            int want = (mask >> r) & 1;
            if (want == pass) homey_set_bool(id[r], relay_cap(r), want);
        }
    fprintf(stderr, "[vent-homey] relays -> %s (mask %d)\n", mode_of(mask), mask);
}

void vent_homey_cmd(const char * cmd) {
    if (!vent_homey_active() || !cmd) return;
    int mins = 0;
    if (!strncmp(cmd, "timer", 5)) mins = 10 * (cmd[5] >= '1' && cmd[5] <= '3' ? cmd[5] - '0' : 1);
    if (mins) {
        if (!g_timer_end) g_timer_back = g_mask > 0 ? g_mask : 0;   /* the position to return to */
        g_timer_end = time(NULL) + mins * 60;
        apply_mask(settings.vent_hy_high & 3);
    } else if (!strcmp(cmd, "auto")) {
        /* the Timer button's 4th press (cancel): back to where it was */
        int back = g_timer_end ? g_timer_back : (g_mask > 0 ? g_mask : 0);
        g_timer_end = 0;
        apply_mask(back);
    } else {
        g_timer_end = 0;
        int m = !strcmp(cmd, "low") ? (settings.vent_hy_low & 3)
              : !strcmp(cmd, "high") ? (settings.vent_hy_high & 3) : 0;   /* off */
        apply_mask(m);
    }
    snprintf((char *)vent_state.last_cmd, sizeof vent_state.last_cmd, "%s", mins ? "timer" : cmd);
    vent_state.last_cmd_ts = (long)time(NULL);
}

double vent_homey_watts(void) { return g_watts; }

static void tick(lv_timer_t * t) {
    (void)t;
    if (!vent_homey_active()) {
        g_timer_end = 0;
        return;
    }
    double w = -1;
    homey_lock();
    int ok = homey_state.status == HOMEY_ST_OK;
    int a = relay_on(settings.vent_hy_r1, relay_cap(0), &w);
    /* both relays on one 2-channel device: count its power once */
    double w2 = -1;
    int b = relay_on(settings.vent_hy_r2, relay_cap(1), strcmp(settings.vent_hy_r1, settings.vent_hy_r2) ? &w : &w2);
    homey_unlock();
    if (a < 0 || b < 0) ok = 0;
    g_watts = w;

    time_t now = time(NULL);
    if (g_timer_end && now >= g_timer_end) {
        g_timer_end = 0;
        fprintf(stderr, "[vent-homey] timer done, back to %s\n", mode_of(g_timer_back));
        apply_mask(g_timer_back);
        return;                                  /* Homey's local state follows at once */
    }

    vent_state.connected   = ok;
    vent_state.itho_online = ok ? 1 : 0;
    if (ok != g_logged_state) {
        g_logged_state = ok;
        fprintf(stderr, "[vent-homey] relays %s\n", ok ? "found in Homey" : "not (yet) available in Homey");
    }
    if (!ok) return;
    int mask = a | (b << 1);
    g_mask = mask;
    const char * mode = g_timer_end ? "timer" : mode_of(mask);
    snprintf((char *)vent_state.fan_info, sizeof vent_state.fan_info, "%s", mode);
    int pct = mask == 0 ? 0 : mask == (settings.vent_hy_high & 3) ? 100 : 50;
    vent_state.speed_pct   = pct;
    vent_state.exh_fan_pct = pct;
    vent_state.fan_rpm     = pct * 12;           /* no rpm: drives the fan icon's spin */
    vent_state.remaining_min = g_timer_end ? (int)((g_timer_end - now + 59) / 60) : 0;
}

void vent_homey_start(void) {
    if (!g_timer) g_timer = lv_timer_create(tick, 1000, NULL);
}
