/*
 * screen_calendar.c — full-screen "Agenda" modal listing upcoming events from
 * calendar_state (HA + iCal, merged/sorted in calendar.c). Opened on demand
 * via screen_calendar_show() — from Settings now, and from the home tile later.
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

static lv_obj_t * modal = NULL;

static void on_close(lv_event_t * e) {
    (void)e;
    if (modal) { lv_obj_del(modal); modal = NULL; }
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

void screen_calendar_show(void) {
    if (modal) lv_obj_del(modal);

    /* Parent to the top layer, NOT lv_scr_act(): this is opened from the
     * Settings list, which is usually scrolled down by the time you reach the
     * Agenda button — a modal on the scrolled screen inherits that scroll
     * offset and its top ends up above the viewport. The top layer is a fixed
     * full-screen overlay that's never scrolled. */
    modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0f1a2a), 0);
    lv_obj_set_style_bg_opa(modal, 240, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * close = lv_btn_create(modal);
    lv_obj_set_size(close, SX(56), SY(56));
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, SX(-16), SY(12));
    lv_obj_set_style_bg_color(close, lv_color_hex(0x33445a), 0);
    lv_obj_set_style_radius(close, 12, 0);
    lv_obj_set_ext_click_area(close, 20);
    lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
    lv_obj_center(cl);

    lv_obj_t * title = lv_label_create(modal);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Agenda", "Calendar"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(20));

    lv_obj_t * cont = lv_obj_create(modal);
    lv_obj_set_size(cont, SX(976), SY(500));
    lv_obj_set_pos(cont, SX(24), SY(84));
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    if (!settings.calendar_enabled || (!settings.calendar_ha_entity[0] && !settings.calendar_ics_url[0])) {
        lv_obj_t * m = lv_label_create(cont);
        lv_label_set_text(m, tr("Agenda niet ingesteld.\nZet een HA-agenda of een iCal-URL in Instellingen.",
                                "Calendar not set up.\nSet a HA calendar or iCal URL in Settings."));
        lv_obj_set_style_text_color(m, lv_color_hex(0x99aabb), 0);
        lv_obj_set_style_text_font(m, SF(22), 0);
        return;
    }
    if (calendar_state.count == 0) {
        lv_obj_t * m = lv_label_create(cont);
        lv_label_set_text(m, calendar_state.connected
            ? tr("Geen aankomende afspraken.", "No upcoming events.")
            : tr("Agenda laden... (of geen verbinding)", "Loading calendar... (or no connection)"));
        lv_obj_set_style_text_color(m, lv_color_hex(0x99aabb), 0);
        lv_obj_set_style_text_font(m, SF(22), 0);
        return;
    }

    /* 2 columns (left and right) to show double the amount of appointments (CR8) */
    lv_obj_t * col_left = lv_obj_create(cont);
    lv_obj_set_pos(col_left, 0, 0);
    lv_obj_set_size(col_left, SX(476), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col_left, 0, 0);
    lv_obj_set_style_pad_all(col_left, 0, 0);
    lv_obj_set_style_pad_row(col_left, 6, 0);
    lv_obj_clear_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * col_right = lv_obj_create(cont);
    lv_obj_set_pos(col_right, SX(496), 0);
    lv_obj_set_size(col_right, SX(476), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col_right, 0, 0);
    lv_obj_set_style_pad_all(col_right, 0, 0);
    lv_obj_set_style_pad_row(col_right, 6, 0);
    lv_obj_clear_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);

    int half = (calendar_state.count + 1) / 2;
    char last_date_left[12] = "";
    char last_date_right[12] = "";

    for (int i = 0; i < calendar_state.count; i++) {
        calendar_event_t * ev = &calendar_state.ev[i];
        int in_left = (i < half);
        lv_obj_t * target_col = in_left ? col_left : col_right;
        char * last_d = in_left ? last_date_left : last_date_right;
        int is_col_first = in_left ? (i == 0) : (i == half);

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
