/*
 * Thermostat detail screen — full-screen room temperature view with
 * setpoint adjustment. Reachable from the home tile.
 */
#include "screens.h"
#include "i18n.h"
#include "display.h"
#include "boxtalk.h"
#include "homewizard.h"
#include "icons.h"
#include "settings.h"
#include "theme.h"
#include "thermo_quick.h"
#include <time.h>
#include "fonts_opensans.h"
#include "pin_modal.h"
#include <stdio.h>
#include <time.h>

/* Identity colours, same in every theme: programme / mode buttons. */
#define COL_PROG_SCHED   0x2f6b6b   /* "Scheduled" mode */
#define COL_PROG_MANUAL  0x6a5424   /* "Manual" mode */
#define COL_PROG_COMFORT 0xcc7733
#define COL_PROG_HOME    0x3377cc
#define COL_PROG_SLEEP   0x553388
#define COL_PROG_AWAY    0x557788
#define COL_ON_IDENT     0xffffff   /* text/border on those identity fills */
#define COL_DROP_ICON    0xff3333   /* hidden legacy drop icon, real-world red */

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_temp;
static lv_obj_t * lbl_humidity;
static lv_obj_t * lbl_voc;
static lv_obj_t * lbl_water;
static lv_obj_t * lbl_setpoint;
static lv_obj_t * lbl_conn;
static lv_obj_t * lbl_burner;
static lv_obj_t * lbl_ch_hdr;
static lv_obj_t * lbl_flow;
static lv_obj_t * lbl_return;
static lv_obj_t * img_flame;
static lv_obj_t * img_faucet;
static lv_obj_t * img_drop;
/* Radiator+flame glyph next to the big indoor-temp, shown while CH is firing. */
static lv_obj_t * img_temp_flame;
static lv_obj_t * lbl_clock;
static lv_obj_t * lbl_date;
static lv_timer_t * refresh_timer = NULL;
/* Program preset buttons just below the indoor temp.
 *   [0] Scheduled  → resume_schedule (whatever the schedule says now)
 *   [1] Manual     → set_manual (permanent hold, activeState = -1).
 *                    This is the dedicated "leave the schedule
 *                    indefinitely" control; the home tile's +/- nudges
 *                    are temporary (auto-resume at the next switch).
 *   [2..5] Comfort/Home/Sleep/Away → set_program(0..3)
 * Sentinel -2 in prog_state[] means "resume schedule" — distinct from -1
 * (manual). The active mode (Scheduled or Manual) AND the active preset
 * both get a white outline; the two highlights live on different buttons
 * so they don't clash. */
static lv_obj_t * btn_prog[6] = {0};
static lv_obj_t * btn_boost = NULL, * lbl_boost = NULL;
static lv_obj_t * btn_holiday = NULL, * lbl_holiday = NULL;
static lv_obj_t * hol_modal = NULL, * hol_lbl_date = NULL;
static time_t     hol_until = 0;
static const int  prog_state[6] = {-2, -1, 0, 1, 2, 3};

static void on_open_advanced(lv_event_t * e) { (void)e; ui_push_owned(screen_heater_advanced_create()); }
/* Gated via pin_modal: setpoint nudges, preset taps, and schedule/manual
 * mode toggle all run through pin_gate so the PIN prompts when enabled. */
static void sp_up_apply(void * c)   { (void)c; boxtalk_setpoint_increase(); }
static void sp_down_apply(void * c) { (void)c; boxtalk_setpoint_decrease(); }
static void on_setpoint_up(lv_event_t * e)   { (void)e; pin_gate(sp_up_apply,   NULL); }
static void on_setpoint_down(lv_event_t * e) { (void)e; pin_gate(sp_down_apply, NULL); }
static void on_back(lv_event_t * e) { ui_pop(); }
static void on_open_schedule(lv_event_t * e) { (void)e; ui_push(screen_schedule_create()); }
static void program_tap_apply(void * ctx) {
    int s = (int)(intptr_t)ctx;
    if      (s == -2) boxtalk_resume_schedule();
    else if (s == -1) boxtalk_set_manual();
    else {
        int ps = toon_state.program_state;
        int on_program = (ps == 1 || ps == 2 || ps == 3 || ps == 8) || (boxtalk_temp_override_origin() >= 0);
        if (!on_program || ps == 0) {
            boxtalk_set_manual_preset(s);
        } else {
            boxtalk_set_program(s);
        }
    }
}
static void on_program_tap(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int s = prog_state[idx];
    pin_gate(program_tap_apply, (void *)(intptr_t)s);
}

