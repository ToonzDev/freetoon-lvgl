/*
 * screen_chart_24h.c — Generic 24-hour multi-series chart screen for FreeToon.
 *
 * Driven declaratively by an integration's manifest.json ("detail_screen"):
 *   - Screen Title & Units from manifest
 *   - Up to 4 series with custom colors, names, and live fields
 *   - Interactive legend chips: tap to toggle series visibility on/off
 *   - 144 data points across the 24 hours (1 sample every 10 minutes)
 *   - X-axis from 00:00 to 24:00 with 2-hour intervals
 *   - Vertical "Now" cursor line showing current time of day with date badges
 *   - Auto-scaling + manual ▲ / ▼ / Auto adjustment buttons
 *
 * Used by TemperatureLogger and any third-party 24h marketplace app.
 */

#include "screen_chart_24h.h"
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "templogger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static inline float chart_floorf(float x) {
    int i = (int)x;
    return (x < (float)i) ? (float)(i - 1) : (float)i;
}

static inline float chart_ceilf(float x) {
    int i = (int)x;
    return (x > (float)i) ? (float)(i + 1) : (float)i;
}

static inline float chart_roundf(float x) {
    return (x >= 0.0f) ? (float)(int)(x + 0.5f) : (float)(int)(x - 0.5f);
}

#define COL_BG         0x101418
#define COL_CARD       0x182230
#define COL_GRID       0x223042
#define COL_BORDER     0x2a384c
#define COL_TEXT_HI    0xffffff
#define COL_TEXT_DIM   0x88aabb

typedef struct {
    char name[INTEG_NAME_MAX];
    uint32_t color_hex;
    char data_file[64];
    lv_chart_series_t * ser;
    lv_obj_t * chip_btn;
    lv_obj_t * chip_lbl;
    float live_val;
    int hidden;
    float points[TEMPLOGGER_POINTS];
} chart_series_ctx_t;

typedef struct {
    integration_meta_t meta;
    int series_count;
    chart_series_ctx_t series[INTEG_MAX_SERIES];
    char y_unit[16];
    lv_obj_t * scr;
    lv_obj_t * chart;
    lv_obj_t * lbl_title;
    lv_obj_t * lbl_range;
    lv_obj_t * y_lbls[6];
    lv_obj_t * cursor_line;
    lv_obj_t * cursor_lbl_left;
    lv_obj_t * cursor_lbl_right;
    float y_min_auto;
    float y_max_auto;
    float y_min_cur;
    float y_max_cur;
    int auto_scale;
    int cur_idx;
} chart_screen_ctx_t;

static chart_screen_ctx_t * g_ctx = NULL;

