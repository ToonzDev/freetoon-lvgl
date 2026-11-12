/*
 * Headless Toon simulator — renders the LVGL UI to an offscreen framebuffer
 * and dumps it as a PPM, with no real hardware, X, or SDL. Lets us eyeball the
 * layout (especially the Toon-1 800x480 target) and iterate on screens without
 * a device. Build via `make TARGET=sim` (1024x600) or `make TARGET=sim1`
 * (800x480, -DTOON1). Hardware-bound bits (meteradapter / Z-Wave / WiFi / live
 * BoxTalk) are not emulated — the global state structs are filled with mock
 * data so the screens have something representative to draw.
 *
 *   ./toonui-sim <screen> [out.ppm]   render one screen directly
 *   ./toonui-sim all [outdir]         render every screen to <outdir>/sim_<name>.ppm
 *   ./toonui-sim nav <out.ppm> X,Y..  start at home, inject taps, dump active screen
 */
#include "lvgl/lvgl.h"
#include "display.h"
#include "screens.h"
#include "settings.h"
#include "homeassistant.h"
#include "layout.h"
#include "boxtalk.h"
#include "weather.h"
#include "client_link.h"
#include "wastecollection.h"
#include "homewizard.h"
#include "energy.h"
#include "meteradapter.h"
#include "ventilation.h"
#include "efanlamp.h"
#include "anim_overlay.h"
#include "hue.h"
#include "calendar.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

int g_x11_running = 1;

static void sim_signal_handler(int sig) {
    (void)sig;
    g_x11_running = 0;
}

static int sim_xio_error_handler(Display * d) {
    (void)d;
    g_x11_running = 0;
    return 0;
}

static Display * g_x_display = NULL;
static Window    g_x_window  = 0;
static GC        g_x_gc      = 0;
static XImage  * g_x_image   = NULL;
static uint32_t* g_x_pixels  = NULL;
static Atom      g_wm_delete = 0;
static int       g_mouse_x = 0, g_mouse_y = 0, g_mouse_down = 0;

static lv_color_t fb[DISP_HOR * DISP_VER];

static int x11_gui_init(int width, int height, const char * title) {
    g_x_display = XOpenDisplay(NULL);
    if (!g_x_display) return 0;
    int screen = DefaultScreen(g_x_display);
    Window root = RootWindow(g_x_display, screen);

    g_x_window = XCreateSimpleWindow(g_x_display, root, 100, 100, width, height, 0,
                                     BlackPixel(g_x_display, screen),
                                     BlackPixel(g_x_display, screen));

    XStoreName(g_x_display, g_x_window, title);

    XSizeHints * size_hints = XAllocSizeHints();
    if (size_hints) {
        size_hints->flags = PMinSize | PMaxSize | PSize;
        size_hints->min_width = size_hints->max_width = size_hints->base_width = width;
        size_hints->min_height = size_hints->max_height = size_hints->base_height = height;
        XSetWMNormalHints(g_x_display, g_x_window, size_hints);
        XFree(size_hints);
    }

    g_wm_delete = XInternAtom(g_x_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_x_display, g_x_window, &g_wm_delete, 1);

    XSelectInput(g_x_display, g_x_window,
                 ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | StructureNotifyMask);

    g_x_gc = DefaultGC(g_x_display, screen);

    g_x_pixels = (uint32_t *)calloc(width * height, sizeof(uint32_t));
    Visual * visual = DefaultVisual(g_x_display, screen);
    g_x_image = XCreateImage(g_x_display, visual, 24, ZPixmap, 0,
                             (char *)g_x_pixels, width, height, 32, 0);

    XWMHints * wm_hints = XAllocWMHints();
    if (wm_hints) {
        wm_hints->flags = InputHint | StateHint;
        wm_hints->input = True;
        wm_hints->initial_state = NormalState;
        XSetWMHints(g_x_display, g_x_window, wm_hints);
        XFree(wm_hints);
    }

    XSetIOErrorHandler(sim_xio_error_handler);
    XMapRaised(g_x_display, g_x_window);
    XRaiseWindow(g_x_display, g_x_window);
    XFlush(g_x_display);
    return 1;
}

static void x11_handle_events(void) {
    if (!g_x_display) return;
    while (XPending(g_x_display) > 0) {
        XEvent ev;
        XNextEvent(g_x_display, &ev);
        switch (ev.type) {
            case Expose:
                if (g_x_display && g_x_window && g_x_image) {
                    XPutImage(g_x_display, g_x_window, g_x_gc, g_x_image,
                              0, 0, 0, 0, DISP_HOR, DISP_VER);
                    XFlush(g_x_display);
                }
                break;
            case ButtonPress:
                if (ev.xbutton.button == Button1) {
                    g_mouse_down = 1;
                    g_mouse_x = ev.xbutton.x;
                    g_mouse_y = ev.xbutton.y;
                }
                break;
            case ButtonRelease:
                if (ev.xbutton.button == Button1) {
                    g_mouse_down = 0;
                    g_mouse_x = ev.xbutton.x;
                    g_mouse_y = ev.xbutton.y;
                }
                break;
            case MotionNotify:
                g_mouse_x = ev.xmotion.x;
                g_mouse_y = ev.xmotion.y;
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == g_wm_delete) {
                    g_x11_running = 0;
                }
                break;
            case DestroyNotify:
            case UnmapNotify:
                g_x11_running = 0;
                break;
            case KeyPress: {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (sym == XK_Escape) {
                    g_x11_running = 0;
                }
                break;
            }
            default:
                break;
        }
    }
}

static void x11_flush(lv_disp_drv_t * drv, const lv_area_t * a, lv_color_t * px) {
    for (int y = a->y1; y <= a->y2; y++) {
        for (int x = a->x1; x <= a->x2; x++) {
            lv_color_t c = *px++;
            fb[y * DISP_HOR + x] = c;
#if LV_COLOR_DEPTH == 16
            uint32_t r = ((c.ch.red << 3) | (c.ch.red >> 2));
            uint32_t g = ((c.ch.green << 2) | (c.ch.green >> 4));
            uint32_t b = ((c.ch.blue << 3) | (c.ch.blue >> 2));
            g_x_pixels[y * DISP_HOR + x] = (r << 16) | (g << 8) | b;
#else
            g_x_pixels[y * DISP_HOR + x] = (c.ch.red << 16) | (c.ch.green << 8) | c.ch.blue;
#endif
        }
    }
    if (g_x_display && g_x_window && g_x_image) {
        int w = a->x2 - a->x1 + 1;
        int h = a->y2 - a->y1 + 1;
        XPutImage(g_x_display, g_x_window, g_x_gc, g_x_image,
                  a->x1, a->y1, a->x1, a->y1, w, h);
        XFlush(g_x_display);
    }
    lv_disp_flush_ready(drv);
}

