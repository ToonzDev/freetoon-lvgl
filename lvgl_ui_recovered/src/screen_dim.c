/*
 * Dim/ambient screen — pure black background, large white clock plus
 * indoor temp and setpoint. Tap anywhere to wake.
 * No colour, no icons; this is the screen we want visible while idle.
 */
#include "screens.h"
#include "ui_lazy.h"
#include "display.h"
#include "i18n.h"
#include "boxtalk.h"
#include "settings.h"
#include "homewizard.h"
#include "energy.h"
#include "homeassistant.h"
#include "packages.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "meteradapter.h"   /* meter_state — for the restored dim energy bar */
#include "icons.h"
#include "theme.h"
#include "fonts_opensans.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* identity colours, same in every theme: Life360 person tints */
#define DIM_COL_LIFE360_A 0x88aaff
#define DIM_COL_LIFE360_B 0xff88cc

LV_FONT_DECLARE(lv_font_montserrat_96_custom);

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_clock_h = NULL;
static lv_obj_t * lbl_clock_m = NULL;
static lv_obj_t * lbl_clock_colon = NULL;   /* pulsing ":" overlay */
static lv_obj_t * lbl_date;
static lv_obj_t * dim_moon_img;   /* moon-phase widget — always shown */
static lv_obj_t * lbl_temp;
static lv_obj_t * lbl_setpoint;
static lv_obj_t * lbl_outside_temp = NULL;
static lv_obj_t * lbl_program;
static lv_obj_t * lbl_metrics;     /* TVOC / eCO2 / CH-water-pressure row */
static lv_obj_t * lbl_burner;      /* "90 C" when CH, hidden otherwise */
static lv_obj_t * dim_img_flame;   /* CH flame — paired with lbl_burner */
static lv_obj_t * dim_img_faucet;  /* DHW faucet — visible only on dhw_on */
static lv_obj_t * dim_img_drop;    /* paired water-drop next to the faucet */
static lv_obj_t * wx_icon = NULL;
static lv_obj_t * lbl_outside = NULL;
static lv_obj_t * waste_icon = NULL;
static lv_obj_t * waste_icon_2 = NULL;   /* 2nd bin shown when same day has 2 types */
static lv_obj_t * lbl_waste = NULL;
static lv_obj_t * waste_box_ptr = NULL;
static lv_obj_t * dim_fc_icon[WEATHER_FORECAST_DAYS];
static lv_obj_t * dim_fc_day[WEATHER_FORECAST_DAYS];
static lv_obj_t * dim_fc_temp[WEATHER_FORECAST_DAYS];
/* City header above the forecast strip — mirrors the home tile. */
static lv_obj_t * dim_lbl_city = NULL;
/* Life360 — stacked TOP_RIGHT under lbl_outside, opposite the waste block. */
static lv_obj_t * dim_lbl_life360_a = NULL;
static lv_obj_t * dim_lbl_life360_b   = NULL;
static lv_obj_t * dim_vent_fan  = NULL;   /* spinning fan icon */
static lv_obj_t * dim_vent_lbl  = NULL;   /* "57 %" — actual ExhFanSpeed */
static lv_obj_t * dim_img_water = NULL;   /* drop icon, visible while pouring */
static lv_obj_t * dim_lbl_water = NULL;   /* "1.4 L/m" / "+1.4 L" */
static int        dim_vent_period_ms = 0; /* current spin animation period */
static lv_obj_t * lbl_dim_nilm  = NULL;  /* NILM transient device event */
static lv_timer_t * refresh_timer = NULL;

/* ---- usage bars flanking the clock: energy now (W) + gas day (m³) + energy day (kWh) ----
 * RESTORED (PR follow-up): 3 usage bars, gated by settings.show_dim_bars (default on).
 * Placement honours settings.dim_bars_swap (0..3):
 *   0: Gas left | Electricity right (actual + day total)
 *   1: Electricity left (actual + day total) | Gas right
 *   2: Day totals left (gas + elec day) | Electricity actual right
 *   3: Electricity actual left | Day totals right (gas + elec day) */
#define DIM_BAR_W     48      /* bar width, design px */
#define DIM_BAR_Y     25      /* vertical centre offset from mid-screen */
#define DIM_CLOCK_H   96      /* clock font px; envelope = 2x this */

static lv_obj_t * bar_gas_env,  * bar_gas_fill,  * bar_gas_cap;
static lv_obj_t * bar_eact_env, * bar_eact_fill, * bar_eact_cap;
static lv_obj_t * bar_eday_env, * bar_eday_fill, * bar_eday_cap;

#define DIM_NSEG 12
static lv_obj_t * bar_gas_segs[DIM_NSEG];
static lv_obj_t * bar_eact_segs[DIM_NSEG];
static lv_obj_t * bar_eday_segs[DIM_NSEG];

static int   dim_bar_h = 0;        /* envelope height (px, computed at create) */
#define DIM_E_FULL_W    5000.0f     /* power at full bar height (fixed scale) */
#define DIM_G_FULL_M3H  2.0f        /* gas (m³/h) at full bar height (fixed scale) */