static uint32_t parse_hex_color(const char * s, uint32_t def_val) {
    if (!s || !s[0]) return def_val;
    if (s[0] == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned int v = 0;
    if (sscanf(s, "%x", &v) == 1) return v;
    return def_val;
}

static void on_back(lv_event_t * e) {
    (void)e;
    ui_pop();
}

static void update_y_axis_labels(void) {
    if (!g_ctx) return;
    float span = g_ctx->y_max_cur - g_ctx->y_min_cur;
    if (span < 1.0f) span = 1.0f;

    for (int i = 0; i < 5; i++) {
        if (!g_ctx->y_lbls[i]) continue;
        float v = g_ctx->y_min_cur + span * ((float)i / 4.0f);
        lv_label_set_text_fmt(g_ctx->y_lbls[i], "%.0f %s", v, g_ctx->y_unit);
    }
    if (g_ctx->lbl_range) {
        lv_label_set_text_fmt(g_ctx->lbl_range, "%.0f .. %.0f %s",
                              g_ctx->y_min_cur, g_ctx->y_max_cur, g_ctx->y_unit);
    }
}

static void apply_chart_range(void) {
    if (!g_ctx || !g_ctx->chart) return;

    /* Scaled into integer tenths of unit (e.g. 20.5 °C -> 205) */
    lv_coord_t r_min = (lv_coord_t)chart_floorf(g_ctx->y_min_cur * 10.0f);
    lv_coord_t r_max = (lv_coord_t)chart_ceilf(g_ctx->y_max_cur * 10.0f);
    if (r_max <= r_min) r_max = r_min + 10;

    lv_chart_set_range(g_ctx->chart, LV_CHART_AXIS_PRIMARY_Y, r_min, r_max);
    update_y_axis_labels();
    lv_chart_refresh(g_ctx->chart);
}

static void recalc_auto_range(void) {
    if (!g_ctx) return;
    float mn = 1e9f, mx = -1e9f;
    int has_points = 0;

    for (int s = 0; s < g_ctx->series_count; s++) {
        if (g_ctx->series[s].hidden) continue;
        for (int i = 0; i < TEMPLOGGER_POINTS; i++) {
            float v = g_ctx->series[s].points[i];
            if (v != TEMPLOGGER_INVALID_VAL && v > -50.0f && v < 150.0f) {
                if (v < mn) mn = v;
                if (v > mx) mx = v;
                has_points = 1;
            }
        }
    }

    if (!has_points) {
        mn = 10.0f;
        mx = 25.0f;
    } else {
        /* Add margin and round to neat 2°C multiples */
        mn = chart_floorf((mn - 1.0f) / 2.0f) * 2.0f;
        mx = chart_ceilf((mx + 1.0f) / 2.0f) * 2.0f;
        if (mx - mn < 6.0f) mx = mn + 6.0f;
    }

    g_ctx->y_min_auto = mn;
    g_ctx->y_max_auto = mx;

    if (g_ctx->auto_scale) {
        g_ctx->y_min_cur = mn;
        g_ctx->y_max_cur = mx;
        apply_chart_range();
    }
}

static void on_scale_up(lv_event_t * e) {
    (void)e;
    if (!g_ctx) return;
    g_ctx->auto_scale = 0;
    g_ctx->y_max_cur += 2.0f;
    apply_chart_range();
}

static void on_scale_down(lv_event_t * e) {
    (void)e;
    if (!g_ctx) return;
    g_ctx->auto_scale = 0;
    if (g_ctx->y_max_cur - g_ctx->y_min_cur > 4.0f) {
        g_ctx->y_max_cur -= 2.0f;
    }
    apply_chart_range();
}

static void on_scale_auto(lv_event_t * e) {
    (void)e;
    if (!g_ctx) return;
    g_ctx->auto_scale = 1;
    g_ctx->y_min_cur = g_ctx->y_min_auto;
    g_ctx->y_max_cur = g_ctx->y_max_auto;
    apply_chart_range();
}

static void on_chip_toggle(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!g_ctx || idx < 0 || idx >= g_ctx->series_count) return;

    chart_series_ctx_t * s = &g_ctx->series[idx];
    s->hidden = !s->hidden;

    if (s->hidden) {
        lv_chart_hide_series(g_ctx->chart, s->ser, true);
        lv_obj_set_style_bg_opa(s->chip_btn, LV_OPA_30, 0);
        lv_obj_set_style_text_opa(s->chip_lbl, LV_OPA_40, 0);
    } else {
        lv_chart_hide_series(g_ctx->chart, s->ser, false);
        lv_obj_set_style_bg_opa(s->chip_btn, LV_OPA_100, 0);
        lv_obj_set_style_text_opa(s->chip_lbl, LV_OPA_100, 0);
    }

    recalc_auto_range();
}

