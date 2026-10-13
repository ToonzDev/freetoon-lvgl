/*
 * screen_calendar.c — full-screen "Agenda" modal listing upcoming events from
 * calendar_state (HA + iCal, merged/sorted in calendar.c).
 * Single screen view with page forward/backward navigation to avoid vertical scrolling.
 */
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"   /* SF()/SX()/SY() scaling for Toon 1 */
#include "i18n.h"
#include "calendar.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CAL_EVENTS_PER_PAGE 16

static lv_obj_t * modal = NULL;
static lv_obj_t * s_col_left = NULL;
static lv_obj_t * s_col_right = NULL;
static lv_obj_t * s_btn_prev = NULL;
static lv_obj_t * s_btn_next = NULL;
static lv_obj_t * s_lbl_page = NULL;
static int        s_cal_page = 0;

static void on_close(lv_event_t * e) {
    (void)e;
    if (modal) { lv_obj_del(modal); modal = NULL; }
    s_col_left = NULL;
    s_col_right = NULL;
    s_btn_prev = NULL;
    s_btn_next = NULL;
    s_lbl_page = NULL;
}

/* "YYYY-MM-DD" → "wo 27 mei" (Dutch). Falls back to the raw date on error. */
static void pretty_date(const char * iso, char * out, size_t osz) {
    const char * wd[] = {
        tr("zo","Sun"), tr("ma","Mon"), tr("di","Tue"), tr("wo","Wed"),
        tr("do","Thu"), tr("vr","Fri"), tr("za","Sat") };
    const char * mo[] = {
        tr("jan","Jan"), tr("feb","Feb"), tr("mrt","Mar"), tr("apr","Apr"),
        tr("mei","May"), tr("jun","Jun"), tr("jul","Jul"), tr("aug","Aug"),
        tr("sep","Sep"), tr("okt","Oct"), tr("nov","Nov"), tr("dec","Dec") };
    int y, m, d;
    if (sscanf(iso, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) {
        snprintf(out, osz, "%s", iso); return;
    }
    struct tm tm = {0};
    tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = 12;
    time_t t = mktime(&tm);
    struct tm lt; localtime_r(&t, &lt);
    snprintf(out, osz, "%s %d %s", wd[lt.tm_wday % 7], d, mo[m - 1]);
}

static void render_calendar_page(void) {
    if (!s_col_left || !s_col_right) return;

    lv_obj_clean(s_col_left);
    lv_obj_clean(s_col_right);

    int total_events = calendar_state.count;
    int total_pages = (total_events + CAL_EVENTS_PER_PAGE - 1) / CAL_EVENTS_PER_PAGE;
    if (total_pages < 1) total_pages = 1;

    if (s_cal_page >= total_pages) s_cal_page = total_pages - 1;
    if (s_cal_page < 0) s_cal_page = 0;

    /* Update pagination controls */
    if (s_lbl_page) {
        if (total_pages > 1) {
            lv_label_set_text_fmt(s_lbl_page, tr("Pagina %d / %d", "Page %d / %d"),
                                  s_cal_page + 1, total_pages);
            lv_obj_clear_flag(s_lbl_page, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_page, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_btn_prev) {
        if (total_pages > 1 && s_cal_page > 0) {
            lv_obj_clear_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_btn_next) {
        if (total_pages > 1 && s_cal_page < total_pages - 1) {
            lv_obj_clear_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int start_idx = s_cal_page * CAL_EVENTS_PER_PAGE;
    int end_idx = start_idx + CAL_EVENTS_PER_PAGE;
    if (end_idx > total_events) end_idx = total_events;
    int page_count = end_idx - start_idx;

    int half = (page_count + 1) / 2;
    char last_date_left[12] = "";
    char last_date_right[12] = "";

    for (int j = 0; j < page_count; j++) {
        int i = start_idx + j;
        calendar_event_t * ev = &calendar_state.ev[i];
        int in_left = (j < half);
        lv_obj_t * target_col = in_left ? s_col_left : s_col_right;
        char * last_d = in_left ? last_date_left : last_date_right;
        int is_col_first = in_left ? (j == 0) : (j == half);

        /* Date header whenever the day changes in this column */
        if (strcmp(ev->date, last_d) != 0) {
            snprintf(last_d, 12, "%s", ev->date);
            char pd[32]; pretty_date(ev->date, pd, sizeof pd);
            lv_obj_t * h = lv_label_create(target_col);
            lv_label_set_text(h, pd);
            lv_obj_set_style_text_color(h, lv_color_hex(0x66ccff), 0);
            lv_obj_set_style_text_font(h, SF(22), 0);
            lv_obj_set_style_pad_top(h, is_col_first ? 0 : 10, 0);
        }

        /* Format time string: start time - end time (CR8) */
        char time_str[32];
        if (ev->time[0]) {
            if (ev->end_time[0] && strcmp(ev->time, ev->end_time) != 0)
                snprintf(time_str, sizeof time_str, "%s - %s", ev->time, ev->end_time);
            else
                snprintf(time_str, sizeof time_str, "%s", ev->time);
        } else {
            snprintf(time_str, sizeof time_str, "%s", tr("hele dag", "all day"));
        }

        lv_obj_t * row = lv_label_create(target_col);
        lv_obj_set_width(row, SX(470));
        lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
        char line[160];
        snprintf(line, sizeof line, "  %s  %s", time_str, ev->summary);
        lv_label_set_text(row, line);
        lv_obj_set_style_text_color(row, lv_color_hex(0xdddddd), 0);
        lv_obj_set_style_text_font(row, SF(18), 0);
    }
}

static void on_cal_prev(lv_event_t * e) {
    (void)e;
    if (s_cal_page > 0) {
        s_cal_page--;
        render_calendar_page();
    }
}

static void on_cal_next(lv_event_t * e) {
    (void)e;
    int total_pages = (calendar_state.count + CAL_EVENTS_PER_PAGE - 1) / CAL_EVENTS_PER_PAGE;
    if (s_cal_page < total_pages - 1) {
        s_cal_page++;
        render_calendar_page();
    }
}

void screen_calendar_show(void) {
    if (modal) lv_obj_del(modal);
    s_cal_page = 0;

    modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0f1a2a), 0);
    lv_obj_set_style_bg_opa(modal, 240, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Close button */
    lv_obj_t * close = lv_btn_create(modal);
    int cb_w = (DISP_HOR < 1000) ? 68 : 76;
    int cb_h = (DISP_VER < 600) ? 46 : 56;
    lv_obj_set_size(close, cb_w, cb_h);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(close, lv_color_hex(0x33445a), 0);
    lv_obj_set_style_radius(close, 12, 0);
    lv_obj_set_ext_click_area(close, 30);
    lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(cl, SF(22), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
    lv_obj_center(cl);

    /* Title */
    lv_obj_t * title = lv_label_create(modal);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Agenda", "Calendar"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(20));

    /* Pagination header controls */
    s_btn_prev = lv_btn_create(modal);
    lv_obj_set_size(s_btn_prev, SX(130), SY(46));
    lv_obj_align(s_btn_prev, LV_ALIGN_TOP_LEFT, SX(320), SY(17));
    lv_obj_set_style_bg_color(s_btn_prev, lv_color_hex(0x223a56), 0);
    lv_obj_set_style_radius(s_btn_prev, 8, 0);
    lv_obj_add_event_cb(s_btn_prev, on_cal_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_t * pl = lv_label_create(s_btn_prev);
    lv_label_set_text_fmt(pl, "%s %s", LV_SYMBOL_LEFT, tr("Vorige", "Previous"));
    lv_obj_set_style_text_color(pl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(pl, SF(18), 0);
    lv_obj_center(pl);

    s_lbl_page = lv_label_create(modal);
    lv_obj_set_style_text_color(s_lbl_page, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(s_lbl_page, SF(18), 0);
    lv_obj_align(s_lbl_page, LV_ALIGN_TOP_LEFT, SX(465), SY(30));

    s_btn_next = lv_btn_create(modal);
    lv_obj_set_size(s_btn_next, SX(130), SY(46));
    lv_obj_align(s_btn_next, LV_ALIGN_TOP_LEFT, SX(600), SY(17));
    lv_obj_set_style_bg_color(s_btn_next, lv_color_hex(0x223a56), 0);
    lv_obj_set_style_radius(s_btn_next, 8, 0);
    lv_obj_add_event_cb(s_btn_next, on_cal_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t * nl = lv_label_create(s_btn_next);
    lv_label_set_text_fmt(nl, "%s %s", tr("Volgende", "Next"), LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(nl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(nl, SF(18), 0);
    lv_obj_center(nl);

    /* Content container - non-scrolling single page */
    lv_obj_t * cont = lv_obj_create(modal);
    lv_obj_set_size(cont, SX(976), SY(500));
    lv_obj_set_pos(cont, SX(24), SY(84));
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    if (!settings.calendar_enabled || (!settings.calendar_ha_entity[0] && !settings.calendar_ics_url[0])) {
        lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t * m = lv_label_create(cont);
        lv_label_set_text(m, tr("Agenda niet ingesteld.\nZet een HA-agenda of een iCal-URL in Instellingen.",
                                "Calendar not set up.\nSet a HA calendar or iCal URL in Settings."));
        lv_obj_set_style_text_color(m, lv_color_hex(0x99aabb), 0);
        lv_obj_set_style_text_font(m, SF(22), 0);
        return;
    }
    if (calendar_state.count == 0) {
        lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t * m = lv_label_create(cont);
        lv_label_set_text(m, calendar_state.connected
            ? tr("Geen aankomende afspraken.", "No upcoming events.")
            : tr("Agenda laden... (of geen verbinding)", "Loading calendar... (or no connection)"));
        lv_obj_set_style_text_color(m, lv_color_hex(0x99aabb), 0);
        lv_obj_set_style_text_font(m, SF(22), 0);
        return;
    }

    /* 2 columns (left and right) */
    s_col_left = lv_obj_create(cont);
    lv_obj_set_pos(s_col_left, 0, 0);
    lv_obj_set_size(s_col_left, SX(476), SY(490));
    lv_obj_set_style_bg_opa(s_col_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_col_left, 0, 0);
    lv_obj_set_style_pad_all(s_col_left, 0, 0);
    lv_obj_set_style_pad_row(s_col_left, 8, 0);
    lv_obj_add_flag(s_col_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_col_left, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_col_left, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_col_left, LV_FLEX_FLOW_COLUMN);

    s_col_right = lv_obj_create(cont);
    lv_obj_set_pos(s_col_right, SX(496), 0);
    lv_obj_set_size(s_col_right, SX(476), SY(490));
    lv_obj_set_style_bg_opa(s_col_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_col_right, 0, 0);
    lv_obj_set_style_pad_all(s_col_right, 0, 0);
    lv_obj_set_style_pad_row(s_col_right, 8, 0);
    lv_obj_add_flag(s_col_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_col_right, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_col_right, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_col_right, LV_FLEX_FLOW_COLUMN);

    render_calendar_page();
}