static void x11_indev_read(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    (void)drv;
    data->point.x = g_mouse_x;
    data->point.y = g_mouse_y;
    data->state = g_mouse_down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    if (data->state == LV_INDEV_STATE_PR) ui_mark_activity();
}

static void x11_gui_close(void) {
    if (g_x_display) {
        if (g_x_image) XDestroyImage(g_x_image);
        if (g_x_window) XDestroyWindow(g_x_display, g_x_window);
        XCloseDisplay(g_x_display);
        g_x_display = NULL;
    }
}

static int rs_trace;   /* redrawstat: REDRAW_TRACE=N prints the first N flushed areas */
static void sim_flush(lv_disp_drv_t * drv, const lv_area_t * a, lv_color_t * px) {
    if (rs_trace > 0) {
        rs_trace--;
        fprintf(stderr, "[redrawstat] flush %d,%d-%d,%d (%dx%d)\n", a->x1, a->y1, a->x2, a->y2,
                a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
    }
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            fb[y * DISP_HOR + x] = *px++;
    lv_disp_flush_ready(drv);
}

static void dump_ppm(const char * path) {
    FILE * f = fopen(path, "wb");
    if (!f) { perror("ppm"); return; }
    fprintf(f, "P6\n%d %d\n255\n", DISP_HOR, DISP_VER);
    for (int i = 0; i < DISP_HOR * DISP_VER; i++) {
#if LV_COLOR_DEPTH == 16
        /* RGB565: channels are 5/6/5-bit. Scale up to full 8-bit (replicate
           the high bits into the low ones) so the PPM isn't a dark cast of
           the real RGB565 device output. */
        unsigned r5 = fb[i].ch.red, g6 = fb[i].ch.green, b5 = fb[i].ch.blue;
        unsigned char rgb[3] = {
            (unsigned char)((r5 << 3) | (r5 >> 2)),
            (unsigned char)((g6 << 2) | (g6 >> 4)),
            (unsigned char)((b5 << 3) | (b5 >> 2)),
        };
#else
        unsigned char rgb[3] = { fb[i].ch.red, fb[i].ch.green, fb[i].ch.blue };
#endif
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* ---- scripted pointer input ---- */
static lv_coord_t tap_x = 0, tap_y = 0;
static int        tap_pressed = 0;
static void sim_indev_read(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    data->point.x = tap_x;
    data->point.y = tap_y;
    data->state = tap_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}
/* redrawstat: LVGL's monitor_cb reports each finished refresh (time, px). */
static uint64_t * rs_px; static uint32_t * rs_n; static uint32_t * rs_ms;
static void redrawstat_ptr(uint64_t * px, uint32_t * n, uint32_t * ms) { rs_px = px; rs_n = n; rs_ms = ms; }
static void redrawstat_monitor(lv_disp_drv_t * drv, uint32_t time, uint32_t px) {
    (void)drv;
    if (rs_px) { *rs_px += px; (*rs_n)++; *rs_ms += time; }
}

/* scrollbench helpers: the scrollable object with the most content to scroll,
 * and a tree walk that switches shadows off (SB_NOSHADOW experiment). */
static lv_obj_t * sb_find_scroller(lv_obj_t * o, lv_coord_t * best) {
    lv_obj_t * found = NULL;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE)) {
        lv_coord_t r = lv_obj_get_scroll_top(o) + lv_obj_get_scroll_bottom(o);
        if (r > *best) { *best = r; found = o; }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) {
        lv_obj_t * f = sb_find_scroller(lv_obj_get_child(o, i), best);
        if (f) found = f;
    }
    return found;
}
static void sb_strip_shadow(lv_obj_t * o) {
    if (getenv("SB_NOSHADOW")) lv_obj_set_style_shadow_width(o, 0, 0);
    if (getenv("SB_NORADIUS")) lv_obj_set_style_radius(o, 0, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) sb_strip_shadow(lv_obj_get_child(o, i));
}

static void pump(int frames) {
    for (int i = 0; i < frames; i++) { lv_timer_handler(); lv_tick_inc(16); usleep(1000); }
}
static void sim_tap(int x, int y) {
    tap_x = x; tap_y = y; tap_pressed = 1; pump(25);
    tap_pressed = 0;                          pump(25);
}

/* Simulate realistic OpenTherm boiler behaviour based on thermostat setpoint vs indoor temp */
static void sim_boiler_tick(void) {
    if (!toon_state.connected) return;

    float diff = toon_state.setpoint - toon_state.indoor_temp;
    if (diff > 0.05f) {
        /* Heat demand */
        toon_state.burner_on = 1;
        float mod = diff * 35.0f;
        if (mod > 100.0f) mod = 100.0f;
        if (mod < 20.0f) mod = 20.0f;
        toon_state.modulation_level = mod;

        float target_flow = 45.0f + diff * 15.0f;
        if (target_flow > 75.0f) target_flow = 75.0f;
        toon_state.ch_setpoint = target_flow;

        /* Aanvoer climbs towards target_flow */
        if (toon_state.boiler_out_temp < target_flow) {
            toon_state.boiler_out_temp += 0.8f;
            if (toon_state.boiler_out_temp > target_flow) toon_state.boiler_out_temp = target_flow;
        } else if (toon_state.boiler_out_temp > target_flow) {
            toon_state.boiler_out_temp -= 0.3f;
        }

        /* Retour climbs towards flow - 12°C */
        float target_ret = toon_state.boiler_out_temp - 12.0f;
        if (target_ret < 25.0f) target_ret = 25.0f;
        if (toon_state.boiler_in_temp < target_ret) {
            toon_state.boiler_in_temp += 0.6f;
            if (toon_state.boiler_in_temp > target_ret) toon_state.boiler_in_temp = target_ret;
        } else if (toon_state.boiler_in_temp > target_ret) {
            toon_state.boiler_in_temp -= 0.3f;
        }

        /* Indoor temp slowly warms up */
        toon_state.indoor_temp += 0.02f;
        if (toon_state.indoor_temp > toon_state.setpoint)
            toon_state.indoor_temp = toon_state.setpoint;
    } else {
        /* Boiler idle */
        toon_state.burner_on = 0;
        toon_state.modulation_level = 0.0f;

        /* Cool down towards ambient */
        if (toon_state.boiler_out_temp > 28.0f) {
            toon_state.boiler_out_temp -= 0.4f;
        }
        if (toon_state.boiler_in_temp > 24.0f) {
            toon_state.boiler_in_temp -= 0.4f;
        }

        /* Slow cooling down if indoor temp is higher than setpoint */
        if (diff < -0.2f && toon_state.indoor_temp > 17.0f) {
            toon_state.indoor_temp -= 0.008f;
        }
    }
}

/* Representative data so the screens render like a live Toon. */
static void mock_state(void) {
    /* Hue: a few lamps so the Hue screen has rows to render. */
    {
        static const struct { const char * id, * name; int on, pct, dim; } H[] = {
            { "1", "Woonkamer plafond", 1, 70, 1 },
            { "2", "Leeslamp",          0, 40, 1 },
            { "3", "Tuinverlichting",   1, 100, 0 },
        };
        hue_state.connected = 1;
        hue_state.count = (int)(sizeof H / sizeof H[0]);
        for (int i = 0; i < hue_state.count; i++) {
            hue_light_t * L = &hue_state.lights[i];
            snprintf(L->id, sizeof L->id, "%s", H[i].id);
            snprintf(L->name, sizeof L->name, "%s", H[i].name);
            L->on = H[i].on; L->bri_pct = H[i].pct; L->bri = H[i].pct * 254 / 100;
            L->reachable = 1; L->is_dimmable = H[i].dim;
        }
        settings.hue_enabled = 1;
        /* A room and a zone ("everything except the reading lamp"). */
        static const struct { const char * id, * name; int zone, n, any, all, pct; } G[] = {
            { "1", "Woonkamer",                  0, 2, 1, 0, 70 },
            { "7", "Woonkamer zonder leeslamp",  1, 1, 1, 1, 70 },
        };
        hue_state.group_count = (int)(sizeof G / sizeof G[0]);
        for (int i = 0; i < hue_state.group_count; i++) {
            hue_group_t * g = &hue_state.groups[i];
            snprintf(g->id, sizeof g->id, "%s", G[i].id);
            snprintf(g->name, sizeof g->name, "%s", G[i].name);
            g->is_zone = G[i].zone; g->n_lights = G[i].n;
            g->any_on = G[i].any; g->all_on = G[i].all; g->bri_pct = G[i].pct;
        }
        if (!settings.hue_tile_group[0]) snprintf(settings.hue_tile_group, sizeof settings.hue_tile_group, "7");
    }
    toon_state.connected     = 1;
    toon_state.msg_count     = 1;
    toon_state.boiler_type   = 1;   /* OpenTherm */
    toon_state.water_pressure= 1.8f;/* 1.8 bar */
    toon_state.indoor_temp   = 20.0f;
    toon_state.setpoint      = 19.5f;
    toon_state.ch_setpoint   = 55.0f;
    toon_state.humidity      = 53.0f;
    toon_state.boiler_in_temp  = 35.0f;
    toon_state.boiler_out_temp = 42.0f;
    toon_state.modulation_level = 0.0f;
    toon_state.burner_on     = 0;
    toon_state.dhw_on        = 0;
    toon_state.program_state = 1;   /* scheme mode = PROG_BASE (weekly schedule) */
    toon_state.active_state  = 1;   /* live comfort preset = Home */
    /* Sim-only: SIM_MANUAL=1 emulates manual hold — happ reports programState=0
     * (PROG_MANUAL) yet activeState still names the value-matching preset, so we
     * can verify no preset/program highlight leaks through in manual mode. */
    if (getenv("SIM_MANUAL")) { toon_state.program_state = 0; toon_state.active_state = 0; }
    /* Render an EXACT live device state (verify against real happ data):
     * SIM_PS=<programState> SIM_AS=<activeState>. */
    if (getenv("SIM_PS")) toon_state.program_state = atoi(getenv("SIM_PS"));
    if (getenv("SIM_AS")) toon_state.active_state  = atoi(getenv("SIM_AS"));

    weather_state.connected   = 1;
    weather_state.current_temp = 22.6f;
    weather_state.feel_temp    = 22.0f;
    if (getenv("SIM_OUT_TEMP")) weather_state.current_temp = weather_state.feel_temp = (float)atof(getenv("SIM_OUT_TEMP"));
    strcpy(weather_state.current_desc, "Zonnig");
    strcpy(weather_state.current_icon, "a");
    static const char * hlbl[] = { "Nu", "16:00", "19:00", "22:00", "01:00", "04:00" };
    static const char * hic[]  = { "a",  "a",     "a",     "aa",    "aa",    "aa"   };
    static const float  htmp[] = { 22.6f,26,24,19,16,15 };
    weather_state.hour_count = WEATHER_FORECAST_HOURS;
    for (int i = 0; i < WEATHER_FORECAST_HOURS; i++) {
        snprintf(weather_state.hours[i].label, 8, "%s", hlbl[i]);
        snprintf(weather_state.hours[i].icon, 8, "%s", hic[i]);
        weather_state.hours[i].temperature = htmp[i];
        weather_state.hours[i].wind_bft = 2;
        snprintf(weather_state.hours[i].wind_dir, 6, "ZO");
    }
    static const char * dday[] = { "vr 22-5","za 23-5","zo 24-5","ma 25-5","di 26-5" };
    static const char * dic[]  = { "a","b","f","q","a" };
    weather_state.day_count = WEATHER_FORECAST_DAYS;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        snprintf(weather_state.days[i].day, 16, "%s", dday[i]);
        snprintf(weather_state.days[i].icon, 8, "%s", dic[i]);
        weather_state.days[i].min_temp = 12 + i;
        weather_state.days[i].max_temp = 22 + i;
        weather_state.days[i].wind_bft = 3;
        snprintf(weather_state.days[i].wind_dir, 6, "ZW");
    }

    /* Energy/gas so the dim-screen usage bars render. Both auto-scale, so a
     * single frame pins them full (first reading == running max). */
    meter_state.connected = 1;
    meter_state.power_w   = 384.0f;
    hw_state.connected_p1 = 1;
    hw_state.power_w      = 384.0f;
    hw_state.gas_hour_m3  = 0.0f;   /* boiler idle → empty gas channel (visible track) */

    /* Itho ventilation so the dim_stock fan spinner renders ("Low 31%"). */
    vent_state.connected     = 1;
    vent_state.itho_online   = 1;
    vent_state.speed_pct     = 31;
    vent_state.fan_rpm       = 900;
    vent_state.remaining_min = 0;
    snprintf(vent_state.fan_info, sizeof vent_state.fan_info, "%s", "low");

    /* ESPHome BLE fan+lamp so the dim_stock BLE spinner renders. */
    efanlamp.connected = 1;
    efanlamp.fan_on    = 1;
    efanlamp.fan_speed = 3;
    efanlamp.light_on  = 1;
    efanlamp.light_brightness = 80;

    /* Waste: plastic today, paper + rest later (windowed logic picks). */
    waste_state.connected = 1;
    struct { const char * lbl; const char * date; } w[] = {
        { "GFT",       "2026-05-27" },
        { "Plastic",   "2026-05-22" },
        { "Papier",    "2026-05-28" },
        { "Restafval", "2026-05-30" },
    };
    for (int i = 0; i < WASTE_TYPES; i++) {
        snprintf(waste_state.items[i].label, sizeof waste_state.items[i].label, "%s", w[i].lbl);
        snprintf(waste_state.items[i].date,  sizeof waste_state.items[i].date,  "%s", w[i].date);
    }

    /* HA devices so the Devices screen renders populated (one of each type). */
    settings.enable_ha = 1;
    /* Sim-only: SIM_BACKEND=domoticz flips the active lights backend so the
     * backend-aware home handle/Devices routing can be verified both ways. */
    if (getenv("SIM_BACKEND") && !strcmp(getenv("SIM_BACKEND"), "domoticz")) {
        settings.enable_domoticz = 1;
        settings.enable_ha = 0;
    }
    ha_state.connected = 1;
    /* Solar production, so the Zonnepanelen tile has something to draw.
     * SIM_NO_SOLAR=1 renders the unconfigured state instead. */
    if (!getenv("SIM_NO_SOLAR")) {
        snprintf(settings.ha_solar_entity, sizeof settings.ha_solar_entity, "sensor.solar_power");
        ha_state.solar_w = 1240.0f;
        ha_state.have_solar = 1;
    }
    /* Sim-only: SIM_TRACKERS=1 seeds the stock-home Trackers tile (one tile,
     * 3 rows) so the new tile can be rendered/verified headless. */
    if (getenv("SIM_TRACKERS")) {
        snprintf(settings.life360_a_entity, sizeof settings.life360_a_entity, "device_tracker.auto");
        snprintf(settings.life360_a_name,   sizeof settings.life360_a_name,   "Auto");
        snprintf(settings.life360_b_entity, sizeof settings.life360_b_entity, "device_tracker.google_pixel_6a");
        snprintf(settings.life360_b_name,   sizeof settings.life360_b_name,   "Ronald");
        snprintf(settings.life360_c_entity, sizeof settings.life360_c_entity, "device_tracker.life360_member");
        snprintf(settings.life360_c_name,   sizeof settings.life360_c_name,   "Caja");
        snprintf(ha_state.loc_a, sizeof ha_state.loc_a, "Niet thuis");
        snprintf(ha_state.loc_b, sizeof ha_state.loc_b, "Thuis");
        snprintf(ha_state.loc_c, sizeof ha_state.loc_c, "Breezand");
        snprintf(settings.stock_tiles, sizeof settings.stock_tiles,
                 "clock,trackers,efan,power,indoor,weather,vent,agenda,empty,empty,empty,empty,empty,empty,empty,empty");
        extern efanlamp_state_t efanlamp;
        efanlamp.connected = 1; efanlamp.fan_on = 1; efanlamp.fan_speed = 3;
        efanlamp.light_on = 1;  efanlamp.light_brightness = 50;
    }
    struct { int type; const char * ent; const char * name; int on; int bri; int pos; int pin; } d[] = {
        { HADEV_LIGHT,  "light.woonkamer",  "Woonkamer",   1, 200, -1, 0 },
        { HADEV_LIGHT,  "light.keuken",      "Keuken",      0,  -1, -1, 0 },
        { HADEV_COVER,  "cover.gordijnen",   "Gordijnen",   0,  -1, 60, 1 },
        { HADEV_SWITCH, "switch.tuinpomp",   "Tuinpomp",    1,  -1, -1, 0 },
        { HADEV_SCRIPT, "script.avondmodus", "Avondmodus",  0,  -1, -1, 0 },
        { HADEV_SCENE,  "scene.film",        "Filmavond",   0,  -1, -1, 0 },
    };
    ha_device_count = (int)(sizeof(d)/sizeof(d[0]));
    for (int i = 0; i < ha_device_count; i++) {
        ha_device_t * D = &ha_devices[i];
        memset((void *)D, 0, sizeof *D);
        D->type = d[i].type;
        snprintf(D->entity_id, sizeof D->entity_id, "%s", d[i].ent);
        snprintf(D->name, sizeof D->name, "%s", d[i].name);
        D->available = 1; D->on = d[i].on; D->brightness = d[i].bri;
        D->position = d[i].pos; D->pin_home = d[i].pin;
        if (d[i].type == HADEV_COVER) snprintf(D->state, sizeof D->state, "%s", d[i].pos > 0 ? "open" : "closed");
    }
}

