/*
 * Forecast detail screen.
 *
 * Final layout (Phase 2 adds the radar image on the left):
 *   [ Radar image | weersverwachting title + body text ]
 *   [ 5-day forecast strip across the bottom ]
 *
 * This Phase-1 version uses a placeholder block where the radar image
 * goes, and shows the weatherreport text body on the right.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "weather.h"
#include "icons.h"
#include "settings.h"
#include "theme.h"
#include "lvgl/src/extra/libs/gif/lv_gif.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_title;
static lv_obj_t * lbl_body;
static lv_obj_t * fc_day_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_temp_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_desc_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_icon[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_arrow[WEATHER_FORECAST_DAYS];
static lv_obj_t * radar_img = NULL;
static int radar_loaded = 0;
static lv_timer_t * refresh_timer = NULL;

/* Radar zoom — LVGL convention: 256 = 100 %. Source GIF is 550x512, frame
 * is 380x380, so we clamp to a range that lets the user see detail without
 * overshooting the visible area too far. */
#define RADAR_ZOOM_MIN  128
#define RADAR_ZOOM_MAX  768
#define RADAR_ZOOM_STEP  64
static int radar_zoom = 195;             /* fits 550-wide source into 380 */

static void on_back(lv_event_t * e) {
    (void)e;
    if (radar_img) {
        lv_gif_t * g = (lv_gif_t *)radar_img;
        if (g->timer) lv_timer_pause(g->timer);
    }
    ui_pop();
}

