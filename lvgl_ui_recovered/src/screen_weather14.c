/*
 * 14-day forecast as a chart (Buienradar, weather_ext[]): per day the date,
 * icon, maximum (orange line) and minimum (blue line) and a rain-chance bar.
 * Opened with "14 dagen" on the weather screen. A tap on a day opens its
 * hourly forecast (the first days, where Buienradar gives hours and
 * weather_state keeps them) or else a detail card with the day values.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "weather.h"
#include "icons.h"
#include "settings.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#undef THEME_BG
#undef THEME_CARD_BG
#undef THEME_TEXT_HI
#undef THEME_TEXT_DIM
#undef THEME_BTN_BG
#define THEME_BG        (settings.home_light_mode ? 0xeef4fa : 0x0f1722)
#define THEME_CARD_BG   (settings.home_light_mode ? 0xffffff : 0x141f2d)
#define THEME_TEXT_HI   (settings.home_light_mode ? 0x112233 : 0xffffff)
#define THEME_TEXT_DIM  (settings.home_light_mode ? 0x667788 : 0x7a8c9e)
#define THEME_BTN_BG    (settings.home_light_mode ? 0xd0d8e2 : 0x223344)
#define COL_MAX         (settings.home_light_mode ? 0xc2410c : 0xffa333)
#define COL_MIN         (settings.home_light_mode ? 0x1d4ed8 : 0x5a9bd3)
#define COL_RAIN        (settings.home_light_mode ? 0x0284c7 : 0x38bdf8)
#define COL_GRID        (settings.home_light_mode ? 0xd5dee8 : 0x22303f)

static lv_obj_t * scr_root = NULL;
static lv_obj_t * s_modal = NULL;
static weather_ext_day_t s_days[WEATHER_EXT_DAYS];
static int s_n = 0;
static lv_point_t s_pts_max[WEATHER_EXT_DAYS], s_pts_min[WEATHER_EXT_DAYS];

static void on_close(lv_event_t * e) { (void)e; ui_pop(); }

/* floor / ceil without libm (no new imports on the Toons) */
static int ifloor(float v) { int i = (int)v; return (v < (float)i) ? i - 1 : i; }
static int iceil(float v)  { int i = (int)v; return (v > (float)i) ? i + 1 : i; }

static void fmt_t(char * b, size_t n, float t) {
    snprintf(b, n, "%.0f°", (double)t);
    if (!strcmp(b, "-0°")) snprintf(b, n, "0°");
}

/* "1,5" (NL) / "1.5" */
static void fmt_dec(char * b, size_t n, double v) {
    snprintf(b, n, "%.1f", v);
    if (settings.language != LANG_EN) { char * d = strchr(b, '.'); if (d) *d = ','; }
}

static void modal_close(void) {
    if (s_modal) { lv_obj_del(s_modal); s_modal = NULL; }
}
static void on_modal_close(lv_event_t * e) { (void)e; modal_close(); }
static void on_modal_delete(lv_event_t * e) {
    if (lv_event_get_target(e) == s_modal) s_modal = NULL;
}

static void detail_row(lv_obj_t * card, int * y, const char * name, const char * val) {
    lv_obj_t * l = lv_label_create(card);
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(l, SF(18), 0);
    lv_label_set_text(l, name);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, SX(24), *y);
    lv_obj_t * r = lv_label_create(card);
    lv_obj_set_style_text_color(r, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(r, SF(20), 0);
    lv_label_set_text(r, val);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, SX(-24), *y - SY(2));
    *y += SY(38);
}