static void load_series_data(void) {
    if (!g_ctx) return;

    /* Check if TemperatureLogger data is requested */
    int is_templogger = 0;
    if (strcmp(g_ctx->meta.id, "temperaturelogger") == 0 ||
        strcmp(g_ctx->meta.id, "local:templogger") == 0 ||
        g_ctx->series_count == 0) {
        is_templogger = 1;
    }

    if (is_templogger) {
        templogger_snapshot_t snap;
        templogger_get_snapshot(&snap);
        g_ctx->cur_idx = snap.cur_idx;

        /* Series 0: Room */
        if (g_ctx->series_count > 0) {
            memcpy(g_ctx->series[0].points, snap.room, sizeof(snap.room));
            g_ctx->series[0].live_val = snap.latest_room;
        }
        /* Series 1: Setpoint */
        if (g_ctx->series_count > 1) {
            memcpy(g_ctx->series[1].points, snap.setpoint, sizeof(snap.setpoint));
            g_ctx->series[1].live_val = snap.latest_setpoint;
        }
        /* Series 2: Outside */
        if (g_ctx->series_count > 2) {
            memcpy(g_ctx->series[2].points, snap.outside, sizeof(snap.outside));
            g_ctx->series[2].live_val = snap.latest_outside;
        }
    } else {
        /* Generic file loader for third-party integrations */
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        g_ctx->cur_idx = (tm.tm_hour * 6) + (tm.tm_min / 10);

        for (int s = 0; s < g_ctx->series_count; s++) {
            chart_series_ctx_t * cs = &g_ctx->series[s];
            for (int i = 0; i < TEMPLOGGER_POINTS; i++) cs->points[i] = TEMPLOGGER_INVALID_VAL;

            if (!cs->data_file[0]) continue;
            char path[256];
            if (cs->data_file[0] == '/') {
                snprintf(path, sizeof(path), "%s", cs->data_file);
            } else {
                snprintf(path, sizeof(path), "/HCBv2/var/%s", cs->data_file);
                if (access(path, R_OK) != 0) {
                    snprintf(path, sizeof(path), "/mnt/data/integrations/%s/%s",
                             g_ctx->meta.id, cs->data_file);
                }
            }

            FILE * f = fopen(path, "r");
            if (!f) continue;
            char line[4096];
            size_t rd = fread(line, 1, sizeof(line) - 1, f);
            fclose(f);
            if (rd == 0) continue;
            line[rd] = 0;

            int count = 0;
            char * ptr = line;
            while (*ptr && count < TEMPLOGGER_POINTS) {
                while (*ptr == ' ' || *ptr == '\n' || *ptr == '\r' || *ptr == '\t') ptr++;
                if (!*ptr) break;
                char * endp = NULL;
                float v = strtof(ptr, &endp);
                if (endp == ptr) break;
                if (v > -25.0f && v < 150.0f) cs->points[count] = v;
                count++;
                ptr = endp;
                while (*ptr == ' ' || *ptr == ',') ptr++;
            }
        }
    }

    /* Populate points into LVGL series */
    for (int s = 0; s < g_ctx->series_count; s++) {
        chart_series_ctx_t * cs = &g_ctx->series[s];
        if (!cs->ser) continue;
        for (int i = 0; i < TEMPLOGGER_POINTS; i++) {
            float v = cs->points[i];
            if (v == TEMPLOGGER_INVALID_VAL || v <= -25.0f) {
                cs->ser->y_points[i] = LV_CHART_POINT_NONE;
            } else {
                cs->ser->y_points[i] = (lv_coord_t)chart_roundf(v * 10.0f);
            }
        }
    }
}

static void on_scr_delete(lv_event_t * e) {
    (void)e;
    if (g_ctx) {
        free(g_ctx);
        g_ctx = NULL;
    }
}

