/*
 * screen_waste.c — Full-screen Waste Calendar (Afvalkalender) modal.
 *
 * Shows full upcoming scheduled waste pickups (beyond the 3 on the home tile).
 */

#include "screens.h"
#include "display.h"
#include "settings.h"
#include "theme.h"
#include "i18n.h"
#include "icons.h"
#include "wastecollection.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WASTE_PER_PAGE 10

static lv_obj_t * modal = NULL;
static lv_obj_t * s_col_left = NULL;
static lv_obj_t * s_col_right = NULL;
static lv_obj_t * s_btn_prev = NULL;
static lv_obj_t * s_btn_next = NULL;
static lv_obj_t * s_lbl_page = NULL;
static int s_waste_page = 0;

static void on_close(lv_event_t * e) {
    (void)e;
    screen_waste_close();
}

void screen_waste_close(void) {
    if (modal) {
        lv_obj_del_async(modal);
        modal = NULL;
        s_col_left = NULL;
        s_col_right = NULL;
        s_btn_prev = NULL;
        s_btn_next = NULL;
        s_lbl_page = NULL;
    }
}

static void format_waste_date(const char * ymd, char * out, size_t outsz) {
    if (!ymd || strlen(ymd) < 10) {
        snprintf(out, outsz, "--");
        return;
    }
    int yr = 0, mo = 0, da = 0;
    if (sscanf(ymd, "%4d-%2d-%2d", &yr, &mo, &da) != 3) {
        snprintf(out, outsz, "%s", ymd);
        return;
    }
    struct tm t = {0};
    t.tm_year = yr - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = da;
    mktime(&t);
    snprintf(out, outsz, "%s %d %s",
             i18n_day_short(t.tm_wday), da, i18n_month_short(t.tm_mon));
}

