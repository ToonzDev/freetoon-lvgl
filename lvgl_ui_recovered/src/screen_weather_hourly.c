/*
 * Detailed 24-hour weather forecast per day screen.
 * Opened by tapping any of the 5 day cards in screen_forecast.c.
 */
#include "screen_weather_hourly.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "weather.h"
#include "icons.h"
#include "settings.h"
#include "theme.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#undef THEME_BG
#undef THEME_CARD_BG
#undef THEME_TEXT_HI
#undef THEME_TEXT_DIM
#undef THEME_BTN_BG

#define THEME_BG          (settings.home_light_mode ? 0xeef4fa : 0x0f1722)
#define THEME_CARD_BG     (settings.home_light_mode ? 0xffffff : 0x141f2d)
#define THEME_CARD_ALT    (settings.home_light_mode ? 0xf2f7fc : 0x192738)
#define THEME_TEXT_HI     (settings.home_light_mode ? 0x112233 : 0xffffff)
#define THEME_TEXT_MID    (settings.home_light_mode ? 0x334455 : 0xc0d0e0)
#define THEME_TEXT_DIM    (settings.home_light_mode ? 0x667788 : 0x7a8c9e)
#define THEME_BTN_BG      (settings.home_light_mode ? 0xd0d8e2 : 0x223344)
#define THEME_ORANGE      (settings.home_light_mode ? 0xa84200 : 0xffa333)
#define THEME_BLUE        (settings.home_light_mode ? 0x1d4ed8 : 0x5a9bd3)

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_day_title = NULL;
static lv_obj_t * lbl_day_summary = NULL;
static lv_obj_t * btn_prev_day = NULL;
static lv_obj_t * btn_next_day = NULL;
static lv_obj_t * hourly_list = NULL;
static int s_active_day = 0;
static int scr_light_mode = -1;

static void on_back(lv_event_t * e) {
    (void)e;
    ui_pop();
}

static void render_active_day(void);

static void on_prev_day(lv_event_t * e) {
    (void)e;
    if (s_active_day > 0) {
        s_active_day--;
        render_active_day();
    }
}

static void on_next_day(lv_event_t * e) {
    (void)e;
    if (s_active_day < WEATHER_FORECAST_DAYS - 1) {
        s_active_day++;
        render_active_day();
    }
}