/* Day values for a day without hours (or any day Buienradar gives no hours for). */
static void show_detail(int i) {
    if (i < 0 || i >= s_n || !scr_root) return;
    modal_close();
    const weather_ext_day_t * D = &s_days[i];
    s_modal = lv_obj_create(scr_root);
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_60, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal, on_modal_close, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_modal, on_modal_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t * card = lv_obj_create(s_modal);
    lv_obj_set_size(card, SX(520), SY(i >= 7 ? 420 : 390));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);      /* taps on the card don't close */

    lv_obj_t * ic = lv_img_create(card);
    lv_img_set_src(ic, weather_icon_for(D->icon));
    lv_obj_set_style_img_recolor(ic, lv_color_hex(weather_icon_color_for(D->icon)), 0);
    lv_obj_set_style_img_recolor_opa(ic, 255, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_RIGHT, SX(-20), SY(16));

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(26), 0);
    lv_label_set_text(t, D->day);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, SX(24), SY(18));
    lv_obj_t * ds = lv_label_create(card);
    lv_obj_set_style_text_color(ds, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(ds, SF(18), 0);
    lv_label_set_text(ds, D->desc);
    lv_obj_align(ds, LV_ALIGN_TOP_LEFT, SX(24), SY(54));

    char a[32], b[32], v[80];
    int y = SY(96);
    fmt_t(a, sizeof a, D->max_temp); fmt_t(b, sizeof b, D->min_temp);
    snprintf(v, sizeof v, "%s / %s", a, b);
    detail_row(card, &y, tr("Max / min", "Max / min"), v);
    fmt_dec(a, sizeof a, D->rain_mm);
    snprintf(v, sizeof v, "%d%%  (%s mm)", D->rain_chance, a);
    detail_row(card, &y, tr("Kans op neerslag", "Chance of rain"), v);
    snprintf(v, sizeof v, "%s %d Bft", D->wind_dir[0] ? D->wind_dir : "-", D->wind_bft);
    detail_row(card, &y, tr("Wind", "Wind"), v);
    if (D->cloud >= 0) {
        snprintf(v, sizeof v, "%d%%", D->cloud);
        detail_row(card, &y, tr("Bewolking", "Cloud cover"), v);
    }
    if (D->sunrise[0] && D->sunset[0]) {
        snprintf(v, sizeof v, "%s - %s", D->sunrise, D->sunset);
        detail_row(card, &y, tr("Zon op / onder", "Sunrise / sunset"), v);
    }
    if (i >= 7) {
        lv_obj_t * n = lv_label_create(card);
        lv_obj_set_style_text_color(n, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(n, SF(15), 0);
        lv_obj_set_width(n, SX(470));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        lv_label_set_text(n, tr("Verwachting voor de tweede week: minder zeker, alleen dagwaarden.",
                                "Second-week forecast: less certain, day values only."));
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, SX(24), y + SY(4));
    }

    lv_obj_t * btn = lv_btn_create(card);
    lv_obj_set_size(btn, SX(150), SY(46));
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, SY(-16));
    lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, on_modal_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(btn);
    lv_label_set_text(bl, tr("Sluiten", "Close"));
    lv_obj_set_style_text_color(bl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(bl, SF(18), 0);
    lv_obj_center(bl);
}

/* Tap on a day: its hours when the weather screen keeps them, else the card. */
static void on_day(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    int hourly = -1;
    weather_lock();
    for (int d = 0; d < weather_state.day_count && d < WEATHER_FORECAST_DAYS; d++)
        if (!strcmp(weather_state.days[d].date_iso, s_days[i].date_iso) && weather_state.days[d].hour_count > 0)
            hourly = d;
    weather_unlock();
    if (hourly >= 0) screen_weather_hourly_show(hourly);
    else show_detail(i);
}

static void on_scr_delete(lv_event_t * e) {
    (void)e;
    scr_root = NULL;
    s_modal = NULL;
}