/* ---- quick actions: Boost, Holiday (thermo_quick.c) ----------------------- */
static void quick_paint(void) {
    if (lbl_boost) {
        int on = thermo_boost_active();
        if (on) {
            struct tm tm; time_t u = (time_t)settings.boost_until;
            localtime_r(&u, &tm);
            lv_label_set_text_fmt(lbl_boost, tr("Boost tot %02d:%02d", "Boost to %02d:%02d"), tm.tm_hour, tm.tm_min);
        } else {
            lv_label_set_text(lbl_boost, tr("Boost +1\xC2\xB0", "Boost +1\xC2\xB0"));
        }
        lv_obj_set_style_bg_color(btn_boost, lv_color_hex(on ? THEME_ACCENT_FILL : THEME_BTN_BG), 0);
        lv_obj_set_style_text_color(lbl_boost, lv_color_hex(on ? THEME_ON_ACCENT : THEME_TEXT_HI), 0);
    }
    if (lbl_holiday) {
        int on = thermo_holiday_active();
        if (on) {
            struct tm tm; time_t u = (time_t)settings.holiday_until;
            localtime_r(&u, &tm);
            lv_label_set_text_fmt(lbl_holiday, tr("Terug %d %s", "Back %d %s"),
                                  tm.tm_mday, i18n_month_short(tm.tm_mon));
        } else {
            lv_label_set_text(lbl_holiday, tr("Vakantie", "Holiday"));
        }
        lv_obj_set_style_bg_color(btn_holiday, lv_color_hex(on ? THEME_ACCENT_FILL : THEME_BTN_BG), 0);
        lv_obj_set_style_text_color(lbl_holiday, lv_color_hex(on ? THEME_ON_ACCENT : THEME_TEXT_HI), 0);
    }
}

/* Behind the PIN like the other thermostat controls (pin_gate). */
static void boost_apply(void * ctx) {
    (void)ctx;
    if (thermo_boost_active()) thermo_boost_stop();
    else                       thermo_boost_start();
    quick_paint();
}
static void on_boost(lv_event_t * e) { (void)e; pin_gate(boost_apply, NULL); }

static void hol_paint(void) {
    if (!hol_lbl_date) return;
    struct tm tm;
    localtime_r(&hol_until, &tm);
    lv_label_set_text_fmt(hol_lbl_date, "%s %d %s  %02d:00", i18n_day_short(tm.tm_wday), tm.tm_mday,
                          i18n_month_short(tm.tm_mon), tm.tm_hour);
}
static void hol_close(void) {
    if (hol_modal) { lv_obj_del_async(hol_modal); hol_modal = NULL; hol_lbl_date = NULL; }
}
static void on_hol_step(lv_event_t * e) {
    long d = (long)(intptr_t)lv_event_get_user_data(e);   /* seconds */
    time_t n = hol_until + d, now = time(NULL);
    if (n < now + 3600) return;                 /* at least an hour ahead */
    if (n > now + 60L * 86400) return;          /* at most 60 days */
    hol_until = n;
    hol_paint();
}
static void hol_start_apply(void * ctx) { (void)ctx; thermo_holiday_start(hol_until); quick_paint(); }
static void hol_stop_apply(void * ctx)  { (void)ctx; thermo_holiday_stop(); quick_paint(); }
static void on_hol_start(lv_event_t * e) { (void)e; hol_close(); pin_gate(hol_start_apply, NULL); }
static void on_hol_stop(lv_event_t * e)  { (void)e; hol_close(); pin_gate(hol_stop_apply, NULL); }
static void on_hol_cancel(lv_event_t * e) { (void)e; hol_close(); }

