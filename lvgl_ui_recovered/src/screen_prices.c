/*
 * screen_prices.c — "Energieprijzen" (#44): today's and tomorrow's hourly
 * all-in electricity prices as bars, the current hour marked, the cheapest
 * 2-hour block highlighted, plus the gas price of the day.
 * Opened from Statistics (dynamic contract only). Owned screen: built fresh
 * on every open, freed when it leaves the stack.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "energy_prices.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define NBARS 48
#define CHEAP_HOURS 2

static lv_obj_t * scr = NULL;
static lv_obj_t * area = NULL;          /* chart area, rebuilt on news */
static lv_obj_t * lbl_now = NULL, * lbl_cheap = NULL, * lbl_note = NULL;

static void eur(char * out, size_t n, double v, int dec) {
    snprintf(out, n, "€ %.*f", dec, v);
    if (settings.language != LANG_EN) { char * d = strchr(out, '.'); if (d) *d = ','; }
}

static long local_midnight(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_isdst = -1;
    return (long)mktime(&tm);
}

static void build_chart(void) {
    if (!area) return;
    lv_obj_clean(area);
    lv_obj_update_layout(area);
    int W = lv_obj_get_content_width(area), Hh = lv_obj_get_content_height(area);

    long mid = local_midnight();
    long now_h = (long)time(NULL) / 3600;
    double v[NBARS];
    int n = 0;
    for (int i = 0; i < NBARS; i++) {
        double mk;
        /* hour i after local midnight (DST days: off by one at the switch, fine) */
        if (ep_market_at(EP_ELEC, mid + i * 3600L, &mk)) { v[i] = ep_allin(EP_ELEC, mk); n = i + 1; }
        else v[i] = NAN;
    }
    if (n < 24) n = 24;                  /* always show today */

    double lo = 0, hi = 0.05;
    for (int i = 0; i < n; i++) if (!isnan(v[i])) { if (v[i] > hi) hi = v[i]; if (v[i] < lo) lo = v[i]; }
    hi *= 1.08;
    /* cheap / expensive per day: against that day's own quartiles */
    double qd[2][2] = { { 0, 0 }, { 0, 0 } };
    int have_q[2] = { 0, 0 };
    for (int d = 0; d < 2; d++) {
        double s[24]; int k = 0;
        for (int i = d * 24; i < d * 24 + 24 && i < n; i++) if (!isnan(v[i])) s[k++] = v[i];
        if (k < 20) continue;
        for (int a = 1; a < k; a++) for (int b = a; b > 0 && s[b - 1] > s[b]; b--) { double t = s[b]; s[b] = s[b - 1]; s[b - 1] = t; }
        qd[d][0] = s[k / 4]; qd[d][1] = s[(3 * k) / 4]; have_q[d] = 1;
    }

    long cheap_start = 0; double cheap_avg = 0;
    int have_cheap = ep_cheapest(CHEAP_HOURS, &cheap_start, &cheap_avg);

    int ylab_w = SX(70), xlab_h = SY(26);
    int cw = W - ylab_w, ch = Hh - xlab_h;
    int slot = cw / n;
    int bw = slot - (slot > 10 ? 3 : 1);
    if (bw < 2) bw = 2;
    int y0 = (int)(ch * hi / (hi - lo));            /* zero line */

    /* gridlines + y labels: 0 and two steps */
    for (int k = 0; k <= 2; k++) {
        double val = hi / 1.08 * k / 2.0;
        int y = y0 - (int)(val / (hi - lo) * ch);
        lv_obj_t * g = lv_obj_create(area);
        lv_obj_remove_style_all(g);
        lv_obj_set_pos(g, ylab_w, y);
        lv_obj_set_size(g, cw, 1);
        lv_obj_set_style_bg_color(g, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_60, 0);
        char b[24];
        eur(b, sizeof b, val, 2);
        lv_obj_t * l = lv_label_create(area);
        lv_label_set_text(l, b);
        lv_obj_set_style_text_font(l, SF(14), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_pos(l, 0, y - SY(9));
    }

    for (int i = 0; i < n; i++) {
        int x = ylab_w + i * slot + (slot - bw) / 2;
        long h = mid / 3600 + i;
        if (!isnan(v[i])) {
            int bh = (int)(fabs(v[i]) / (hi - lo) * ch);
            if (bh < 2) bh = 2;
            lv_obj_t * b = lv_obj_create(area);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, bw, bh);
            lv_obj_set_pos(b, x, v[i] >= 0 ? y0 - bh : y0);
            lv_obj_set_style_radius(b, 2, 0);
            uint32_t c = 0x4aa3ff;                                     /* normal */
            int dd = i >= 24;
            if (have_q[dd] && v[i] <= qd[dd][0]) c = 0x2fbf9f;           /* cheap */
            if (have_q[dd] && v[i] >= qd[dd][1]) c = 0xff9a3c;           /* expensive */
            if (have_cheap && h >= cheap_start / 3600 && h < cheap_start / 3600 + CHEAP_HOURS) c = 0x3ccf7a;
            lv_obj_set_style_bg_color(b, lv_color_hex(c), 0);
            lv_obj_set_style_bg_opa(b, h < now_h ? LV_OPA_40 : LV_OPA_COVER, 0);
            if (h == now_h) {
                lv_obj_set_style_border_color(b, lv_color_hex(0xffffff), 0);
                lv_obj_set_style_border_width(b, 2, 0);
            }
        }
        /* x labels every 3 h; "morgen" at tomorrow 00 */
        if (i % 3 == 0 && i != 27) {     /* 27: room for "morgen" */
            lv_obj_t * l = lv_label_create(area);
            if (i == 24) lv_label_set_text(l, tr("morgen", "tmrw"));
            else         lv_label_set_text_fmt(l, "%02d", i % 24);
            lv_obj_set_style_text_font(l, SF(14), 0);
            lv_obj_set_style_text_color(l, lv_color_hex(i == 24 ? THEME_TEXT_HI : THEME_TEXT_DIM), 0);
            lv_obj_set_pos(l, ylab_w + i * slot, ch + SY(4));
        }
    }
    if (n <= 24) {
        lv_obj_t * l = lv_label_create(area);
        lv_label_set_text(l, tr("Prijzen van morgen komen rond 13:00", "Tomorrow's prices arrive around 13:00"));
        lv_obj_set_style_text_font(l, SF(14), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    /* header lines */
    char b1[32], b2[32], line[160];
    double mk;
    if (ep_market_at(EP_ELEC, (long)time(NULL), &mk)) {
        eur(b1, sizeof b1, ep_allin(EP_ELEC, mk), 2);
        if (ep_market_at(EP_GAS, (long)time(NULL), &mk)) {
            eur(b2, sizeof b2, ep_allin(EP_GAS, mk), 2);
            snprintf(line, sizeof line, tr("Nu %s/kWh    |    Gas vandaag %s/m3", "Now %s/kWh    |    Gas today %s/m3"), b1, b2);
        } else
            snprintf(line, sizeof line, tr("Nu %s/kWh", "Now %s/kWh"), b1);
    } else {
        snprintf(line, sizeof line, "%s", ep_busy() ? tr("Prijzen laden...", "Loading prices...")
                                                    : tr("Geen prijzen (geen internet?)", "No prices (no internet?)"));
    }
    lv_label_set_text(lbl_now, line);
    if (have_cheap) {
        time_t a = (time_t)cheap_start, z = (time_t)(cheap_start + CHEAP_HOURS * 3600);
        struct tm ta, tz;
        localtime_r(&a, &ta); localtime_r(&z, &tz);
        eur(b1, sizeof b1, cheap_avg, 2);
        int tomorrow = cheap_start >= mid + 86400;
        snprintf(line, sizeof line, tr("Goedkoopst (%d uur): %s%02d:00 - %02d:00, gemiddeld %s/kWh",
                                       "Cheapest (%d h): %s%02d:00 - %02d:00, average %s/kWh"),
                 CHEAP_HOURS, tomorrow ? tr("morgen ", "tomorrow ") : "", ta.tm_hour, tz.tm_hour, b1);
        lv_label_set_text(lbl_cheap, line);
    } else lv_label_set_text(lbl_cheap, "");
    eur(b1, sizeof b1, settings.dyn_elec_surcharge, 3);
    eur(b2, sizeof b2, settings.dyn_gas_surcharge, 3);
    snprintf(line, sizeof line, tr("All-in: marktprijs (EnergyZero) x 1,21 + opslag %s/kWh, gas + %s/m3",
                                   "All-in: market price (EnergyZero) x 1.21 + surcharge %s/kWh, gas + %s/m3"), b1, b2);
    lv_label_set_text(lbl_note, line);
}

static void on_news(void) {
    if (scr && lv_scr_act() == scr) build_chart();
}
static void on_close(lv_event_t * e) { (void)e; ui_pop(); }
static void on_delete(lv_event_t * e) {
    (void)e;
    ep_remove_listener(on_news);
    scr = NULL; area = NULL; lbl_now = lbl_cheap = lbl_note = NULL;
}

lv_obj_t * screen_prices_create(void) {
    ep_start();
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);

    int x_w = (DISP_HOR < 1000) ? 68 : 74, x_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_t * x = lv_btn_create(scr);
    lv_obj_set_size(x, x_w, x_h);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, SX(-12), SY(12));
    lv_obj_set_style_bg_color(x, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(x, 12, 0);
    lv_obj_set_ext_click_area(x, 25);
    lv_obj_add_event_cb(x, on_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_obj_center(xl);

    lv_obj_t * t = lv_label_create(scr);
    lv_label_set_text(t, tr("Energieprijzen", "Energy prices"));
    lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(28), 0);
    lv_obj_set_pos(t, SX(24), SY(18));

    lbl_now = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_now, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_now, SF(22), 0);
    lv_obj_set_pos(lbl_now, SX(24), SY(78));

    lbl_cheap = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_cheap, lv_color_hex(0x3ccf7a), 0);
    lv_obj_set_style_text_font(lbl_cheap, SF(18), 0);
    lv_obj_set_pos(lbl_cheap, SX(24), SY(116));

    area = lv_obj_create(scr);
    lv_obj_remove_style_all(area);
    lv_obj_set_pos(area, SX(16), SY(156));
    lv_obj_set_size(area, LV_HOR_RES - SX(32), LV_VER_RES - SY(156) - SY(44));
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lbl_note = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_note, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_note, SF(14), 0);
    lv_obj_set_width(lbl_note, LV_HOR_RES - SX(48));
    lv_label_set_long_mode(lbl_note, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_note, LV_ALIGN_BOTTOM_LEFT, SX(24), -SY(12));

    build_chart();
    ep_add_listener(on_news);
    return scr;
}