lv_obj_t * screen_chart_24h_create(const integration_meta_t * meta) {
    if (g_ctx) {
        free(g_ctx);
        g_ctx = NULL;
    }
    g_ctx = calloc(1, sizeof(chart_screen_ctx_t));
    if (!g_ctx) return NULL;

    if (meta) {
        memcpy(&g_ctx->meta, meta, sizeof(integration_meta_t));
    } else {
        snprintf(g_ctx->meta.id, sizeof(g_ctx->meta.id), "temperaturelogger");
        snprintf(g_ctx->meta.name, sizeof(g_ctx->meta.name), "Temperature Logger");
        snprintf(g_ctx->meta.detail_title, sizeof(g_ctx->meta.detail_title),
                 "%s", tr("Temperatuurlog afgelopen 24 uur", "Temperature log past 24 hours"));
        snprintf(g_ctx->meta.detail_y_unit, sizeof(g_ctx->meta.detail_y_unit), "°C");
    }

    /* Configure default series for TemperatureLogger if not declared */
    if (g_ctx->meta.series_count == 0) {
        g_ctx->series_count = 3;
        snprintf(g_ctx->series[0].name, sizeof(g_ctx->series[0].name), "%s", tr("Kamer", "Room"));
        g_ctx->series[0].color_hex = 0x3388ff;

        snprintf(g_ctx->series[1].name, sizeof(g_ctx->series[1].name), "%s", tr("Setpoint", "Setpoint"));
        g_ctx->series[1].color_hex = 0xff4444;

        snprintf(g_ctx->series[2].name, sizeof(g_ctx->series[2].name), "%s", tr("Buiten", "Outside"));
        g_ctx->series[2].color_hex = 0xffbb00;
    } else {
        g_ctx->series_count = g_ctx->meta.series_count;
        for (int i = 0; i < g_ctx->series_count; i++) {
            snprintf(g_ctx->series[i].name, sizeof(g_ctx->series[i].name), "%s", g_ctx->meta.series[i].name);
            g_ctx->series[i].color_hex = parse_hex_color(g_ctx->meta.series[i].color, 0x3388ff);
            snprintf(g_ctx->series[i].data_file, sizeof(g_ctx->series[i].data_file), "%s", g_ctx->meta.series[i].data_file);
        }
    }

    snprintf(g_ctx->y_unit, sizeof(g_ctx->y_unit), "%s",
             g_ctx->meta.detail_y_unit[0] ? g_ctx->meta.detail_y_unit : "°C");
    g_ctx->auto_scale = 1;

    /* ---- Screen Root ---- */
    lv_obj_t * scr = lv_obj_create(NULL);
    g_ctx->scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_scr_delete, LV_EVENT_DELETE, NULL);

    /* ---- Top Bar: Back Button ---- */
    lv_obj_t * btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, SX(110), SY(48));
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, SX(12), SY(10));
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(btn_back, 10, 0);
    lv_obj_set_ext_click_area(btn_back, 25);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, tr(LV_SYMBOL_LEFT " Terug", LV_SYMBOL_LEFT " Back"));
    lv_obj_set_style_text_font(lbl_back, SF(18), 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_center(lbl_back);

    /* ---- Top Bar: Title ---- */
    const char * title_text = g_ctx->meta.detail_title[0] ? g_ctx->meta.detail_title : g_ctx->meta.name;
    lv_obj_t * title = lv_label_create(scr);
    g_ctx->lbl_title = title;
    lv_obj_set_style_text_font(title, SF(22), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_label_set_text(title, title_text);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, SY(14));

    /* ---- Top Bar: Controls (Auto, ▲, ▼) ---- */
    lv_obj_t * ctl_box = lv_obj_create(scr);
    lv_obj_set_size(ctl_box, SX(240), SY(48));
    lv_obj_align(ctl_box, LV_ALIGN_TOP_RIGHT, SX(-12), SY(10));
    lv_obj_set_style_bg_opa(ctl_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctl_box, 0, 0);
    lv_obj_set_style_pad_all(ctl_box, 0, 0);
    lv_obj_clear_flag(ctl_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * btn_down = lv_btn_create(ctl_box);
    lv_obj_set_size(btn_down, SX(44), SY(44));
    lv_obj_align(btn_down, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_down, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(btn_down, 8, 0);
    lv_obj_set_ext_click_area(btn_down, 15);
    lv_obj_add_event_cb(btn_down, on_scale_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ld = lv_label_create(btn_down);
    lv_label_set_text(ld, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(ld, SF(18), 0);
    lv_obj_center(ld);

    lv_obj_t * btn_up = lv_btn_create(ctl_box);
    lv_obj_set_size(btn_up, SX(44), SY(44));
    lv_obj_align(btn_up, LV_ALIGN_RIGHT_MID, SX(-48), 0);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(btn_up, 8, 0);
    lv_obj_set_ext_click_area(btn_up, 15);
    lv_obj_add_event_cb(btn_up, on_scale_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lu = lv_label_create(btn_up);
    lv_label_set_text(lu, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(lu, SF(18), 0);
    lv_obj_center(lu);

    lv_obj_t * btn_auto = lv_btn_create(ctl_box);
    lv_obj_set_size(btn_auto, SX(64), SY(44));
    lv_obj_align(btn_auto, LV_ALIGN_RIGHT_MID, SX(-96), 0);
    lv_obj_set_style_bg_color(btn_auto, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(btn_auto, 8, 0);
    lv_obj_set_ext_click_area(btn_auto, 15);
    lv_obj_add_event_cb(btn_auto, on_scale_auto, LV_EVENT_CLICKED, NULL);
    lv_obj_t * la = lv_label_create(btn_auto);
    lv_label_set_text(la, "Auto");
    lv_obj_set_style_text_font(la, SF(14), 0);
    lv_obj_center(la);

    /* ---- Interactive Legend Bar ---- */
    lv_obj_t * leg_bar = lv_obj_create(scr);
    lv_obj_set_size(leg_bar, SX(800), SY(48));
    lv_obj_align(leg_bar, LV_ALIGN_TOP_MID, 0, SY(64));
    lv_obj_set_style_bg_opa(leg_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(leg_bar, 0, 0);
    lv_obj_set_style_pad_all(leg_bar, 0, 0);
    lv_obj_set_flex_flow(leg_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(leg_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(leg_bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < g_ctx->series_count; i++) {
        chart_series_ctx_t * s = &g_ctx->series[i];

        lv_obj_t * chip = lv_btn_create(leg_bar);
        s->chip_btn = chip;
        lv_obj_set_height(chip, SY(38));
        lv_obj_set_style_pad_hor(chip, SX(14), 0);
        lv_obj_set_style_pad_ver(chip, SY(6), 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_radius(chip, 19, 0);
        lv_obj_set_style_border_width(chip, 2, 0);
        lv_obj_set_style_border_color(chip, lv_color_hex(s->color_hex), 0);
        lv_obj_set_ext_click_area(chip, 10);
        lv_obj_add_event_cb(chip, on_chip_toggle, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t * row = lv_obj_create(chip);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* Colored dot / indicator box */
        lv_obj_t * dot = lv_obj_create(row);
        lv_obj_set_size(dot, SX(14), SY(14));
        lv_obj_set_style_bg_color(dot, lv_color_hex(s->color_hex), 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);

        lv_obj_t * lbl = lv_label_create(row);
        s->chip_lbl = lbl;
        lv_obj_set_style_text_font(lbl, SF(16), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_HI), 0);
        lv_obj_set_style_pad_left(lbl, SX(8), 0);
        lv_label_set_text(lbl, s->name);
    }

    /* ---- Main 24-Hour Chart ---- */
    int chart_w = SX(DISP_HOR > 800 ? 910 : 710);
    int chart_h = SY(DISP_VER > 480 ? 390 : 290);
    int chart_x = SX(DISP_HOR > 800 ? 70 : 60);
    int chart_y = SY(DISP_VER > 480 ? 125 : 115);

    lv_obj_t * chart = lv_chart_create(scr);
    g_ctx->chart = chart;
    lv_obj_set_size(chart, chart_w, chart_h);
    lv_obj_set_pos(chart, chart_x, chart_y);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x0e1722), 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_radius(chart, 8, 0);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);

    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, TEMPLOGGER_POINTS);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);    /* No point dots */
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);  /* Thicker graph lines (3px) */
    lv_obj_set_style_line_color(chart, lv_color_hex(0x223246), LV_PART_MAIN); /* Darker raster/grid lines */
    lv_obj_set_style_line_opa(chart, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_chart_set_div_line_count(chart, 5, 13);             /* 5 horizontal, 13 vertical = 2h each */

    /* Add series to chart */
    for (int i = 0; i < g_ctx->series_count; i++) {
        chart_series_ctx_t * s = &g_ctx->series[i];
        s->ser = lv_chart_add_series(chart, lv_color_hex(s->color_hex), LV_CHART_AXIS_PRIMARY_Y);
    }

    /* Load history values into series */
    load_series_data();

    /* Update legend chips with live readings if present */
    for (int i = 0; i < g_ctx->series_count; i++) {
        chart_series_ctx_t * s = &g_ctx->series[i];
        if (s->live_val != TEMPLOGGER_INVALID_VAL && s->live_val > -50.0f) {
            lv_label_set_text_fmt(s->chip_lbl, "%s: %.1f %s", s->name, s->live_val, g_ctx->y_unit);
        }
    }

    /* ---- Y-Axis Labels (Left of chart) ---- */
    for (int i = 0; i < 5; i++) {
        lv_obj_t * yl = lv_label_create(scr);
        g_ctx->y_lbls[i] = yl;
        lv_obj_set_style_text_font(yl, SF(12), 0);
        lv_obj_set_style_text_color(yl, lv_color_hex(COL_TEXT_DIM), 0);
        /* Align Y-labels evenly along the chart's vertical height */
        int y_pos = chart_y + chart_h - (i * chart_h / 4) - SY(8);
        lv_obj_set_pos(yl, SX(4), y_pos);
    }

    /* ---- X-Axis Hour Labels (0:00 to 24:00, every 2 hours) ---- */
    for (int h = 0; h <= 24; h += 2) {
        lv_obj_t * xl = lv_label_create(scr);
        lv_obj_set_style_text_font(xl, SF(12), 0);
        lv_obj_set_style_text_color(xl, lv_color_hex(COL_TEXT_DIM), 0);
        char hbuf[16];
        snprintf(hbuf, sizeof(hbuf), "%02d:00", h % 24);
        lv_label_set_text(xl, hbuf);

        int xp = chart_x + (h * chart_w / 24) - SX(16);
        if (xp < chart_x) xp = chart_x;
        if (xp > chart_x + chart_w - SX(32)) xp = chart_x + chart_w - SX(32);
        lv_obj_set_pos(xl, xp, chart_y + chart_h + SY(4));
    }

    /* ---- Vertical "Now" Cursor Line & Date Badges ---- */
    int cursor_x = chart_x + (g_ctx->cur_idx * chart_w / TEMPLOGGER_POINTS);
    lv_obj_t * cur_line = lv_obj_create(scr);
    g_ctx->cursor_line = cur_line;
    lv_obj_set_size(cur_line, 2, chart_h + SY(16));
    lv_obj_set_pos(cur_line, cursor_x, chart_y - SY(8));
    lv_obj_set_style_bg_color(cur_line, lv_color_hex(0x55aaff), 0);
    lv_obj_set_style_bg_opa(cur_line, LV_OPA_80, 0);
    lv_obj_set_style_border_width(cur_line, 0, 0);
    lv_obj_clear_flag(cur_line, LV_OBJ_FLAG_SCROLLABLE);

    /* Date markers: today (left) and yesterday (right) */
    time_t now = time(NULL);
    struct tm tm_today;
    localtime_r(&now, &tm_today);

    time_t yesterday = now - 86400;
    struct tm tm_yesterday;
    localtime_r(&yesterday, &tm_yesterday);

    lv_obj_t * dl = lv_label_create(scr);
    g_ctx->cursor_lbl_left = dl;
    lv_obj_set_style_text_font(dl, SF(12), 0);
    lv_obj_set_style_text_color(dl, lv_color_hex(0x55aaff), 0);
    lv_label_set_text_fmt(dl, "%02d-%02d " LV_SYMBOL_LEFT,
                          tm_today.tm_mday, tm_today.tm_mon + 1);
    lv_obj_align_to(dl, cur_line, LV_ALIGN_OUT_LEFT_TOP, SX(-6), SY(10));

    lv_obj_t * dr = lv_label_create(scr);
    g_ctx->cursor_lbl_right = dr;
    lv_obj_set_style_text_font(dr, SF(12), 0);
    lv_obj_set_style_text_color(dr, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text_fmt(dr, LV_SYMBOL_RIGHT " %02d-%02d",
                          tm_yesterday.tm_mday, tm_yesterday.tm_mon + 1);
    lv_obj_align_to(dr, cur_line, LV_ALIGN_OUT_RIGHT_TOP, SX(6), SY(10));

    /* Calculate auto bounds and render initial view */
    recalc_auto_range();

    return scr;
}