static lv_obj_t * hol_btn(lv_obj_t * par, const char * txt, lv_coord_t w, uint32_t bg, uint32_t fg,
                          lv_event_cb_t cb, intptr_t ud) {
    lv_obj_t * b = lv_btn_create(par);
    lv_obj_set_size(b, w, SY(52));
    lv_obj_set_style_radius(b, THEME_RADIUS_BTN, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, SF(18), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_center(l);
    return b;
}

static void on_holiday(lv_event_t * e) {
    (void)e;
    if (hol_modal) return;
    int active = thermo_holiday_active();
    if (active) {
        hol_until = (time_t)settings.holiday_until;
    } else {
        /* default: back in a week, at 17:00 */
        time_t t = time(NULL) + 7 * 86400;
        struct tm tm; localtime_r(&t, &tm);
        tm.tm_hour = 17; tm.tm_min = 0; tm.tm_sec = 0;
        hol_until = mktime(&tm);
    }
    hol_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(hol_modal);
    lv_obj_set_size(hol_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(hol_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hol_modal, LV_OPA_60, 0);
    lv_obj_add_flag(hol_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hol_modal, on_hol_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t * p = lv_obj_create(hol_modal);
    lv_obj_set_size(p, SX(600), SY(340));
    lv_obj_center(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, THEME_RADIUS_PANEL, 0);
    lv_obj_set_style_pad_all(p, SX(24), 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);          /* taps stay in the panel */

    lv_obj_t * t = lv_label_create(p);
    lv_label_set_text(t, tr("Vakantie", "Holiday"));
    lv_obj_set_style_text_font(t, SF(28), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t * h = lv_label_create(p);
    lv_label_set_text(h, tr("Weg tot je terug bent, daarna weer het programma",
                            "Away until you are back, then the programme again"));
    lv_obj_set_style_text_font(h, SF(14), 0);
    lv_obj_set_style_text_letter_space(h, THEME_LABEL_TRACK, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, SY(40));

    lv_obj_t * tl = lv_label_create(p);
    lv_label_set_text(tl, tr("Terug op", "Back on"));
    lv_obj_set_style_text_font(tl, SF(14), 0);
    lv_obj_set_style_text_letter_space(tl, THEME_LABEL_TRACK, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_align(tl, LV_ALIGN_TOP_MID, 0, SY(84));
    hol_lbl_date = lv_label_create(p);
    lv_obj_set_style_text_font(hol_lbl_date, SF(36), 0);
    lv_obj_set_style_text_color(hol_lbl_date, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_align(hol_lbl_date, LV_ALIGN_TOP_MID, 0, SY(104));
    hol_paint();

    lv_coord_t sw = SX(118);
    lv_obj_t * b;
    b = hol_btn(p, tr("- dag", "- day"), sw, THEME_BTN_BG, THEME_TEXT_HI, on_hol_step, -86400);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 0, SY(160));
    b = hol_btn(p, tr("+ dag", "+ day"), sw, THEME_BTN_BG, THEME_TEXT_HI, on_hol_step, 86400);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, sw + SX(8), SY(160));
    b = hol_btn(p, tr("- uur", "- hour"), sw, THEME_BTN_BG, THEME_TEXT_HI, on_hol_step, -3600);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -(sw + SX(8)), SY(160));
    b = hol_btn(p, tr("+ uur", "+ hour"), sw, THEME_BTN_BG, THEME_TEXT_HI, on_hol_step, 3600);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, SY(160));

    lv_coord_t bw = SX(268);
    if (active) {
        b = hol_btn(p, tr("Vakantie stoppen", "End holiday"), bw, THEME_BTN_BG, THEME_TEXT_HI, on_hol_stop, 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        b = hol_btn(p, tr("Datum opslaan", "Save date"), bw, THEME_ACCENT_FILL, THEME_ON_ACCENT, on_hol_start, 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    } else {
        b = hol_btn(p, tr("Annuleren", "Cancel"), bw, THEME_BTN_BG, THEME_TEXT_HI, on_hol_cancel, 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        b = hol_btn(p, tr("Vakantie starten", "Start holiday"), bw, THEME_ACCENT_FILL, THEME_ON_ACCENT, on_hol_start, 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
}

static lv_obj_t * quick_pill(lv_coord_t y, lv_event_cb_t cb, lv_obj_t ** out_lbl) {
    lv_obj_t * b = lv_btn_create(scr_root);
    lv_obj_set_size(b, SX(180), SY(44));
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -SX(20), y);
    lv_obj_set_style_radius(b, THEME_RADIUS_BTN, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    *out_lbl = lv_label_create(b);
    lv_obj_set_style_text_font(*out_lbl, SF(18), 0);
    lv_obj_center(*out_lbl);
    return b;
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root || lv_scr_act() != scr_root) return;
    boxtalk_tick();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof(clk), "%H:%M", &tm);
    lv_label_set_text(lbl_clock, clk);
    char dt[48];
    /* Localised (strftime gave "Wednesday 18 November" in the C locale). */
    snprintf(dt, sizeof dt, "%s %d %s %d", i18n_day_long(tm.tm_wday), tm.tm_mday,
             i18n_month_long(tm.tm_mon), 1900 + tm.tm_year);
    lv_label_set_text(lbl_date, dt);

    if (toon_state.connected) {
        lv_obj_add_flag(lbl_conn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(lbl_conn, tr("BoxTalk: verbinden...", "BoxTalk: connecting..."));
        lv_obj_set_style_text_color(lbl_conn, lv_color_hex(THEME_WARNING), 0);
        lv_obj_clear_flag(lbl_conn, LV_OBJ_FLAG_HIDDEN);
    }

    if (toon_state.indoor_temp > 0)
        lv_label_set_text_fmt(lbl_temp, "%.1f°", toon_state.indoor_temp);

    /* Highlight logic: the mode button (Scheduled[0] or Off[1]) gets a white
     * border based on whether we're on the schedule (program_state 1, 2, 3, 8);
     * the preset button (Comfort[2] / Home[3] / Sleep[4] / Away[5]) gets one
     * based on the LIVE comfort preset, which is active_state (happ's "activeState").
     * A +/- temporary override counts as "still on the schedule" so the Scheduled
     * mode + origin preset stay highlighted while it's in flight. */
    int temp_origin = boxtalk_temp_override_origin();   /* -1 if none */
    int ps = toon_state.program_state;
    int on_schedule = (ps == 1 || ps == 2 || ps == 3 || ps == 8) || (temp_origin >= 0);
    int mode_idx    = on_schedule ? 0 : 1;
    int preset;
    if (toon_state.active_state >= 0 && toon_state.active_state <= 3) {
        preset = toon_state.active_state;
    } else {
        preset = temp_origin;
    }
    int preset_idx = (preset >= 0 && preset <= 3) ? (2 + preset) : -1;
    static const uint32_t prog_cols[6] = {
        COL_PROG_SCHED, COL_PROG_MANUAL,
        COL_PROG_COMFORT, COL_PROG_HOME, COL_PROG_SLEEP, COL_PROG_AWAY
    };
    for (int i = 0; i < 6; i++) {
        if (!btn_prog[i]) continue;
        if (i >= 2) {
            if (!on_schedule) {
                lv_obj_add_flag(btn_prog[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            } else {
                lv_obj_clear_flag(btn_prog[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        /* One button style (design 1.1): active = accent pill, no borders. */
        int active = (i == mode_idx) || (i == preset_idx);
        (void)prog_cols;
        lv_obj_set_style_border_width(btn_prog[i], 0, 0);
        lv_obj_set_style_bg_color(btn_prog[i],
                                  lv_color_hex(active ? THEME_ACCENT_FILL : THEME_BTN_BG), 0);
        lv_obj_t * bl = lv_obj_get_child(btn_prog[i], 0);
        if (bl) {
            lv_obj_set_style_text_color(bl, lv_color_hex(active ? THEME_ON_ACCENT : THEME_TEXT_DIM), 0);
        }
    }
    quick_paint();
    if (toon_state.humidity > 0)
        lv_label_set_text_fmt(lbl_humidity, tr("RV %.0f%%", "RH %.0f%%"), toon_state.humidity);
    if (toon_state.eco2 || toon_state.tvoc)
        lv_label_set_text_fmt(lbl_voc, tr("eCO2 %d ppm", "eCO2 %d ppm"), toon_state.eco2);

    if (hw_state.connected_water) {
        /* 3 decimals — every litre poured ticks the displayed total. */
        if (hw_state.water_lpm > 0.05f)
            lv_label_set_text_fmt(lbl_water, "%.3f m3  /  %.1f L/min",
                                  hw_state.water_total_m3, hw_state.water_lpm);
        else
            lv_label_set_text_fmt(lbl_water, "%.3f m3",
                                  hw_state.water_total_m3);
    } else {
        lv_label_set_text(lbl_water, tr("Water -- m3", "Water -- m3"));
    }
    if (toon_state.setpoint > 0)
        lv_label_set_text_fmt(lbl_setpoint, tr("Streefwaarde: %.1f°C", "Setpoint: %.1f°C"), toon_state.setpoint);

    /* Status line — short word only ("Heating" / "Hot water" / "Boiler idle").
     * The radiator+flame glyph next to the indoor temp carries the visual,
     * and the right-column "CH water Flow … Return …" panel has the temps.
     * The old left-side flame/faucet/drop icons are now redundant — kept as
     * widgets but always hidden. */
    if (boxtalk_preheat_shown()) {
        /* qt-gui: "To 20.0° in 1h30" — the next block's setpoint. */
        char eta[32];
        boxtalk_preheat_eta(eta, sizeof eta);
        lv_label_set_text_fmt(lbl_burner, tr("Voorverwarmen naar %.1f°%s", "Pre-heating to %.1f°%s"),
                              boxtalk_preheat_target(), eta);
        lv_obj_set_style_text_color(lbl_burner, lv_color_hex(THEME_WARNING), 0);
    } else if (toon_state.burner_on) {
        lv_label_set_text(lbl_burner, tr("Verwarmen", "Heating"));
        lv_obj_set_style_text_color(lbl_burner, lv_color_hex(THEME_GLOW), 0);
    } else if (toon_state.dhw_on) {
        lv_label_set_text(lbl_burner, tr("Warm water", "Hot water"));
        lv_obj_set_style_text_color(lbl_burner, lv_color_hex(THEME_ACCENT), 0);
    } else {
        lv_label_set_text(lbl_burner, tr("Ketel rust", "Boiler idle"));
        lv_obj_set_style_text_color(lbl_burner, lv_color_hex(THEME_TEXT_DIM), 0);
    }
    lv_obj_add_flag(img_flame,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(img_faucet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(img_drop,   LV_OBJ_FLAG_HIDDEN);

    /* CH water inlet/outlet temps. Flow falls back to the boiler
       TemperatureSensor (always present) if the boilerTemps query hasn't
       answered; return comes only from the boilerRetTemps query. */
    float flow = toon_state.boiler_out_temp > 0 ? toon_state.boiler_out_temp
                                                : toon_state.boiler_temp;
    if (flow > 0) lv_label_set_text_fmt(lbl_flow, tr("Aanvoer  %.1f°C", "Flow  %.1f°C"), flow);
    else          lv_label_set_text(lbl_flow, tr("Aanvoer  -- C", "Flow  -- C"));
    if (toon_state.boiler_in_temp > 0)
        lv_label_set_text_fmt(lbl_return, tr("Retour  %.1f°C", "Return  %.1f°C"), toon_state.boiler_in_temp);
    else
        /* Many boilers leave OT DID 28 (CH return temp) unimplemented; OTGW
         * reports returnwatertemperature=0 in that case and happ_thermstat
         * forwards the zero unchanged. Mark it n/a so this row doesn't look
         * like a UI bug. */
        lv_label_set_text(lbl_return, tr("Retour  n.v.t.", "Return  n/a"));

    /* Radiator+flame next to the big indoor-temp — visible while CH fires. */
    if (img_temp_flame) {
        if (toon_state.burner_on)
            lv_obj_clear_flag(img_temp_flame, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(img_temp_flame, LV_OBJ_FLAG_HIDDEN);
    }

    /* Force a full screen invalidate to defeat any partial-flush quirk. */
    lv_obj_invalidate(scr_root);
}

static int scr_light_mode = -1;   /* cached UI theme (settings.ui_theme) */

lv_obj_t * screen_thermostat_create(void) {
    if (scr_root && scr_light_mode == settings.ui_theme) return scr_root;  /* cached */
    if (scr_root) {
        lv_obj_del(scr_root);
        scr_root = NULL;
    }
    scr_light_mode = settings.ui_theme;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Close "X" button */
    int x_w = (DISP_VER < 600) ? 60 : 70;
    int x_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_t * btn_back = lv_btn_create(scr_root);
    lv_obj_set_size(btn_back, x_w, x_h);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, SX(12), SY(12));
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_back, 12, 0);
    lv_obj_set_ext_click_area(btn_back, 25);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_back_lbl = lv_label_create(btn_back);
    lv_label_set_text(btn_back_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(btn_back_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(btn_back_lbl, SF(22), 0);
    lv_obj_center(btn_back_lbl);

    /* Clock + date */
    lbl_clock = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_clock, SF(28), 0);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_LEFT, SX(100), SY(20));
    lv_label_set_text(lbl_clock, "--:--");

    lbl_date = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_date, SF(18), 0);
    lv_obj_align_to(lbl_date, lbl_clock, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);

    /* Schedule button (top-right) */
    lv_obj_t * btn_sched = lv_btn_create(scr_root);
    lv_obj_set_size(btn_sched, 180, 70);
    lv_obj_align(btn_sched, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_set_style_bg_color(btn_sched, lv_color_hex(THEME_BTN_ACCENT), 0);
    lv_obj_set_style_radius(btn_sched, 12, 0);
    lv_obj_set_ext_click_area(btn_sched, 16);
    lv_obj_add_event_cb(btn_sched, on_open_schedule, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sched_lbl = lv_label_create(btn_sched);
    lv_label_set_text(sched_lbl, tr("Programma", "Schedule"));
    lv_obj_set_style_text_color(sched_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(sched_lbl, SF(22), 0);
    lv_obj_center(sched_lbl);

    /* Connection indicator just under the Schedule button (hidden during normal operation) */
    lbl_conn = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_conn, lv_color_hex(THEME_WARNING), 0);
    lv_obj_set_style_text_font(lbl_conn, SF(18), 0);
    lv_obj_align(lbl_conn, LV_ALIGN_TOP_RIGHT, -20, 95);
    lv_label_set_text(lbl_conn, tr("BoxTalk: verbinden...", "BoxTalk: connecting..."));
    if (toon_state.connected) {
        lv_obj_add_flag(lbl_conn, LV_OBJ_FLAG_HIDDEN);
    }

    /* Big indoor temp */
    lbl_temp = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(THEME_WARN_ORANGE), 0);
    lv_obj_set_style_text_font(lbl_temp, THEME_FONT_NUM(90), 0);   /* light numerals */
    lv_obj_align(lbl_temp, LV_ALIGN_CENTER, 0, SY(-96));
    lv_label_set_text(lbl_temp, "--.-°");

    /* Flame icon at left of the big temp — visible only when the burner
     * is firing CH. Same source/colour as the home-tile flame for
     * consistency. */
    img_temp_flame = lv_img_create(scr_root);
    lv_img_set_src(img_temp_flame, &icon_radiator);
    lv_img_set_zoom(img_temp_flame, 256);
    lv_obj_set_style_img_recolor(img_temp_flame, lv_color_hex(THEME_GLOW), 0);
    lv_obj_set_style_img_recolor_opa(img_temp_flame, 255, 0);
    lv_obj_align(img_temp_flame, LV_ALIGN_CENTER, SX(145), SY(-75));
    lv_obj_add_flag(img_temp_flame, LV_OBJ_FLAG_HIDDEN);

    /* Program preset buttons — two rows centered horizontally below the indoor temp.
     * Row 1 (SY(-20)): Scheduled / Manual mode toggle (2 wider buttons).
     * Row 2 (SY(36)):  Comfort / Home / Sleep / Away presets (4 buttons),
     *                  shown only when the schedule is active. */
    {
        const char * names[6] = {tr("Programma", "Scheduled"), tr("Handmatig", "Manual"),
                                 tr("Comfort", "Comfort"), tr("Thuis", "Home"),
                                 tr("Slapen", "Sleep"), tr("Weg", "Away")};
        uint32_t     cols[6]  = {COL_PROG_SCHED, COL_PROG_MANUAL,
                                 COL_PROG_COMFORT, COL_PROG_HOME, COL_PROG_SLEEP, COL_PROG_AWAY};

        /* Row 1: Mode buttons (0..1) */
        const int bw_mode = SX(170), bh_mode = SY(44), gap_mode = SX(14);
        int total_mode = 2 * bw_mode + gap_mode;
        for (int i = 0; i < 2; i++) {
            lv_obj_t * b = lv_btn_create(scr_root);
            lv_obj_set_size(b, bw_mode, bh_mode);
            lv_obj_align(b, LV_ALIGN_CENTER,
                         -total_mode / 2 + i * (bw_mode + gap_mode) + bw_mode / 2, SY(-20));
            lv_obj_set_style_bg_color(b, lv_color_hex(cols[i]), 0);
            lv_obj_set_style_radius(b, 10, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(THEME_TEXT_HI), 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_add_event_cb(b, on_program_tap, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
            lv_obj_t * bl = lv_label_create(b);
            lv_label_set_text(bl, names[i]);
            lv_obj_set_style_text_color(bl, lv_color_hex(COL_ON_IDENT), 0);
            lv_obj_set_style_text_font(bl, SF(18), 0);
            lv_obj_center(bl);
            btn_prog[i] = b;
        }

        /* Row 2: Preset buttons (2..5) */
        const int bw_pre = SX(125), bh_pre = SY(42), gap_pre = SX(8);
        int total_pre = 4 * bw_pre + 3 * gap_pre;
        for (int i = 2; i < 6; i++) {
            lv_obj_t * b = lv_btn_create(scr_root);
            lv_obj_set_size(b, bw_pre, bh_pre);
            lv_obj_align(b, LV_ALIGN_CENTER,
                         -total_pre / 2 + (i - 2) * (bw_pre + gap_pre) + bw_pre / 2, SY(36));
            lv_obj_set_style_bg_color(b, lv_color_hex(cols[i]), 0);
            lv_obj_set_style_radius(b, 10, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(THEME_TEXT_HI), 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_add_event_cb(b, on_program_tap, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
            lv_obj_t * bl = lv_label_create(b);
            lv_label_set_text(bl, names[i]);
            lv_obj_set_style_text_color(bl, lv_color_hex(COL_ON_IDENT), 0);
            lv_obj_set_style_text_font(bl, SF(18), 0);
            lv_obj_center(bl);
            btn_prog[i] = b;
        }
    }

    /* Environment readings live in the left column just above the
       setpoint row, where "Boiler idle" used to sit. Boiler state has
       moved further up. */
    /* Environment readings */
    lbl_humidity = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_humidity, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_humidity, SF(22), 0);
    lv_obj_align(lbl_humidity, LV_ALIGN_BOTTOM_LEFT, 30,
                 SY(-260) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_humidity, tr("RV --%", "RH --%"));

    lbl_voc = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_voc, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_voc, SF(22), 0);
    lv_obj_align(lbl_voc, LV_ALIGN_BOTTOM_LEFT, 30,
                 SY(-225) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_voc, tr("eCO2 -- ppm", "eCO2 -- ppm"));

    lbl_water = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_water, lv_color_hex(THEME_INFO), 0);
    lv_obj_set_style_text_font(lbl_water, SF(22), 0);
    lv_obj_align(lbl_water, LV_ALIGN_BOTTOM_LEFT, 30,
                 SY(-190) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_water, tr("Water -- m3", "Water -- m3"));

    /* Setpoint row with +/- buttons. On Toon 1, keep the original 560x130
       layout. On Toon 2, scale modestly so - and + buttons are well-proportioned
       instead of oversized boxes. */
    int is_t1 = (DISP_VER < 600);
    int sp_w  = is_t1 ? 460 : 580;   /* T1: clears the Boost / Holiday column on the right */
    int sp_h  = is_t1 ? 130 : 92;
    int btn_w = is_t1 ? 110 : 84;
    int btn_h = is_t1 ? 100 : 70;
    const lv_font_t * btn_f = is_t1 ? SF(28) : SF(30);
    const lv_font_t * sp_f  = is_t1 ? SF(20) : SF(24);

    lv_obj_t * sp_row = lv_obj_create(scr_root);
    lv_obj_set_size(sp_row, sp_w, sp_h);
    /* Lift a little extra on Toon 1 so its bottom clears the Advanced btn. */
    lv_obj_align(sp_row, LV_ALIGN_BOTTOM_MID, 0,
                 is_t1 ? (SY(-40) - 8) : SY(-40));
    lv_obj_set_style_bg_color(sp_row, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_width(sp_row, 0, 0);
    lv_obj_set_style_radius(sp_row, 14, 0);
    lv_obj_clear_flag(sp_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * btn_dn = lv_btn_create(sp_row);
    lv_obj_set_size(btn_dn, btn_w, btn_h);
    lv_obj_align(btn_dn, LV_ALIGN_LEFT_MID, is_t1 ? 5 : 10, 0);
    lv_obj_set_style_bg_color(btn_dn, lv_color_hex(THEME_BTN_ACCENT), 0);
    lv_obj_add_event_cb(btn_dn, on_setpoint_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_dn_lbl = lv_label_create(btn_dn);
    lv_label_set_text(btn_dn_lbl, "-");
    lv_obj_set_style_text_color(btn_dn_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(btn_dn_lbl, btn_f, 0);
    lv_obj_center(btn_dn_lbl);

    lbl_setpoint = lv_label_create(sp_row);
    lv_obj_set_style_text_color(lbl_setpoint, lv_color_hex(THEME_TEXT_HI), 0);
    /* Smaller font gives plenty of breathing room between the - and + buttons */
    lv_obj_set_style_text_font(lbl_setpoint, sp_f, 0);
    lv_obj_align(lbl_setpoint, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(lbl_setpoint, tr("Streefwaarde: --", "Setpoint: --"));

    lv_obj_t * btn_up = lv_btn_create(sp_row);
    lv_obj_set_size(btn_up, btn_w, btn_h);
    lv_obj_align(btn_up, LV_ALIGN_RIGHT_MID, is_t1 ? -5 : -10, 0);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(THEME_BTN_ACCENT), 0);
    lv_obj_add_event_cb(btn_up, on_setpoint_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_up_lbl = lv_label_create(btn_up);
    lv_label_set_text(btn_up_lbl, "+");
    lv_obj_set_style_text_color(btn_up_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(btn_up_lbl, btn_f, 0);
    lv_obj_center(btn_up_lbl);

    /* Boiler state — icons + text. Icons live at left; text label sits
       to their right and is the only thing that ever fills its area. */
    img_flame = lv_img_create(scr_root);
    lv_img_set_src(img_flame, &icon_flame);
    lv_obj_set_style_img_recolor(img_flame, lv_color_hex(THEME_GLOW), 0);
    lv_obj_set_style_img_recolor_opa(img_flame, 255, 0);
    lv_obj_align(img_flame, LV_ALIGN_BOTTOM_LEFT, 30, -340);
    lv_obj_add_flag(img_flame, LV_OBJ_FLAG_HIDDEN);

    img_faucet = lv_img_create(scr_root);
    lv_img_set_src(img_faucet, &icon_faucet);
    lv_obj_set_style_img_recolor(img_faucet, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_img_recolor_opa(img_faucet, 255, 0);
    lv_obj_align(img_faucet, LV_ALIGN_BOTTOM_LEFT, 26, -344);
    lv_obj_add_flag(img_faucet, LV_OBJ_FLAG_HIDDEN);

    /* Red drop overlapping the faucet spout */
    img_drop = lv_img_create(scr_root);
    lv_img_set_src(img_drop, &icon_drop);
    lv_obj_set_style_img_recolor(img_drop, lv_color_hex(COL_DROP_ICON), 0);
    lv_obj_set_style_img_recolor_opa(img_drop, 255, 0);
    lv_obj_align(img_drop, LV_ALIGN_BOTTOM_LEFT, 48, -320);
    lv_obj_add_flag(img_drop, LV_OBJ_FLAG_HIDDEN);

    /* "Boiler idle / Heating / Hot water" status label */
    lbl_burner = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_burner, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_burner, SF(22), 0);
    lv_obj_align(lbl_burner, LV_ALIGN_TOP_LEFT, 180, SY(130));
    lv_label_set_text(lbl_burner, tr("Ketel rust", "Boiler idle"));

    /* CH water inlet/outlet block */
    lbl_ch_hdr = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_ch_hdr, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_ch_hdr, SF(18), 0);
    lv_obj_align(lbl_ch_hdr, LV_ALIGN_BOTTOM_RIGHT, -40,
                 SY(-250) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_ch_hdr, tr("CV-water", "CH water"));

    lbl_flow = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_flow, lv_color_hex(THEME_GLOW), 0);
    lv_obj_set_style_text_font(lbl_flow, SF(22), 0);
    lv_obj_align(lbl_flow, LV_ALIGN_BOTTOM_RIGHT, -40,
                 SY(-220) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_flow, tr("Aanvoer  -- C", "Flow  -- C"));

    lbl_return = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_return, lv_color_hex(THEME_INFO), 0);
    lv_obj_set_style_text_font(lbl_return, SF(22), 0);
    lv_obj_align(lbl_return, LV_ALIGN_BOTTOM_RIGHT, -40,
                 SY(-192) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_return, tr("Retour  -- C", "Return  -- C"));

    /* Quick actions above "Advanced": Boost +1° for an hour, Holiday. */
    btn_boost = quick_pill(SY(-76), on_boost, &lbl_boost);
    btn_holiday = quick_pill(SY(-132), on_holiday, &lbl_holiday);
    quick_paint();

    /* "Advanced" button */
    lv_obj_t * adv = lv_btn_create(scr_root);
    lv_obj_set_size(adv, SX(180), SY(44));   /* same column as Boost / Holiday */
    lv_obj_align(adv, LV_ALIGN_BOTTOM_RIGHT, -SX(20), -SY(20));
    lv_obj_set_style_bg_color(adv, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_add_event_cb(adv, on_open_advanced, LV_EVENT_CLICKED, NULL);
    lv_obj_t * advl = lv_label_create(adv);
    lv_obj_set_style_text_color(advl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(advl, SF(18), 0);
    lv_label_set_text(advl, tr("Geavanceerd", "Advanced"));
    lv_obj_center(advl);

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 500, NULL);
    return scr_root;
}