typedef lv_obj_t * (*create_fn)(void);
static const struct { const char * name; create_fn fn; } SCREENS[] = {
    { "home",        screen_home_create },
    { "home_stock",  screen_home_stock_create },
    { "dim",         screen_dim_create },
    { "dim_stock",   screen_dim_stock_create },
    { "thermostat",  screen_thermostat_create },
    { "settings",    screen_settings_create },
    { "schedule",    screen_schedule_create },
    { "stats",       screen_stats_create },
    { "forecast",    screen_forecast_create },
    { "heater",      screen_heater_advanced_create },
    { "vent_remote", screen_vent_remote_create },
    { "vent_adv",    screen_vent_advanced_create },
    { "lights",      screen_lights_create },
    { "marketplace", screen_marketplace_create },
    { "zwave",       screen_zwave_create },
    { "wifi",        screen_wifi_create },
    { "adapters",    screen_adapters_create },
    { "domoticz",    screen_domoticz_create },
    { "ha_devices",  screen_ha_devices_create },
    { "layout",      screen_layout_editor_create },
    { "dim_layout",  screen_dim_layout_editor_create },
    { "dim_grid",    screen_dim_grid_create },
    { "bootpick",    screen_bootpick_create },
    { "crypto_pick", screen_crypto_picker_create },
    { "crypto",      screen_crypto_create },
    { "templogger",  screen_templogger_create },
    { "hue",         screen_hue_create },
};
#define N_SCREENS ((int)(sizeof SCREENS / sizeof SCREENS[0]))

