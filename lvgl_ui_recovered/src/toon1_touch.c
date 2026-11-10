/*
 * toon1_touch.c — TSC2007 evdev shim for Toon 1.
 *
 * Whole file compiles to nothing unless TOON1 is defined, so it can sit in
 * the unconditional source list without affecting the Toon 2 build.
 *
 * What it does (vs. lv_drivers/indev/evdev.c, which doesn't fit Toon 1):
 *   - opens /dev/input/event0 (the TSC2007 reported in /proc/bus/input/devices
 *     on a real Toon 1; LVGL's EVDEV_NAME default of /dev/input/event1 is
 *     wrong here).
 *   - queries ABS_X / ABS_Y min/max via EVIOCGABS once at init and scales
 *     each event linearly to DISP_HOR × DISP_VER (the kernel reports raw
 *     12-bit ADC, not pixel coords — without scaling, taps register at
 *     thousand-px coords and miss every target).
 *   - tracks press state from BTN_TOUCH and (as a fallback) ABS_PRESSURE.
 *   - non-blocking read so the LVGL timer thread doesn't stall on idle.
 */
#ifdef TOON1

#include "toon1_touch.h"
#include "display.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define TOUCH_DEVICE "/dev/input/event0"

/* Mirrors evdev_read_with_activity in main.c: any press marks activity so
 * the idle/dim timers reset. Defined in ui_stack.c; declared inline here to
 * avoid pulling another header through main.c. */
extern void ui_mark_activity(void);

static int  g_fd = -1;
static int  g_x = 0, g_y = 0;
static int  g_rx = 0, g_ry = 0;          /* raw ADC values (for the calibration) */
static int  g_pressed = 0;
static long g_abs_x_min = 0,    g_abs_x_max = 4095;
static long g_abs_y_min = 0,    g_abs_y_max = 4095;

/* Factory touch calibration, as the stock UI uses it (tslib "linear" module,
 * /etc/pointercal = "a0 a1 a2 a3 a4 a5 a6"):
 *     x = (a2 + a0*X + a1*Y) / a6,   y = (a5 + a3*X + a4*Y) / a6
 * The resistive panel's raw range is not the screen's: mapping 0..4095
 * linearly put touches ~27 px too low at the top and ~18 px too far left on
 * the right edge of a real Toon 1 — the top-right close (X) button then only
 * reacted on its upper part. The calibration includes the panel orientation,
 * so the touch_swap/invert settings only apply to the linear fallback.
 * Touch /mnt/data/touch_linear to force the old linear mapping. */
#define POINTERCAL "/etc/pointercal"
static int       g_cal_ok = 0;
static long long g_cal[7];

static void cal_map(long long rx, long long ry, long long * x, long long * y) {
    *x = (g_cal[2] + g_cal[0] * rx + g_cal[1] * ry) / g_cal[6];
    *y = (g_cal[5] + g_cal[3] * rx + g_cal[4] * ry) / g_cal[6];
}

static void load_pointercal(void) {
    if (access("/mnt/data/touch_linear", F_OK) == 0) {
        fprintf(stderr, "[toon1_touch] /mnt/data/touch_linear present: linear mapping\n");
        return;
    }
    FILE * f = fopen(POINTERCAL, "r");
    if (!f) return;
    long long c[7];
    int got = fscanf(f, "%lld %lld %lld %lld %lld %lld %lld",
                     &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6]);
    fclose(f);
    if (got != 7 || c[6] == 0) {
        fprintf(stderr, "[toon1_touch] " POINTERCAL " unreadable, using linear mapping\n");
        return;
    }
    memcpy(g_cal, c, sizeof g_cal);
    /* Sanity: the raw corners must land around the screen corners (a panel
     * overshoots the visible area a bit; reject anything wildly off). */
    long long xmin = 1LL << 40, xmax = -(1LL << 40), ymin = 1LL << 40, ymax = -(1LL << 40);
    long rxs[2] = { g_abs_x_min, g_abs_x_max }, rys[2] = { g_abs_y_min, g_abs_y_max };
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {
        long long x, y; cal_map(rxs[i], rys[j], &x, &y);
        if (x < xmin) xmin = x;
        if (x > xmax) xmax = x;
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    int sane = xmin < DISP_HOR / 6 && xmax > DISP_HOR * 5 / 6 &&
               ymin < DISP_VER / 6 && ymax > DISP_VER * 5 / 6 &&
               xmax - xmin < DISP_HOR * 3 / 2 && ymax - ymin < DISP_VER * 3 / 2;
    fprintf(stderr, "[toon1_touch] " POINTERCAL " maps raw corners to x %lld..%lld, y %lld..%lld: %s\n",
            xmin, xmax, ymin, ymax, sane ? "using factory calibration" : "implausible, using linear mapping");
    g_cal_ok = sane;
}