static void on_radar_zoom(lv_event_t * e) {
    intptr_t d = (intptr_t)lv_event_get_user_data(e);
    int z = radar_zoom + (int)d;
    if (z < RADAR_ZOOM_MIN) z = RADAR_ZOOM_MIN;
    if (z > RADAR_ZOOM_MAX) z = RADAR_ZOOM_MAX;
    radar_zoom = z;
    if (radar_img) lv_img_set_zoom(radar_img, radar_zoom);
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root) return;
    if (lv_scr_act() != scr_root) {
        if (radar_img) {
            lv_gif_t * g = (lv_gif_t *)radar_img;
            if (g->timer) lv_timer_pause(g->timer);
        }
        return;
    }
    if (radar_img) {
        lv_gif_t * g = (lv_gif_t *)radar_img;
        if (g->timer) lv_timer_resume(g->timer);
    }
    weather_state_t wx;
    weather_get_snapshot(&wx);
    lv_label_set_text(lbl_title, wx.weatherreport_title);
    lv_label_set_text(lbl_body,  wx.weatherreport_text);

    if (radar_img && !radar_loaded && access("/tmp/toonui_radar.gif", R_OK) == 0) {
        lv_gif_set_src(radar_img, "S:/tmp/toonui_radar.gif");
        lv_img_set_zoom(radar_img, radar_zoom);
        lv_obj_center(radar_img);
        radar_loaded = 1;
    }

    int show_hourly = settings.forecast_mode != FORECAST_DAILY
                      && wx.hour_count > 0;
    if (show_hourly) {
        for (int i = 0; i < wx.hour_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_hour_t * h = &wx.hours[i];
            lv_label_set_text(fc_day_lbl[i], h->label);
            lv_label_set_text_fmt(fc_temp_lbl[i], "%.0f\xc2\xb0",
                                  weather_hour_display_temp(h));
            if (fc_icon[i]) {
                lv_img_set_src(fc_icon[i], weather_icon_for_lg(h->icon));
                lv_obj_set_style_img_recolor(fc_icon[i],
                    lv_color_hex(weather_icon_color_for(h->icon)), 0);
            }
            if (fc_wind_lbl[i]) {
                if (h->wind_dir[0])
                    lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                          h->wind_dir, h->wind_bft);
                else
                    lv_label_set_text(fc_wind_lbl[i], "");
            }
            if (fc_wind_arrow[i]) {
                int ang = wind_dir_angle(h->wind_dir);
                if (ang >= 0) {
                    lv_img_set_angle(fc_wind_arrow[i], ang);
                    lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    } else {
        for (int i = 0; i < wx.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &wx.days[i];
            lv_label_set_text(fc_day_lbl[i], d->day);
            lv_label_set_text_fmt(fc_temp_lbl[i],
                                  "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                  d->max_temp, d->min_temp);
            if (fc_icon[i]) {
                lv_img_set_src(fc_icon[i], weather_icon_for_lg(d->icon));
                lv_obj_set_style_img_recolor(fc_icon[i],
                    lv_color_hex(weather_icon_color_for(d->icon)), 0);
            }
            if (fc_wind_lbl[i]) {
                if (d->wind_dir[0])
                    lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                          d->wind_dir, d->wind_bft);
                else
                    lv_label_set_text(fc_wind_lbl[i], "");
            }
            if (fc_wind_arrow[i]) {
                int ang = wind_dir_angle(d->wind_dir);
                if (ang >= 0) {
                    lv_img_set_angle(fc_wind_arrow[i], ang);
                    lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
}

static int scr_light_mode = -1;

lv_obj_t * screen_forecast_create(void) {
    if (scr_root && scr_light_mode == settings.home_light_mode) {
        if (radar_img) {
            lv_gif_t * g = (lv_gif_t *)radar_img;
            if (g->timer) lv_timer_resume(g->timer);
        }
        return scr_root;
    }
    if (scr_root) {
        lv_obj_del(scr_root);
        scr_root = NULL;
        radar_img = NULL;
        radar_loaded = 0;
    }
    scr_light_mode = settings.home_light_mode;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Close button top-right */
    lv_obj_t * close_btn = lv_btn_create(scr_root);
    int cb_w = (DISP_HOR < 1000) ? 68 : 74;
    int cb_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_set_size(close_btn, cb_w, cb_h);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_ext_click_area(close_btn, 30);
    lv_obj_add_event_cb(close_btn, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(close_lbl, SF(22), 0);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    /* Radar image — buienradar serves a 500x512 PNG of the Netherlands
       precipitation map. Our weather thread saves it to disk every 5 min;
       we render via LVGL's stdio FS driver + PNG decoder. */
    int rf_sz = (DISP_VER < 600) ? 275 : 360;
    int rf_x = (DISP_VER < 600) ? 14 : 24;
    int rf_y = (DISP_VER < 600) ? 44 : 52;
    lv_obj_t * radar_frame = lv_obj_create(scr_root);
    lv_obj_set_size(radar_frame, rf_sz, rf_sz);
    lv_obj_set_pos(radar_frame, rf_x, rf_y);
    lv_obj_set_style_bg_color(radar_frame, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_radius(radar_frame, 12, 0);
    lv_obj_set_style_border_color(radar_frame, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(radar_frame, 1, 0);
    lv_obj_set_style_pad_all(radar_frame, 0, 0);
    lv_obj_set_style_clip_corner(radar_frame, true, 0);
    lv_obj_clear_flag(radar_frame, LV_OBJ_FLAG_SCROLLABLE);

    /* Buienradar radar GIF. With our local gifdec patch the no-GCT
       case is now accepted; the per-frame LCT populates the palette. */
    radar_img = lv_gif_create(radar_frame);
    if (access("/tmp/toonui_radar.gif", R_OK) == 0) {
        lv_gif_set_src(radar_img, "S:/tmp/toonui_radar.gif");
        radar_loaded = 1;
    } else {
        radar_loaded = 0;
    }
    lv_img_set_zoom(radar_img, radar_zoom);
    lv_obj_center(radar_img);

    /* Zoom + / - buttons stacked on top of the radar frame's top-left
       corner. Translucent so they don't fully hide the map underneath. */
    struct { lv_align_t a; int x, y; int d; const char * t; } z[] = {
        { LV_ALIGN_TOP_LEFT, rf_x + 4, rf_y + 4,  +RADAR_ZOOM_STEP, "+" },
        { LV_ALIGN_TOP_LEFT, rf_x + 4, rf_y + 46, -RADAR_ZOOM_STEP, "-" },
    };
    for (size_t i = 0; i < sizeof(z)/sizeof(z[0]); i++) {
        lv_obj_t * b = lv_btn_create(scr_root);
        lv_obj_set_size(b, 38, 38);
        lv_obj_align(b, z[i].a, z[i].x, z[i].y);
        lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_ACCENT), 0);
        lv_obj_set_style_bg_opa(b, 180, 0);
        lv_obj_set_style_radius(b, 19, 0);
        lv_obj_add_event_cb(b, on_radar_zoom, LV_EVENT_CLICKED,
                            (void *)(intptr_t)z[i].d);
        lv_obj_t * bl = lv_label_create(b);
        lv_obj_set_style_text_color(bl, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(bl, SF(24), 0);
        lv_label_set_text(bl, z[i].t);
        lv_obj_center(bl);
    }

    /* Right side: title + body text */
    int right_x = rf_x + rf_sz + (DISP_VER < 600 ? 14 : 24);
    int right_w = DISP_HOR - right_x - (DISP_VER < 600 ? 14 : 24);
    lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_title, (DISP_VER < 600 ? SF(22) : SF(28)), 0);
    lv_obj_set_width(lbl_title, right_w - 80);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, right_x, rf_y);
    lv_label_set_text(lbl_title, tr("Weersverwachting", "Weather forecast"));

    /* Body text — vertically-scrollable container matching radar frame height */
    int body_y = rf_y + (DISP_VER < 600 ? 30 : 38);
    int body_h = rf_sz - (body_y - rf_y);
    lv_obj_t * body_scroll = lv_obj_create(scr_root);
    lv_obj_set_size(body_scroll, right_w, body_h);
    lv_obj_align(body_scroll, LV_ALIGN_TOP_LEFT, right_x, body_y);
    lv_obj_set_style_bg_opa(body_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body_scroll, 0, 0);
    lv_obj_set_style_pad_all(body_scroll, 0, 0);
    lv_obj_set_scroll_dir(body_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_scroll, LV_SCROLLBAR_MODE_AUTO);
    lbl_body = lv_label_create(body_scroll);
    lv_obj_set_style_text_color(lbl_body, lv_color_hex(settings.home_light_mode ? 0x334455 : 0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_body, SF(18), 0);
    lv_obj_set_width(lbl_body, right_w - 12);
    lv_label_set_long_mode(lbl_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_body, tr("(laden...)", "(loading...)"));

    /* 5-day forecast strip — positioned below radar & forecast text without any overlap */
    int col_w = (DISP_HOR - 20) / WEATHER_FORECAST_DAYS;
    int strip_y = (DISP_VER < 600) ? 336 : 435;
    int strip_h = (DISP_VER < 600) ? 134 : 155;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        lv_obj_t * col = lv_obj_create(scr_root);
        lv_obj_set_size(col, col_w - 8, strip_h);
        lv_obj_set_pos(col, 10 + i * col_w, strip_y);
        lv_obj_set_style_bg_color(col, lv_color_hex(THEME_CARD_BG), 0);
        lv_obj_set_style_radius(col, 10, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 8, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        /* Centred big icon */
        fc_icon[i] = lv_img_create(col);
        lv_img_set_src(fc_icon[i], &icon_wx_cloud_lg);
        lv_obj_set_style_img_recolor(fc_icon[i], lv_color_hex(settings.home_light_mode ? 0x64748b : 0xddeeff), 0);
        lv_obj_set_style_img_recolor_opa(fc_icon[i], 255, 0);
        if (DISP_VER < 600) {
            lv_img_set_zoom(fc_icon[i], 140);
            lv_obj_align(fc_icon[i], LV_ALIGN_CENTER, 0, 10);
        } else {
            lv_img_set_zoom(fc_icon[i], 200);
            lv_obj_align(fc_icon[i], LV_ALIGN_CENTER, 0, 12);
        }

        /* Wind-direction arrow + Bft at the bottom of each column. */
        fc_wind_arrow[i] = lv_img_create(col);
        lv_img_set_src(fc_wind_arrow[i], &icon_wind_arrow);
        lv_img_set_pivot(fc_wind_arrow[i], 16, 16);
        lv_obj_align(fc_wind_arrow[i], LV_ALIGN_BOTTOM_LEFT, 0, 2);
        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);

        fc_wind_lbl[i] = lv_label_create(col);
        lv_obj_set_style_text_color(fc_wind_lbl[i], lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(fc_wind_lbl[i], (DISP_VER < 600) ? SF(14) : SF(18), 0);
        lv_label_set_text(fc_wind_lbl[i], "");
        lv_obj_align(fc_wind_lbl[i], LV_ALIGN_BOTTOM_LEFT, (DISP_VER < 600) ? 22 : 30, -2);

        /* Day and temperature stacked vertically to avoid horizontal overlap */
        fc_day_lbl[i] = lv_label_create(col);
        lv_obj_set_style_text_color(fc_day_lbl[i], lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(fc_day_lbl[i], (DISP_VER < 600) ? SF(14) : SF(18), 0);
        lv_label_set_text(fc_day_lbl[i], "--");
        lv_obj_align(fc_day_lbl[i], LV_ALIGN_TOP_MID, 0, (DISP_VER < 600 ? 2 : 4));

        fc_temp_lbl[i] = lv_label_create(col);
        lv_obj_set_style_text_color(fc_temp_lbl[i], lv_color_hex(THEME_WARN_ORANGE), 0);
        lv_obj_set_style_text_font(fc_temp_lbl[i], (DISP_VER < 600) ? SF(14) : SF(18), 0);
        lv_label_set_text(fc_temp_lbl[i], "");
        lv_obj_align(fc_temp_lbl[i], LV_ALIGN_TOP_MID, 0, (DISP_VER < 600 ? 18 : 26));

        /* Verbose desc moved to the title body — match the home band. */
        fc_desc_lbl[i] = NULL;
    }

    refresh_cb(NULL);
    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 5000, NULL);
    return scr_root;
}