lv_obj_t * screen_weather14_create(void) {
    weather_lock();
    s_n = weather_ext_count;
    memcpy(s_days, weather_ext, sizeof s_days);
    weather_unlock();

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("14-daagse verwachting", "14-day forecast"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(16));

    lv_obj_t * close_btn = lv_btn_create(scr_root);
    int cb_w = (DISP_HOR < 1000) ? 68 : 74;
    int cb_h = (DISP_VER < 600) ? 42 : 46;
    lv_obj_set_size(close_btn, cb_w, cb_h);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_set_ext_click_area(close_btn, 20);
    lv_obj_add_event_cb(close_btn, on_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(close_btn);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_obj_center(xl);

    if (s_n == 0) {
        lv_obj_t * l = lv_label_create(scr_root);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(l, SF(20), 0);
        lv_label_set_text(l, tr("Nog geen verwachting van Buienradar.", "No forecast from Buienradar yet."));
        lv_obj_center(l);
        return scr_root;
    }

    /* Layout: columns over the full width. Rows: day, date, icon, max label,
     * chart (lines), min label, rain bar + %. */
    const int left = SX(12), right = DISP_HOR - SX(12);
    const int colw = (right - left) / WEATHER_EXT_DAYS;
    const int y_day = SY(70), y_icon = SY(116), y_top = SY(178), y_bot = SY(388);
    const int y_rain = SY(470), rain_h = SY(56);

    float tmin = 1e9f, tmax = -1e9f;
    for (int i = 0; i < s_n; i++) {
        if (s_days[i].min_temp < tmin) tmin = s_days[i].min_temp;
        if (s_days[i].max_temp > tmax) tmax = s_days[i].max_temp;
    }
    tmin = (float)ifloor(tmin) - 1; tmax = (float)iceil(tmax) + 1;
    if (tmax - tmin < 6) { float m = (tmax + tmin) / 2; tmin = m - 3; tmax = m + 3; }

    /* week split: the second week is less certain (no hours) */
    if (s_n > 7) {
        lv_obj_t * sep = lv_obj_create(scr_root);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, 2, y_rain + rain_h - y_day + SY(30));
        lv_obj_set_pos(sep, left + 7 * colw - 1, y_day - SY(6));
        lv_obj_set_style_bg_color(sep, lv_color_hex(COL_GRID), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE);
    }

    for (int i = 0; i < s_n && i < WEATHER_EXT_DAYS; i++) {
        const weather_ext_day_t * D = &s_days[i];
        int cx = left + i * colw + colw / 2;
        int dim = i >= 7;

        /* tap area for the whole column */
        lv_obj_t * col = lv_obj_create(scr_root);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, colw - 2, y_rain + rain_h + SY(28) - (y_day - SY(8)));
        lv_obj_set_pos(col, left + i * colw + 1, y_day - SY(8));
        lv_obj_set_style_radius(col, 8, 0);
        lv_obj_set_style_bg_color(col, lv_color_hex(THEME_CARD_BG), 0);
        lv_obj_set_style_bg_opa(col, (i % 2) ? LV_OPA_TRANSP : LV_OPA_60, 0);
        lv_obj_set_style_bg_color(col, lv_color_hex(settings.home_light_mode ? 0xccdae8 : 0x223348), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(col, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(col, on_day, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* "ma 13-5" -> "ma" over "13-5" */
        char dn[8] = "", dd[12] = "";
        sscanf(D->day, "%7s %11s", dn, dd);
        lv_obj_t * l1 = lv_label_create(scr_root);
        lv_obj_set_style_text_color(l1, lv_color_hex(dim ? THEME_TEXT_DIM : THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(l1, SF(18), 0);
        lv_label_set_text(l1, i == 0 ? tr("vand.", "today") : dn);
        lv_obj_update_layout(l1);
        lv_obj_set_pos(l1, cx - lv_obj_get_width(l1) / 2, y_day);
        lv_obj_t * l2 = lv_label_create(scr_root);
        lv_obj_set_style_text_color(l2, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(l2, SF(14), 0);
        lv_label_set_text(l2, dd);
        lv_obj_update_layout(l2);
        lv_obj_set_pos(l2, cx - lv_obj_get_width(l2) / 2, y_day + SY(22));

        lv_obj_t * ic = lv_img_create(scr_root);
        lv_img_set_src(ic, weather_icon_for(D->icon));
        lv_obj_set_style_img_recolor(ic, lv_color_hex(weather_icon_color_for(D->icon)), 0);
        lv_obj_set_style_img_recolor_opa(ic, 255, 0);
        lv_obj_update_layout(ic);
        lv_obj_set_pos(ic, cx - lv_obj_get_width(ic) / 2, y_icon);
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);

        int ymax = y_top + (int)((tmax - D->max_temp) / (tmax - tmin) * (float)(y_bot - y_top));
        int ymin = y_top + (int)((tmax - D->min_temp) / (tmax - tmin) * (float)(y_bot - y_top));
        s_pts_max[i].x = (lv_coord_t)cx; s_pts_max[i].y = (lv_coord_t)ymax;
        s_pts_min[i].x = (lv_coord_t)cx; s_pts_min[i].y = (lv_coord_t)ymin;

        char tb[16];
        lv_obj_t * lx = lv_label_create(scr_root);
        lv_obj_set_style_text_color(lx, lv_color_hex(COL_MAX), 0);
        lv_obj_set_style_text_font(lx, SF(18), 0);
        fmt_t(tb, sizeof tb, D->max_temp);
        lv_label_set_text(lx, tb);
        lv_obj_update_layout(lx);
        lv_obj_set_pos(lx, cx - lv_obj_get_width(lx) / 2, ymax - SY(30));
        lv_obj_t * ln = lv_label_create(scr_root);
        lv_obj_set_style_text_color(ln, lv_color_hex(COL_MIN), 0);
        lv_obj_set_style_text_font(ln, SF(18), 0);
        fmt_t(tb, sizeof tb, D->min_temp);
        lv_label_set_text(ln, tb);
        lv_obj_update_layout(ln);
        lv_obj_set_pos(ln, cx - lv_obj_get_width(ln) / 2, ymin + SY(8));

        /* rain chance: bar from the bottom of its band + % under it */
        int rc = D->rain_chance < 0 ? 0 : D->rain_chance > 100 ? 100 : D->rain_chance;
        int bh = rc * rain_h / 100;
        lv_obj_t * track = lv_obj_create(scr_root);
        lv_obj_remove_style_all(track);
        lv_obj_set_size(track, colw / 3, rain_h);
        lv_obj_set_pos(track, cx - colw / 6, y_rain);
        lv_obj_set_style_bg_color(track, lv_color_hex(COL_GRID), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(track, 3, 0);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
        if (bh > 0) {
            lv_obj_t * bar = lv_obj_create(scr_root);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, colw / 3, bh < 2 ? 2 : bh);
            lv_obj_set_pos(bar, cx - colw / 6, y_rain + rain_h - (bh < 2 ? 2 : bh));
            lv_obj_set_style_bg_color(bar, lv_color_hex(COL_RAIN), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bar, 3, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_t * lr = lv_label_create(scr_root);
        lv_obj_set_style_text_color(lr, lv_color_hex(rc >= 50 ? COL_RAIN : THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(lr, SF(14), 0);
        lv_label_set_text_fmt(lr, "%d%%", rc);
        lv_obj_update_layout(lr);
        lv_obj_set_pos(lr, cx - lv_obj_get_width(lr) / 2, y_rain + rain_h + SY(4));
    }

    lv_obj_t * lmax = lv_line_create(scr_root);
    lv_line_set_points(lmax, s_pts_max, (uint16_t)s_n);
    lv_obj_set_style_line_color(lmax, lv_color_hex(COL_MAX), 0);
    lv_obj_set_style_line_width(lmax, 3, 0);
    lv_obj_set_style_line_rounded(lmax, true, 0);
    lv_obj_t * lmin = lv_line_create(scr_root);
    lv_line_set_points(lmin, s_pts_min, (uint16_t)s_n);
    lv_obj_set_style_line_color(lmin, lv_color_hex(COL_MIN), 0);
    lv_obj_set_style_line_width(lmin, 3, 0);
    lv_obj_set_style_line_rounded(lmin, true, 0);
    for (int i = 0; i < s_n; i++) {          /* dots on the points */
        for (int k = 0; k < 2; k++) {
            lv_obj_t * dot = lv_obj_create(scr_root);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, SX(9), SX(9));
            const lv_point_t * p = k ? &s_pts_min[i] : &s_pts_max[i];
            lv_obj_set_pos(dot, p->x - SX(9) / 2, p->y - SX(9) / 2);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(k ? COL_MIN : COL_MAX), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    lv_obj_t * hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(hint, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(hint, SF(14), 0);
    lv_label_set_text(hint, tr("Tik op een dag voor de details  |  bron: Buienradar",
                               "Tap a day for details  |  source: Buienradar"));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, SY(-6));

    lv_obj_move_foreground(close_btn);
    return scr_root;
}

void screen_weather14_show(void) {
    if (scr_root) { lv_obj_del(scr_root); scr_root = NULL; }
    ui_push(screen_weather14_create());
}