int toon1_touch_init(void) {
    g_fd = open(TOUCH_DEVICE, O_RDONLY | O_NONBLOCK);
    if (g_fd < 0) {
        fprintf(stderr, "[toon1_touch] open %s failed\n", TOUCH_DEVICE);
        return -1;
    }
    struct input_absinfo ai;
    if (ioctl(g_fd, EVIOCGABS(ABS_X), &ai) == 0) {
        g_abs_x_min = ai.minimum;
        g_abs_x_max = ai.maximum;
    }
    if (ioctl(g_fd, EVIOCGABS(ABS_Y), &ai) == 0) {
        g_abs_y_min = ai.minimum;
        g_abs_y_max = ai.maximum;
    }
    fprintf(stderr,
        "[toon1_touch] %s ABS_X=%ld..%ld ABS_Y=%ld..%ld -> %dx%d\n",
        TOUCH_DEVICE, g_abs_x_min, g_abs_x_max,
        g_abs_y_min, g_abs_y_max, DISP_HOR, DISP_VER);
    load_pointercal();
    return 0;
}

void toon1_touch_read(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    (void)drv;
    if (g_fd < 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int prev_pressed = g_pressed;
    struct input_event ev;
    while (read(g_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) {
                g_rx = ev.value;
                long w = g_abs_x_max - g_abs_x_min;
                if (w > 0) {
                    long v = ((long)(ev.value - g_abs_x_min)) * (long)DISP_HOR / w;
                    if (v < 0) v = 0;
                    if (v >= DISP_HOR) v = DISP_HOR - 1;
                    g_x = (int)v;
                }
            } else if (ev.code == ABS_Y) {
                g_ry = ev.value;
                long h = g_abs_y_max - g_abs_y_min;
                if (h > 0) {
                    long v = ((long)(ev.value - g_abs_y_min)) * (long)DISP_VER / h;
                    if (v < 0) v = 0;
                    if (v >= DISP_VER) v = DISP_VER - 1;
                    g_y = (int)v;
                }
            } else if (ev.code == ABS_PRESSURE) {
                /* Fallback when the driver doesn't emit BTN_TOUCH:
                 * any non-zero pressure counts as "down". */
                if (!g_pressed) g_pressed = (ev.value > 0);
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            g_pressed = (ev.value != 0);
        }
        /* EV_SYN frames the report; values accumulate into g_x/g_y/g_pressed. */
    }
    /* Apply user-configured axis transforms. Resistive panels are mounted
     * any-which-way; three booleans cover all 8 orientations. Tweak in
     * /mnt/data/toonui.cfg without rebuilding. */
    int out_x = g_x, out_y = g_y;
    if (g_cal_ok) {
        long long cx, cy;
        cal_map(g_rx, g_ry, &cx, &cy);
        if (cx < 0) cx = 0;
        if (cx >= DISP_HOR) cx = DISP_HOR - 1;
        if (cy < 0) cy = 0;
        if (cy >= DISP_VER) cy = DISP_VER - 1;
        data->point.x = (lv_coord_t)cx;
        data->point.y = (lv_coord_t)cy;
        data->state   = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        if (g_pressed && !prev_pressed) ui_mark_activity();
        return;
    }
    if (settings.touch_swap_xy) {
        /* Swap after scaling — but X was scaled to DISP_HOR, Y to DISP_VER.
         * After swap we need to re-scale the swapped value into the right
         * axis range so it fits the screen. */
        long sx = (long)g_y * DISP_HOR / DISP_VER;
        long sy = (long)g_x * DISP_VER / DISP_HOR;
        out_x = (int)sx;
        out_y = (int)sy;
    }
    if (settings.touch_invert_x) out_x = DISP_HOR - 1 - out_x;
    if (settings.touch_invert_y) out_y = DISP_VER - 1 - out_y;

    data->point.x = out_x;
    data->point.y = out_y;
    data->state   = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (g_pressed && !prev_pressed) ui_mark_activity();
}

#endif /* TOON1 */