/* ---- soak test --------------------------------------------------------
 * `toonui-sim soak [rounds]`: opens and closes every per-open ("owned")
 * screen many times, via the back path (ui_pop), via auto-home, and via
 * auto-home while the dim screen is up, then checks that the number of
 * LVGL screens and timers stays flat from round to round. Leaked screens
 * stay reachable through LVGL's screen list, so LeakSanitizer can't see
 * them; these counts can. Build with SAN=-fsanitize=address (see Makefile)
 * to also catch use-after-free in timers / delete handlers. Exit code 0 =
 * stable, 1 = growth. */
static lv_obj_t * soak_chart(void) { return screen_chart_24h_create(NULL); }
static lv_obj_t * soak_ha_picker(void) { screen_ha_picker_open("light", NULL); return NULL; }

static int count_timers(void) {
    int n = 0;
    for (lv_timer_t * t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) n++;
    return n;
}

static void soak_advance(uint32_t ms) {          /* let idle time pass */
    lv_tick_inc(ms);
    lv_timer_handler();
}

/* Producer thread for the calendar race check: publishes new events while
 * the UI thread applies them (ui_idle_tick) and draws them. Build with
 * SAN=-fsanitize=thread to have TSan verify calendar_state is only touched
 * under the documented rules. */
static int soak_cal_stop = 0;   /* accessed with __atomic_* */
static void * soak_cal_thread(void * arg) {
    (void)arg;
    static calendar_event_t ev[8];
    for (int k = 0; !__atomic_load_n(&soak_cal_stop, __ATOMIC_RELAXED); k++) {
        for (int i = 0; i < 8; i++) {
            snprintf(ev[i].date, sizeof ev[i].date, "2026-11-%02d", 10 + i);
            snprintf(ev[i].time, sizeof ev[i].time, "%02d:00", (k + i) % 24);
            snprintf(ev[i].summary, sizeof ev[i].summary,
                     (k & 1) ? "short %d" : "a much longer event summary text %d", k);
        }
        calendar_publish(ev, 8, 1);
        calendar_lock();                       /* a non-UI reader, like the PWA */
        volatile int c = calendar_state.count; (void)c;
        calendar_unlock();
        usleep(500);
    }
    return NULL;
}