static void render_waste_page(void) {
    if (!s_col_left || !s_col_right) return;
    lv_obj_clean(s_col_left);
    lv_obj_clean(s_col_right);

    int total_events = waste_state.event_count;
    int total_pages = (total_events + WASTE_PER_PAGE - 1) / WASTE_PER_PAGE;
    if (total_pages < 1) total_pages = 1;
    if (s_waste_page >= total_pages) s_waste_page = total_pages - 1;

    if (s_lbl_page) {
        if (total_pages > 1) {
            lv_obj_clear_flag(s_lbl_page, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(s_lbl_page, "%s %d / %d",
                                  tr("Pagina", "Page"), s_waste_page + 1, total_pages);
        } else {
            lv_obj_add_flag(s_lbl_page, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_btn_prev) {
        if (total_pages > 1 && s_waste_page > 0)
            lv_obj_clear_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_btn_next) {
        if (total_pages > 1 && s_waste_page < total_pages - 1)
            lv_obj_clear_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
    }

    int start_idx = s_waste_page * WASTE_PER_PAGE;
    int end_idx = start_idx + WASTE_PER_PAGE;
    if (end_idx > total_events) end_idx = total_events;
    int page_count = end_idx - start_idx;
    int half = (page_count + 1) / 2;

    for (int j = 0; j < page_count; j++) {
        int i = start_idx + j;
        waste_event_t * ev = &waste_state.events[i];
        lv_obj_t * target_col = (j < half) ? s_col_left : s_col_right;

        /* Row card */
        lv_obj_t * card = lv_obj_create(target_col);
        lv_obj_set_size(card, SX(470), SY(80));
        lv_obj_set_style_bg_color(card, lv_color_hex(THEME_CARD_BG), 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_border_width(card, settings.home_light_mode ? 1 : 0, 0);
        lv_obj_set_style_pad_all(card, SX(10), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* Icon */
        lv_obj_t * ico = lv_img_create(card);
        lv_img_set_src(ico, waste_icon_for_label(ev->label));
        lv_obj_set_style_img_recolor(ico, lv_color_hex(waste_accent_for_label(ev->label)), 0);
        lv_obj_set_style_img_recolor_opa(ico, 255, 0);
        lv_obj_align(ico, LV_ALIGN_LEFT_MID, SX(4), 0);

        /* Stream label - full row width */
        lv_obj_t * lbl = lv_label_create(card);
        lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(lbl, SF(19), 0);
        lv_obj_set_width(lbl, SX(408));
        lv_obj_set_height(lbl, SY(26));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_label_set_text(lbl, ev->label);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(48), SY(6));

        /* Formatted date (bottom left) */
        char dt_str[48];
        format_waste_date(ev->date, dt_str, sizeof dt_str);
        lv_obj_t * dlbl = lv_label_create(card);
        lv_obj_set_style_text_color(dlbl, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(dlbl, SF(16), 0);
        lv_label_set_text(dlbl, dt_str);
        lv_obj_align(dlbl, LV_ALIGN_BOTTOM_LEFT, SX(48), -SY(6));

        /* Days until badge (bottom right, same height as date) */
        long days = waste_days_until(ev->date);
        lv_obj_t * b_lbl = lv_label_create(card);
        lv_obj_set_style_text_font(b_lbl, SF(15), 0);
        if (days == 0) {
            lv_label_set_text(b_lbl, tr("Vandaag", "Today"));
            lv_obj_set_style_text_color(b_lbl, lv_color_hex(0x22cc66), 0);
        } else if (days == 1) {
            lv_label_set_text(b_lbl, tr("Morgen", "Tomorrow"));
            lv_obj_set_style_text_color(b_lbl, lv_color_hex(THEME_WARN_ORANGE), 0);
        } else if (days > 1) {
            lv_label_set_text_fmt(b_lbl, tr("over %ld dgn", "in %ld d"), days);
            lv_obj_set_style_text_color(b_lbl, lv_color_hex(THEME_TEXT_MUTED), 0);
        } else {
            lv_label_set_text(b_lbl, "");
        }
        lv_obj_align(b_lbl, LV_ALIGN_BOTTOM_RIGHT, -SX(10), -SY(6));
    }
}

static void on_waste_prev(lv_event_t * e) {
    (void)e;
    if (s_waste_page > 0) {
        s_waste_page--;
        render_waste_page();
    }
}

static void on_waste_next(lv_event_t * e) {
    (void)e;
    int total_pages = (waste_state.event_count + WASTE_PER_PAGE - 1) / WASTE_PER_PAGE;
    if (s_waste_page < total_pages - 1) {
        s_waste_page++;
        render_waste_page();
    }
}

void screen_waste_show(void) {
    if (modal) lv_obj_del(modal);
    s_waste_page = 0;

    modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
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
    lv_obj_set_style_bg_color(close, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_border_color(close, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(close, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_radius(close, 12, 0);
    lv_obj_set_ext_click_area(close, 30);
    lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(cl, SF(22), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(cl);

    /* Title */
    lv_obj_t * title = lv_label_create(modal);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Afvalkalender", "Waste Calendar"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(20));

    /* Pagination controls */
    s_btn_prev = lv_btn_create(modal);
    lv_obj_set_size(s_btn_prev, SX(130), SY(46));
    lv_obj_align(s_btn_prev, LV_ALIGN_TOP_LEFT, SX(340), SY(17));
    lv_obj_set_style_bg_color(s_btn_prev, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_border_color(s_btn_prev, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(s_btn_prev, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_radius(s_btn_prev, 8, 0);
    lv_obj_add_event_cb(s_btn_prev, on_waste_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_t * pl = lv_label_create(s_btn_prev);
    lv_label_set_text_fmt(pl, "%s %s", LV_SYMBOL_LEFT, tr("Vorige", "Previous"));
    lv_obj_set_style_text_color(pl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(pl, SF(18), 0);
    lv_obj_center(pl);

    s_lbl_page = lv_label_create(modal);
    lv_obj_set_style_text_color(s_lbl_page, lv_color_hex(THEME_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(s_lbl_page, SF(18), 0);
    lv_obj_align(s_lbl_page, LV_ALIGN_TOP_LEFT, SX(485), SY(30));

    s_btn_next = lv_btn_create(modal);
    lv_obj_set_size(s_btn_next, SX(130), SY(46));
    lv_obj_align(s_btn_next, LV_ALIGN_TOP_LEFT, SX(630), SY(17));
    lv_obj_set_style_bg_color(s_btn_next, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_border_color(s_btn_next, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(s_btn_next, settings.home_light_mode ? 1 : 0, 0);
    lv_obj_set_style_radius(s_btn_next, 8, 0);
    lv_obj_add_event_cb(s_btn_next, on_waste_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t * nl = lv_label_create(s_btn_next);
    lv_label_set_text_fmt(nl, "%s %s", tr("Volgende", "Next"), LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(nl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(nl, SF(18), 0);
    lv_obj_center(nl);

    /* Content container */
    lv_obj_t * cont = lv_obj_create(modal);
    lv_obj_set_size(cont, SX(976), SY(500));
    lv_obj_set_pos(cont, SX(24), SY(84));
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    if (waste_state.event_count == 0) {
        lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t * m = lv_label_create(cont);
        lv_label_set_text(m, tr("Geen ophaaldata gevonden.\nControleer Afval-instellingen.",
                                "No collection dates found.\nCheck Waste settings."));
        lv_obj_set_style_text_color(m, lv_color_hex(THEME_TEXT_MUTED), 0);
        lv_obj_set_style_text_font(m, SF(22), 0);
        lv_obj_align(m, LV_ALIGN_CENTER, 0, -SY(40));
        return;
    }

    /* 2 columns */
    s_col_left = lv_obj_create(cont);
    lv_obj_set_pos(s_col_left, 0, 0);
    lv_obj_set_size(s_col_left, SX(476), SY(490));
    lv_obj_set_style_bg_opa(s_col_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_col_left, 0, 0);
    lv_obj_set_style_pad_all(s_col_left, 0, 0);
    lv_obj_set_style_pad_row(s_col_left, SY(8), 0);
    lv_obj_clear_flag(s_col_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_col_left, LV_FLEX_FLOW_COLUMN);

    s_col_right = lv_obj_create(cont);
    lv_obj_set_pos(s_col_right, SX(496), 0);
    lv_obj_set_size(s_col_right, SX(476), SY(490));
    lv_obj_set_style_bg_opa(s_col_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_col_right, 0, 0);
    lv_obj_set_style_pad_all(s_col_right, 0, 0);
    lv_obj_set_style_pad_row(s_col_right, SY(8), 0);
    lv_obj_clear_flag(s_col_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_col_right, LV_FLEX_FLOW_COLUMN);

    render_waste_page();
    lv_obj_move_foreground(close);
}