static void render_active_day(void) {
    if (!scr_root || !hourly_list) return;

    weather_state_t wx;
    weather_get_snapshot(&wx);

    if (s_active_day < 0) s_active_day = 0;
    if (s_active_day >= wx.day_count && wx.day_count > 0) s_active_day = wx.day_count - 1;

    const weather_day_t * d = (s_active_day < wx.day_count) ? &wx.days[s_active_day] : NULL;

    /* Day Title & Summary */
    if (d && d->day[0]) {
        if (s_active_day == 0) {
            lv_label_set_text_fmt(lbl_day_title, "%s (%s)", d->day, tr("Vandaag", "Today"));
        } else if (s_active_day == 1) {
            lv_label_set_text_fmt(lbl_day_title, "%s (%s)", d->day, tr("Morgen", "Tomorrow"));
        } else {
            lv_label_set_text(lbl_day_title, d->day);
        }

        char sum_buf[128];
        snprintf(sum_buf, sizeof(sum_buf),
                 "Min %.1f\xc2\xb0\x43  \xe2\x80\xa2  Max %.1f\xc2\xb0\x43  \xe2\x80\xa2  "
                 "%s: %d%%  \xe2\x80\xa2  Wind: %s %d Bft",
                 d->min_temp, d->max_temp,
                 tr("Regenkans", "Rain chance"), d->rain_chance,
                 d->wind_dir, d->wind_bft);
        lv_label_set_text(lbl_day_summary, sum_buf);
    } else {
        lv_label_set_text(lbl_day_title, tr("Uurlijkse verwachting", "Hourly forecast"));
        lv_label_set_text(lbl_day_summary, tr("Laden...", "Loading..."));
    }

    /* Update prev/next button visual state */
    if (btn_prev_day) {
        if (s_active_day == 0) lv_obj_add_state(btn_prev_day, LV_STATE_DISABLED);
        else lv_obj_clear_state(btn_prev_day, LV_STATE_DISABLED);
    }
    if (btn_next_day) {
        if (s_active_day >= wx.day_count - 1 || s_active_day >= WEATHER_FORECAST_DAYS - 1)
            lv_obj_add_state(btn_next_day, LV_STATE_DISABLED);
        else
            lv_obj_clear_state(btn_next_day, LV_STATE_DISABLED);
    }

    /* Clear existing list rows */
    lv_obj_clean(hourly_list);

    if (!d || d->hour_count == 0) {
        lv_obj_t * empty_lbl = lv_label_create(hourly_list);
        lv_obj_set_style_text_color(empty_lbl, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(empty_lbl, SF(16), 0);
        lv_label_set_text(empty_lbl, tr("Geen uurlijkse gegevens beschikbaar", "No hourly data available"));
        lv_obj_center(empty_lbl);
        return;
    }

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int cur_hour = lt.tm_hour;

    lv_obj_t * target_scroll_row = NULL;

    /* Column coordinate offsets */
    int col_time_x = SX(16);
    int col_icon_x = SX(120);
    int col_temp_x = SX(230);
    int col_rain_x = SX(470);
    int col_wind_x = SX(730);

    if (DISP_HOR < 1000) {
        col_time_x = SX(12);
        col_icon_x = SX(90);
        col_temp_x = SX(170);
        col_rain_x = SX(360);
        col_wind_x = SX(560);
    }

    int row_h = (DISP_VER < 600) ? 38 : 42;

    for (int h = 0; h < d->hour_count; h++) {
        const weather_hourly_slot_t * slot = &d->hours[h];

        lv_obj_t * row = lv_obj_create(hourly_list);
        lv_obj_set_size(row, LV_PCT(100), row_h);
        lv_obj_set_flex_grow(row, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        int is_current = (s_active_day == 0 && h == cur_hour);

        if (is_current) {
            lv_obj_set_style_bg_color(row, lv_color_hex(settings.home_light_mode ? 0xd8eafc : 0x1c3046), 0);
            lv_obj_set_style_border_color(row, lv_color_hex(THEME_BLUE), 0);
            lv_obj_set_style_border_width(row, 2, 0);
        } else {
            lv_obj_set_style_bg_color(row, lv_color_hex((h % 2 == 0) ? THEME_CARD_ALT : THEME_CARD_BG), 0);
            lv_obj_set_style_border_width(row, 0, 0);
        }

        /* 1. Tijd */
        lv_obj_t * lbl_t = lv_label_create(row);
        lv_obj_set_style_text_font(lbl_t, SF(15), 0);
        if (is_current) {
            lv_obj_set_style_text_color(lbl_t, lv_color_hex(THEME_BLUE), 0);
            lv_label_set_text_fmt(lbl_t, "%s (nu)", slot->label);
        } else {
            lv_obj_set_style_text_color(lbl_t, lv_color_hex(THEME_TEXT_HI), 0);
            lv_label_set_text(lbl_t, slot->label);
        }
        lv_obj_align(lbl_t, LV_ALIGN_LEFT_MID, col_time_x, 0);

        /* 2. Weer Icoon */
        lv_obj_t * img_i = lv_img_create(row);
        lv_img_set_src(img_i, weather_icon_for(slot->icon));
        lv_obj_set_style_img_recolor(img_i, lv_color_hex(weather_icon_color_for(slot->icon)), 0);
        lv_obj_set_style_img_recolor_opa(img_i, 255, 0);
        lv_img_set_zoom(img_i, (DISP_VER < 600) ? 110 : 130);
        lv_obj_align(img_i, LV_ALIGN_LEFT_MID, col_icon_x, 0);

        /* 3. Temperatuur */
        lv_obj_t * lbl_temp = lv_label_create(row);
        lv_obj_set_style_text_font(lbl_temp, SF(15), 0);
        lv_obj_set_style_text_color(lbl_temp, lv_color_hex(THEME_ORANGE), 0);

        float feel_diff = slot->feel_temp - slot->temperature;
        if (feel_diff < -1.0f || feel_diff > 1.0f) {
            lv_label_set_text_fmt(lbl_temp, "%.1f\xc2\xb0\x43  (gevoel %.0f\xc2\xb0)",
                                  slot->temperature, slot->feel_temp);
        } else {
            lv_label_set_text_fmt(lbl_temp, "%.1f\xc2\xb0\x43", slot->temperature);
        }
        lv_obj_align(lbl_temp, LV_ALIGN_LEFT_MID, col_temp_x, 0);

        /* 4. Neerslag */
        lv_obj_t * lbl_rain = lv_label_create(row);
        lv_obj_set_style_text_font(lbl_rain, SF(15), 0);
        if (slot->rain_chance > 0 || slot->rain_mm >= 0.05f) {
            lv_obj_set_style_text_color(lbl_rain, lv_color_hex(THEME_BLUE), 0);
            if (slot->rain_mm >= 0.05f) {
                lv_label_set_text_fmt(lbl_rain, "%d%% (%.1f mm)", slot->rain_chance, slot->rain_mm);
            } else {
                lv_label_set_text_fmt(lbl_rain, "%d%%", slot->rain_chance);
            }
        } else {
            lv_obj_set_style_text_color(lbl_rain, lv_color_hex(THEME_TEXT_DIM), 0);
            lv_label_set_text(lbl_rain, "0% (droog)");
        }
        lv_obj_align(lbl_rain, LV_ALIGN_LEFT_MID, col_rain_x, 0);

        /* 5. Wind */
        lv_obj_t * img_w = lv_img_create(row);
        lv_img_set_src(img_w, &icon_wind_arrow);
        lv_img_set_pivot(img_w, 16, 16);
        int ang = wind_dir_angle(slot->wind_dir);
        if (ang >= 0) {
            lv_img_set_angle(img_w, ang);
            lv_obj_clear_flag(img_w, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(img_w, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_align(img_w, LV_ALIGN_LEFT_MID, col_wind_x, 0);

        lv_obj_t * lbl_wind = lv_label_create(row);
        lv_obj_set_style_text_font(lbl_wind, SF(15), 0);
        lv_obj_set_style_text_color(lbl_wind, lv_color_hex(THEME_TEXT_HI), 0);
        if (slot->wind_dir[0]) {
            lv_label_set_text_fmt(lbl_wind, "%s %d Bft", slot->wind_dir, slot->wind_bft);
        } else {
            lv_label_set_text_fmt(lbl_wind, "%d Bft", slot->wind_bft);
        }
        lv_obj_align(lbl_wind, LV_ALIGN_LEFT_MID, col_wind_x + SX(28), 0);
    }

    if (s_active_day == 0 && cur_hour > 1) {
        lv_obj_scroll_to_y(hourly_list, (cur_hour - 1) * (row_h + 2), LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(hourly_list, 0, LV_ANIM_OFF);
    }
}

void screen_weather_hourly_show(int day_idx) {
    s_active_day = day_idx;

    if (scr_root && scr_light_mode == settings.home_light_mode) {
        render_active_day();
        ui_push(scr_root);
        return;
    }

    if (scr_root) {
        lv_obj_del(scr_root);
        scr_root = NULL;
    }

    scr_light_mode = settings.home_light_mode;
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Close button (top-right) */
    lv_obj_t * close_btn = lv_btn_create(scr_root);
    int cb_w = (DISP_HOR < 1000) ? 68 : 74;
    int cb_h = (DISP_VER < 600) ? 42 : 46;
    lv_obj_set_size(close_btn, cb_w, cb_h);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_add_event_cb(close_btn, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(close_lbl, (DISP_VER < 600 ? SF(16) : SF(20)), 0);
    lv_obj_center(close_lbl);

    /* Header navigation bar — centered with arrow buttons snug around title block */
    int tw = (DISP_HOR < 1000) ? 380 : 460;
    int nav_w = tw + 2 * cb_w + SX(24);
    lv_obj_t * nav_bar = lv_obj_create(scr_root);
    lv_obj_set_size(nav_bar, nav_w, SY(54));
    lv_obj_align(nav_bar, LV_ALIGN_TOP_MID, 0, SY(4));
    lv_obj_set_style_bg_opa(nav_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_bar, 0, 0);
    lv_obj_set_style_pad_all(nav_bar, 0, 0);
    lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Prev Day [<] Button — same size & style as "X" button */
    btn_prev_day = lv_btn_create(nav_bar);
    lv_obj_set_size(btn_prev_day, cb_w, cb_h);
    lv_obj_align(btn_prev_day, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_prev_day, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_prev_day, 10, 0);
    lv_obj_add_event_cb(btn_prev_day, on_prev_day, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lp = lv_label_create(btn_prev_day);
    lv_label_set_text(lp, "<");
    lv_obj_set_style_text_color(lp, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lp, (DISP_VER < 600 ? SF(16) : SF(20)), 0);
    lv_obj_center(lp);

    /* Next Day [>] Button — same size & style as "X" button */
    btn_next_day = lv_btn_create(nav_bar);
    lv_obj_set_size(btn_next_day, cb_w, cb_h);
    lv_obj_align(btn_next_day, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_next_day, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_next_day, 10, 0);
    lv_obj_add_event_cb(btn_next_day, on_next_day, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ln = lv_label_create(btn_next_day);
    lv_label_set_text(ln, ">");
    lv_obj_set_style_text_color(ln, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(ln, (DISP_VER < 600 ? SF(16) : SF(20)), 0);
    lv_obj_center(ln);

    /* Day Title & Summary */
    lbl_day_title = lv_label_create(nav_bar);
    lv_obj_set_style_text_color(lbl_day_title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_day_title, (DISP_VER < 600 ? SF(18) : SF(22)), 0);
    lv_obj_align(lbl_day_title, LV_ALIGN_TOP_MID, 0, SY(2));

    lbl_day_summary = lv_label_create(nav_bar);
    lv_obj_set_style_text_color(lbl_day_summary, lv_color_hex(THEME_TEXT_MID), 0);
    lv_obj_set_style_text_font(lbl_day_summary, (DISP_VER < 600 ? SF(12) : SF(14)), 0);
    lv_obj_align(lbl_day_summary, LV_ALIGN_BOTTOM_MID, 0, SY(-2));

    /* Column titles header */
    lv_obj_t * thdr = lv_obj_create(scr_root);
    lv_obj_set_size(thdr, DISP_HOR - SX(32), SY(32));
    lv_obj_align(thdr, LV_ALIGN_TOP_MID, 0, SY(62));
    lv_obj_set_style_bg_color(thdr, lv_color_hex(settings.home_light_mode ? 0xdce6f2 : 0x182433), 0);
    lv_obj_set_style_radius(thdr, 8, 0);
    lv_obj_set_style_border_width(thdr, 0, 0);
    lv_obj_set_style_pad_all(thdr, 0, 0);
    lv_obj_clear_flag(thdr, LV_OBJ_FLAG_SCROLLABLE);

    int col_time_x = SX(16);
    int col_icon_x = SX(120);
    int col_temp_x = SX(230);
    int col_rain_x = SX(470);
    int col_wind_x = SX(730);

    if (DISP_HOR < 1000) {
        col_time_x = SX(12);
        col_icon_x = SX(90);
        col_temp_x = SX(170);
        col_rain_x = SX(360);
        col_wind_x = SX(560);
    }

    const char * h_titles[5] = {
        tr("Tijd", "Time"),
        tr("Weer", "Weather"),
        tr("Temperatuur", "Temperature"),
        tr("Neerslag", "Precipitation"),
        tr("Wind", "Wind")
    };
    int h_xs[5] = { col_time_x, col_icon_x, col_temp_x, col_rain_x, col_wind_x };

    for (int i = 0; i < 5; i++) {
        lv_obj_t * l = lv_label_create(thdr);
        lv_label_set_text(l, h_titles[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(l, SF(13), 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, h_xs[i], 0);
    }

    /* Scrollable list container */
    hourly_list = lv_obj_create(scr_root);
    lv_obj_set_size(hourly_list, DISP_HOR - SX(32), DISP_VER - SY(104));
    lv_obj_align(hourly_list, LV_ALIGN_BOTTOM_MID, 0, SY(-6));
    lv_obj_set_style_bg_opa(hourly_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hourly_list, 0, 0);
    lv_obj_set_style_pad_all(hourly_list, 0, 0);
    lv_obj_set_flex_flow(hourly_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hourly_list, 2, 0);
    lv_obj_set_scroll_dir(hourly_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(hourly_list, LV_SCROLLBAR_MODE_AUTO);

    render_active_day();
    ui_push(scr_root);
}

void screen_weather_hourly_close(void) {
    if (scr_root) {
        lv_obj_del(scr_root);
        scr_root = NULL;
        lbl_day_title = NULL;
        lbl_day_summary = NULL;
        btn_prev_day = NULL;
        btn_next_day = NULL;
        hourly_list = NULL;
    }
}