static int sim_soak(int rounds) {
    static const struct { const char * name; create_fn fn; } OWNED[] = {
        { "efanlamp",    screen_efanlamp_create },
        { "chart_24h",   soak_chart },
        { "hue",         screen_hue_create },
        { "crypto_pick", screen_crypto_picker_create },
        { "vent_adv",    screen_vent_advanced_create },
        { "heater",      screen_heater_advanced_create },
        { "ha_picker",   soak_ha_picker },       /* pushes itself */
    };
    const int n_owned = (int)(sizeof OWNED / sizeof OWNED[0]);
    settings.auto_dim_enabled  = 0;
    settings.auto_home_enabled = 0;
    ui_init();
    pump(5);
    lv_disp_t * d = lv_disp_get_default();
    int base_scr = -1, base_tmr = -1, grew = 0;

    for (int r = 1; r <= rounds; r++) {
        /* 1. open + back, every screen */
        for (int i = 0; i < n_owned; i++) {
            lv_obj_t * s = OWNED[i].fn();
            if (s) ui_push_owned(s);
            pump(3);
            ui_pop();
            pump(2);
        }
        /* 2. two stacked screens dropped by auto-home (no back button) */
        settings.auto_home_enabled = 1; settings.auto_home_seconds = 1;
        settings.auto_dim_enabled  = 0;
        ui_mark_activity();
        ui_push_owned(screen_efanlamp_create());
        ui_push_owned(screen_vent_advanced_create());
        pump(3);
        soak_advance(1200);
        ui_idle_tick();
        pump(2);
        /* 3. dim first, then auto-home collapses the screen under the dim */
        settings.auto_dim_enabled = 1;  settings.auto_dim_seconds  = 1;
        settings.auto_home_seconds = 3;
        ui_mark_activity();
        ui_push_owned(screen_heater_advanced_create());
        pump(3);
        soak_advance(1200); ui_idle_tick();      /* dims */
        soak_advance(2500); ui_idle_tick();      /* auto-home under the dim */
        ui_wake_now(); ui_idle_tick();           /* wake -> home */
        pump(2);
        settings.auto_dim_enabled = 0; settings.auto_home_enabled = 0;
        ui_mark_activity();

        int scr = (int)d->screen_cnt, tmr = count_timers();
        if (r == 2) { base_scr = scr; base_tmr = tmr; }
        printf("[soak] round %3d: screens=%d timers=%d\n", r, scr, tmr);
        if (r > 2 && (scr > base_scr || tmr > base_tmr)) grew = 1;
    }
    printf("[soak] %s (baseline after round 2: screens=%d timers=%d)\n",
           grew ? "FAIL: screens/timers keep growing" : "OK: stable", base_scr, base_tmr);

    /* Calendar handover: worker publishes, UI thread applies + draws home. */
    pthread_t ct;
    __atomic_store_n(&soak_cal_stop, 0, __ATOMIC_RELAXED);
    if (pthread_create(&ct, NULL, soak_cal_thread, NULL) == 0) {
        for (int i = 0; i < 300; i++) { ui_idle_tick(); pump(1); }
        __atomic_store_n(&soak_cal_stop, 1, __ATOMIC_RELAXED);
        pthread_join(ct, NULL);
        ui_idle_tick();
        printf("[soak] calendar handover: %d events applied on the UI thread\n", calendar_state.count);
    }
    return grew ? 1 : 0;
}

static void render_one(create_fn fn, const char * out) {
    lv_obj_t * scr = fn();
    lv_scr_load(scr);
    pump(150);
    dump_ppm(out);
}