static void dim_vent_fan_anim_cb(void * obj, int32_t v) {
    /* ~10 fps is plenty for a spinning icon on an idle screen. */
    static uint32_t last;
    ui_img_set_angle_every((lv_obj_t *)obj, v, &last, 100);
}
static void dim_vent_apply_anim(int rpm) {
    if (!dim_vent_fan) return;
    /* Park below 50 rpm. See screen_home.c vent_apply_fan_anim — driving
       off rpm because Itho's ExhFanSpeed is unreliable and its Low/High
       labels are backwards on this unit. */
    if (rpm < 50) {
        if (dim_vent_period_ms == 0) return;
        dim_vent_period_ms = 0;
        lv_anim_del(dim_vent_fan, NULL);
        return;
    }
    /* Same steeper curve as the home tile, mapped onto the CVE's real rpm
       band (~1150 low → ~2700 high) so High visibly spins up. ~1.3 ms/rpm. */
    int period = 2600 - (rpm - 1000) * 13 / 10;
    if (period < 280)  period = 280;
    if (period > 2600) period = 2600;
    /* Hysteresis: every poll the rpm jitters ±1 which would re-spin the
       anim from 0° if we treated each tiny period delta as a change. Only
       restart when the period actually moves > 100 ms. */
    if (abs(period - dim_vent_period_ms) < 100) return;
    dim_vent_period_ms = period;
    lv_anim_del(dim_vent_fan, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dim_vent_fan);
    lv_anim_set_exec_cb(&a, dim_vent_fan_anim_cb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, period);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* Build one bar slot. Envelope is a dark track at the screen edge; the fill
 * is a child anchored to the bottom that grows upward. Caption is align_to'd to
 * the envelope. Neither is CLICKABLE, so a tap on the bar falls through to wake. */
static void dim_make_bar(lv_obj_t ** env, lv_obj_t ** fill,
                         lv_obj_t ** cap, lv_obj_t * segs[DIM_NSEG]) {
    int bw = DIM_BAR_W;

    *env = lv_obj_create(scr_root);
    lv_obj_remove_style_all(*env);
    lv_obj_set_size(*env, bw, dim_bar_h);
    ui_obj_set_style_bg_opa(*env, LV_OPA_TRANSP, 0);
    ui_obj_clear_flag(*env, LV_OBJ_FLAG_CLICKABLE);
    ui_obj_clear_flag(*env, LV_OBJ_FLAG_SCROLLABLE);

    *fill = lv_obj_create(*env);
    lv_obj_remove_style_all(*fill);
    lv_obj_set_width(*fill, bw);
    lv_obj_set_height(*fill, 0);
    ui_obj_set_style_bg_color(*fill, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(*fill, 4, 0);
    ui_obj_clear_flag(*fill, LV_OBJ_FLAG_CLICKABLE);
    ui_obj_align(*fill, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* Vertical segmented elements (classic Toon style) */
    int seg_gap = 4;
    int seg_h = (dim_bar_h - (DIM_NSEG - 1) * seg_gap) / DIM_NSEG;
    int seg_w = bw - 8;
    int th = DIM_NSEG * seg_h + (DIM_NSEG - 1) * seg_gap;
    int y0 = (dim_bar_h - th) / 2;

    for (int i = 0; i < DIM_NSEG; i++) {
        segs[i] = lv_obj_create(*env);
        lv_obj_remove_style_all(segs[i]);
        lv_obj_set_size(segs[i], seg_w, seg_h);
        lv_obj_set_pos(segs[i], 4, y0 + i * (seg_h + seg_gap));
        lv_obj_set_style_radius(segs[i], 2, 0);
        ui_obj_set_style_border_width(segs[i], 0, 0);
        ui_obj_set_style_bg_color(segs[i], lv_color_hex(THEME_OFF), 0);
        ui_obj_set_style_bg_opa(segs[i], LV_OPA_COVER, 0);
        ui_obj_clear_flag(segs[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    *cap = lv_label_create(scr_root);
    ui_obj_set_style_text_color(*cap, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_text_font(*cap, (DISP_VER < 600) ? &lv_font_montserrat_14 : &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(*cap, LV_TEXT_ALIGN_CENTER, 0);
    ui_label_set_text(*cap, "");
}

/* Dynamically position the 3 bars according to settings.dim_bars_swap (0..3) */
static void dim_layout_bars(void) {
    if (!bar_gas_env || !bar_eact_env || !bar_eday_env) return;
    int bw = DIM_BAR_W;
    int x_single = SX(24);
    int x_outer  = SX(16);
    int x_inner  = SX(16 + bw + 10);

    int gas_side, gas_x;
    int act_side, act_x;
    int day_side, day_x;

    switch (settings.dim_bars_swap) {
        case 1: /* Stroom links (Actueel + Dag), Gas rechts */
            act_side = -1; act_x = x_outer;
            day_side = -1; day_x = x_inner;
            gas_side = +1; gas_x = x_single;
            break;
        case 2: /* Dagtotalen links (Gas + Stroom Dag), Stroom Actueel rechts */
            gas_side = -1; gas_x = x_outer;
            day_side = -1; day_x = x_inner;
            act_side = +1; act_x = x_single;
            break;
        case 3: /* Stroom Actueel links, Dagtotalen rechts (Gas + Stroom Dag) */
            act_side = -1; act_x = x_single;
            gas_side = +1; gas_x = x_outer;
            day_side = +1; day_x = x_inner;
            break;
        default: /* 0: Gas links, Stroom rechts (Actueel + Dag) */
            gas_side = -1; gas_x = x_single;
            act_side = +1; act_x = x_inner;
            day_side = +1; day_x = x_outer;
            break;
    }

    ui_obj_align(bar_gas_env,  (gas_side < 0) ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 (gas_side < 0) ? gas_x : -gas_x, SY(DIM_BAR_Y));
    ui_obj_align(bar_eact_env, (act_side < 0) ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 (act_side < 0) ? act_x : -act_x, SY(DIM_BAR_Y));
    ui_obj_align(bar_eday_env, (day_side < 0) ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 (day_side < 0) ? day_x : -day_x, SY(DIM_BAR_Y));
}

/* Apply a value to a bar slot. ratio 0..1 of the envelope. Hidden when !show. */
static void dim_bar_set(lv_obj_t * env, lv_obj_t * fill, lv_obj_t * cap,
                        lv_obj_t * segs[DIM_NSEG],
                        int show, float ratio, uint32_t color, const char * txt) {
    if (!env) return;
    if (!show) {
        ui_obj_add_flag(env, LV_OBJ_FLAG_HIDDEN);
        if (cap) ui_obj_add_flag(cap, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    int vert = settings.dim_bars_vertical;
    if (vert) {
        ui_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
        int lit = (int)(ratio * DIM_NSEG + 0.5f);
        if (lit > DIM_NSEG) lit = DIM_NSEG;
        for (int i = 0; i < DIM_NSEG; i++) {
            if (!segs[i]) continue;
            ui_obj_clear_flag(segs[i], LV_OBJ_FLAG_HIDDEN);
            int fb = DIM_NSEG - 1 - i;   /* 0 = bottom, 11 = top */
            uint32_t c = (fb < lit) ? (fb < 7 ? THEME_SUCCESS : THEME_WARNING) : THEME_OFF;
            ui_obj_set_style_bg_color(segs[i], lv_color_hex(c), 0);
        }
    } else {
        for (int i = 0; i < DIM_NSEG; i++) {
            if (segs[i]) ui_obj_add_flag(segs[i], LV_OBJ_FLAG_HIDDEN);
        }
        int h = (int)(ratio * dim_bar_h + 0.5f);
        if (ratio > 0 && h < 3) h = 3;            /* show a sliver when nonzero */
        lv_obj_set_height(fill, h);
        ui_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        ui_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
        ui_obj_clear_flag(fill, LV_OBJ_FLAG_HIDDEN);
    }

    ui_obj_clear_flag(env, LV_OBJ_FLAG_HIDDEN);
    if (cap) {
        /* Value over unit ("384\nW"): two bars sit 10px apart, and side by
         * side captions like "384 W" + "-- kWh" ran into each other. */
        char two[32];
        snprintf(two, sizeof two, "%s", txt ? txt : "");
        char * sp = strrchr(two, ' ');
        if (sp) *sp = '\n';
        ui_label_set_text(cap, two);
        ui_obj_set_style_text_color(cap, lv_color_hex(THEME_DIM_TEXT), 0);
        ui_obj_set_style_text_font(cap, (DISP_VER < 600) ? &lv_font_montserrat_14 : &lv_font_montserrat_18, 0);
        lv_obj_align_to(cap, env, LV_ALIGN_OUT_BOTTOM_MID, 0, SY(10));
        ui_obj_clear_flag(cap, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_wake_tap(lv_event_t * e) {
    (void)e;
    ui_wake_now();
}

static const lv_font_t * pick_date_font(const char * txt, lv_coord_t max_w, lv_coord_t max_h) {
    static const lv_font_t * const fonts[] = {
        &lv_font_montserrat_48,
        &lv_font_montserrat_36,
        &lv_font_montserrat_28,
        &lv_font_montserrat_24,
        &lv_font_montserrat_22,
        &lv_font_montserrat_20,
        &lv_font_montserrat_18,
        &lv_font_montserrat_14,
    };
    if (!txt || !txt[0]) return &lv_font_montserrat_22;
    for (size_t i = 0; i < sizeof(fonts)/sizeof(fonts[0]); i++) {
        const lv_font_t * f = fonts[i];
        if (!f) continue;
        lv_point_t sz;
        lv_txt_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= max_w && (max_h <= 0 || sz.y <= max_h)) {
            return f;
        }
    }
    return &lv_font_montserrat_14;
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (lv_scr_act() != scr_root) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk_h[8], clk_m[8];
    strftime(clk_h, sizeof(clk_h), "%H", &tm);
    strftime(clk_m, sizeof(clk_m), "%M", &tm);
    ui_label_set_text(lbl_clock_h, clk_h);
    ui_label_set_text(lbl_clock_m, clk_m);
    lv_obj_update_layout(lbl_clock_h);
    lv_obj_update_layout(lbl_clock_m);
    lv_obj_align_to(lbl_clock_h, lbl_clock_colon, LV_ALIGN_OUT_LEFT_MID, -SX(2), 0);
    lv_obj_align_to(lbl_clock_m, lbl_clock_colon, LV_ALIGN_OUT_RIGHT_MID, SX(2), 0);
    char dt[64];
    snprintf(dt, sizeof(dt), "%s\n%d %s",
             i18n_day_long(tm.tm_wday),
             tm.tm_mday,
             i18n_month_long(tm.tm_mon));
    const lv_font_t * df = pick_date_font(dt, DISP_HOR - SX(120), SY(60));
    ui_obj_set_style_text_font(lbl_date, df, 0);
    ui_label_set_text(lbl_date, dt);

    /* Moon (top-right, beside the current-weather icon): white at night,
       hidden during the day. Day/night from a real sunrise/sunset calc. */
    if (dim_moon_img) {
        if (is_daytime_now()) {
            ui_obj_add_flag(dim_moon_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            const lv_img_dsc_t * ph = moon_phase_icon(80);
            if (lv_img_get_src(dim_moon_img) != ph)
                ui_img_set_src(dim_moon_img, ph);
            ui_obj_clear_flag(dim_moon_img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Always paint values: if data not yet present, fall back to a
       "wait..." marker instead of leaving the stale "-- C" default. */
    if (toon_state.indoor_temp > 0)
        ui_label_set_text_fmt(lbl_temp, "%.1f°", toon_state.indoor_temp);
    else
        ui_label_set_text(lbl_temp, "...");
    lv_obj_update_layout(lbl_temp);
    if (dim_img_flame) lv_obj_align_to(dim_img_flame, lbl_temp, LV_ALIGN_OUT_BOTTOM_MID, 0, SY(8));
    /* Ventilation fan + % sit left of the big temperature, whatever its
     * width (a fixed offset put the fan on top of "20.0°C"). */
    if (dim_vent_fan) lv_obj_align_to(dim_vent_fan, lbl_temp, LV_ALIGN_OUT_LEFT_MID, -SX(8), -SY(10));
    if (dim_vent_lbl && dim_vent_fan) lv_obj_align_to(dim_vent_lbl, dim_vent_fan, LV_ALIGN_OUT_BOTTOM_MID, 0, -SY(14));
    if (dim_img_faucet) lv_obj_align_to(dim_img_faucet, lbl_temp, LV_ALIGN_OUT_BOTTOM_MID, -SX(10), SY(8));
    if (dim_img_drop) lv_obj_align_to(dim_img_drop, dim_img_faucet, LV_ALIGN_OUT_RIGHT_MID, SX(8), 0);

    /* Setpoint / program state displayed in small font (CR2) */
    if (lbl_setpoint) {
        const char * prog = program_label();
        if (toon_state.setpoint > 0) {
            ui_label_set_text_fmt(lbl_setpoint, "%s - %.1f°C", prog, toon_state.setpoint);
        } else {
            ui_label_set_text(lbl_setpoint, prog);
        }
        lv_obj_update_layout(lbl_setpoint);
        if (dim_img_flame) lv_obj_align_to(dim_img_flame, lbl_setpoint, LV_ALIGN_OUT_BOTTOM_MID, 0, SY(8));
        if (dim_img_faucet) lv_obj_align_to(dim_img_faucet, lbl_setpoint, LV_ALIGN_OUT_BOTTOM_MID, -SX(10), SY(8));
        if (dim_img_drop) lv_obj_align_to(dim_img_drop, dim_img_faucet, LV_ALIGN_OUT_RIGHT_MID, SX(8), 0);
    }

    if (lbl_metrics) {
        if (!settings.show_dim_metrics) {
            ui_obj_add_flag(lbl_metrics, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* TVOC / eCO2 ppm / CH water pressure / air-quality badge on one
               greyed row. Missing inputs collapse to "--" so the strip layout
               stays stable. AQ label is appended only when we actually have
               air-quality data to classify. */
            char buf[200];
            char bar[24]  = "CV --";
            if (toon_state.water_pressure > 0.1f)
                snprintf(bar,  sizeof bar,  "CV %.1f bar", toon_state.water_pressure);
#ifndef TOON1
            /* TVOC / eCO2 / air-quality come from the eCO2/TVOC air sensor
               that only Toon 2 has -- on Toon 1 the row is just CH pressure. */
            char tvoc[24] = "TVOC --";
            char co2[24]  = "CO2 --";
            if (toon_state.tvoc)
                snprintf(tvoc, sizeof tvoc, "TVOC %d ppb", toon_state.tvoc);
            if (toon_state.eco2)
                snprintf(co2,  sizeof co2,  "CO2 %d ppm", toon_state.eco2);
            const char * aql = air_quality_label(toon_state.eco2, toon_state.tvoc);
            if (*aql)
                snprintf(buf, sizeof buf, tr("%s    %s    %s    Lucht: %s",
                                              "%s    %s    %s    Air: %s"),
                         tvoc, co2, bar, aql);
            else
                snprintf(buf, sizeof buf, "%s    %s    %s", tvoc, co2, bar);
#else
            snprintf(buf, sizeof buf, "%s", bar);
#endif
            ui_label_set_text(lbl_metrics, buf);
            ui_obj_clear_flag(lbl_metrics, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Burner state — symbol-first now. CH-heating shows just the target
       degrees ("-> 90 C", red); DHW shows a faucet + water-drop icon pair
       (no text — the icons say it). Idle hides everything so the dim
       screen stays clean. */
    /* Live water-flow indicator on dim, right side below the radiator slot
     * so it can co-exist with the CH flame. Same visibility rules as the
     * home-tile version: drop+L/m while pouring, "+X.X L" briefly after. */
    if (dim_img_water && dim_lbl_water) {
        if (hw_state.connected_water && hw_state.water_lpm > 0.05f) {
            ui_label_set_text_fmt(dim_lbl_water, "%.1f L/m",
                                  hw_state.water_lpm);
            ui_obj_clear_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);
            ui_obj_clear_flag(dim_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else if (hw_state.connected_water && hw_state.water_session_l > 0) {
            ui_label_set_text_fmt(dim_lbl_water, "+%.1f L",
                                  hw_state.water_session_l);
            ui_obj_clear_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);
            ui_obj_clear_flag(dim_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);
            ui_obj_add_flag(dim_lbl_water, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Show the radiator-with-flame glyph next to the indoor temp when the
     * boiler is firing CH — original-Toon style. No "90 C" target text:
     * the glyph itself is the signal. */
    if (dim_img_flame) {
        if (toon_state.burner_on)
            ui_obj_clear_flag(dim_img_flame, LV_OBJ_FLAG_HIDDEN);
        else
            ui_obj_add_flag(dim_img_flame, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_burner) ui_obj_add_flag(lbl_burner, LV_OBJ_FLAG_HIDDEN);
    if (dim_img_faucet && dim_img_drop) {
        if (toon_state.dhw_on) {
            ui_obj_clear_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);
            ui_obj_clear_flag(dim_img_drop,   LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);
            ui_obj_add_flag(dim_img_drop,   LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Vent — fan icon spin tracks fan_rpm; label shows the ventilation MODE
       (Low/High/Auto/Medium/Timer) + pct + remaining ("Auto 3 %" or
       "Timer 25m 100 %"). The mode word comes from fan_info if the firmware
       still publishes FanInfo, else from last_cmd (the itho/lastcmd MQTT
       topic) — current firmware dropped FanInfo so last_cmd is the source.
       Both are already swap-corrected to the user-intent label. Online/offline
       (itho/lwt) is shown on the HOME tile, not here. */
    if (dim_vent_fan && dim_vent_lbl) {
        if (vent_state.connected && vent_state.itho_online == 0) {
            /* Itho reported itself offline via its MQTT LWT — show that
               (overrides the mode) and park the spinner. Normal case below
               shows what it's doing (mode + %). */
            ui_label_set_text(dim_vent_lbl, tr("offline", "offline"));
            dim_vent_apply_anim(0);
            ui_obj_clear_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);
            ui_obj_clear_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);
        } else if (vent_state.connected) {
            const char * pretty = vent_mode_label();
            if (vent_state.remaining_min > 0)
                ui_label_set_text_fmt(dim_vent_lbl, "%s %dm %d %%",
                                      pretty, vent_state.remaining_min,
                                      vent_state.speed_pct);
            else
                ui_label_set_text_fmt(dim_vent_lbl, "%s %d %%",
                                      pretty, vent_state.speed_pct);
            dim_vent_apply_anim(vent_state.fan_rpm);
            ui_obj_clear_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);
            ui_obj_clear_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);
            ui_obj_add_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (wx_icon) {
        if (settings.show_dim_weather && weather_state.day_count > 0) {
            const char * ic = weather_state.days[0].icon;
            ui_img_set_src(wx_icon, weather_icon_for_lg(ic));
            ui_obj_set_style_img_recolor(wx_icon,
                lv_color_hex(weather_icon_color_for(ic)), 0);
            ui_obj_clear_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Forecast strip — 3-hourly to match home screen. Falls back to daily
     * if the hourly feed hasn't populated yet (first 30 s after boot). */
    /* City header — weather only. */
    if (dim_lbl_city) {
        if (settings.show_dim_weather && weather_state.connected) {
            const char * city = settings.weather_location[0]
                                ? settings.weather_location : tr("Verwachting", "Forecast");
            ui_label_set_text_fmt(dim_lbl_city, tr("%s  -  %.1f°C nu", "%s  -  %.1f°C now"),
                                  city, weather_display_temp());
            ui_obj_clear_flag(dim_lbl_city, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(dim_lbl_city, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Life360 — top-right stack under the outside temp. Name prefix +
     * address; colour still identifies who's who. Hidden until data lands. */
    if (dim_lbl_life360_a) {
        if (ha_state.loc_a[0]) {
            ui_label_set_text_fmt(dim_lbl_life360_a, "%s: %s",
                                  settings.life360_a_name[0] ? settings.life360_a_name : "A",
                                  ha_state.loc_a);
            ui_obj_clear_flag(dim_lbl_life360_a, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(dim_lbl_life360_a, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (dim_lbl_life360_b) {
        if (ha_state.loc_b[0]) {
            ui_label_set_text_fmt(dim_lbl_life360_b, "%s: %s",
                                  settings.life360_b_name[0] ? settings.life360_b_name : "B",
                                  ha_state.loc_b);
            ui_obj_clear_flag(dim_lbl_life360_b, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(dim_lbl_life360_b, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Forecast strip: 5 days, weekday + max temperature + lighter icon (#49). */
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++)
        dim_fc_paint(dim_fc_icon[i], dim_fc_day[i], dim_fc_temp[i], i);
    if (lbl_outside_temp) {
        if (settings.show_dim_weather && weather_state.connected) {
            dim_set_outside_temp(lbl_outside_temp, weather_display_temp(), SX(330));
            /* keep the icon centred under the (right-aligned) temp */
            lv_obj_update_layout(lbl_outside_temp);
            if (wx_icon) lv_obj_align_to(wx_icon, lbl_outside_temp, LV_ALIGN_OUT_BOTTOM_MID, 0, SY(2));
            ui_obj_clear_flag(lbl_outside_temp, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(lbl_outside_temp, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (wx_icon) {
        if (settings.show_dim_weather && weather_state.connected) {
            ui_img_set_src(wx_icon, weather_icon_for_lg(weather_state.current_icon));
            ui_obj_clear_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
            if (lbl_outside) ui_obj_clear_flag(lbl_outside, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
            if (lbl_outside) ui_obj_add_flag(lbl_outside, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Waste: next upcoming pickup — simplified and persistent for symmetry. */
    if (waste_icon && lbl_waste) {
        if (settings.show_dim_waste && waste_state.connected) {
            waste_pickup_t wp;
            /* Honour dim_waste_lead_days and dim_waste_hour: alert window starts at
               dim_waste_hour N days before pickup, and ends at dim_waste_hour on collection day. */
            int within = waste_get_dim_pickup(&wp, settings.dim_waste_lead_days, settings.dim_waste_hour);
            if (within) {
                char l_short[48];
                waste_shorten_label(wp.labels, l_short, sizeof l_short);
                const char * l_clean = l_short;
                /* Same day, two types ("Papier+Plastic") → one icon per type.
                   Split on the first '+'. */
                char t1[48], t2[48] = "";
                snprintf(t1, sizeof t1, "%s", l_clean);
                char * plus = strchr(t1, '+');
                if (plus) {
                    *plus = 0;
                    const char * p2 = plus + 1; while (*p2 == ' ') p2++;
                    snprintf(t2, sizeof t2, "%s", p2);
                    size_t n = strlen(t1); while (n && t1[n-1] == ' ') t1[--n] = 0;
                }
                ui_img_set_src(waste_icon, waste_icon_for_label_lg(t1));
                ui_obj_set_style_img_recolor(waste_icon, lv_color_hex(THEME_DIM_TEXT), 0);
                ui_obj_set_style_img_recolor_opa(waste_icon, 255, 0);
                if (waste_icon_2) {
                    if (t2[0]) {
                        ui_img_set_src(waste_icon_2, waste_icon_for_label_lg(t2));
                        ui_obj_set_style_img_recolor(waste_icon_2, lv_color_hex(THEME_DIM_TEXT), 0);
                        ui_obj_set_style_img_recolor_opa(waste_icon_2, 255, 0);
                        ui_obj_clear_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
                    } else ui_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
                }

                long days_until = waste_days_until(wp.date);
                const char * when = (days_until == 0) ? tr("Vandaag", "Today") : (days_until == 1) ? tr("Morgen", "Tomorrow") : NULL;
                if (when) ui_label_set_text_fmt(lbl_waste, "%s: %s", when, l_clean);
                else {
                    int mo = atoi(wp.date + 5), dy = atoi(wp.date + 8);
                    ui_label_set_text_fmt(lbl_waste, "%d-%d: %s", dy, mo, l_clean);
                }
                ui_obj_clear_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
                ui_obj_clear_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
            } else if (settings.dim_waste_lead_days <= 0) {
                /* Filter off and genuinely no upcoming pickup → dimmed placeholder. */
                ui_img_set_src(waste_icon, &icon_trash_lg);
                ui_obj_set_style_img_recolor(waste_icon, lv_color_hex(THEME_DIM_TEXT), 0);
                ui_obj_set_style_img_recolor_opa(waste_icon, 100, 0); /* dimmed */
                ui_label_set_text(lbl_waste, tr("Geen", "None"));
                if (waste_icon_2) ui_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
                ui_obj_clear_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
                ui_obj_clear_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
            } else {
                /* Lead filter on but the next pickup is beyond the window → hide. */
                ui_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
                ui_obj_add_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
                if (waste_icon_2) ui_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            ui_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
            ui_obj_add_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
            if (waste_icon_2) ui_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Usage bars: energy now (W), gas daily (m³), energy daily (kWh).
     * Side honours dim_bars_swap (0..3); gated by show_dim_bars. */
    if (bar_gas_env) {
        dim_layout_bars();
        int show = settings.show_dim_bars;

        /* 1. Stroom actueel (W / kW) */
        int   e_conn = energy_connected();
        float e = energy_power_w();
        if (e < 0) e = 0;                          /* export → empty */
        float er = e / DIM_E_FULL_W;
        if (er > 1.0f) er = 1.0f;
        char etxt[24];
        if (e >= 1000) snprintf(etxt, sizeof etxt, "%.1f kW", e / 1000.0f);
        else           snprintf(etxt, sizeof etxt, "%.0f W", e);
        if (!e_conn) { er = 0; snprintf(etxt, sizeof etxt, "-- W"); }

        /* 2. Gas dagtotaal (m³) */
        float g_today = energy_gas_today_m3();
        float gh = (g_today >= 0) ? g_today : energy_gas_hour_m3();
        int   g_conn = (gh >= 0);
        float g = gh; if (g < 0) g = 0;
        float gr = (g_today >= 0) ? (g / 5.0f) : (g / DIM_G_FULL_M3H);
        if (gr > 1.0f) gr = 1.0f;
        char gtxt[24];
        if (g_today >= 0) snprintf(gtxt, sizeof gtxt, "%.2f m3", g);
        else              snprintf(gtxt, sizeof gtxt, "%.2f m3/h", g);
        if (!g_conn) { gr = 0; snprintf(gtxt, sizeof gtxt, "-- m3"); }

        /* 3. Stroom dagtotaal (kWh) */
        float ed = energy_elec_today_kwh();
        int   ed_conn = (ed >= 0);
        float max_kwh = (settings.dim_elec_max_kwh > 0) ? (float)settings.dim_elec_max_kwh : 15.0f;
        float edr = (ed > 0) ? (ed / max_kwh) : 0.0f;
        if (edr > 1.0f) edr = 1.0f;
        char edtxt[24];
        if (ed_conn) {
            if (ed < 10.0f) snprintf(edtxt, sizeof edtxt, "%.2f kWh", ed);
            else            snprintf(edtxt, sizeof edtxt, "%.1f kWh", ed);
        } else {
            snprintf(edtxt, sizeof edtxt, "-- kWh");
        }

        dim_bar_set(bar_gas_env,  bar_gas_fill,  bar_gas_cap,  bar_gas_segs,  show, gr,  THEME_DIM_TEXT, gtxt);
        dim_bar_set(bar_eact_env, bar_eact_fill, bar_eact_cap, bar_eact_segs, show, er,  THEME_DIM_TEXT, etxt);
        dim_bar_set(bar_eday_env, bar_eday_fill, bar_eday_cap, bar_eday_segs, show, edr, THEME_DIM_TEXT, edtxt);
    }
    /* NILM transient device label */
    if (lbl_dim_nilm) {
        if (meter_state.nilm_event_ts &&
            time(NULL) - (time_t)meter_state.nilm_event_ts < 10) {
            ui_label_set_text(lbl_dim_nilm, meter_state.nilm_device);
            ui_obj_clear_flag(lbl_dim_nilm, LV_OBJ_FLAG_HIDDEN);
        } else {
            ui_obj_add_flag(lbl_dim_nilm, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void dim_set_outside_temp(lv_obj_t * lbl, float t, lv_coord_t max_w) {
    static const char * const fmts[] = { "%.1f°", "%.0f°" };
    const lv_font_t * f = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
    char b[24] = "";
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        snprintf(b, sizeof b, fmts[i], t);
        lv_point_t sz;
        lv_txt_get_size(&sz, b, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= max_w) break;
    }
    ui_label_set_text(lbl, b);
}

void dim_fc_build(lv_obj_t * parent, int x, int y, int col_w, int h,
                  lv_obj_t ** icon, lv_obj_t ** day, lv_obj_t ** temp) {
    const lv_font_t * fd = SF(24), * ft = SF(28);
    const int icon_w = 40;                              /* icon_wx_* are 40 px */
    int gap = SX(10), text_w = SX(70);
    int gx = x + (col_w - (icon_w + gap + text_w)) / 2;
    if (gx < x) gx = x;
    int lh_d = lv_font_get_line_height(fd), lh_t = lv_font_get_line_height(ft);
    int ty = y + (h - (lh_d + lh_t)) / 2;
    if (ty < y) ty = y;
    int iy = y + (h - icon_w) / 2;
    if (iy < y) iy = y;

    *icon = lv_img_create(parent);
    ui_img_set_src(*icon, &icon_wx_cloud);
    ui_obj_set_style_img_recolor_opa(*icon, 255, 0);
    lv_obj_set_pos(*icon, gx, iy);
    ui_obj_add_flag(*icon, LV_OBJ_FLAG_HIDDEN);

    *day = lv_label_create(parent);
    ui_obj_set_style_text_color(*day, lv_color_hex(THEME_DIM_TEXT_DIM), 0);
    ui_obj_set_style_text_font(*day, fd, 0);
    lv_obj_set_width(*day, text_w);
    lv_label_set_long_mode(*day, LV_LABEL_LONG_CLIP);
    ui_label_set_text(*day, "");
    lv_obj_set_pos(*day, gx + icon_w + gap, ty);
    ui_obj_add_flag(*day, LV_OBJ_FLAG_HIDDEN);

    *temp = lv_label_create(parent);
    ui_obj_set_style_text_color(*temp, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_text_font(*temp, ft, 0);
    lv_obj_set_width(*temp, text_w);
    lv_label_set_long_mode(*temp, LV_LABEL_LONG_CLIP);
    ui_label_set_text(*temp, "");
    lv_obj_set_pos(*temp, gx + icon_w + gap, ty + lh_d);
    ui_obj_add_flag(*temp, LV_OBJ_FLAG_HIDDEN);
}

void dim_fc_paint(lv_obj_t * icon, lv_obj_t * day, lv_obj_t * temp, int i) {
    if (!icon || !day || !temp) return;
    if (!settings.show_dim_weather || i >= weather_state.day_count) {
        ui_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        ui_obj_add_flag(day,  LV_OBJ_FLAG_HIDDEN);
        ui_obj_add_flag(temp, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const weather_day_t * d = &weather_state.days[i];
    ui_img_set_src(icon, weather_icon_for(d->icon));
    /* lighter tint: the icon colour mixed halfway to white */
    ui_obj_set_style_img_recolor(icon,
        lv_color_lighten(lv_color_hex(weather_icon_color_for(d->icon)), LV_OPA_50), 0);
    char wd[8];                                         /* "Zo 15-11" -> "Zo" */
    snprintf(wd, sizeof wd, "%.*s", (int)strcspn(d->day, " "), d->day);
    ui_label_set_text(day, wd);
    ui_label_set_text_fmt(temp, "%.0f°", d->max_temp);
    ui_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
    ui_obj_clear_flag(day,  LV_OBJ_FLAG_HIDDEN);
    ui_obj_clear_flag(temp, LV_OBJ_FLAG_HIDDEN);
}

static void on_screen_loaded(lv_event_t * e) {
    (void)e;
    refresh_cb(NULL);
}

lv_obj_t * screen_dim_create(void) {
    /* Opt-in custom dim: only when the user has enabled it in the dim editor.
     * Default (flag 0) is this original absolute dim layout. Mirrors the home
     * screen's custom_layout_enabled gate. */
    if (settings.dim_custom_enabled) return screen_dim_grid_create();
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    ui_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_DIM_BG), 0);
    ui_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    ui_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Package banner overlay (hidden when queue empty). Attached BEFORE
     * the wake-tap event handler so its CLICKABLE flag wins over the
     * screen-wide wake — tapping the banner dismisses it without also
     * waking the home screen. */
    packages_banner_attach(scr_root);

    /* Whole screen is a wake target. */
    ui_obj_add_flag(scr_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_root, on_wake_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_root, on_screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);

    /* Freetoon wordmark, top-left — brands the standby screen and paints over
     * any boot-splash "TOON" residue that can linger in this corner (the bare
     * dim background otherwise leaves no element here). */
    lv_obj_t * dim_brand = lv_label_create(scr_root);
    ui_obj_set_style_text_color(dim_brand, lv_color_hex(THEME_DIM_TEXT_DIM), 0);
    ui_obj_set_style_text_font(dim_brand, THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_letter_space(dim_brand, SX(2), 0);
    ui_label_set_text(dim_brand, "Freetoon");
    ui_obj_align(dim_brand, LV_ALIGN_TOP_LEFT, SX(20), SY(16));

    /* Clock — custom 96pt Montserrat (digits + ':' + space only,
       generated via lv_font_conv into lv_font_montserrat_96_custom.c).
       The pulsing ":" is centered; hours anchor to its left and minutes
       to its right so proportional digit widths can never cause the colon
       to overlap the digits. */
    lbl_clock_colon = lv_label_create(scr_root);
    ui_obj_set_style_text_color(lbl_clock_colon, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_text_font(lbl_clock_colon, THEME_FONT_NUM(96), 0);   /* light numerals */
    ui_label_set_text(lbl_clock_colon, ":");
    ui_obj_align(lbl_clock_colon, LV_ALIGN_CENTER, 0, SY(-130));

    lbl_clock_h = lv_label_create(scr_root);
    ui_obj_set_style_text_color(lbl_clock_h, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_text_font(lbl_clock_h, THEME_FONT_NUM(96), 0);   /* light numerals */
    ui_label_set_text(lbl_clock_h, "--");

    lbl_clock_m = lv_label_create(scr_root);
    ui_obj_set_style_text_color(lbl_clock_m, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_text_font(lbl_clock_m, THEME_FONT_NUM(96), 0);   /* light numerals */
    ui_label_set_text(lbl_clock_m, "--");

    lv_obj_update_layout(lbl_clock_colon);
    lv_obj_update_layout(lbl_clock_h);
    lv_obj_update_layout(lbl_clock_m);
    lv_obj_align_to(lbl_clock_h, lbl_clock_colon, LV_ALIGN_OUT_LEFT_MID, -SX(2), 0);
    lv_obj_align_to(lbl_clock_m, lbl_clock_colon, LV_ALIGN_OUT_RIGHT_MID, SX(2), 0);

    /* NILM transient device label — floats above the energy bar cap at the
     * bottom of the screen; hidden until a step-change event fires. */
    lbl_dim_nilm = lv_label_create(scr_root);
    ui_obj_set_style_text_color(lbl_dim_nilm, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_text_font(lbl_dim_nilm, SF(16), 0);
    lv_obj_set_style_text_align(lbl_dim_nilm, LV_TEXT_ALIGN_CENTER, 0);
    ui_label_set_text(lbl_dim_nilm, "");
    ui_obj_align(lbl_dim_nilm, LV_ALIGN_BOTTOM_MID, 0, -SY(22));
    ui_obj_add_flag(lbl_dim_nilm, LV_OBJ_FLAG_HIDDEN);

    /* All labels positioned against screen center with explicit Y offsets so
       different content widths can't drift them out of alignment. */
    lbl_date = lv_label_create(scr_root);
    ui_obj_set_style_text_color(lbl_date, lv_color_hex(THEME_DIM_TEXT_DIM), 0);
    ui_obj_set_style_text_font(lbl_date, SF(22), 0);
    lv_obj_set_style_text_letter_space(lbl_date, THEME_LABEL_TRACK, 0);
    lv_obj_set_style_text_align(lbl_date, LV_TEXT_ALIGN_CENTER, 0);
    ui_label_set_text(lbl_date, "");
    ui_obj_align(lbl_date, LV_ALIGN_CENTER, 0, SY(-50));

    /* Moon phase icon removed per CR11 */
    dim_moon_img = NULL;

    /* Room temperature readout: enlarged to 96pt Montserrat (clock font/size).
       CR2: Setpoint + program state in small font (SF(22)) under room temp. */
    lbl_temp = lv_label_create(scr_root);
    ui_obj_set_style_text_color(lbl_temp, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_text_font(lbl_temp, THEME_FONT_NUM(96), 0);   /* light numerals */
    ui_label_set_text(lbl_temp, "--.-°C");
    ui_obj_align(lbl_temp, LV_ALIGN_CENTER, 0, SY(40));

    lbl_setpoint = lv_label_create(scr_root);
    ui_obj_set_style_text_color(lbl_setpoint, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_text_font(lbl_setpoint, SF(22), 0);
    lv_obj_set_style_text_align(lbl_setpoint, LV_TEXT_ALIGN_CENTER, 0);
    ui_label_set_text(lbl_setpoint, "");
    ui_obj_align(lbl_setpoint, LV_ALIGN_CENTER, 0, SY(90));

    lbl_program  = NULL;
    lbl_metrics  = NULL;
    lbl_burner   = NULL;

    /* Toon-style radiator+flame glyph, parked underneath setpoint */
    dim_img_flame = lv_img_create(scr_root);
    ui_img_set_src(dim_img_flame, &icon_radiator);
    lv_img_set_zoom(dim_img_flame, 256);
    ui_obj_set_style_img_recolor(dim_img_flame, lv_color_hex(THEME_GLOW), 0);
    ui_obj_set_style_img_recolor_opa(dim_img_flame, 255, 0);
    lv_obj_align_to(dim_img_flame, lbl_setpoint, LV_ALIGN_OUT_BOTTOM_MID, 0, SY(8));
    ui_obj_add_flag(dim_img_flame, LV_OBJ_FLAG_HIDDEN);

    /* DHW faucet fully red — see screen_home.c for rationale. */
    dim_img_faucet = lv_img_create(scr_root);
    ui_img_set_src(dim_img_faucet, &icon_faucet);
    lv_img_set_zoom(dim_img_faucet, 256);
    ui_obj_set_style_img_recolor(dim_img_faucet, lv_color_hex(THEME_GLOW), 0);
    ui_obj_set_style_img_recolor_opa(dim_img_faucet, 255, 0);
    lv_obj_align_to(dim_img_faucet, lbl_setpoint, LV_ALIGN_OUT_BOTTOM_MID, -SX(10), SY(8));
    ui_obj_add_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);

    /* DHW drop in RED to distinguish from the blue cold-water-flow drop */
    dim_img_drop = lv_img_create(scr_root);
    ui_img_set_src(dim_img_drop, &icon_drop);
    lv_img_set_zoom(dim_img_drop, 256);
    ui_obj_set_style_img_recolor(dim_img_drop, lv_color_hex(THEME_GLOW), 0);
    ui_obj_set_style_img_recolor_opa(dim_img_drop, 255, 0);
    lv_obj_align_to(dim_img_drop, dim_img_faucet, LV_ALIGN_OUT_RIGHT_MID, SX(8), 0);
    ui_obj_add_flag(dim_img_drop, LV_OBJ_FLAG_HIDDEN);

    /* Live water flow — drop icon + "X.X L/m" right of the indoor temp,
     * sits below the radiator slot so both can be visible at once. */
    dim_img_water = lv_img_create(scr_root);
    ui_img_set_src(dim_img_water, &icon_drop);
    lv_img_set_zoom(dim_img_water, 256);
    ui_obj_set_style_img_recolor(dim_img_water, lv_color_hex(THEME_INFO), 0);
    ui_obj_set_style_img_recolor_opa(dim_img_water, 255, 0);
    ui_obj_align(dim_img_water, LV_ALIGN_CENTER, SX(130), SY(80));
    ui_obj_add_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_water = lv_label_create(scr_root);
    ui_obj_set_style_text_color(dim_lbl_water, lv_color_hex(THEME_INFO), 0);
    ui_obj_set_style_text_font(dim_lbl_water, &lv_font_montserrat_22, 0);
    ui_label_set_text(dim_lbl_water, "");
    ui_obj_align(dim_lbl_water, LV_ALIGN_CENTER, SX(175), SY(80));
    ui_obj_add_flag(dim_lbl_water, LV_OBJ_FLAG_HIDDEN);

    /* Vent — small fan icon top-RIGHT (mirrors the waste icon on top-LEFT)
       with the actual ExhFanSpeed % below. Spin animation tracks % so
       the user can read at-a-glance whether the unit is idling or
       blasting. Hidden when the Itho bridge isn't reachable. */
    dim_vent_fan = lv_img_create(scr_root);
    ui_img_set_src(dim_vent_fan, &icon_fan);
    /* icon_fan is large (~96px); scale down for the dim chrome row. */
    lv_img_set_zoom(dim_vent_fan, 128);
    ui_obj_set_style_img_recolor(dim_vent_fan, lv_color_hex(THEME_DIM_TEXT_DIM), 0);
    ui_obj_set_style_img_recolor_opa(dim_vent_fan, 255, 0);
    /* Set pivot to center so rotation looks natural — has to be set
       AFTER the source is bound. icon_fan is 80×80 native (not 128 like
       the original comment claimed — wrong pivot made the icon orbit a
       point 24px off-centre, which is what "spinner acting weird" was). */
    lv_img_set_pivot(dim_vent_fan, 40, 40);
    /* Mirror the radiator+flame at (+145, +45): vent indicator sits to the
     * LEFT of the big indoor temp at the same offset. Label tucks under it
     * so the preset/% stays glanceable without crowding the temp row. */
    ui_obj_align(dim_vent_fan, LV_ALIGN_CENTER, SX(-145), SY(45));
    ui_obj_add_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);

    dim_vent_lbl = lv_label_create(scr_root);
    ui_obj_set_style_text_color(dim_vent_lbl, lv_color_hex(THEME_DIM_TEXT_DIM), 0);
    ui_obj_set_style_text_font(dim_vent_lbl, &lv_font_montserrat_18, 0);
    ui_label_set_text(dim_vent_lbl, "-- %");
    lv_obj_set_style_text_align(dim_vent_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(dim_vent_lbl, SX(120));
    ui_obj_align(dim_vent_lbl, LV_ALIGN_CENTER, SX(-145), SY(85));
    ui_obj_add_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Weather cluster (right column): fixed top/width coordinates instead of
       RIGHT_MID offsets, so the icon and labels stay as one visual block even
       when text widths change. */
    /* Outside temp in the same big font as the room temp (readable from
     * across the room); icon + "Buiten" in a row underneath. */
    lbl_outside_temp = lv_label_create(scr_root);
    ui_obj_set_style_text_color(lbl_outside_temp, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_text_font(lbl_outside_temp, THEME_FONT_NUM(96), 0);   /* light numerals */
    ui_label_set_text(lbl_outside_temp, "");
    ui_obj_align(lbl_outside_temp, LV_ALIGN_TOP_RIGHT, SX(-24), SY(28));

    wx_icon = lv_img_create(scr_root);
    ui_img_set_src(wx_icon, &icon_wx_cloud_lg);
    ui_obj_set_style_img_recolor(wx_icon, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_img_recolor_opa(wx_icon, 255, 0);
    lv_obj_align_to(wx_icon, lbl_outside_temp, LV_ALIGN_OUT_BOTTOM_MID, 0, SY(2));

    /* No "Buiten" caption: the big temp + icon speak for themselves. */
    lbl_outside = NULL;

    /* Life360 — sits under the outside temp on the right edge, mirroring
     * the Family tile on the home screen. Right-aligned so longer street
     * names extend to the LEFT instead of clipping the bezel. */
    dim_lbl_life360_a = lv_label_create(scr_root);
    ui_obj_set_style_text_color(dim_lbl_life360_a, lv_color_hex(DIM_COL_LIFE360_A), 0);
    ui_obj_set_style_text_font(dim_lbl_life360_a, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dim_lbl_life360_a, SX(340));
    lv_obj_set_style_text_align(dim_lbl_life360_a, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(dim_lbl_life360_a, LV_LABEL_LONG_DOT);
    ui_label_set_text(dim_lbl_life360_a, "");
    ui_obj_align(dim_lbl_life360_a, LV_ALIGN_TOP_RIGHT, SX(-24), SY(322));
    ui_obj_add_flag(dim_lbl_life360_a, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_life360_b = lv_label_create(scr_root);
    ui_obj_set_style_text_color(dim_lbl_life360_b, lv_color_hex(DIM_COL_LIFE360_B), 0);
    ui_obj_set_style_text_font(dim_lbl_life360_b, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dim_lbl_life360_b, SX(340));
    lv_obj_set_style_text_align(dim_lbl_life360_b, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(dim_lbl_life360_b, LV_LABEL_LONG_DOT);
    ui_label_set_text(dim_lbl_life360_b, "");
    ui_obj_align(dim_lbl_life360_b, LV_ALIGN_TOP_RIGHT, SX(-24), SY(346));
    ui_obj_add_flag(dim_lbl_life360_b, LV_OBJ_FLAG_HIDDEN);

    /* Waste cluster (left column): mirror the weather cluster. Use the native
       80 px trash glyph and recolor it by waste type; no lv_img zoom here
       because zoomed alpha-only icons can disappear on Toon 1. */
    waste_icon = lv_img_create(scr_root);
    ui_img_set_src(waste_icon, &icon_trash_lg);
    ui_obj_set_style_img_recolor(waste_icon, lv_color_hex(THEME_DIM_TEXT), 0);
    ui_obj_set_style_img_recolor_opa(waste_icon, 255, 0);
    ui_obj_align(waste_icon, LV_ALIGN_TOP_LEFT, SX(70), SY(52));
    ui_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);

    /* 2nd bin, shown only when the next pickup has two types on one day
       (e.g. Papier+Plastic) — each bin recoloured by its own type. */
    waste_icon_2 = lv_img_create(scr_root);
    ui_img_set_src(waste_icon_2, &icon_trash_lg);
    ui_obj_set_style_img_recolor_opa(waste_icon_2, 255, 0);
    ui_obj_align(waste_icon_2, LV_ALIGN_TOP_LEFT, SX(150), SY(52));
    ui_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);

    lbl_waste = lv_label_create(scr_root);
    ui_obj_set_style_text_color(lbl_waste, lv_color_hex(THEME_DIM_TEXT_DIM), 0);
    const lv_font_t * waste_font = (DISP_VER < 600) ? &lv_font_montserrat_14 : &lv_font_montserrat_18;
    int waste_lbl_y = (DISP_VER < 600) ? 128 : SY(138);
    ui_obj_set_style_text_font(lbl_waste, waste_font, 0);
    lv_obj_set_width(lbl_waste, SX(260));
    lv_obj_set_style_text_align(lbl_waste, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_waste, LV_LABEL_LONG_WRAP);
    ui_obj_align(lbl_waste, LV_ALIGN_TOP_LEFT, SX(10), waste_lbl_y);
    ui_obj_add_flag(lbl_waste, LV_OBJ_FLAG_HIDDEN);

    /* (City header above forecast strip removed — location moved under wx icon) */
    dim_lbl_city = NULL;
    waste_box_ptr = NULL;  /* absolute layout doesn't use the box pointer */

    /* 5-day forecast strip across the bottom of dim (#49): per day a lighter
     * weather icon with weekday + max temperature next to it, larger print.
     * Same vertical band as before (below the city header). */
    int strip_y = (DISP_VER < 600) ? 398 : SY(508);
    int col_w   = DISP_HOR / WEATHER_FORECAST_DAYS;
    int strip_h = DISP_VER - strip_y - SY(6);
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++)
        dim_fc_build(scr_root, i * col_w, strip_y, col_w, strip_h,
                     &dim_fc_icon[i], &dim_fc_day[i], &dim_fc_temp[i]);

    /* 3 Usage bars: gas day, electricity actual, electricity day total.
     * Positioned per settings.dim_bars_swap (0..3) in dim_layout_bars(). */
    dim_bar_h = SY(2 * DIM_CLOCK_H);
    dim_make_bar(&bar_gas_env,  &bar_gas_fill,  &bar_gas_cap,  bar_gas_segs);
    dim_make_bar(&bar_eact_env, &bar_eact_fill, &bar_eact_cap, bar_eact_segs);
    dim_make_bar(&bar_eday_env, &bar_eday_fill, &bar_eday_cap, bar_eday_segs);
    dim_layout_bars();

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}

/* Theme change: drop the cached screen so the next dim builds it in the new palette. Clears every widget pointer, so refresh code never touches a freed object. */
void screen_dim_destroy(void) {
    if (!scr_root) return;
    if (refresh_timer) { lv_timer_del(refresh_timer); refresh_timer = NULL; }
    lv_obj_t * s = scr_root;
    scr_root = NULL;
    lbl_clock_h = NULL;
    lbl_clock_m = NULL;
    lbl_clock_colon = NULL;
    lbl_date = NULL;
    dim_moon_img = NULL;
    lbl_temp = NULL;
    lbl_setpoint = NULL;
    lbl_outside_temp = NULL;
    lbl_program = NULL;
    lbl_metrics = NULL;
    lbl_burner = NULL;
    dim_img_flame = NULL;
    dim_img_faucet = NULL;
    dim_img_drop = NULL;
    wx_icon = NULL;
    lbl_outside = NULL;
    waste_icon = NULL;
    waste_icon_2 = NULL;
    lbl_waste = NULL;
    waste_box_ptr = NULL;
    dim_lbl_city = NULL;
    dim_lbl_life360_a = NULL;
    dim_lbl_life360_b = NULL;
    dim_vent_fan = NULL;
    dim_vent_lbl = NULL;
    dim_img_water = NULL;
    dim_lbl_water = NULL;
    lbl_dim_nilm = NULL;
    bar_gas_env = NULL;
    bar_gas_fill = NULL;
    bar_gas_cap = NULL;
    bar_eact_env = NULL;
    bar_eact_fill = NULL;
    bar_eact_cap = NULL;
    bar_eday_env = NULL;
    bar_eday_fill = NULL;
    bar_eday_cap = NULL;
    memset(dim_fc_icon, 0, sizeof dim_fc_icon);
    memset(dim_fc_day, 0, sizeof dim_fc_day);
    memset(dim_fc_temp, 0, sizeof dim_fc_temp);
    memset(bar_gas_segs, 0, sizeof bar_gas_segs);
    memset(bar_eact_segs, 0, sizeof bar_eact_segs);
    memset(bar_eday_segs, 0, sizeof bar_eday_segs);
    ui_del_screen(s);
}