int main(int argc, char ** argv) {
    signal(SIGINT, sim_signal_handler);
    signal(SIGTERM, sim_signal_handler);
    signal(SIGHUP, sim_signal_handler);

    lv_init();
    static lv_disp_draw_buf_t db;
    static lv_color_t line_buf[DISP_HOR * 60];
    lv_disp_draw_buf_init(&db, line_buf, NULL, DISP_HOR * 60);
    static lv_disp_drv_t dd;
    lv_disp_drv_init(&dd);
    dd.draw_buf = &db;
    dd.flush_cb = sim_flush;
    dd.hor_res  = DISP_HOR;
    dd.ver_res  = DISP_VER;
    lv_disp_drv_register(&dd);
    ui_theme_install();

    static lv_indev_drv_t id;
    lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER;
    id.read_cb = sim_indev_read;
    lv_indev_drv_register(&id);

    settings_load();   /* defaults when /mnt/data/toonui.cfg is absent */
    mock_state();
    layout_load_named(settings.active_layout);  /* home-tile layout: active preset */
    dim_layout_load();  /* dim block layout (seeds defaults if no file) */
    /* Sim-only: exercise a custom layout. $TOONUI_SIM_CUSTOM_LAYOUT turns it on
     * and loads $TOONUI_DATA_DIR/toonui_layout.cfg so screens render from it. */
    if (getenv("TOONUI_SIM_CUSTOM_LAYOUT")) {
        settings.custom_layout_enabled = 1;
        layout_load_named("");
    }
    /* Sim-only: $TOONUI_SIM_STOCK_TILES sets the stock-theme tile layout so the
     * stock home and its dim screen can be rendered for an arbitrary layout
     * without a /mnt/data/toonui.cfg. */
    { const char * st = getenv("TOONUI_SIM_STOCK_TILES");
      if (st) snprintf(settings.stock_tiles, sizeof settings.stock_tiles, "%s", st); }

    const char * mode = (argc > 1) ? argv[1] : "home";

    if (!strcmp(mode, "--help") || !strcmp(mode, "-h")) {
        printf("Usage: toonui-sim [OPTIONS] [COMMAND]\n\n"
               "Options:\n"
               "  --gui, --interactive, (no args)  Run interactive graphical simulator (X11/WSLg)\n"
               "  all [dir]                        Dump all screens as PPM images to dir (default /tmp)\n"
               "  nav [file] [x,y ...]             Interactive navigation replay with taps\n"
               "  <screen> [file] [x,y ...]        Render named screen (e.g. home, settings, etc.)\n");
        return 0;
    }

    if (!strcmp(mode, "gui") || !strcmp(mode, "--gui") || !strcmp(mode, "--interactive") ||
        !strcmp(mode, "--freetoon") || (!strcmp(mode, "home") && argc <= 1 && getenv("DISPLAY"))) {
        char title[128];
#ifdef TOON1
        snprintf(title, sizeof(title), "FreeToon Simulator - Toon 1 (%dx%d)", DISP_HOR, DISP_VER);
#else
        snprintf(title, sizeof(title), "FreeToon Simulator - Toon 2 (%dx%d)", DISP_HOR, DISP_VER);
#endif
        if (x11_gui_init(DISP_HOR, DISP_VER, title)) {
            dd.flush_cb = x11_flush;
            id.read_cb  = x11_indev_read;
            ui_init();
            anim_overlay_init();
            {   /* live bot in the GUI sim when a token is configured */
                signal(SIGPIPE, SIG_IGN);
                extern int telegram_start(void);
                telegram_start();
            }
            lv_refr_now(NULL);
            XFlush(g_x_display);
            fprintf(stderr, "[sim] Interactive GUI active in window (%dx%d)... Click with mouse; press ESC or close window to exit.\n", DISP_HOR, DISP_VER);

            struct timespec last_t, now_t;
            clock_gettime(CLOCK_MONOTONIC, &last_t);
            uint32_t last_idle_check = lv_tick_get();
            uint32_t last_boiler_tick = lv_tick_get();
            while (g_x11_running) {
                x11_handle_events();
                lv_timer_handler();
                clock_gettime(CLOCK_MONOTONIC, &now_t);
                uint32_t elapsed_ms = (now_t.tv_sec - last_t.tv_sec) * 1000 +
                                      (now_t.tv_nsec - last_t.tv_nsec) / 1000000;
                if (elapsed_ms > 0) {
                    lv_tick_inc(elapsed_ms);
                    last_t = now_t;
                }
                uint32_t now = lv_tick_get();
                if (now - last_idle_check > 200) {
                    ui_idle_tick();
                    last_idle_check = now;
                }
                if (now - last_boiler_tick >= 1000) {
                    sim_boiler_tick();
                    last_boiler_tick = now;
                }
                usleep(5000);
            }
            x11_gui_close();
            fprintf(stderr, "[sim] GUI closed cleanly.\n");
            exit(0);
        } else {
            fprintf(stderr, "[sim] ERROR: Failed to open graphical window (DISPLAY=%s).\n"
                            "[sim] On Windows, WSLg or an X-server (VcXsrv/Xming) is required for the desktop window.\n",
                            getenv("DISPLAY") ? getenv("DISPLAY") : "<unset>");
            return 1;
        }
    }

    /* Headless Telegram bot run: `toonui-sim telegram [seconds]`. Point
     * $TOONUI_TELEGRAM_API at a fake Bot API (or use a real test token in
     * $TOONUI_DATA_DIR/toonui.cfg) to exercise the full command loop. */
    if (!strcmp(mode, "telegram")) {
        int secs = (argc > 2) ? atoi(argv[2]) : 60;
        signal(SIGPIPE, SIG_IGN);
        extern int telegram_start(void);
        telegram_start();
        for (int i = 0; i < secs * 10; i++) usleep(100000);
        return 0;
    }

    /* Redraw cost of an idle screen: `toonui-sim redrawstat [seconds] [screen]`
     * (screen: home (default), any SCREENS name; "dim" = classic dim, "dimgrid" = custom dim).
     * Nothing changes in the mock state, so ideally almost nothing is redrawn.
     * Runs twice: with the refresh timers and with every timer of >= 500 ms
     * paused, to separate refresh callbacks from animations. */
    /* Cost of one scroll frame: `toonui-sim scrollbench [screen] [step_px] [frames]`.
     * Scrolls the screen's biggest scroller step_px per frame (bouncing at the
     * ends) and times each lv_refr_now(). Host ms are far below a Toon 1's
     * (~65x for full-screen work) -- compare variants, not absolutes.
     * SB_NOSHADOW=1 removes all shadows first. */
    /* Weather location lookup against Buienradar: `toonui-sim geocode <name>...`.
     * Full weather fetch + parse: `toonui-sim wxfetch <name> [lat lon] [id]` (id: the stored one, default De Bilt). */
    if (!strcmp(mode, "wxfetch")) {
        extern int weather_fetch_now(void);
        if (argc > 2) snprintf(settings.weather_location, sizeof settings.weather_location, "%s", argv[2]);
        settings.weather_lat[0] = settings.weather_lon[0] = 0;
        if (argc > 4) {
            snprintf(settings.weather_lat, sizeof settings.weather_lat, "%s", argv[3]);
            snprintf(settings.weather_lon, sizeof settings.weather_lon, "%s", argv[4]);
        }
        if (argc > 5) settings.weather_location_id = atoi(argv[5]);
        return weather_fetch_now() == 0 ? 0 : 1;
    }
    /* Remote page + API on :10081 with the mock state: `toonui-sim webserve [seconds]`.
     * Web login follows toonui.cfg in $TOONUI_DATA_DIR (pwa_login_enabled=0 to skip). */
    if (!strcmp(mode, "webserve")) {
        extern int pwa_start(void);
        signal(SIGPIPE, SIG_IGN);
        if (pwa_start() != 0) return 1;
        int secs = (argc > 2) ? atoi(argv[2]) : 600;
        for (int s = 0; s < secs; s++) sleep(1);
        return 0;
    }
    /* Client mode against a running master: `toonui-sim clientlink <master> [seconds]`.
     * Uses master_web_user/pass from toonui.cfg; prints the mirrored state and
     * sends one setpoint after 15 s. */
    if (!strcmp(mode, "clientlink")) {
        signal(SIGPIPE, SIG_IGN);
        settings.client_mode = 1;
        if (argc > 2) snprintf(settings.master_host, sizeof settings.master_host, "%s", argv[2]);
        toon_state.connected = 0; toon_state.setpoint = 0;
        client_link_start();
        int secs = (argc > 3) ? atoi(argv[3]) : 30;
        for (int s = 1; s <= secs; s++) {
            sleep(1);
            if (s % 5 == 0)
                printf("t=%2ds connected=%d setpoint=%.2f indoor=%.2f\n", s,
                       toon_state.connected, toon_state.setpoint, toon_state.indoor_temp);
            if (s == 15) printf("setpoint 22.5 -> %s\n", client_link_setpoint(22.5f) == 0 ? "ok" : "FAILED");
            fflush(stdout);
        }
        return 0;
    }
    /* HomeWizard kWh meter + Energy Sockets against (fake) devices from
     * toonui.cfg: `toonui-sim hwtest [seconds]`. Switches socket 0 at t=8 s. */
    if (!strcmp(mode, "hwtest")) {
        extern int homewizard_start(void);
        memset((void *)&hw_state, 0, sizeof hw_state);
        homewizard_start();
        int secs = (argc > 2) ? atoi(argv[2]) : 24;
        for (int s = 1; s <= secs; s++) {
            sleep(1);
            if (s == 8) {
                printf("t=%2ds switching socket 0 %s\n", s, hw_sockets[0].on ? "off" : "on");
                homewizard_socket_set_async(0, !hw_sockets[0].on);
            }
            if (s % 4 == 0) {
                printf("t=%2ds solar have=%d %.0f W |", s, energy_have_solar(), energy_solar_w());
                for (int i = 0; i < HW_SOCKET_MAX; i++)
                    if (settings.hw_socket_host[i][0])
                        printf(" sock%d conn=%d on=%d %.0f W", i, hw_sockets[i].connected,
                               hw_sockets[i].on, hw_sockets[i].power_w);
                printf("\n");
            }
            fflush(stdout);
        }
        return 0;
    }
    /* Water meter poller against a (fake) sensor: `toonui-sim watertest [toonwater-host] [seconds]`.
     * Without a host it relies on toonui.cfg / the ToonWater app's settings file. */
    if (!strcmp(mode, "watertest")) {
        extern int homewizard_start(void);
        if (argc > 2 && argv[2][0]) {
            snprintf(settings.toonwater_host, sizeof settings.toonwater_host, "%s", argv[2]);
            settings.enable_p1_water = 1;
        }
        memset((void *)&hw_state, 0, sizeof hw_state);
        homewizard_start();
        printf("toonwater_host=%s enable_p1_water=%d\n", settings.toonwater_host, settings.enable_p1_water);
        int secs = (argc > 3) ? atoi(argv[3]) : 40;
        for (int s = 1; s <= secs; s++) {
            sleep(1);
            if (s % 3 == 0)
                printf("t=%2ds connected=%d total=%.3f m3 flow=%.1f L/min session=%.1f L active=%d\n", s,
                       hw_state.connected_water, hw_state.water_total_m3, hw_state.water_lpm,
                       hw_state.water_session_l, hw_state.water_session_active);
            fflush(stdout);
        }
        return 0;
    }
    if (!strcmp(mode, "geocode")) {
        extern int weather_geocode(const char * city);
        for (int a = 2; a < argc; a++) weather_geocode(argv[a]);
        return 0;
    }

    if (!strcmp(mode, "scrollbench")) {
        const char * which = (argc > 2) ? argv[2] : "settings";
        int step   = (argc > 3) ? atoi(argv[3]) : 8;
        int frames = (argc > 4) ? atoi(argv[4]) : 300;
        ui_init();
        create_fn sfn = NULL;
        for (int i = 0; i < N_SCREENS; i++)
            if (!strcmp(which, SCREENS[i].name)) sfn = SCREENS[i].fn;
        if (!sfn) { fprintf(stderr, "[scrollbench] unknown screen '%s'\n", which); return 1; }
        ui_push(sfn());
        pump(100);
        for (lv_timer_t * t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
            if (t->period >= 500) lv_timer_pause(t);
        if (getenv("SB_NOSHADOW") || getenv("SB_NORADIUS")) sb_strip_shadow(lv_scr_act());
        lv_coord_t range = 0;
        lv_obj_t * sc = sb_find_scroller(lv_scr_act(), &range);
        if (!sc) { fprintf(stderr, "[scrollbench] nothing scrollable\n"); return 1; }
        static uint64_t px_sum; static uint32_t refr_n, refr_ms;
        dd.monitor_cb = redrawstat_monitor;
        redrawstat_ptr(&px_sum, &refr_n, &refr_ms);
        lv_refr_now(NULL);
        px_sum = 0; refr_n = 0;
        double total = 0, worst = 0;
        int dir = 1;
        for (int i = 0; i < frames; i++) {
            if (dir > 0 && lv_obj_get_scroll_bottom(sc) < step) dir = -1;
            if (dir < 0 && lv_obj_get_scroll_top(sc) < step) dir = 1;
            lv_obj_scroll_by(sc, 0, -dir * step, LV_ANIM_OFF);
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            lv_refr_now(NULL);
            clock_gettime(CLOCK_MONOTONIC, &b);
            double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
            total += ms; if (ms > worst) worst = ms;
        }
        fprintf(stderr, "[scrollbench] %s step %d px%s: %.3f ms/frame avg, %.3f worst, %.0f px/frame, scroll range %d px\n",
                which, step, getenv("SB_NOSHADOW") ? " (no shadows)" : "",
                total / frames, worst, refr_n ? (double)px_sum / refr_n : 0.0, (int)range);
        return 0;
    }

    if (!strcmp(mode, "redrawstat")) {
        int secs = (argc > 2) ? atoi(argv[2]) : 20;
        const char * which = (argc > 3) ? argv[3] : "home";
        ui_init();
        if (strcmp(which, "home")) {
            create_fn sfn = NULL;
            if (!strcmp(which, "dimgrid")) { settings.dim_custom_enabled = 1; sfn = screen_dim_create; }
            if (!strcmp(which, "dim"))     { settings.dim_custom_enabled = 0; sfn = screen_dim_create; }
            for (int i = 0; !sfn && i < N_SCREENS; i++)
                if (!strcmp(which, SCREENS[i].name)) sfn = SCREENS[i].fn;
            if (!sfn) { fprintf(stderr, "[redrawstat] unknown screen '%s'\n", which); return 1; }
            ui_push(sfn());
        }
        pump(200);                                     /* settle: first layout + loads */
        static uint64_t px_sum; static uint32_t refr_n, refr_ms;
        dd.monitor_cb = redrawstat_monitor;
        redrawstat_ptr(&px_sum, &refr_n, &refr_ms);
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {                           /* pause the refresh timers (>= 500 ms) */
                for (lv_timer_t * t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
                    if (t->period >= 500) lv_timer_pause(t);
                pump(40);
            }
            px_sum = 0; refr_n = 0; refr_ms = 0;
            rs_trace = getenv("REDRAW_TRACE") ? atoi(getenv("REDRAW_TRACE")) : 0;
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            int frames = secs * 1000 / 16;
            for (int i = 0; i < frames; i++) { lv_timer_handler(); lv_tick_inc(16); }
            clock_gettime(CLOCK_MONOTONIC, &b);
            double cpu_ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
            fprintf(stderr, "[redrawstat] %s %s: %d s, %u redraws, %.0f px/s (%.2f screens/s), host CPU %.1f ms/s\n",
                    which, pass == 0 ? "with refresh  " : "refresh paused",
                    secs, refr_n, (double)px_sum / secs,
                    (double)px_sum / secs / (DISP_HOR * DISP_VER), cpu_ms / secs);
        }
        return 0;
    }

    /* Settings → Telegram render: `toonui-sim tgui out.ppm [scroll_px]`. */
    if (!strcmp(mode, "tgui")) {
        const char * out = (argc > 2) ? argv[2] : "/tmp/sim_tgui.ppm";
        int scroll = (argc > 3) ? atoi(argv[3]) : 0;
        signal(SIGPIPE, SIG_IGN);
        extern int telegram_start(void);
        telegram_start();
        lv_obj_t * scr = screen_settings_create();
        lv_scr_load(scr);
        pump(60);
        extern lv_obj_t * screen_settings_open_telegram(void);
        lv_obj_t * m = screen_settings_open_telegram();
        pump(80);
        if (scroll && m) {
            /* backdrop → panel → body (first child) */
            lv_obj_t * panel = lv_obj_get_child(m, 0);
            lv_obj_t * body = panel ? lv_obj_get_child(panel, 0) : NULL;
            if (body) lv_obj_scroll_to_y(body, scroll, LV_ANIM_OFF);
            pump(40);
        }
        dump_ppm(out);
        fprintf(stderr, "[sim] tgui -> %s\n", out);
        return 0;
    }

    if (!strcmp(mode, "soak")) {
        int rounds = (argc > 2) ? atoi(argv[2]) : 30;
        if (rounds < 2) rounds = 2;
        return sim_soak(rounds);
    }

    if (!strcmp(mode, "all")) {
        const char * dir = (argc > 2) ? argv[2] : "/tmp";
        char path[256];
        for (int i = 0; i < N_SCREENS; i++) {
            snprintf(path, sizeof path, "%s/sim_%s.ppm", dir, SCREENS[i].name);
            render_one(SCREENS[i].fn, path);
            fprintf(stderr, "[sim] %-12s -> %s  (%dx%d)\n", SCREENS[i].name, path, DISP_HOR, DISP_VER);
        }
        return 0;
    }

    if (!strcmp(mode, "nav")) {
        const char * out = (argc > 2) ? argv[2] : "/tmp/sim_nav.ppm";
        ui_init();                 /* home + ui stack so ui_push navigation works */
        pump(100);
        if (getenv("TOONUI_SIM_PAGE2")) { screen_home_force_page(1); pump(40); }  /* page 2 is gesture-only */
        for (int a = 3; a < argc; a++) {
            int x, y;
            if (sscanf(argv[a], "%d,%d", &x, &y) == 2) sim_tap(x, y);
        }
        pump(60);
        dump_ppm(out);
        fprintf(stderr, "[sim] nav (%d taps) -> %s  (%dx%d)\n", argc - 3, out, DISP_HOR, DISP_VER);
        return 0;
    }

    if (!strcmp(mode, "anim")) {
        const char * effect_name = (argc > 2) ? argv[2] : "leaves";
        const char * out = (argc > 3) ? argv[3] : "/tmp/sim_anim.ppm";
        ui_init();
        anim_overlay_init();
        anim_overlay_trigger_by_name(effect_name, 25);
        pump(40);
        dump_ppm(out);
        fprintf(stderr, "[sim] anim %s -> %s\n", effect_name, out);
        return 0;
    }

    /* single named screen, optionally followed by "x,y" taps to drive it */
    const char * out = (argc > 2) ? argv[2] : "/tmp/sim.ppm";
    create_fn fn = screen_home_create;
    for (int i = 0; i < N_SCREENS; i++)
        if (!strcmp(mode, SCREENS[i].name)) { fn = SCREENS[i].fn; break; }
    lv_obj_t * scr = fn();
    lv_scr_load(scr);
    pump(150);
    /* Steps: "x,y" tap; "scroll:<dy>" scrolls the biggest scroller (+ = down);
     * "kbtype:<text>" / "kbclear" type into / empty the textarea the on-screen
     * keyboard is bound to (what tapping its keys does); "shot:<file>" dumps
     * the screen mid-sequence. */
    int taps = 0;
    for (int a = 3; a < argc; a++) {
        int x, y;
        if (sscanf(argv[a], "%d,%d", &x, &y) == 2) { sim_tap(x, y); taps++; continue; }
        if (!strncmp(argv[a], "scroll:", 7)) {
            lv_coord_t best = 0;
            lv_obj_t * sc = sb_find_scroller(lv_layer_top(), &best);   /* open modal first */
            if (!sc) { best = 0; sc = sb_find_scroller(lv_scr_act(), &best); }
            if (sc) lv_obj_scroll_by_bounded(sc, 0, -atoi(argv[a] + 7), LV_ANIM_OFF);
            pump(20);
            continue;
        }
        if (!strncmp(argv[a], "kbtype:", 7) || !strcmp(argv[a], "kbclear")) {
            lv_obj_t * kb = NULL;
            for (uint32_t i = 0; !kb && i < lv_obj_get_child_cnt(lv_layer_top()); i++) {
                lv_obj_t * c = lv_obj_get_child(lv_layer_top(), i);
                if (lv_obj_check_type(c, &lv_keyboard_class)) kb = c;
            }
            lv_obj_t * ta = kb ? lv_keyboard_get_textarea(kb) : NULL;
            if (!ta) { fprintf(stderr, "[sim] %s: no keyboard textarea\n", argv[a]); continue; }
            if (argv[a][2] == 'c') { while (lv_textarea_get_text(ta)[0]) { lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); lv_textarea_del_char(ta); } }
            else lv_textarea_add_text(ta, argv[a] + 7);
            fprintf(stderr, "[sim] keyboard textarea now '%s'\n", lv_textarea_get_text(ta));
            pump(10);
            continue;
        }
        if (!strncmp(argv[a], "shot:", 5)) { pump(30); dump_ppm(argv[a] + 5); continue; }
    }
    pump(60);
    dump_ppm(out);
    fprintf(stderr, "[sim] %s (%d taps) -> %s  (%dx%d)\n", mode, taps, out, DISP_HOR, DISP_VER);
    return 0;
}
